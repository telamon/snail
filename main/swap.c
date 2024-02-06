/**
* Software AP - Swapping
*/
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi_types.h"
#include "lwip/ip_addr.h"
#include "snail.h"
#include "swap.h"
#include "rpc.h"
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_mac.h"

#define TAG "swap.c"
#define SSID "snail"
#define CHANNEL 6
#define EV_IP_LINK_UP BIT0
#define EV_AP_NODE_ATTACHED BIT1
/* Spend 2s waiting for peers between scans */
#define NOTIFY_TIME 4000

static char OUI[3] = {0xAA, 0xAA, 0xAA};
/* Size of max-scanned peers */
#define N_PEERS 7

struct peer_info {
  uint8_t bssid[6];
  int rssi;
  /* uint8_t flags; // Reconcilliated|
  uint16_t seen; // Last seen in ticks?
  uint32_t date; // Node.date = Last Block.date (decentralized swarm clock)
  */
  uint8_t payload[32]; /* TODO: Redefine to something meaningful */
};

static struct swap_state {
  esp_netif_t *netif_ap;
  esp_netif_t *netif_sta;
  EventGroupHandle_t events;
  TaskHandle_t seeker_task;
  wifi_config_t ap_config;
  wifi_config_t sta_config;
  bool initiator;
  struct peer_info peers[N_PEERS];
} state = {
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
      .ssid_hidden = 1
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

/**
 * @param i out, contains number of peers discovered
 */
int peer_select_num (uint16_t *i) {
  int best = *i = 0;
  for (; *i < N_PEERS; ++*i) {
    int is_empty = 1;
    for (int j = 0; is_empty && j < 6; ++j) is_empty = !state.peers[*i].bssid[j];
    if (*i == 0 && is_empty) return -1; // No peers
    if (is_empty) break;
    ESP_LOGI(TAG, "peer%i: "MACSTR" RSSI: %i", *i, MAC2STR(state.peers[*i].bssid), state.peers[*i].rssi);
    /* Update best criterion along the way */
    if (state.peers[*i].rssi > state.peers[best].rssi) best = *i;
  }
  ESP_LOGI(TAG, "SELECTED peer%i "MACSTR" RSSI: %i",
    best,
    MAC2STR(state.peers[best].bssid),
    state.peers[best].rssi);
  return best;
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
  memcpy(registry[i].bssid, source_mac, 6);
  registry[i].rssi = rssi;
  memcpy(registry[i].payload, vnd_ie->payload, sizeof(registry[i].payload));
  // ESP_LOGI(TAG, "Store peer "MACSTR" in slot %i", MAC2STR(registry[i].bssid), i);
}


/* Initialize Access Point */
void init_softap(void) {
  wifi_config_t *config = &state.ap_config;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, config));
  ESP_LOGI(TAG, "init_softap finished. SSID:%s channel:%d", SSID, config->ap.channel);
  /* Reconfigure DHCP-server */
  // esp_netif_dhcps_option();

  // esp_wifi_80211_tx(WIFI_IF_AP, &buffer, length, true); // ulitmate fallback raw frames.
  // esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR); // Weird "patended" longrange-mode (maybe periodically enable)

  /* Custom data in AP-beacons */
  // TODO: move to update_beacon_info() and call at beginning of every NOTIFY
  uint8_t ie_data[38]; // sizeof(vendor_ie_data_t) + 32
  vendor_ie_data_t *hdr = (vendor_ie_data_t*)&ie_data;
  hdr->element_id = 0xDD;
  hdr->length = 36; // after elem + length; remain OUI(3) + type (1) + Payload(32) = 36
  memcpy(hdr->vendor_oui, OUI, 3);
  hdr->vendor_oui_type = 0;
  memset(hdr->payload, 0xff, 32); // TODO: blockheight/hash
  esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, 0, &ie_data);

  /* Enable napt on the AP netif (Network Address & Port Translation) */
  /* if (esp_netif_napt_enable(state.netif_ap) != ESP_OK) {
    ESP_LOGE(TAG, "NAPT not enabled on the netif: %p", state.netif_ap);
  } */
}


/* Reconfigure station to different Access Point */
int sta_connect(const uint8_t *bssid) {
  wifi_config_t *config = &state.sta_config;
  ESP_LOGI(TAG, "sta_connect("MACSTR")", MAC2STR(bssid));
  memcpy(config->sta.bssid, bssid, sizeof(config->sta.bssid));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, config));
  ESP_LOGI(TAG, "station reconfigured to "MACSTR, MAC2STR(config->sta.bssid));
  int err = esp_wifi_connect();
  if (err == ESP_OK) ESP_LOGI(TAG, "sta_connect() succeeded");
  else {
    ESP_LOGE(TAG, "sta_connect() failed %i", err);
    ESP_ERROR_CHECK(err);
  }

  err = esp_netif_dhcpc_stop(state.netif_sta); // Disable to-begin-with?
  if (err != ESP_OK || err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
    ESP_ERROR_CHECK(err);
  }
  // esp_netif_dhcpc_option();
  // APIPA range: 169.254.0.0/255.255.0.0
  esp_netif_ip_info_t info = { // TODO: move to state
    .ip.addr = esp_ip4addr_aton("169.254.19.84"),
    .netmask.addr = esp_ip4addr_aton("255.255.0.0"),
    .gw.addr = 0
  };
  esp_netif_set_ip_info(state.netif_sta, &info);
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
      case WIFI_EVENT_AP_STACONNECTED:{
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "EV_AP: Station "MACSTR" joined, AID=%d",MAC2STR(event->mac), event->aid);
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

      case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "EV_STA: Connected!");
        break;
      default:
        ESP_LOGW(TAG, "EV_WIFI unknown event, base: %s, id: %"PRIu32, event_base, event_id);
        break;
    }
  }
}

