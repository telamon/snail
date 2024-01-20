#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nanr.h"
#include "esp_log.h"
#include "esp_random.h"
// WS2812
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "led_strip.h"

#define TAG "snail.c"

#define BTN GPIO_NUM_39

#define DISPLAY_LED
#ifdef DISPLAY_LED
static led_strip_handle_t led_strip;
static void set_led (uint32_t rgb) {
  led_strip_set_pixel(led_strip, 0, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
  ESP_ERROR_CHECK(led_strip_refresh(led_strip));
}
static void init_display(void) {
  /* LED strip initialization with the GPIO and pixels number*/
  led_strip_config_t strip_config = {
      .strip_gpio_num = 27,
      .max_leds = 1, // at least one LED on board
  };
  led_strip_rmt_config_t rmt_config = {
      .resolution_hz = 10 * 1000 * 1000, // 10MHz
      .flags.with_dma = false,
  };
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
  led_strip_clear(led_strip);
  set_led(0x121212);
}


void display_state (struct nan_state *state) {
  switch (state->status) {
    case SEEK:
      for (int i = 0; i < 0xff; i++) {
        led_strip_set_pixel_hsv(led_strip, 0, 360, 0xff, i);
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
        set_led(0xff0000);
        delay(5);
      }
      delay(300);
      break;

    case NOTIFY:
     set_led(0x0000ff);
     delay(100);
     led_strip_clear(led_strip);
     delay(50);
     set_led(0x0000ff);
     delay(200);
     led_strip_clear(led_strip);
     delay(900);
     break;

    case ATTACH:
     set_led(0x9010ff);  // PURPLE
     delay(100);
     break;

    case INFORM:
     set_led(0x10ff10);   // GREEN
     delay(100);
     break;

    case LEAVE:
    case OFFLINE:
     for (int i = 0; i < 0xff; i++) {
       set_led((i << 16) | (i << 8) | i); // WHITE
       delay(10);
     }
     led_strip_clear(led_strip);
     delay(500);
     break;
  }
}
#endif

/* The main task drives optional UI
 * and wifi NAN discovery.
 */
void app_main(void) {
  /* Initialization code */
  ESP_LOGI(TAG, "snail.c main()");
  init_display();

  struct nan_state state = {0};
  display_state(&state);
  nan_discovery_start(&state);

  /* Hookup button */
  ESP_ERROR_CHECK(gpio_set_direction(BTN, GPIO_MODE_INPUT));
  ESP_ERROR_CHECK(gpio_pullup_en(BTN));
  // ESP_ERROR_CHECK(gpio_intr_enable(BTN));

  int hold = gpio_get_level(BTN);
  TickType_t mode_start = xTaskGetTickCount();
  while (1) {
    int b = gpio_get_level(BTN); // TODO: attempt interrupts
    if (hold != b && !b) {
      set_led(0x121212);
      int res = nan_swap_polarity(&state);
      if (res == -1) set_led(0xff0000); // Fail
      else set_led(0x00ff00); // success
      delay(50);
    }
    hold = b;

    display_state(&state);
    delay(10);
    // ESP_LOGI(TAG, "free: %zu", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if (false && (state.status == SEEK || state.status == NOTIFY)) {
      uint32_t r = esp_random() & 0xff; // Force drift/desync
      if (xTaskGetTickCount() - mode_start - r> 10000 / portTICK_PERIOD_MS) {
        nan_swap_polarity(&state);
        mode_start = xTaskGetTickCount();
      }
    }
  }
}
