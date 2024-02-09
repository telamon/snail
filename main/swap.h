#ifndef SWAP_H
#define SWAP_H

#define CLOAK_SSID 1
#define CHANNEL 6
/* Size of Active Peer Registry */
#define N_PEERS 7

/* Spend millis waiting for peers between scans */
#define NOTIFY_TIME 6000

/* How many seconds to wait before reconnecting to
 * previously synced peer */
#define BACKOFF_TIME 20

void swap_init(void);
void swap_deauth(int exit_code);
void swap_deinit(void);
void swap_dump_peer_list(void);
#endif
