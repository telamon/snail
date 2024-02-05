#include "snail.h"
#include "nanr.h"
#include "rpc.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
// #include "repo.h"
#include "esp_timer.h"
#define TAG "rpc.c"
#define PORT                        1984
// IDLE = seconds idle to send heartbeat, INTERVAL = seconds between heartbeats.
// COUNT = N-fails to connection reset.
#define KEEPALIVE_IDLE              6
#define KEEPALIVE_INTERVAL          KEEPALIVE_IDLE
#define KEEPALIVE_COUNT             3

#define EV_RPC_PEER_SOCKET BIT1
#define XF_BUFFER_SIZE 5000
static struct {
  EventGroupHandle_t event_group;
  TaskHandle_t server_task;
  bool listening;
  int listen_sock;

  TaskHandle_t client_task;
  esp_netif_t *netif;
  ip_addr_t *addr;
  char rx_buffer[XF_BUFFER_SIZE];
} rpc_state;

static int dispatch_preloaded (int socket, u8_t type, u16_t len) {
  char *rx_buffer = rpc_state.rx_buffer;
  struct tlv_header* hdr = (void*)rx_buffer;
  hdr->type = type;
  hdr->length = len;
  // send() can return less bytes than supplied length.
  // Walk-around for robust implementation.
  int total = len + sizeof(struct tlv_header);
  int remain = total;
  while (remain > 0) {
    int written = send(socket, rx_buffer + (total - remain), remain, 0);
    if (written < 0) return written;
    remain -= written;
  }
  // ESP_LOGI(TAG, "send_msg(%zu) done", len + sizeof(len));
  return len + sizeof(len);
}

static int recv_msg (int socket) {
  char *rx_buffer = rpc_state.rx_buffer;
  struct tlv_header* hdr = (void*)rx_buffer;
  hdr->type = 0;
  hdr->length = 0;
  size_t offset = 0;
  /* Read TLV header */
  do {
    int len = recv(socket, rx_buffer, sizeof(struct tlv_header) - offset, 0);
    if (len < 1) return len; // READERR|CLOSE
    offset += len;
  } while (offset < sizeof(struct tlv_header));

  u16_t total = hdr->length + sizeof(struct tlv_header);

  // ESP_LOGI(TAG, "Incoming transmission, len: %i + hsize: %zu = total %i", hdr->length, sizeof(struct tlv_header), total);
  if (total > XF_BUFFER_SIZE) {
    ESP_LOGE(TAG, "RX Invalid Message Length (%i), MAX=%zu", hdr->length, XF_BUFFER_SIZE - sizeof(struct tlv_header));
    return -1;
  }

  /* Quick return on L=0 signals */
  if (hdr->length == 0) return offset;

  /* Read body */
  do {
    // ESP_LOGI(TAG, "block read range (%zu => %zu)", offset, total - offset);
    int len = recv(socket, rx_buffer + offset, total - offset, 0);
    if (len < 1) return len; // READERR|CLOSE
    offset += len;
    // ESP_LOGI(TAG, "recv_msg::recv buffering %zuB", offset);
  } while (offset < total);

  if (offset > total) {
    ESP_LOGE(TAG, "RX overflow, expected (%i), received (%zu)", total, offset);
    return -1;
  }
  // ESP_LOGI(TAG, "recv_msg(%zu) done", offset);
  return offset;
}


struct conversation_handlers {
  void (*on_init) (bool initiator, uint16_t MAX_SIZE);
  int (*on_message) (struct tlv_header *io_header, char *io_message);
  void (*on_deinit) (int exit_code);
};

struct {
  bool initiator;
  uint16_t round;
  uint32_t rx;
  uint32_t tx;
  int64_t start;
  uint16_t max_size;
} dummy_bench;

