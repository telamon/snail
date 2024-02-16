/**
* Software AP - Swapping
*/
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "esp_wifi_types.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "snail.h"
#include "swap.h"
#include "wrpc.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_random.h"
#include <time.h>

#define TAG "swap.c"
#define SSID "SNAIL"
#define EV_IP_LINK_UP BIT0
#define EV_AP_NODE_ATTACHED BIT1
#define EV_AP_NODE_DETACHED BIT2

static char OUI[3] = {0xAA, 0xAA, 0xAA};
struct peer_info {
  uint8_t bssid[6];
  int rssi;
  uint16_t seen; // Last seen in ticks?
  uint32_t synced;
  int sync_result;
  uint32_t clock; // Node.date = Last Block.date (decentralized swarm clock)
  uint8_t payload[32]; /* TODO: Redefine to something meaningful */
};

static struct swap_state {
  esp_netif_t *netif_ap;
  esp_netif_t *netif_sta;
  EventGroupHandle_t events;
  TaskHandle_t seeker_task;
  wifi_config_t ap_config;
  wifi_config_t sta_config;
  int initiate_to;
  struct peer_info peers[N_PEERS];
} state = {
  .initiate_to = -1,
  .ap_config = {
    .ap = {
      .ssid = SSID,
      .ssid_len = strlen(SSID),
      .channel = CHANNEL,
      .max_connection = 2,
      .authmode = WIFI_AUTH_OPEN,
      .pmf_cfg.required = false,
      /* power consumption tuning */
      .beacon_interval = 200,
      /* Cloak beacons */
      .ssid_hidden = CLOAK_SSID
    },
  },
  .sta_config = {
    .sta = {
      .ssid = SSID,
      .scan_method = WIFI_FAST_SCAN,
      .failure_retry_cnt = 3,
      .channel = CHANNEL,
      .bssid_set = 1
    },
  }
};

#define IP_MASK 0x00ffffff
static uint32_t random_ap_ipv4() {
  esp_random();
  esp_random();
  return (esp_random() & 0x00ffff00)
    | (1 << 24)
    | 10;
}

static esp_netif_ip_info_t ip_info_ap = {
  .netmask.addr = IP_MASK,
  .gw.addr = 0
};

/**
 * Turns out this function does 3 things.
 * - writes length to *i
 * - returns most interesting idx
 * - dumps peer list to console.
 * @param i out, contains number of peers discovered
 */
static int peer_select_num (uint16_t *i) {
  *i = 0;
  int best_rssi = -100;
  int best_idx = -1;
  time_t now = time(NULL);
  ESP_LOGE(TAG, "======= [PEERS] ========");
  for (; *i < N_PEERS; ++*i) {
    struct peer_info *peer = &state.peers[*i];
    int is_empty = 1;
    for (int j = 0; is_empty && j < 6; ++j) is_empty = !peer->bssid[j];
    if (*i == 0 && is_empty) return -1; // No peers
    if (is_empty) break;
    int seen = now - peer->seen;
    int synced = now - peer->synced;
    ESP_LOGI(TAG, "peer%i: "MACSTR" RSSI: %i, Seen: %i, Synced: [%i] %"PRIu32, *i, MAC2STR(peer->bssid), peer->rssi, seen, peer->sync_result, peer->synced);
    if (peer->sync_result == 1 && synced < BACKOFF_TIME) continue;
    if (peer->sync_result == -1 && synced < BACKOFF_TIME / 3) continue;


    // if (high_clock < peer->clock) update high_clock_idx + high_clock
    /* Update best criterion along the way */
    if (peer->rssi > best_rssi) best_idx = *i;
  }
  ESP_LOGE(TAG, "SELECTED peer%i "MACSTR" RSSI: %i",
    best_idx,
    MAC2STR(state.peers[best_idx].bssid),
    state.peers[best_idx].rssi);
  return best_idx;
}

