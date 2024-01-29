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
#include "esp_random.h"
#include "lwip/ip6_addr.h"
#include "nvs_flash.h"

#include "rpc.c" /* Mhmm... */

#define TAG "nanr.c"

int nan_unpublish(struct nan_state *state) {
  if (state->pub_id == 0) {
    ESP_LOGW(TAG, "unecessary nan_unpublish() pub_id is zero");
    return 0;
  }
  int res = esp_wifi_nan_cancel_service(state->pub_id);
  ESP_ERROR_CHECK(res);
  if (res == ESP_OK) {
    state->pub_id = 0;
    state->peer_id = 0;
    state->status = LEAVE;
  }
  return res;
}

int nan_unsubscribe(struct nan_state *state) {
  if (state->sub_id == 0) {
    ESP_LOGW(TAG, "unecessary nan_unsubscribe() sub_id is zero");
    return 0;
  }
  int res = esp_wifi_nan_cancel_service(state->sub_id);
  ESP_ERROR_CHECK(res);
  if (res == ESP_OK) {
    state->sub_id = 0;
    memset(&state->svc_match_evt, 0, sizeof(state->svc_match_evt));
    state->status = LEAVE;
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
  bool accept = g_state->status == NOTIFY;
  ndp_resp.accept = accept; /* Accept incoming datapath request */
  ndp_resp.ndp_id = evt->ndp_id;
  memcpy(ndp_resp.peer_mac, evt->peer_nmi, 6);
  esp_wifi_nan_datapath_resp(&ndp_resp);
  ESP_LOGI(TAG, "datapath-response, accept: %i, [%s]", accept, status_str(g_state->status));
}

/* This handler get's triggered on both ends
 * when a data-path is established.
 */
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
      memcpy(&g_state->ev_ndp_up, evt, sizeof(wifi_event_ndp_confirm_t));
      // memcpy(g_state->peer_ndi, evt->peer_ndi, sizeof(g_state->peer_ndi)); // stash NAN Data Interface MAC
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
  if (state->sub_id) {
    ESP_LOGW(TAG, "nan_subscribe() Already subscribed! %i", state->sub_id);
    return state->sub_id;
  }
  wifi_nan_subscribe_cfg_t config = {
    .service_name = NAN_TOPIC,
    .type = NAN_SUBSCRIBE_PASSIVE,
    .single_match_event = true,
    .matching_filter = NAN_FILTER,
  };
  state->sub_id = esp_wifi_nan_subscribe_service(&config);
  if (state->sub_id == 0) ESP_LOGE(TAG, "Subscribe Failed, %i", state->sub_id);
  else {
    state->status = SEEK;
    ESP_LOGI(TAG, "Subscribed Succeeded %i", state->sub_id);
  }
  return state->sub_id;
}

uint8_t nan_publish(struct nan_state *state) {
  if (state->pub_id) {
    ESP_LOGW(TAG, "nan_publish() Already published! %i", state->pub_id);
    return state->pub_id;
  }
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
    state->status = NOTIFY;
    ESP_LOGI(TAG, "Published Suceeded %i", state->pub_id);
  }
  return state->pub_id;
}

void nan_init(struct nan_state *state) {
#define COWABUNGA
#ifndef COWABUNGA
  /* Initialize NVS - What does this do? */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
#endif

  /* Initialize Wifi / netif*/
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.nvs_enable = false;
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM) );

  /* Start NAN Clustering */
  state->esp_netif = esp_netif_create_default_wifi_nan();
  wifi_nan_config_t nan_cfg = WIFI_NAN_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_nan_start(&nan_cfg));

  /* create event group */
  state->event_group = xEventGroupCreate();

  /* Register Subscription Event Handlers for Service Match & Datapath confirm */
  esp_event_handler_instance_t handler_id; // TODO: This should be memoed and freed
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_NAN_SVC_MATCH,
    &nan_service_match_event_handler,
    NULL,
    &handler_id
  ));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_NDP_CONFIRM,
    &nan_ndp_confirmed_event_handler,
    NULL,
    &handler_id
  ));

  /* Register Publish Event handlers*/
  //esp_event_handler_instance_t handler_id;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_NAN_RECEIVE,
    &nan_receive_event_handler,
    NULL,
    &handler_id
  ));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    WIFI_EVENT_NDP_INDICATION,
    &nan_ndp_indication_event_handler,
    NULL,
    &handler_id
  ));


  rpc_listen(); // Start tcp-server task
  state->status = LEAVE;
  g_state = state; // Leak state
}

