#ifndef WRPC_H
#define WRPC_H
#include <esp_wifi.h>
#include <esp_err.h>
#include "lwip/ip_addr.h"
#include "pwire.h"

esp_err_t wrpc_init(pwire_handlers_t *handlers);
esp_err_t wrpc_connect(esp_netif_t *interface, ip_addr_t *target_address);
#endif