static void vsie_callback (
    void *ctx,
    wifi_vendor_ie_type_t type,
    const uint8_t source_mac[6], // BSSID
    const vendor_ie_data_t *vnd_ie,
    int rssi
) {
  if (snail_current_status() != SEEK) return;
  if (type != WIFI_VND_IE_TYPE_BEACON) return;
  if (memcmp(vnd_ie->vendor_oui, OUI, sizeof(OUI))) return;
  if (vnd_ie->length != 36) return;
  ESP_LOGI(TAG, "[PeerSense] "MACSTR" frame-type: %i, RSSI: %i, E: 0x%X OUI: %X%X%X, t: %x, len: %i",
    MAC2STR(source_mac),
    type,
    rssi,
    vnd_ie->element_id,
    vnd_ie->vendor_oui[0],
    vnd_ie->vendor_oui[1],
    vnd_ie->vendor_oui[2],
    vnd_ie->vendor_oui_type,
    vnd_ie->length
  );
  // ESP_LOG_BUFFER_HEXDUMP(TAG, vnd_ie->payload, vnd_ie->length - 4, ESP_LOG_INFO);
  /* Find previous or empty slot */
  struct peer_info *registry = state.peers;
  int weakest = 0;
  int i = 0;
  for (; i < N_PEERS; i++) {
    int is_empty = 1;
    for (int j = 0; is_empty && j < 6; ++j) is_empty = !registry[i].bssid[j];
    if (is_empty) break;
    if (memcmp(registry[i].bssid, source_mac, 6) == 0) break;
    /* Update weakest rssi along the way */
    if (registry[i].rssi < registry[weakest].rssi) weakest = i;
  }
  if (i == N_PEERS) i = weakest;
  struct peer_info *slot = &registry[i];

  slot->rssi = rssi;
  slot->seen = time(NULL);
  // slot->clock = decode(vnd_ie->payload);
  // slot->id = decode(vnd_ie->payload);
  memcpy(slot->bssid, source_mac, 6);
  memcpy(slot->payload, vnd_ie->payload, sizeof(registry[i].payload));
  // ESP_LOGI(TAG, "Store peer "MACSTR" in slot %i", MAC2STR(registry[i].bssid), i);
}

static void update_ap_beacons (void) {
  /* Custom data in AP-beacons */
  uint8_t ie_data[38]; // sizeof(vendor_ie_data_t) + 32
  vendor_ie_data_t *hdr = (vendor_ie_data_t*)&ie_data;
  hdr->element_id = 0xDD;
  hdr->length = 36; // after elem + length; remain OUI(3) + type (1) + Payload(32) = 36
  memcpy(hdr->vendor_oui, OUI, 3);
  hdr->vendor_oui_type = 0;
  memset(hdr->payload, 0xff, 32); // TODO: blockheight/hash
  esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, 0, &ie_data);
  // esp_wifi_80211_tx(WIFI_IF_AP, &buffer, length, true); // ulitmate fallback raw frames.
}

/* TODO: rename swap_cloak */
uint8_t swap_gateway_is_enabled(void) {
  return state.ap_config.ap.ssid_hidden;
}
esp_err_t swap_gateway_enable (uint8_t enable) {
  wifi_config_t *config = &state.ap_config;
  config->ap.ssid_hidden = enable;
  esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, config);
  if (err != ESP_OK) return err;
  // TODO: Activate websocket/httpd?
  return err;
}

/* Initialize Access Point */
static void init_softap(void) {
  wifi_config_t *config = &state.ap_config;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, config));
  // esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR); // Weird "patended" longrange-mode (maybe periodically enable)

  /* Reconfigure IP & DHCP-server */
  uint8_t bssid[6];
  esp_wifi_get_mac(WIFI_IF_AP, bssid);
  ip_info_ap.ip.addr = random_ap_ipv4(); // bssid_to_ipv4(bssid);
  ip_info_ap.gw.addr = ip_info_ap.ip.addr;
  ESP_ERROR_CHECK(esp_netif_dhcps_stop(state.netif_ap));
  ESP_ERROR_CHECK(esp_netif_set_ip_info(state.netif_ap, &ip_info_ap));
  ESP_ERROR_CHECK(esp_netif_dhcps_start(state.netif_ap));
  ESP_LOGI(TAG, "init_softap finished. SSID:%s channel:%d ip:"IPSTR, SSID, config->ap.channel, IP2STR(&ip_info_ap.ip));
  update_ap_beacons();
}


