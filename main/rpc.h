#ifndef RPC_H
#define RPC_H
#include "esp_netif.h"
#include "lwip/ip_addr.h"


#pragma pack(push, 1)
struct tlv_header {
  int8_t type; /* -128 ... +128 */
  uint16_t length;
};
#pragma pack(pop)

enum CMD_TYPE {
  CMD_ERR = -1,
  CMD_BYE = 0,
  CMD_OK = 1,
  CMD_RECON = 2,
  CMD_BLOCK_REQ = 3,
  CMD_BLOCK = 4,
  CMD_FIRMWARE = 71
};
int rpc_connect(esp_netif_t *interface, ip_addr_t *target_address);
int rpc_wait_for_peer_socket(TickType_t timeout);
void rpc_listen(void);
#endif
