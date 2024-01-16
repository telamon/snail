#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nanr.h"
#include "esp_log.h"
#define TAG "snail.c"

void app_main(void) {
  ESP_LOGI(TAG, "snail.c main()");
  struct nan_state state = {0};
  nan_init(&state);
  // nan_publish(&state);
  ESP_LOGI(TAG, "NAN Initialized");
  nan_subscribe(&state);
  // int initiator = nan_swap_polarity(&state);
  loop_events(&state);
}