/* Reconfigure station to different Access Point */
static int sta_associate(const uint8_t *bssid) {
  wifi_config_t *config = &state.sta_config;
  ESP_LOGI(TAG, "sta_associate("MACSTR")", MAC2STR(bssid));
  memcpy(config->sta.bssid, bssid, sizeof(config->sta.bssid));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, config));
  ESP_LOGI(TAG, "station reconfigured to "MACSTR, MAC2STR(config->sta.bssid));

  /* Connect */
  int err = esp_wifi_connect();
  if (err == ESP_OK) ESP_LOGI(TAG, "sta_associate("MACSTR") success", MAC2STR(bssid));
  else {
    ESP_LOGE(TAG, "sta_associate("MACSTR") failed %i", MAC2STR(bssid), err);
    ESP_ERROR_CHECK(err);
  }
  return err;
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
    ) {
  if (event_base == IP_EVENT) {
    switch (event_id) {
      case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "EV_STA: Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(state.events, EV_IP_LINK_UP);
      } break;
      default:
        ESP_LOGW(TAG, "EV_IP unknown event, base: %s, id: %"PRIu32, event_base, event_id);
        break;
    }
  }

  if (event_base == WIFI_EVENT) {
    switch(event_id) {
      case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "EV_AP: Soft-AP started");
        break;
      case WIFI_EVENT_AP_STOP: /**< Soft-AP stop */
        ESP_LOGI(TAG, "EV_AP: Soft-AP stopped");
        break;
      case WIFI_EVENT_AP_STACONNECTED:{
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "EV_AP: Station "MACSTR" joined, AID=%d",MAC2STR(event->mac), event->aid);
        // if (snail_current_status() == NOTIFY) ??
        xEventGroupSetBits(state.events, EV_AP_NODE_ATTACHED);
      } break;

      case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "EV_AP: Station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
      } break;

      case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "EV_STA: Station started");
        break;

      case WIFI_EVENT_SCAN_DONE: {
        wifi_event_sta_scan_done_t *event = (wifi_event_sta_scan_done_t*) event_data;
        ESP_LOGI(TAG, "EV_SCAN: done! status: %"PRIu32" found: %i", event->status, event->number);
        // xEventGroupSetBits(state.events, EV_SCAN_COMPLETE);
      } break;

      case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t*) event_data;
        ESP_LOGI(TAG, "EV_STA: station assoc to "MACSTR, MAC2STR(event->bssid));
      } break;

      case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGI(TAG, "EV_STA: station deauth from "MACSTR" reason(%i)", MAC2STR(event->bssid), event->reason);
      } break;

      default:
        ESP_LOGW(TAG, "EV_WIFI unknown event, base: %s, id: %"PRIu32, event_base, event_id);
        break;
    }
  }
}

#define MAX_SCAN 10
static wifi_ap_record_t records[MAX_SCAN];

