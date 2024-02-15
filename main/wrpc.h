#include <esp_wifi.h>
#include <esp_err.h>
#include "lwip/ip_addr.h"
esp_err_t wrpc_init(void);
esp_err_t wrpc_connect(esp_netif_t *interface, ip_addr_t *target_address);
