#include "lwip/ip_addr.h"
#include "nanr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#define RPC "rpc.c"
#define PORT                        1337
// IDLE = seconds idle to send heartbeat, INTERVAL = seconds between heartbeats.
// COUNT = N-fails to connection reset.
#define KEEPALIVE_IDLE              2
#define KEEPALIVE_INTERVAL          KEEPALIVE_IDLE
#define KEEPALIVE_COUNT             3

#define XF_BUFFER_SIZE 4096
static struct {
  TaskHandle_t server_task;
  bool listening;
  int listen_sock;

  TaskHandle_t client_task;
  esp_netif_t *netif;
  ip_addr_t *addr;
  char rx_buffer[XF_BUFFER_SIZE];
} rpc_state;

static int send_msg (int socket, const char *payload) {
  char *rx_buffer = rpc_state.rx_buffer;
  const u16_t len = strlen(payload);
  // TODO: avoid this memcpy
  memcpy(rx_buffer, &len, sizeof(len));
  memcpy(rx_buffer + sizeof(len), payload, len);

  // send() can return less bytes than supplied length.
  // Walk-around for robust implementation.
  int total = len + sizeof(len);
  int remain = total;
  while (remain > 0) {
    int written = send(socket, rx_buffer + (total - remain), remain, 0);
    if (written < 0) return written;
    remain -= written;
  }
  ESP_LOGI(RPC, "send_msg(%zu) done", len + sizeof(len));
  return len + sizeof(len);
}

static int recv_msg (int socket) {
  char *rx_buffer = rpc_state.rx_buffer;
  u16_t msg_len = 0;
  size_t offset = 0;
  while (1) {
    int len = recv(socket, rx_buffer + offset, XF_BUFFER_SIZE - offset - 1, 0);
    // ESP_LOGI(RPC, "recv_msg::recv => %i, offset: %zu", len, offset);
    if (len < 1) return len;
    /* Wait message size */
    if (!msg_len && offset + len >= sizeof(u16_t)) {
      memcpy(&msg_len, rx_buffer, sizeof(u16_t));
      // ESP_LOGI(RPC, "rxMSG[%i] Incoming message", msg_len);
      if (msg_len > XF_BUFFER_SIZE) {
        ESP_LOGE(RPC, "rxMSG[%i] > rx_buffer[%i] would cause overflow, disconnecting!", msg_len, XF_BUFFER_SIZE);
        return -1;
      }
    }

    offset += len;

    /* Check read finished */
    if (msg_len) {
      if (offset > msg_len + sizeof(u16_t)) {
        ESP_LOGE(RPC, "rx Read overflow, expected (%zu), received (%zu)", msg_len + sizeof(u16_t), offset);
        return -1;
      }
      /* Success */
      if (offset == msg_len + sizeof(u16_t)) {
        ESP_LOGI(RPC, "recv_msg(%zu) done", offset);
        return offset;
      }
    }
  }
}

static void initiator_comm (const int sock) {
  char *rx_buffer = rpc_state.rx_buffer;
  const char *payload = "Message from ESP32 ";
  int rounds = 0; // Tmp;
  while (1) {
    int err = send_msg(sock, payload);
    if (err < 0) {
      ESP_LOGE(RPC, "Error occurred during sending: (%d) %s", errno, lwip_strerr(errno));
      break;
    }
    delay(50);

    int len = recv_msg(sock);
    // Error occurred during receiving
    if (len < 0) {
      ESP_LOGE(RPC, "recv failed: errno (%d) %s", errno, lwip_strerr(errno));
      break;
    } else if (len == 0) {
      ESP_LOGI(RPC, "Connection closed by remote");
      break;
    }
    // Data received
    else {
      rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
      ESP_LOGI(RPC, "C: Received %d bytes, round %i", len, rounds);
      ESP_LOGI(RPC, "C: %s", rx_buffer + 2);
    }
    if (rounds++ > 10) break;
  }
}