static int swap_seek_scan(void) {
  wifi_scan_config_t scan_conf = {
    .channel = 6,
    .show_hidden = true,
    .scan_type = WIFI_SCAN_TYPE_PASSIVE,
  };
  ESP_LOGI(TAG, "swap_seeker:scan() started");
  ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_conf, true)); // block

  /* Scan complete */
  uint16_t n_accesspoints;
  uint16_t n_peers;

  /* Process Peers */
  int selected_peer = peer_select_num(&n_peers);
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&n_accesspoints));
  ESP_LOGI(TAG, "scan complete.. %iAPs %iPeers", n_accesspoints, n_peers);

  /* Process APs (5.1.2-style) */
  n_accesspoints = MAX_SCAN;
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&n_accesspoints, records));
  for (int i = 0; i < n_accesspoints; i++) {
    /* TODO: write to journey.log */
    ESP_LOGI(TAG, "AP [%i] auth: %i, ssid: %s",
        records[i].rssi,
        records[i].authmode,
        records[i].ssid
    );
  }
  ESP_ERROR_CHECK(esp_wifi_clear_ap_list());

  int ret = -1;
  /* Initiate Connection */
  if (selected_peer >= 0) ret = sta_associate(state.peers[selected_peer].bssid); /* Connect */
  state.initiate_to = selected_peer;
  return ret;
}

static void swap_main_task (void* pvParams) {
  /* kinda silly, but with STA-AP mode we SEEK & NOTIFY simultaneously <3 */
  snail_transition(SEEK);

  while (1) {
    peer_status s = snail_current_status();
    switch (s) {
      case SEEK: {
        UBaseType_t uxHighWaterMark;
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGW(TAG, "free stack: %i WORD, heap: %"PRIu32"\n", uxHighWaterMark, esp_get_free_heap_size());

        /* Listen for beacons & associate */
        if (swap_seek_scan() == ESP_OK) snail_transition(ATTACH);
        else snail_transition(NOTIFY);
      } break;

      case NOTIFY: {
        /* Update published beacons */
        update_ap_beacons();

        state.initiate_to = -1;
        uint32_t drift = NOTIFY_TIME + (uint16_t)(esp_random() & 2047) ; // Force drift/desync
        EventBits_t bits = xEventGroupWaitBits(state.events, EV_AP_NODE_ATTACHED, pdFALSE, pdFALSE, pdMS_TO_TICKS(drift));
        if (bits & EV_AP_NODE_ATTACHED) {
          xEventGroupClearBits(state.events, EV_AP_NODE_ATTACHED);
          snail_transition(ATTACH);
        } else snail_transition(SEEK);
      } break;

      case ATTACH: {
        if (state.initiate_to != -1) {
          ESP_LOGI(TAG, "STA [Initiator] Waiting for link up");
          EventBits_t bits = xEventGroupWaitBits(state.events, EV_IP_LINK_UP, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
          if (!(bits & EV_IP_LINK_UP)) {
            /* BUG! While waiting for link-up,
             * another station can connect to Server-end,
             * Steal ATTACH state then INFORM, LEAVE, NOTIFY,
             * next line generates errors */
            snail_transition(LEAVE); /* Give up */
            continue;
          }
          xEventGroupClearBits(state.events, EV_IP_LINK_UP);
          ESP_LOGI(TAG, "STA [initiator] IP link up! rpc_connect() imminent");
          /* Connect to gateway-addr */
          ip_addr_t target = {0};
          esp_netif_ip_info_t ip_info_sta = {0};
          ESP_ERROR_CHECK(esp_netif_get_ip_info(state.netif_sta, &ip_info_sta));
          target.u_addr.ip4.addr = ip_info_sta.gw.addr;
          // TODO: ws-connect
          int res = wrpc_connect(state.netif_sta, &target);
          if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed spawning client, exit: %i", res);
            snail_transition(LEAVE); // <-- Invalid LEAVE => LEAVE (2nd invoc)
            continue;
          }
        }

        // Both initiator|non-initiator
        ESP_LOGI(TAG, "Waiting for socket, initiator: %i, PEER%i:", state.initiate_to != -1, state.initiate_to);
        // int res = rpc_wait_for_peer_socket(10000 / portTICK_PERIOD_MS);
        int err = -1; // TODO: semaphore on close?
        if (err == ESP_OK) {
          snail_transition(INFORM);
        } else {
          ESP_LOGE(TAG, "No call.. giving up");
          snail_transition(LEAVE);
        }
      } break;

      case INFORM:
        vTaskDelay(pdMS_TO_TICKS(100));
        break;

      case LEAVE: {
        ESP_LOGI(TAG, "Leaving, was initator: %i", state.initiate_to);
        if (state.initiate_to != -1){
          int err = esp_wifi_disconnect();
          ESP_ERROR_CHECK_WITHOUT_ABORT(err);
        }
        state.initiate_to = -1;
        /* Not sure- drop all accumulated events? */
        xEventGroupClearBits(state.events, EV_AP_NODE_ATTACHED);
        snail_transition(NOTIFY);
      } break;

      case OFFLINE: break;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
// DEINIT:
  vTaskDelete(NULL);
}

void swap_init(pwire_handlers_t *wire_io) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  /* Init NVS */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }

  /* Initialize wifi interface */
  ESP_ERROR_CHECK(ret);
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  config.nvs_enable = false; /* Don't try to auto-reconnect to prev peers on boot*/
  ret = esp_wifi_init(&config);
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); /* AP + Station */

  /* Initalize NICs */
  state.netif_ap = esp_netif_create_default_wifi_ap();
  state.netif_sta = esp_netif_create_default_wifi_sta();

  /* Initialize Event Handler */
  state.events = xEventGroupCreate();
  #define add_handler(a, b, c) ESP_ERROR_CHECK(esp_event_handler_instance_register((a), (b), (c), NULL, NULL))
  add_handler(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
  add_handler(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);

  /* Init Access Point */
  init_softap(); // Mostly Deprecated;

  /* Init Peer discovery/registry */
  memset(state.peers, 0, sizeof(struct peer_info) * N_PEERS);
  esp_wifi_set_vendor_ie_cb(&vsie_callback, NULL);

  /* Boot up Radios */
  ESP_ERROR_CHECK(esp_wifi_start());

  wrpc_init(wire_io);
  // rpc_listen(state.netif_ap);

  /* Initialize task */
  xTaskCreate(swap_main_task, "swap_seeker", 4096, NULL, 5, &state.seeker_task);
}

