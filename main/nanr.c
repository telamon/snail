/*
 * SPDX-FileCopyrightText: 2023 Decent Labs
 * SPDX-License-Identifier: AGPLv3 During R&D; 1.0 Release as CC0-1.0
 * Current Version: 0.1.0
 */
/*
 * nanr.c
 * Neighbour Aware Network Relay
 */
#include "nanr.h"
#include <string.h>
#include "esp_wifi_types.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_nan.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#define TAG "nanr.c"
static struct nan_state *g_state;

int nan_unpublish(struct nan_state *state) {
  if (state->status != PUBLISHED) return -1; // NO-OP
  int res = esp_wifi_nan_cancel_service(state->pub_id);
  ESP_ERROR_CHECK(res);
  if (res == ESP_OK) {
    state->pub_id = 0;
    state->peer_id = 0;
    state->status = CLUSTERING;
  }
  return res;
}

int nan_unsubscribe(struct nan_state *state) {
  if (state->status != SUBSCRIBED) return -1; // NO-OP
  int res = esp_wifi_nan_cancel_service(state->sub_id);
  ESP_ERROR_CHECK(res);
  if (res == ESP_OK) {
    state->sub_id = 0;
    memset(&state->peer_ndi, 0, sizeof(state->peer_ndi));
    memset(&state->svc_match_evt, 0, sizeof(state->svc_match_evt));
    state->status = CLUSTERING;
  }
  return res;
}

// TODO: Supported but not used?
static void nan_receive_event_handler(
  void *arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void *event_data
) {
  wifi_event_nan_receive_t *evt = (wifi_event_nan_receive_t *)event_data;
  g_state->peer_id = evt->peer_inst_id;
  xEventGroupSetBits(g_state->event_group, EV_RECEIVE);
}