void dummy_init(bool initiator, uint16_t max_size){
  dummy_bench.rx = dummy_bench.tx = dummy_bench.round = 0;
  dummy_bench.start = esp_timer_get_time();
  dummy_bench.max_size = max_size;
  dummy_bench.initiator = initiator;
}
int dummy_reply (struct tlv_header *io_header, char *io_message) {
  if (dummy_bench.initiator) {
    if (dummy_bench.round > 60) return 0;
  }
  if (!(dummy_bench.initiator && dummy_bench.round == 0)) {
    dummy_bench.rx += io_header->length;
  }
  io_header->length = dummy_bench.max_size;
  io_header->type = CMD_BLOCK;
  memset(io_message, dummy_bench.round, dummy_bench.max_size);
  dummy_bench.tx += io_header->length;
  int64_t duration = esp_timer_get_time() - dummy_bench.start;
  ESP_LOGI(TAG, "BW-BENCH: msg %i %uB [RX %.2f KB/s, TX: %.2f KB/s]",
      dummy_bench.round,
      io_header->length,
      (dummy_bench.rx / (duration / 1000000.0)) / 1024,
      (dummy_bench.tx / (duration / 1000000.0)) / 1024
      );
  dummy_bench.round++;
  return io_header->length;
}

void dummy_deinit(int exit_code){
  int64_t duration = esp_timer_get_time() - dummy_bench.start;
  ESP_LOGI(TAG, "Throughput test complete exit: %i (%"PRId64" ms) [RX %.2f B/s, TX: %.2f B/s]",
      exit_code,
      duration / 1000,
      dummy_bench.rx / (duration / 1000000.0),
      dummy_bench.tx / (duration / 1000000.0)
      );
}
struct conversation_handlers bench_convo = {
  .on_init=dummy_init,
  .on_deinit=dummy_deinit,
  .on_message=dummy_reply
};

/* Mind clearing exercise */
int do_communicate (bool initiator, const int sock) {
  struct conversation_handlers *handlers = &bench_convo;
  char* buffer = rpc_state.rx_buffer;
  memset(buffer, 0, XF_BUFFER_SIZE); /* clear previous communication */
  int exit_code = -1;
  struct tlv_header *header = (void*)buffer;
  char *message = buffer + sizeof(struct tlv_header);
  const uint16_t MAX_SIZE = XF_BUFFER_SIZE - sizeof(struct tlv_header);
  handlers->on_init(initiator, MAX_SIZE);
  /* using wire protocol - piconet style */
  bool phase = initiator;
  while (true) {
    if (!phase) { // Receive
      int received = recv_msg(sock);
      if (received < 0) {
        exit_code = received;
        ESP_LOGE(TAG, "Readerror: %i", received);
        ESP_LOGE(TAG, "Global error: %s", strerror(errno));
        break;
      } else if (received == 0 || header->type == CMD_BYE) {
        message[header->length] = 0; /* Nullterm bye message */
        ESP_LOGI(TAG, "[BYE] %s", message);
        break;
      }
    } else { // Transmit
      int to_send = handlers->on_message(header, message);

      if (to_send == 0) {
        header->type = CMD_BYE;
        header->length = 0;
      } else if (to_send < 0) {
        exit_code = to_send;
        header->type = CMD_ERR;
        header->length = 4;
        memcpy(message, "ERR!", 4);
      }

      exit_code = dispatch_preloaded(sock, header->type, header->length);
      if (exit_code < 0) break; /* TX Failed */

      /* After TX */
      if(header->type == CMD_BYE || header->type == CMD_ERR) {
        break;
      }
    }
    phase = !phase;
    delay(10);
  }
  handlers->on_deinit(exit_code);
  return exit_code;
}

