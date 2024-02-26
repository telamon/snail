/* NAN Radio Peer Provider
 * A.k.a NAN-Relay */
#ifndef NANR_H
#define NANR_H
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"

#define NAN_TOPIC "snail"
#define NAN_FILTER ""
#define NAN_MSG "snail0"
// Publisher; // TODO: unused?
#define EV_RECEIVE BIT0
// Subscriber / initiator
#define EV_SERVICE_MATCH BIT1
#define EV_NDP_CONFIRMED BIT2
#define EV_NDP_FAILED BIT3
#define EV_NDP_DEINIT BIT4

struct nanr_state {
  EventGroupHandle_t event_group;
  TaskHandle_t discovery_task;
  esp_netif_t *esp_netif; // Localhost
  /* Pub state */
  uint8_t pub_id; // Published Service id
  uint8_t peer_id; // incoming Peer
  /* Sub state */
  uint8_t sub_id; // Subscribed Service id
  wifi_event_nan_svc_match_t svc_match_evt; // Matched Service info
  /* DataPath */
  wifi_event_ndp_confirm_t ev_ndp_up;
};


void nanr_discovery_start();
uint8_t nanr_publish();
int nanr_unpublish();
uint8_t nanr_subscribe();
int nanr_unsubscribe();
int nanr_swap_polarity();

void nanr_inform_complete(int exit_code);
#endif
