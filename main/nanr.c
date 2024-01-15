/*
 * SPDX-FileCopyrightText: 2023 Decent Labs
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/*
 * nanr.c
 * Neighbour Aware Network Relay
 */
#include <string.h>
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_nan.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#define NAN_SERVICE "snail"
#define NAN_FILTER ""
#define NAN_MSG "snail0"
const char *TAG = NAN_SERVICE;

// Publisher
static const int NAN_RECEIVE = BIT0;
// Subscriber / initiator
static const int NAN_SERVICE_MATCH = BIT1;
static const int NDP_CONFIRMED = BIT2;
static const int NDP_FAILED = BIT3;

// TODO: replace with state
static EventGroupHandle_t nan_event_group;
static esp_netif_t *esp_netif; // Localhost
static uint8_t g_peer_inst_id; // incoming Peer
static wifi_event_nan_svc_match_t g_svc_match_evt; // Matched Service info
static uint8_t g_peer_ndi[6]; // MAC of DataPathed Peer


static void nan_receive_event_handler(
  void *arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void *event_data
) {
  wifi_event_nan_receive_t *evt = (wifi_event_nan_receive_t *)event_data;
  g_peer_inst_id = evt->peer_inst_id;
  xEventGroupSetBits(nan_event_group, NAN_RECEIVE);
}

/**
 * This handler accepts datapath requests
 */
static void nan_ndp_indication_event_handler(
  void *arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void *event_data
) {
  if (event_data == NULL) return;
  wifi_event_ndp_indication_t *evt = (wifi_event_ndp_indication_t *)event_data;
  ESP_LOGI(TAG, "recv: ndp_indication_ev{ id: %i, peer: "MACSTR" }", evt->ndp_id, MAC2STR(evt->peer_nmi));
  wifi_nan_datapath_resp_t ndp_resp = {0};
  ndp_resp.accept = true; /* Accept incoming datapath request */
  ndp_resp.ndp_id = evt->ndp_id;
  memcpy(ndp_resp.peer_mac, evt->peer_nmi, 6);

  esp_wifi_nan_datapath_resp(&ndp_resp);
}

static void nan_ndp_confirmed_event_handler(
  void *arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void *event_data
) {
  wifi_event_ndp_confirm_t *evt = (wifi_event_ndp_confirm_t *)event_data;
  if (evt->status == NDP_STATUS_REJECTED) {
      ESP_LOGE(TAG, "NDP request to Peer "MACSTR" rejected [NDP ID - %d]", MAC2STR(evt->peer_nmi), evt->ndp_id);
      xEventGroupSetBits(nan_event_group, NDP_FAILED);
  } else {
      memcpy(g_peer_ndi, evt->peer_ndi, sizeof(g_peer_ndi)); // stash NAN Data Interface MAC
      xEventGroupSetBits(nan_event_group, NDP_CONFIRMED);
  }
}

static void nan_service_match_event_handler(
  void* arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void* event_data
) {
    wifi_event_nan_svc_match_t *evt = (wifi_event_nan_svc_match_t *)event_data;
    ESP_LOGI(TAG, "NAN Publisher found for Serv ID %d", evt->subscribe_id);
    memcpy(&g_svc_match_evt, evt, sizeof(wifi_event_nan_svc_match_t));
    xEventGroupSetBits(nan_event_group, NAN_SERVICE_MATCH);
}


uint8_t nan_subscribe (void) {
  // Setup Event Handlers for Service Match & Datapath confirm
  esp_event_handler_instance_t instance_any_id;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_NAN_SVC_MATCH,
    &nan_service_match_event_handler,
    NULL,
    &instance_any_id
  ));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_NDP_CONFIRM,
    &nan_ndp_confirmed_event_handler,
    NULL,
    &instance_any_id
  ));
  wifi_nan_subscribe_cfg_t config = {
    .service_name = NAN_SERVICE,
    .type = NAN_SUBSCRIBE_PASSIVE,
    .single_match_event = true,
    .matching_filter = NAN_FILTER,
  };
  uint8_t sub_id = esp_wifi_nan_subscribe_service(&config);
  if (sub_id == 0) ESP_LOGE(TAG, "Subscribe Failed, %i", sub_id);
  else ESP_LOGI(TAG, "Subscribed Succeeded %i", sub_id);
  return sub_id;
}

