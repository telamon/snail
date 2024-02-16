#ifndef SWAP_H
#define SWAP_H
#include "esp_err.h"
#include <stdint.h>
#include "pwire.h"
/* Defaults */
#define CLOAK_SSID 1
#define CHANNEL 6
/* Size of Active Peer Registry */
#define N_PEERS 7

/* Spend millis waiting for peers between scans */
#define NOTIFY_TIME 6000

/* How many seconds to wait before reconnecting to
 * previously synced peer */
#define BACKOFF_TIME 20

void swap_init(pwire_handlers_t *wire_io);
void swap_deauth(int exit_code);
void swap_deinit(void);
void swap_dump_peer_list(void);
uint8_t swap_gateway_is_enabled(void);
esp_err_t swap_gateway_enable (uint8_t enable);
#endif
