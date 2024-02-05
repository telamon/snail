/**
* Software AP - Swapping
*/
#include "esp_log.h"
#include "esp_wifi.h"
#include "snail.h"
#include "swap.h"
#include "nvs_flash.h"

static const char TAG[] = "swap.c";
// static const uint8_t SSID[] = "snail";
void swap_init(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  ret = esp_wifi_init(&config);
  ESP_ERROR_CHECK(ret);
}

void swap_deinit(void) {
  ESP_ERROR_CHECK(esp_wifi_deinit());
}

void swap_seek(void) {
  wifi_mode_t mode;
  ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
  ESP_LOGI(TAG, "Current Mode %i", mode);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

  // Starts whatever mode we set
  ESP_ERROR_CHECK(esp_wifi_start());

  // scanning...
  wifi_scan_config_t scan_conf = {
    .channel = 6,
    .show_hidden = true,
    // .ssid = &SSID,
    .scan_type = WIFI_SCAN_TYPE_PASSIVE,
  };
  ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_conf, true)); // block
  uint16_t n_accesspoints = 0;
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&n_accesspoints));
  wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * n_accesspoints);
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&n_accesspoints, records));
  for (int i = 0; i < n_accesspoints; i++) {
    ESP_LOGI(TAG, "found: [%i] %s",
        records[i].rssi,
        records[i].ssid
    );
  }
  free(records);
}
