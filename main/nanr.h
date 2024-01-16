#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"

#define APP_MAIN
#define NAN_TOPIC "snail"
#define NAN_FILTER ""
#define NAN_MSG "snail0"
// Publisher; // TODO: unused?
#define EV_RECEIVE BIT0
// Subscriber / initiator
#define EV_SERVICE_MATCH BIT1
#define EV_NDP_CONFIRMED BIT2
#define EV_NDP_FAILED BIT3

struct nan_state {
  EventGroupHandle_t event_group;
  esp_netif_t *esp_netif; // Localhost
  // Pub state
  uint8_t pub_id; // Published Service id
  uint8_t peer_id; // incoming Peer
  // Sub state
  uint8_t sub_id; // Subscribed Service id
  wifi_event_nan_svc_match_t svc_match_evt; // Matched Service info
  uint8_t peer_ndi[6]; // MAC of DataPathed Peer
};

void nan_init(struct nan_state *state);
uint8_t nan_publish(struct nan_state *state);
int nan_unpublish(struct nan_state *state);
uint8_t nan_subscribe(struct nan_state *state);
int nan_unsubscribe(struct nan_state *state);
int nan_swap_polarity(struct nan_state *state);
void loop_events (struct nan_state *state);