int nan_ndp_deinit(struct nan_state *state) {
  wifi_nan_datapath_end_req_t end_req = {0};
  end_req.ndp_id = state->ev_ndp_up.ndp_id;
  memcpy(&end_req.peer_mac, &state->ev_ndp_up.peer_ndi, sizeof(end_req.peer_mac));
  int res = esp_wifi_nan_datapath_end(&end_req);
  ESP_LOGW(TAG, "NDP deinitalized: %i", res);
  memset(&state->ev_ndp_up, 0, sizeof(state->ev_ndp_up));
  return res;
}

void nan_reroll(struct nan_state *state) {
  ESP_LOGI(TAG, "Rolling state [%i, %i]", state->pub_id, state->sub_id);
  /* Auto unsub/unpub */
  if (state->pub_id != 0) nan_unpublish(state);
  if (state->sub_id != 0) nan_unsubscribe(state);
  /* re-roll re-enter */
  if (esp_random() & 1) nan_subscribe(state);
  else nan_publish(state);
}

esp_err_t nan_restart(struct nan_state *state) {
  /* I Hate to say it but restarting 
   * the entire piece is the most stable way of doing
   * NAN Communication atm. 
   */
  esp_restart();
  /* ATTEMPT 1 */
  nan_ndp_deinit(state);
  delay(500);
  nan_reroll(state);
  /*
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_nan_stop());
  wifi_nan_config_t nan_cfg = WIFI_NAN_CONFIG_DEFAULT();
  state->pub_id = 0;
  state->sub_id = 0;
  state->peer_id = 0;
  memset(&state->svc_match_evt, 0, sizeof(state->svc_match_evt));
  memset(&state->ev_ndp_up, 0, sizeof(state->ev_ndp_up));
  esp_err_t res = esp_wifi_nan_start(&nan_cfg);
  if (res != ESP_OK) return res;
  nan_reroll(state);
  */
  return ESP_OK;
}