int swap_seek_scan(void) {
  // scanning...
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
  /* Process APs */
  wifi_ap_record_t record;
  for (int i = 0; i < n_accesspoints; i++) {
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_record(&record));
    ESP_LOGI(TAG, "AP [%i] auth: %i, ssid: %s",
        record.rssi,
        record.authmode,
        record.ssid
    );
  }
  int ret = -1;
  /* Initiate Connection */
  if (selected_peer >= 0) {
    ret = sta_connect(state.peers[selected_peer].bssid); /* Connect */
  }
  /* Clear/ Discard scanned registry; TODO: Keep it via (n_beacons + last_seen)*/
  memset(state.peers, 0, sizeof(struct peer_info) * N_PEERS);
  return ret;
}

static void swap_seeker_task (void* pvParams) {
  while (1) {
    peer_status s = snail_current_status();
    switch (s) {
      case SEEK: {
        state.initiator = true;
        UBaseType_t uxHighWaterMark;
        uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGW(TAG, "free stack: %i WORD\n", uxHighWaterMark);

        /* Listen for beacons & associate */
        if (swap_seek_scan() == ESP_OK) snail_transition(ATTACH);
        else snail_transition(NOTIFY);
      } break;

      case NOTIFY: {
        state.initiator = false;
        EventBits_t bits = xEventGroupWaitBits(state.events, EV_AP_NODE_ATTACHED, pdFALSE, pdFALSE, NOTIFY_TIME / portTICK_PERIOD_MS);
        if (bits & EV_AP_NODE_ATTACHED) {
          xEventGroupClearBits(state.events, EV_AP_NODE_ATTACHED);
          snail_transition(ATTACH);
        } else snail_transition(SEEK);
      } break;

      case ATTACH: {
        if (state.initiator) {
          ESP_LOGI(TAG, "Waiting link");
          EventBits_t bits = xEventGroupWaitBits(state.events, EV_IP_LINK_UP, pdFALSE, pdFALSE, 10000 / portTICK_PERIOD_MS);
          if (!(bits & EV_IP_LINK_UP)) {
            snail_transition(LEAVE); /* Give up */
            continue;
          }
          xEventGroupClearBits(state.events, EV_IP_LINK_UP);
          ESP_LOGI(TAG, "STA: IP link up! rpc_connect() imminent");

          ip_addr_t target = {
            .u_addr.ip4.addr = 0xc0a80401 //192.168.4.1
          }; // TODO: IPv6

          // TODO: ptr of target ends up in rpc-state and corrupts after loop-iter
          int res = rpc_connect(state.netif_sta, &target);
          if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed spawning client, exit: %i", res);
            snail_transition(LEAVE);
            continue;
          }
        }

        // Both initiator|non-initiator
        ESP_LOGI(TAG, "Waiting for socket, initiator: %i", state.initiator);
        int res = rpc_wait_for_peer_socket(10000 / portTICK_PERIOD_MS);
        if (res == ESP_OK) {
          snail_transition(INFORM);
        } else {
          ESP_LOGE(TAG, "No call.. giving up");
          snail_transition(LEAVE);
        }
      } break;

      case INFORM:
        vTaskDelay(500 / portTICK_PERIOD_MS);
        break;

      case LEAVE: {
        ESP_LOGI(TAG, "Leaving, was initator: %i", state.initiator);
        if (state.initiator){
          int err = esp_wifi_disconnect();
          ESP_ERROR_CHECK_WITHOUT_ABORT(err);
        }
        /* Not sure- drop all accumulated events? */
        xEventGroupClearBits(state.events, EV_AP_NODE_ATTACHED);
        snail_transition(NOTIFY);
      } break;

      case OFFLINE: break;
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
// DEINIT:
  vTaskDelete(NULL);
}

void swap_init(void) {
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
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    ESP_EVENT_ANY_ID,
    &wifi_event_handler,
    NULL,
    NULL
  ));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    IP_EVENT,
    IP_EVENT_STA_GOT_IP,
    &wifi_event_handler,
    NULL,
    NULL
  ));

  /* Init Access Point */
  init_softap();
  ESP_ERROR_CHECK(esp_wifi_start());

  /* Init Peer discovery/registry */
  // state.peers = malloc(sizeof(struct peer_info) * N_PEERS);
  // ESP_LOGW(TAG, "REGISTRY peer ptr %p", state.peers);
  esp_wifi_set_vendor_ie_cb(&vsie_callback, NULL);

  rpc_listen();

  xTaskCreate(swap_seeker_task, "swap_seeker", 2048, NULL, 5, &state.seeker_task);

  /* kinda silly,but with STA-AP mode we SEEK & NOTIFY simultaneously <3 */
  snail_transition(SEEK);
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
