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

typedef struct {
  peer_status status;
  EventGroupHandle_t event_group;

#ifdef PROTO_NAN
#endif
} snail_state;

const char* status_str(peer_status s);
int validate_transition(peer_status from, peer_status to);
void swap_polarity(snail_state *state);
#endif