int nan_process_events (struct nan_state *state) {
  /* Block for 50ms waiting for events */
  EventBits_t bits = xEventGroupWaitBits(
    state->event_group,
    EV_RECEIVE | EV_SERVICE_MATCH | EV_NDP_CONFIRMED | EV_NDP_FAILED | EV_NDP_DEINIT,
    pdFALSE,
    pdFALSE,
    portMAX_DELAY
  );
  if (!bits) return ESP_OK; // No event received, quick return
  ESP_LOGI(TAG, "nan_process_events(status: %s, bits: %lu)", status_str(state->status), (unsigned long) bits);

  if (bits & EV_RECEIVE) { // TODO: Supported but not used?
    ESP_LOGI(TAG, "[EV_RECEIVE] Incoming Peer Handshake svc: %i; peer_id: %i", state->pub_id, state->peer_id);
    xEventGroupClearBits(state->event_group, EV_RECEIVE);
    wifi_nan_followup_params_t fup = {0};
    fup.inst_id = state->pub_id;
    fup.peer_inst_id = state->peer_id;
    // 64-bytes handshake, TODO: suggestion; advertiese protocol-version + Latest Block timestimpe?
    strlcpy(fup.svc_info, NAN_MSG, ESP_WIFI_MAX_SVC_INFO_LEN);
    esp_wifi_nan_send_message(&fup);
  }

  else if (bits & EV_SERVICE_MATCH) { // Service match
    xEventGroupClearBits(state->event_group, EV_SERVICE_MATCH);
    if (state->status == SEEK || state->status == NOTIFY) {
      ESP_LOGI(TAG, "[EV_SERVICE_MATCH] Service Found");
    } else {
      ESP_LOGW(TAG, "[EV_SERVICE_MATCH] Ignored during %s-state", status_str(state->status));
      return ESP_OK;
    }

    /* [Optional] Respond with 64byte followup? */
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

    /* Initiate Datapath Request */
    wifi_nan_datapath_req_t ndp_req = {0};
    ndp_req.confirm_required = true;
    ndp_req.pub_id = state->svc_match_evt.publish_id;
    memcpy(ndp_req.peer_mac, state->svc_match_evt.pub_if_mac, sizeof(ndp_req.peer_mac));
    // Outgoing Connection
    ESP_LOGI(TAG, "Connecting to "MACSTR"", MAC2STR(ndp_req.peer_mac));
    esp_wifi_nan_datapath_req(&ndp_req);
  }

  else if (bits & EV_NDP_CONFIRMED) {
    xEventGroupClearBits(state->event_group, EV_NDP_CONFIRMED);
    if (state->status == SEEK || state->status == NOTIFY) {
      ESP_LOGI(TAG, "[EV_NDP_CONFIRMED] NAN Datapath READY");
    } else {
      ESP_LOGW(TAG, "[EV_NDP_CONFIRMED] Ignored during %s-state", status_str(state->status));
      return ESP_OK;
    }

    bool initiator = state->status == SEEK;
    state->status = ATTACH;
    ip_addr_t target_addr = {0};
    esp_wifi_nan_get_ipv6_linklocal_from_mac(&target_addr.u_addr.ip6, state->ev_ndp_up.peer_ndi);

    if (initiator) {
      // nan_unsubscribe(state);
      ESP_LOGI(TAG, "Connecting to remote peer ip: %s", ip6addr_ntoa(&target_addr.u_addr.ip6));
      // delay(100); // Reference code had big delay here.
      int res = rpc_connect(state->esp_netif, &target_addr);
      if (res != ESP_OK) {
        xEventGroupSetBits(g_state->event_group, EV_NDP_DEINIT);
        return ESP_OK;
      }
    } else {
      // nan_unpublish(state);
      ESP_LOGI(TAG, "Waiting for peer ip: %s", ip6addr_ntoa(&target_addr.u_addr.ip6));
    }
    int res = rpc_wait_for_peer_socket(8000 / portTICK_PERIOD_MS);
    if (res == 0) {
      ESP_LOGI(TAG, "Changing state [%s] => [INFORM]", status_str(state->status));
      state->status = INFORM;
    } else {
      ESP_LOGE(TAG, "No call... terminating ndp");
      xEventGroupSetBits(g_state->event_group, EV_NDP_DEINIT);
    }
  }

  else if (bits & EV_NDP_FAILED) {
    ESP_LOGE(TAG, "[EV_NDP_FAILED] Failed to setup NAN Datapath");
    xEventGroupClearBits(state->event_group, EV_NDP_FAILED);
    // nan_reroll(state);
    xEventGroupSetBits(g_state->event_group, EV_NDP_DEINIT);
  }

  else if (bits & EV_NDP_DEINIT) {
    ESP_LOGI(TAG, "[EV_NDP_DEINIT] Closing datapath");
    xEventGroupClearBits(state->event_group, EV_NDP_DEINIT);
    g_state->status = LEAVE;
    ESP_ERROR_CHECK(nan_restart(state));
  }

  else if (bits) {
    ESP_LOGE(TAG, "[UNKNOWN EVENT] bits: %lu", (unsigned long)bits);
    xEventGroupClearBits(state->event_group, bits);
    abort();
  }
  return ESP_OK;
}

/**
 * Turns subscriber into publisher and vice-versa
 * @returns 0: Publisher, 1: Subscriber, -1: Error
 */
int nan_swap_polarity(struct nan_state *state) {
  const int s = state->pub_id ? 1 : state->sub_id ? -1 : 0;
  switch (s) {
      case 1:
        nan_unpublish(state);
        nan_subscribe(state);
        break;
      case -1:
        nan_unsubscribe(state);
        nan_publish(state);
        break;
      case 0:
        nan_reroll(state);
        break;
    }
  return ESP_OK;
}

void nan_discovery_task(void *pvParameter) {
  struct nan_state *state = pvParameter;
  nan_init(state);
  ESP_LOGI(TAG, "NAN Initialized");
  /* dev-force startup state */
  // nan_publish(&state);
  nan_subscribe(state);
  // nan_reroll(state);

  while (nan_process_events(state) == ESP_OK);

// DEINIT:
  nan_unpublish(state);
  nan_unsubscribe(state);
  nan_ndp_deinit(state);
  vEventGroupDelete(state->event_group);
  ESP_ERROR_CHECK(esp_wifi_nan_stop());
}

void nan_discovery_start(struct nan_state *state) {
  // if (state->discovery_task != 0) xEventGroupSetBits(g_state->event_group, EV_DISCOVERY_EXIT);
  xTaskCreate(nan_discovery_task, "nan_discovery", 4096, state, 3, &state->discovery_task);
}