uint8_t nan_publish(void) {
  esp_event_handler_instance_t instance_any_id;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_NAN_RECEIVE,
    &nan_receive_event_handler,
    NULL,
    &instance_any_id
  ));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_NDP_INDICATION,
    &nan_ndp_indication_event_handler,
    NULL,
    &instance_any_id
  ));

  /* ndp_require_consent
   * If set to false - All incoming NDP requests will be internally accepted if valid.
   * If set to true - All incoming NDP requests raise NDP_INDICATION event, upon which application can either accept or reject them.
   */
  bool ndp_require_consent = true;
  wifi_nan_publish_cfg_t publish_cfg = {
    .service_name = NAN_SERVICE,
#if CONFIG_EXAMPLE_NAN_PUBLISH_UNSOLICITED
    .type = NAN_PUBLISH_UNSOLICITED,
#else
    .type = NAN_PUBLISH_SOLICITED,
#endif
    .matching_filter = NAN_FILTER,
    .single_replied_event = 1,
  };
  uint8_t pub_id = esp_wifi_nan_publish_service(&publish_cfg, ndp_require_consent);
  if (pub_id == 0) ESP_LOGE(TAG, "Publish Failed %i", pub_id);
  else ESP_LOGI(TAG, "Published Suceeded %i", pub_id);
  return pub_id;
}

void nan_init(void) {
  // Initialized event-group/'private emitter'/bus
  nan_event_group = xEventGroupCreate();

  // Initialize NVS - What does this do?
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  // Initialize Wifi
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM) );

  // Maybe redundant due esp_wifi_nan_start()
  // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
  // ESP_ERROR_CHECK(esp_wifi_start());

  /* Start NAN Discovery */
  wifi_nan_config_t nan_cfg = WIFI_NAN_CONFIG_DEFAULT();
  esp_netif = esp_netif_create_default_wifi_nan();
  ESP_ERROR_CHECK(esp_wifi_nan_start(&nan_cfg));
}

void sub_loop(void) {
    // Await service match
    EventBits_t bits_1 = xEventGroupWaitBits(nan_event_group, NAN_SERVICE_MATCH, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits_1 & NAN_SERVICE_MATCH) {
    /* Broadcast not used or maybe should to signal latest timestamp;
     * let publisher decide if worth it to establish datapath or not?
      wifi_nan_followup_params_t fup = {
          .inst_id = sub_id,
          .peer_inst_id = g_svc_match_evt.publish_id,
          .svc_info = EXAMPLE_NAN_SVC_MSG,
      };
      memcpy(fup.peer_mac, g_svc_match_evt.pub_if_mac, sizeof(fup.peer_mac));

      if (esp_wifi_nan_send_message(&fup) == ESP_OK)
          ESP_LOGI(TAG, "Sending message '%s' to Publisher "MACSTR" ...",
          EXAMPLE_NAN_SVC_MSG, MAC2STR(g_svc_match_evt.pub_if_mac));
    */
    wifi_nan_datapath_req_t ndp_req = {0};
    ndp_req.confirm_required = true;
    ndp_req.pub_id = g_svc_match_evt.publish_id;
    memcpy(ndp_req.peer_mac, g_svc_match_evt.pub_if_mac, sizeof(ndp_req.peer_mac));
    esp_wifi_nan_datapath_req(&ndp_req);

    // Await DataPath
    EventBits_t bits_2 = xEventGroupWaitBits(nan_event_group, NDP_CONFIRMED, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits_2 & NDP_CONFIRMED) {
      vTaskDelay(5000 / portTICK_PERIOD_MS); // Why?
      ESP_LOGI(TAG, "NAN Datapath READY");
      // ping_nan_peer(nan_netif);
    } else if (bits_2 & NDP_FAILED) {
      ESP_LOGI(TAG, "Failed to setup NAN Datapath");
    }
  }
}

void loop_events (uint8_t pub_id) {
  ESP_LOGI(TAG, "Entering Loop");
  while (1) {
    EventBits_t bits = xEventGroupWaitBits(nan_event_group, NAN_RECEIVE, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & NAN_RECEIVE) {
      xEventGroupClearBits(nan_event_group, NAN_RECEIVE);
      wifi_nan_followup_params_t fup = {0};
      fup.inst_id = pub_id,
        fup.peer_inst_id = g_peer_inst_id,
        strlcpy(fup.svc_info, NAN_MSG, ESP_WIFI_MAX_SVC_INFO_LEN);
      /* Reply to the message from a subscriber */
      esp_wifi_nan_send_message(&fup);
    }
    vTaskDelay(10);
  }
}

#ifndef APP_MAIN
void app_main(void) {
  nan_init();
  uint8_t pub_id = nan_publish();
  ESP_LOGI(TAG, "NAN Initialized");
  nan_subscribe();
  loop_events(pub_id);

  // Dream API?
  // nan_init();
  // uint8_t sub_id = nan_join('TOPIC', on_peer_connection_callback);
  // loop_events().... ?
  // nan_leave(sub_id)
  // nan_deinit();
}
#endif