/**
 * This handler accepts datapath requests
 * Incoming connection
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

// This handler get's triggered on both ends
// when data-path is established.
static void nan_ndp_confirmed_event_handler(
  void *arg,
  esp_event_base_t event_base,
  int32_t event_id,
  void *event_data
) {
  wifi_event_ndp_confirm_t *evt = (wifi_event_ndp_confirm_t *)event_data;
  if (evt->status == NDP_STATUS_REJECTED) {
      ESP_LOGE(TAG, "NDP request with Peer "MACSTR" rejected [NDP ID - %d]", MAC2STR(evt->peer_nmi), evt->ndp_id);
      xEventGroupSetBits(g_state->event_group, EV_NDP_FAILED);
  } else {
      // TODO: memcpy entire event?
      memcpy(g_state->peer_ndi, evt->peer_ndi, sizeof(g_state->peer_ndi)); // stash NAN Data Interface MAC
      xEventGroupSetBits(g_state->event_group, EV_NDP_CONFIRMED);
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
    memcpy(&g_state->svc_match_evt, evt, sizeof(wifi_event_nan_svc_match_t));
    xEventGroupSetBits(g_state->event_group, EV_SERVICE_MATCH);
}


uint8_t nan_subscribe (struct nan_state *state) {
  if (state->status != CLUSTERING) return -1;
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
    .service_name = NAN_TOPIC,
    .type = NAN_SUBSCRIBE_PASSIVE,
    .single_match_event = true,
    .matching_filter = NAN_FILTER,
  };
  state->sub_id = esp_wifi_nan_subscribe_service(&config);
  if (state->sub_id == 0) ESP_LOGE(TAG, "Subscribe Failed, %i", state->sub_id);
  else {
    state->status = SUBSCRIBED;
    ESP_LOGI(TAG, "Subscribed Succeeded %i", state->sub_id);
  }
  return state->sub_id;
}

uint8_t nan_publish(struct nan_state *state) {
  if (state->status != CLUSTERING) return -1;
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
    .service_name = NAN_TOPIC,
#if CONFIG_EXAMPLE_NAN_PUBLISH_UNSOLICITED
    .type = NAN_PUBLISH_UNSOLICITED,
#else
    .type = NAN_PUBLISH_SOLICITED,
#endif
    .matching_filter = NAN_FILTER,
    .single_replied_event = 1,
  };
  state->pub_id = esp_wifi_nan_publish_service(&publish_cfg, ndp_require_consent);
  if (state->pub_id == 0) ESP_LOGE(TAG, "Publish Failed %i", state->pub_id);
  else {
    state->status = PUBLISHED;
    ESP_LOGI(TAG, "Published Suceeded %i", state->pub_id);
  }
  return state->pub_id;
}

void nan_init(struct nan_state *state) {
  if (state->status != OFFLINE) return;
  // Initialized event-group/'private emitter'/bus
  state->event_group = xEventGroupCreate();

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
  state->esp_netif = esp_netif_create_default_wifi_nan();
  ESP_ERROR_CHECK(esp_wifi_nan_start(&nan_cfg));
  state->status = CLUSTERING;
  g_state = state; // Leak state
}

void sub_loop(struct nan_state *state) {
    // Await service match
    EventBits_t bits_1 = xEventGroupWaitBits(state->event_group, EV_SERVICE_MATCH, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits_1 & EV_SERVICE_MATCH) {
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
  }
}

EventBits_t nan_process_events (struct nan_state *state) {
  EventBits_t bits = xEventGroupWaitBits(
    state->event_group,
    EV_RECEIVE | EV_SERVICE_MATCH | EV_NDP_CONFIRMED | EV_NDP_FAILED,
    pdFALSE,
    pdFALSE,
    1000 / portTICK_PERIOD_MS // portMAX_DELAY
  );
  ESP_LOGI(TAG, "nan_process_events(status: %i, bits: %lu)", state->status, (unsigned long) bits);

  if (bits & EV_RECEIVE) { // TODO: Supported but not used?
    ESP_LOGI(TAG, "Incoming Peer Handshake svc: %i; peer_id: %i", state->pub_id, state->peer_id);
    xEventGroupClearBits(state->event_group, EV_RECEIVE);
    wifi_nan_followup_params_t fup = {0};
    fup.inst_id = state->pub_id;
    fup.peer_inst_id = state->peer_id;
    // 64-bytes handshake, TODO: suggestion; advertiese protocol-version + Latest Block timestimpe?
    strlcpy(fup.svc_info, NAN_MSG, ESP_WIFI_MAX_SVC_INFO_LEN);
    esp_wifi_nan_send_message(&fup);
  } else if (bits & EV_SERVICE_MATCH) { // Service match
    ESP_LOGI(TAG, "Service Found");
    xEventGroupClearBits(state->event_group, EV_SERVICE_MATCH);
    wifi_nan_datapath_req_t ndp_req = {0};
    ndp_req.confirm_required = true;
    ndp_req.pub_id = state->svc_match_evt.publish_id;
    memcpy(ndp_req.peer_mac, state->svc_match_evt.pub_if_mac, sizeof(ndp_req.peer_mac));
    // Outgoing Connection
    ESP_LOGI(TAG, "Connecting to "MACSTR"", MAC2STR(ndp_req.peer_mac));
    esp_wifi_nan_datapath_req(&ndp_req);
  } else if (bits & EV_NDP_CONFIRMED) {
    xEventGroupClearBits(state->event_group, EV_NDP_CONFIRMED);
    bool initiator = state->status == SUBSCRIBED;
    state->status = NDP_ESTABLISHED;
    ESP_LOGI(TAG, "NAN Datapath READY");
    if (initiator) {
      vTaskDelay(2000 / portTICK_PERIOD_MS); // Give non-initiator a headstart
      ip_addr_t target_addr = {0};
      esp_wifi_nan_get_ipv6_linklocal_from_mac(&target_addr.u_addr.ip6, state->peer_ndi);
      // rpc_connect(&state, &target_addr, 1337);
    } else {
      // rpc_listen(&state, 1337);
    }
    // xEventGroupSetBits(state->event_group, REMOTE_PEER_NETIF);
  } else if (bits & EV_NDP_FAILED) {
    xEventGroupClearBits(state->event_group, EV_NDP_FAILED);
    ESP_LOGI(TAG, "Failed to setup NAN Datapath");
  } else if (bits) {
    // TODO: not sure about this,
    // have to rethink the event-group later
    // if goal is to have multiple non-blocking components.
    // return xEventGroupClearBits(state->event_group, bits); // Clear Unknown Event;
    return bits;
  }
  // vTaskDelay(10);
  return xEventGroupGetBits(state->event_group);
}

/**
 * Turns subscriber into publisher and vice-versa
 * @returns 0: Publisher, 1: Subscriber, -1: Error
 */
int nan_swap_polarity(struct nan_state *state) {
  if (state->pub_id != 0 && state->sub_id != 0) {
    ESP_LOGI(TAG, "Impossible state pub_id: %i, sub_id: %i", state->pub_id, state->sub_id);
    return -1;
  } else if (state->pub_id != 0) {
    nan_unpublish(state);
    nan_subscribe(state);
    return 0;
  } else {
    nan_unsubscribe(state);
    nan_publish(state);
    return 1;
  }
}

#ifndef APP_MAIN
void app_main(void) {
  struct nan_state state = {0};
  nan_init(&state);
  // nan_publish(&state);
  ESP_LOGI(TAG, "NAN Initialized");
  nan_subscribe(&state);
  // int initiator = nan_swap_polarity(&state);
  loop_events(&state);

  // Dream API?
  // nan_init();
  // uint8_t sub_id = nan_join('TOPIC', on_peer_connection_callback);
  // loop_events().... ?
  // nan_leave(sub_id)
  // nan_deinit();
}
#endif
