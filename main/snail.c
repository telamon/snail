#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nanr.h"
#include "esp_log.h"
// WS2812
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "led_strip.h"

#define TAG "snail.c"

#define BTN GPIO_NUM_39

static led_strip_handle_t led_strip;

static void configure_led(void) {
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
}

static void set_led (uint32_t rgb) {
  led_strip_set_pixel(led_strip, 0, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
  ESP_ERROR_CHECK(led_strip_refresh(led_strip));
}

void display_state (struct nan_state *state) {
  switch (state->status) {
    case OFFLINE: return set_led(0x101010);
    case CLUSTERING: return set_led(0x008080);
    // ...
    case SEEK: return set_led(0xff0000);
    case NOTIFY: return set_led(0x0000ff);
    case ATTACH: return set_led(0x154015);
    case INFORM: return set_led(0x10ff010);
    case LEAVE: return set_led(0xffffff);
  }
}

void APP_ERR (int errcode) {
  // 1. Error check without abort()
  // 2. stick into error state
  // 3. optionally reboot after 5 sec.
  // (crashed snails should just reset)
}

void app_main(void) {
  /* Initialization code */
  ESP_LOGI(TAG, "snail.c main()");

  configure_led();
  set_led(0x121212);

  struct nan_state state = {0};
  display_state(&state);
  nan_init(&state);
  display_state(&state);

  // nan_publish(&state);
  ESP_LOGI(TAG, "NAN Initialized");
  nan_subscribe(&state);
  display_state(&state);

  /* Hookup button */
  ESP_ERROR_CHECK(gpio_set_direction(BTN, GPIO_MODE_INPUT));
  ESP_ERROR_CHECK(gpio_pullup_en(BTN));
  // ESP_ERROR_CHECK(gpio_intr_enable(BTN));

  int hold = gpio_get_level(BTN);
  int i = 0;
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
    APP_ERR(nan_process_events(&state));

    /* Clear Display */
    led_strip_clear(led_strip);
    delay(20);

    if (i > 4) {
      // nan_swap_polarity(&state);
      i = 0;
    } else i++;
  }
}
