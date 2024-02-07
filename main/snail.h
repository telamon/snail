#ifndef SNAIL_H
#define SNAIL_H
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
/*--------------------
 * CONFIG
 *--------------------*/
#define DISPLAY_LED
// #define PROTO_NAN
#define PROTO_SWAP
// #define USE_V6

//--------------------
#define delay(ms) vTaskDelay((ms) / portTICK_PERIOD_MS)

typedef enum {
  OFFLINE = 0,
  SEEK = 1,
  NOTIFY = 2,
  ATTACH = 3,
  INFORM = 4,
  LEAVE = 5,
} peer_status;

struct snail_state {
  peer_status status;
  EventGroupHandle_t event_group;
};

const char* status_str(peer_status s);
void snail_transition(peer_status target);
peer_status snail_current_status(void);
void snail_inform_complete(const int exit_code);
int validate_transition(peer_status from, peer_status to);
int snail_transition_valid(peer_status to);
void swap_polarity();
#endif