void swap_deinit(void) {
  ESP_ERROR_CHECK(esp_wifi_deinit());
  esp_netif_destroy(state.netif_ap);
  esp_netif_destroy(state.netif_sta);
  ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler));
  ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_GOT_IP6, &wifi_event_handler));
  ESP_ERROR_CHECK(esp_wifi_stop());

  if (state.seeker_task != 0) {
    eTaskState s = eTaskGetState(state.seeker_task);
    if (s != eDeleted && s != eInvalid) {
      ESP_LOGW(TAG, "Attempting to kill seeker task (%i)", s);
      vTaskDelete(state.seeker_task);
      delay(100);
      s = eTaskGetState(state.seeker_task);
      if (s != eDeleted && s != eInvalid) ESP_LOGW(TAG, "seeker_task might still be running %i", s);
    }
  }
  // free(state.peers);
  // Where is esp_wifi_unset_vendor_ie_cb() ?
  memset(&state, 0, sizeof(state));
}

void swap_deauth(int exit_code) {
  /* Update Peer-stats on Event Complete */
  if (state.initiate_to != -1) {
    struct peer_info *peer = &state.peers[state.initiate_to];
    peer->synced = time(NULL);
    peer->seen = time(NULL);
    peer->sync_result = exit_code == 0 ? 1 : -1;
    ESP_LOGW(TAG, "Reconcilliation complete, deauthing %i, exit: %i", state.initiate_to, exit_code);
  } else {
    // TODO: Keep track of incoming peer identities<->VSIE
    ESP_LOGW(TAG, "Reconcilliation complete, exit: %i", exit_code);
  }
  /* Main task resets state */
  snail_transition(LEAVE); // <-- bug
}

void swap_dump_peer_list(void) {
  ESP_LOGE(TAG, "TODO: Dumping active peers not implemented");
  uint16_t i = 0;
  peer_select_num(&i);
}
