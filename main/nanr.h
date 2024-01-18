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

/* This belongs in snail.h */
enum nan_peer_status {
  OFFLINE = 0,
  CLUSTERING = 1, // TODO: Replace with LEAVE
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
  esp_netif_t *esp_netif; // Localhost
  // Pub state
  uint8_t pub_id; // Published Service id
  uint8_t peer_id; // incoming Peer
  // Sub state
  uint8_t sub_id; // Subscribed Service id
  wifi_event_nan_svc_match_t svc_match_evt; // Matched Service info
  uint8_t peer_ndi[6]; // MAC of DataPathed Peer / TODO: replace with remote_peer event

  /* ip addr */
};

static struct nan_state *g_state; /* sorry */

void nan_init(struct nan_state *state);
uint8_t nan_publish(struct nan_state *state);
int nan_unpublish(struct nan_state *state);
uint8_t nan_subscribe(struct nan_state *state);
int nan_unsubscribe(struct nan_state *state);
int nan_swap_polarity(struct nan_state *state);
EventBits_t nan_process_events (struct nan_state *state);
const char* status_str(enum nan_peer_status s);

#endif
