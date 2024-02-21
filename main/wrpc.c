#include "wrpc.h"
#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_log.h>
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_websocket_client.h"
#include "snail.h"
/***
 *  TODO: Rename this wrpc->ws_wire
 *  Because RPC-protocl running on the wire
 *  is not included here.
 *
 * ********************/

// #define assert(expr) ESP_ERROR_CHECK((expr) ? ESP_OK : ESP_FAIL)
static const char *TAG_S = "wrpc.c:HOST";
static const char *TAG_C = "wrpc.c:GUEST";
static pwire_handlers_t handlers = {0};
static SemaphoreHandle_t connection_mutex; /* Single peered for 4 now */
// static SemaphoreHandle_t active; // = xSemaphoreCreateCounting(3, 0);
//
/*************** client/WS ************************/
#define NO_DATA_TIMEOUT_SEC 5
#define LOGE_NZ(msg, err) if (err != 0) ESP_LOGE(TAG_C, "Last error %s: 0x%x", msg, err)
static SemaphoreHandle_t client_shutdown;
static TimerHandle_t shutdown_timer;

static int connection_lock(const char* TAG) {
  if (xSemaphoreTake(connection_mutex, pdMS_TO_TICKS(1000))) {
    snail_transition(INFORM);
    return 0;
  } else {
    ESP_LOGW(TAG, "Lock Failed");
    return ESP_FAIL;
  }
}

static void connection_unlock(void) {
  xSemaphoreGive(connection_mutex);
  snail_inform_complete(0);
}

static void kill_client(TimerHandle_t t) {
  if (t == NULL) {
    ESP_LOGW(TAG_C, "kill_client(%p) by timeout", t);
  } else {
    ESP_LOGI(TAG_C, "kill_client(%p) invoked manually", t);
    if (xTimerIsTimerActive(t)) {
      ESP_LOGI(TAG_C, "Stopping timer");
      xTimerStop(t, portMAX_DELAY);
    }
  }
  xSemaphoreGive(client_shutdown);
}

static void cframe_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  esp_websocket_client_handle_t client = args;
  switch(event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG_C, "connection established");
      pwire_event_t event = { .initiator = true, .message = NULL, .size = 0 };
      pwire_ret_t reply = handlers.on_open(&event);
      /* Initiators must initiate on open */
      assert(reply == PW_REPLY);
      assert(event.message != NULL);
      assert(event.size != 0);
      esp_websocket_client_send_bin(client, (const char*) event.message, event.size, portMAX_DELAY);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGI(TAG_C, "WEBSOCKET_EVENT_DISCONNECTED");
      LOGE_NZ("HTTP status code",  data->error_handle.esp_ws_handshake_status_code);
      if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
	  LOGE_NZ("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
	  LOGE_NZ("reported from tls stack", data->error_handle.esp_tls_stack_err);
	  LOGE_NZ("captured as transport's socket errno",  data->error_handle.esp_transport_sock_errno);
      }
      kill_client(NULL);
      break;
    case WEBSOCKET_EVENT_DATA: {
      ESP_LOGI(TAG_C, "WEBSOCKET_EV_DATA fin: %i, data_ptr: %p, data_len: %i, payload_offset: %i, payload_len %i, op_code: %i",
	  data->fin,
	  data->data_ptr,
	  data->data_len,
	  data->payload_offset,
	  data->payload_len,
	  data->op_code);
      if (data->op_code == 8) return; /* 8 means clean close? */
      xTimerReset(shutdown_timer, portMAX_DELAY);
      pwire_event_t event = { .initiator = true, .message = NULL, .size = 0 };
      event.size = data->payload_len;
      event.message = (uint8_t *)(data->data_ptr + data->payload_offset);
      pwire_ret_t reply = handlers.on_data(&event);
      if (reply == PW_REPLY) {
	assert(event.message != NULL);
	assert(event.size != 0);
	esp_websocket_client_send_bin(client, (const char*) event.message, event.size, portMAX_DELAY);
      } else {
	kill_client(NULL);
      }
    } break;
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGI(TAG_C, "WEBSOCKET_EVENT_ERROR");
      LOGE_NZ("HTTP status code",  data->error_handle.esp_ws_handshake_status_code);
      if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
	LOGE_NZ("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
	LOGE_NZ("reported from tls stack", data->error_handle.esp_tls_stack_err);
	LOGE_NZ("captured as transport's socket errno",  data->error_handle.esp_transport_sock_errno);
      }
      kill_client(NULL);
      break;
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
      ESP_LOGI(TAG_C, "WEBSOCKET_EVENT_BEFORE_CONNECT");
      break;
    case WEBSOCKET_EVENT_CLOSED:
      ESP_LOGI(TAG_C, "WEBSOCKET_EVENT_CLOSED");
      break;
    default:
      ESP_LOGI(TAG_C, "UNKNOWN EVENT (%li)", event_id);
  };
};