static void tcp_server_task(void *pvParameters) {
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

    if (addr_family == AF_INET6) {
        struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
        bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
        dest_addr_ip6->sin6_family = AF_INET6;
        dest_addr_ip6->sin6_port = htons(PORT);
        ip_protocol = IPPROTO_IPV6;
    }

    rpc_state.listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (rpc_state.listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: (%d) %s", errno, lwip_strerr(errno));
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(rpc_state.listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
    // Note that by default IPV6 binds to both protocols, it is must be disabled
    // if both protocols used at the same time (used in CI)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif
    ESP_LOGI(TAG, "Socket created");
    int err = bind(rpc_state.listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: (%d) %s", errno, lwip_strerr(errno));
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto DEINIT;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(rpc_state.listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: (%d) %s", errno, lwip_strerr(errno));
        goto DEINIT;
    }
    rpc_state.listening = true;
    while (rpc_state.listening) {
        ESP_LOGI(TAG, "blocking task with accept()");
        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(rpc_state.listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: (%d) %s", errno, lwip_strerr(errno));
            break;
        }
        xEventGroupSetBits(rpc_state.event_group, EV_RPC_PEER_SOCKET);
        // g_state->status = INFORM;

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        // Convert ip address to string
        if (source_addr.ss_family == PF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        }

        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);
        int ret = do_communicate(false, sock);
        shutdown(sock, 0);
        close(sock);
        nanr_inform_complete(ret);
        vTaskDelay(1);
    }

DEINIT:
    close(rpc_state.listen_sock);
    rpc_state.server_task = 0; // Bad idea?
    vTaskDelete(NULL);
}

void rpc_listen () {
  rpc_state.event_group = xEventGroupCreate();
  xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET6, 5, &rpc_state.server_task);
}

void tcp_client_task(void *pvParameters) {
  ip_addr_t *target = rpc_state.addr;
  const char *host = ip6addr_ntoa(&target->u_addr.ip6);
  int exit_code = -1;

  struct sockaddr_in6 dest_addr = { 0 };
  /* lwip is confusing; in6_addr posix compliant while ip6_addr_t is LWIP local? */
  memcpy(&dest_addr.sin6_addr, &target->u_addr.ip6.addr, sizeof(dest_addr.sin6_addr));
  dest_addr.sin6_family = AF_INET6;
  dest_addr.sin6_port = htons(PORT);

  int sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Unable to create socket: (%d) %s", errno, lwip_strerr(errno));
    return;
  }
  ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host, PORT);

  dest_addr.sin6_scope_id = esp_netif_get_netif_impl_index(rpc_state.netif);
  ESP_LOGI(TAG, "Interface index: %" PRIu32, dest_addr.sin6_scope_id);

  int errCount = 0;
  int err = 0;

  do  {
    err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) ESP_LOGE(TAG, "Socket unable to connect: (%d) %s", errno, lwip_strerr(errno));
    if (10 < errCount++) goto DEINIT;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  } while (err != 0);

  struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ESP_LOGI(TAG, "Successfully connected");
  xEventGroupSetBits(rpc_state.event_group, EV_RPC_PEER_SOCKET);

  exit_code = do_communicate(true, sock);

DEINIT:
  if (sock != -1) {
    ESP_LOGI(TAG, "Socket disconnected %s:%d", host, PORT);
    shutdown(sock, 0);
    close(sock);
  }
  nanr_inform_complete(exit_code);
  rpc_state.client_task = 0;
  vTaskDelete(NULL);
}

int rpc_connect(esp_netif_t *netif, ip_addr_t *target) {
  if (rpc_state.client_task != 0) {
    eTaskState s = eTaskGetState(rpc_state.client_task);
    if (s != eDeleted && s != eInvalid) {
      ESP_LOGW(TAG, "rpc_connect() will attempt to delete previous task (%i)", s);
      vTaskDelete(rpc_state.client_task); // Attempt to delete it
      delay(100);
      s = eTaskGetState(rpc_state.client_task);
      if (s != eDeleted && s != eInvalid) return -1;
    }
  }
  rpc_state.addr = target;
  rpc_state.netif = netif;
  xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 10, &rpc_state.client_task);
  return 0;
}

int rpc_wait_for_peer_socket(TickType_t t) {
  EventBits_t bits = xEventGroupWaitBits(rpc_state.event_group, EV_RPC_PEER_SOCKET, pdFALSE, pdFALSE, t);
  xEventGroupClearBits(rpc_state.event_group, EV_RPC_PEER_SOCKET);
  if (bits & EV_RPC_PEER_SOCKET) return 0;
  else return -1;
}
