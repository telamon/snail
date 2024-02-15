#include "wrpc.h"
#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_log.h>
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_websocket_client.h"

static const char *TAG = "wrpc";
// static SemaphoreHandle_t active; // = xSemaphoreCreateCounting(3, 0);
//
/*************** client/WS ************************/
#define NO_DATA_TIMEOUT_SEC 5
#define LOGE_NZ(msg, err) if (err != 0) ESP_LOGE(TAG, "Last error %s: 0x%x", msg, err)
static SemaphoreHandle_t client_shutdown;
static SemaphoreHandle_t client_lock;
static TimerHandle_t shutdown_timer;

static void kill_client(TimerHandle_t t) {
  if (t != NULL && xTimerIsTimerActive(t)) xTimerStop(t, portMAX_DELAY);
  xSemaphoreGive(client_shutdown);
}

static void cframe_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  esp_websocket_client_handle_t client = args;
  switch(event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "client connection established");
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        LOGE_NZ("HTTP status code",  data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            LOGE_NZ("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
            LOGE_NZ("reported from tls stack", data->error_handle.esp_tls_stack_err);
            LOGE_NZ("captured as transport's socket errno",  data->error_handle.esp_transport_sock_errno);
        }
	kill_client(NULL);
        break;
    case WEBSOCKET_EVENT_DATA:
	xTimerReset(shutdown_timer, portMAX_DELAY);
	if (esp_websocket_client_is_connected(client)) {
	  // uint8_t feed[32];
	  // esp_websocket_client_send_bin(client, (const char*)feed, 32, portMAX_DELAY);
	}
	break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
      LOGE_NZ("HTTP status code",  data->error_handle.esp_ws_handshake_status_code);
      if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
	LOGE_NZ("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
	LOGE_NZ("reported from tls stack", data->error_handle.esp_tls_stack_err);
	LOGE_NZ("captured as transport's socket errno",  data->error_handle.esp_transport_sock_errno);
      }
      kill_client(NULL);
      break;
    default:
      ESP_LOGI(TAG, "UNKNOWN EVENT (%li)", event_id);
  };
};

esp_err_t wrpc_connect(esp_netif_t *interface, ip_addr_t *target_address) {
  if (!xSemaphoreTake(client_lock, pdMS_TO_TICKS(1000))) {
    ESP_LOGE(TAG, "Active client exists");
    return -1;
  }
  struct ifreq if_name = {0};
  esp_netif_get_netif_impl_name(interface, (char*)&if_name);
  char url[32];
  sprintf(url, "ws://%s/socket", ip4addr_ntoa(&target_address->u_addr.ip4));
  esp_websocket_client_config_t config = {
    .if_name = &if_name,
    .uri = url,
    .disable_auto_reconnect = 1,
    .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
    .reconnect_timeout_ms = 10000,
    .network_timeout_ms = 10000
  };

  esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "client initialization failed");
    return -1;
  }

  ESP_ERROR_CHECK(esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, cframe_handler, (void *)client));
  ESP_ERROR_CHECK(esp_websocket_client_start(client));
  xTimerStart(shutdown_timer, portMAX_DELAY);

  // cool -> esp_websocket_client_is_connected(client)
  uint8_t feed[32];
  esp_websocket_client_send_bin(client, (const char*)feed, 32, portMAX_DELAY);

  /* deinit */
  xSemaphoreTake(client_shutdown, portMAX_DELAY);
  esp_websocket_client_close(client, portMAX_DELAY);
  ESP_LOGI(TAG, "Websocket Stopped");
  esp_websocket_client_destroy(client);
  xSemaphoreGive(client_lock);
  return ESP_OK;
}

/*************** httpd/WS ************************/
static const char TMP_RESP[] = "S.N.A.I.L OK";

static esp_err_t frame_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    uint8_t *buf = NULL;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    /* Set max_len = 0 to get the frame len */
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_ws_recv_frame failed to get frame len with %d", err);
        return err;
    }

    if (ws_pkt.len == 0 || ws_pkt.len > 4096) {
      ESP_LOGE(TAG, "invalid frame length: %zu", ws_pkt.len);
      return ESP_ERR_NO_MEM;
    } else ESP_LOGI(TAG, "frame len is %zu", ws_pkt.len);

    /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
    buf = calloc(1, ws_pkt.len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to calloc memory for buf");
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    /* Set max_len = ws_pkt.len to get the frame payload */
    err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", err);
        free(buf);
        return err;
    }
    ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);

    ws_pkt.payload = (uint8_t*)TMP_RESP;
    ws_pkt.len = 12;

    err = httpd_ws_send_frame(req, &ws_pkt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", err);
    }
    free(buf);
    return err;
}

static const httpd_uri_t ws = {
        .uri        = "/socket",
        .method     = HTTP_GET,
        .handler    = frame_handler,
        .user_ctx   = NULL,
        .is_websocket = true
};

esp_err_t onconnect(httpd_handle_t hd, int sockfd) {
  ESP_LOGI(TAG, "httpd connected %i", sockfd);
  return ESP_OK; // -1 to fast disconnect
}
void onclose(httpd_handle_t hd, int sockfd) {
  ESP_LOGI(TAG, "httpd disconnected %i", sockfd);
}

esp_err_t wrpc_init(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.open_fn = onconnect;
    config.close_fn = onclose;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting httpd on port: '%d'", config.server_port);
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;

    // Registering the ws handler
    ESP_LOGI(TAG, "Registering URI handlers");
    httpd_register_uri_handler(server, &ws);

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
    // ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    // Initialize Client scope
    client_shutdown = xSemaphoreCreateBinary();
    client_lock = xSemaphoreCreateMutex();
    shutdown_timer = xTimerCreate("Websocket shutdown timer", 10 * 1000 / portTICK_PERIOD_MS,
                                         pdFALSE, NULL, kill_client);
    return ESP_OK;
}
