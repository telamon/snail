/* NAN Radio Peer Provider */
#ifndef NANR_H
#define NANR_H
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#define delay(ms) vTaskDelay((ms) / portTICK_PERIOD_MS)

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

/* This belongs in snail.h */
enum nan_peer_status {
  OFFLINE = 0,
  // Significant ones
  SEEK = 2,
  NOTIFY = 3,
  ATTACH = 4,
  INFORM = 5,
  LEAVE = 6,
};

struct nan_state {
  enum nan_peer_status status;
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

static struct nan_state *g_state; /* sorry */

void nan_discovery_start(struct nan_state *state);
uint8_t nan_publish(struct nan_state *state);
int nan_unpublish(struct nan_state *state);
uint8_t nan_subscribe(struct nan_state *state);
int nan_unsubscribe(struct nan_state *state);
int nan_swap_polarity(struct nan_state *state);
const char* status_str(enum nan_peer_status s);
#endif
