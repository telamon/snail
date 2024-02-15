#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_log.h>
static const char *TAG = "wrpc";
static SemaphoreHandle_t active; // = xSemaphoreCreateCounting(3, 0);

/*
 * Structure holding server handle
 * and internal socket fd in order
 * to use out of request send
 *
struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

 *
 * async send function, which we put into the httpd work queue
 *
static void ws_async_send(void *arg)
{
    static const char * data = "Async data";
    struct async_resp_arg *resp_arg = arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = strlen(data);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    free(resp_arg);
}

static esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req)
{
    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    if (resp_arg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    resp_arg->hd = req->handle;
    resp_arg->fd = httpd_req_to_sockfd(req);
    esp_err_t ret = httpd_queue_work(handle, ws_async_send, resp_arg);
    if (ret != ESP_OK) {
        free(resp_arg);
    }
    return ret;
}
*/

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
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "http_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %zu", ws_pkt.len);
    // TODO: limit length
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }

    /*
    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT &&
        strcmp((char*)ws_pkt.payload,"Trigger async") == 0) {
        free(buf);
        return trigger_async_send(req->handle, req);
    }
    */

    ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
    }
    free(buf);
    return ret;
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
    return ESP_OK;
}