esp_err_t wrpc_connect(esp_netif_t *interface, ip_addr_t *target_address) {
  if (0 != connection_lock(TAG_C)) return ESP_FAIL;
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
    ESP_LOGE(TAG_C, "client initialization failed");
    return -1;
  }

  ESP_ERROR_CHECK(esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, cframe_handler, (void *)client));
  ESP_ERROR_CHECK(esp_websocket_client_start(client));
  xTimerReset(shutdown_timer, portMAX_DELAY); /* Should start stopped timers */
  // xTimerStart(shutdown_timer, portMAX_DELAY);

  // cool -> esp_websocket_client_is_connected(client)

  /* Block callee until shutdown signal */
  xSemaphoreTake(client_shutdown, portMAX_DELAY);

  /* deinit */
  ESP_LOGI(TAG_C, "Websocket Stopped");
  esp_websocket_client_close(client, pdMS_TO_TICKS(2000));
  esp_websocket_client_destroy(client);
  pwire_event_t event = { .initiator = true, .message = NULL, .size = 0 };
  handlers.on_close(&event);
  connection_unlock();
  return ESP_OK;
}

/*************** httpd/WS ************************/
static esp_err_t frame_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG_S, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    uint8_t *buf = NULL;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_BINARY; //HTTPD_WS_TYPE_TEXT;
    /* Set max_len = 0 to get the frame len */
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_S, "http_ws_recv_frame failed to get frame len with %d", err);
        return err;
    }

    if (ws_pkt.len == 0 || ws_pkt.len > 4096) {
      ESP_LOGE(TAG_S, "invalid frame length: %zu", ws_pkt.len);
      return ESP_ERR_NO_MEM;
    } else ESP_LOGI(TAG_S, "got data with len: %zu", ws_pkt.len);

    buf = calloc(1, ws_pkt.len);
    if (buf == NULL) {
        ESP_LOGE(TAG_S, "Failed to calloc memory for buf");
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    /* Set max_len = ws_pkt.len to get the frame payload */
    err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_S, "httpd_ws_recv_frame failed with %d", err);
        free(buf);
        return err;
    }
    pwire_event_t event = {
      .initiator = 0,
      .message = buf,
      .size = ws_pkt.len,
    };
    pwire_ret_t rep = handlers.on_data(&event);
    if (rep == PW_REPLY) {
      assert(event.message != NULL);
      assert(event.size != 0);
      ws_pkt.payload = event.message;
      ws_pkt.len = event.size;
      err = httpd_ws_send_frame(req, &ws_pkt);
      free(buf);
      if (err != ESP_OK) {
	ESP_LOGE(TAG_S, "httpd_ws_send_frame failed with %d", err);
	return -1;
      }
    } else {
      free(buf);
      return 1; // Signal connection close;
    }

    return err;
}

static const httpd_uri_t ws = {
        .uri        = "/socket",
        .method     = HTTP_GET,
        .handler    = frame_handler,
        .user_ctx   = NULL,
        .is_websocket = true
};

esp_err_t httpd_onconnect(httpd_handle_t hd, int sockfd) {
  ESP_LOGI(TAG_S, "httpd connected %i", sockfd);
  if (0 != connection_lock(TAG_S)) return ESP_FAIL;
  pwire_event_t ev = { .initiator = false, .size = 0, .message = NULL };
  handlers.on_open(&ev);
  return ESP_OK; // -1 to fast disconnect
}

void httpd_onclose(httpd_handle_t hd, int sockfd) {
  ESP_LOGI(TAG_S, "httpd disconnected %i", sockfd);
  pwire_event_t ev = { .initiator = false, .size = 0, .message = NULL };
  handlers.on_close(&ev);
  connection_unlock();
}

esp_err_t wrpc_init(pwire_handlers_t *message_handlers) {
    memcpy(&handlers, message_handlers, sizeof(pwire_handlers_t));
    assert(handlers.on_data != NULL);
    assert(handlers.on_open != NULL);
    assert(handlers.on_close != NULL);
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.open_fn = httpd_onconnect;
    config.close_fn = httpd_onclose;

    // Start the httpd server
    ESP_LOGI(TAG_S, "Starting httpd on port: '%d'", config.server_port);
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;

    // Registering the ws handler
    ESP_LOGI(TAG_S, "Registering URI handlers");
    httpd_register_uri_handler(server, &ws);

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection. (really needed?)
     */
    // ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    // Initialize Client scope
    client_shutdown = xSemaphoreCreateBinary();
    connection_mutex = xSemaphoreCreateMutex();
    shutdown_timer = xTimerCreate("Websocket shutdown timer", 10 * 1000 / portTICK_PERIOD_MS, pdFALSE, NULL, kill_client);
    return ESP_OK;
}