/* not sure why static */
static void server_comm(const int sock) {
    char *rx_buffer = rpc_state.rx_buffer;
    int len;
    do {
        len = recv_msg(sock);
        if (len < 0) {
            ESP_LOGE(RPC, "Error occurred during receiving: (%d) %s", errno, lwip_strerr(errno));
        } else if (len == 0) {
            ESP_LOGW(RPC, "Connection closed by remote");
        } else {
            rx_buffer[len] = 0; // Null-terminate whatever is received and treat it like a string
            ESP_LOGI(RPC, "S: Received %d bytes: %s", len, rx_buffer);
            ESP_LOGI(RPC, "S: %s", rx_buffer + 2);
            // Echo back
            int n = send_msg(sock, "Gotcha");
            if (n < 0) {
              ESP_LOGE(RPC, "S: write error: (%d) %s", n, lwip_strerr(n));
              return; // Disconnect
            }
        }
      vTaskDelay(50); // Sleep inbetween messages
    } while (len > 0);
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
        ESP_LOGE(RPC, "Unable to create socket: (%d) %s", errno, lwip_strerr(errno));
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
    ESP_LOGI(RPC, "Socket created");
    int err = bind(rpc_state.listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(RPC, "Socket unable to bind: (%d) %s", errno, lwip_strerr(errno));
        ESP_LOGE(RPC, "IPPROTO: %d", addr_family);
        goto DEINIT;
    }
    ESP_LOGI(RPC, "Socket bound, port %d", PORT);

    err = listen(rpc_state.listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(RPC, "Error occurred during listen: (%d) %s", errno, lwip_strerr(errno));
        goto DEINIT;
    }
    rpc_state.listening = true;
    while (rpc_state.listening) {
        ESP_LOGI(RPC, "Socket listening");
        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(rpc_state.listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(RPC, "Unable to accept connection: (%d) %s", errno, lwip_strerr(errno));
            break;
        }
        g_state->status = INFORM;
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

        ESP_LOGI(RPC, "Socket accepted ip address: %s", addr_str);
        server_comm(sock);
        shutdown(sock, 0);
        close(sock);
        xEventGroupSetBits(g_state->event_group, EV_NDP_DEINIT);
        vTaskDelay(1);
    }

DEINIT:
    close(rpc_state.listen_sock);
    rpc_state.server_task = 0; // Bad idea?
    vTaskDelete(NULL);
}

void rpc_listen () {
  xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET6, 5, &rpc_state.server_task);
}
static struct client_target {
} client_state;


void tcp_client_task(void *pvParameters);
int rpc_connect(esp_netif_t *netif, ip_addr_t *target) {
  if (rpc_state.client_task != 0) {
    eTaskState s = eTaskGetState(rpc_state.client_task);
    if (s != eDeleted && s != eInvalid) {
      ESP_LOGW(RPC, "rpc_connect() will attempt to delete previous task (%i)", s);
      vTaskDelete(rpc_state.client_task); // Attempt to delete it
      delay(100);
      s = eTaskGetState(rpc_state.client_task);
      if (s != eDeleted && s != eInvalid) return -1;
    }
  }
  rpc_state.addr = target;
  rpc_state.netif = netif;
  xTaskCreate(tcp_client_task, "tcp_client", 4096, &client_state, 10, &rpc_state.client_task);
  return 0;
}

void tcp_client_task(void *pvParameters) {
  ip_addr_t *target = rpc_state.addr;
  const char *host = ip6addr_ntoa(&target->u_addr.ip6);

  struct sockaddr_in6 dest_addr = { 0 };
  /* lwip is confusing; in6_addr posix compliant while ip6_addr_t is LWIP local? */
  memcpy(&dest_addr.sin6_addr, &target->u_addr.ip6.addr, sizeof(dest_addr.sin6_addr));
  dest_addr.sin6_family = AF_INET6;
  dest_addr.sin6_port = htons(PORT);

  int sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGE(RPC, "Unable to create socket: (%d) %s", errno, lwip_strerr(errno));
    return;
  }
  ESP_LOGI(RPC, "Socket created, connecting to %s:%d", host, PORT);

  dest_addr.sin6_scope_id = esp_netif_get_netif_impl_index(rpc_state.netif);
  ESP_LOGI(RPC, "Interface index: %" PRIu32, dest_addr.sin6_scope_id);

  int errCount = 0;
  int err = 0;

  do  {
    err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) ESP_LOGE(RPC, "Socket unable to connect: (%d) %s", errno, lwip_strerr(errno));
    if (10 < errCount++) goto DEINIT;
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Reference code had a 5-sec delay here.
  } while (err != 0);

  ESP_LOGI(RPC, "Successfully connected");
  g_state->status = INFORM;

  struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  initiator_comm(sock);

DEINIT:
  if (sock != -1) {
    ESP_LOGI(RPC, "Socket disconnected %s:%d", host, PORT);
    shutdown(sock, 0);
    close(sock);
  }
  xEventGroupSetBits(g_state->event_group, EV_NDP_DEINIT);
  rpc_state.client_task = 0;
  vTaskDelete(NULL);
}
