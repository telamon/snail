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
#include "math.h"

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

#define BW(s, w) (((s) % (w)) / ((float)w))
#define MAX(a, b) ((a)>(b)?(a):(b))
void display_state (struct nan_state *state) {
  const TickType_t seed = xTaskGetTickCount();
  /* Global Animation Duration */
  const uint16_t duration = 5000 / portTICK_PERIOD_MS;
  /* Timer 0: (0.0 to 1.0 in duration millis) */
  const float t0 = BW(seed, duration);

  switch (state->status) {
    case SEEK: {
      const float t1 = fmod(t0 * 3, 1);
      const float f = (1 + sin(M_PI * 2 * t1)) / 2 * 10;
      led_strip_set_pixel_hsv(led_strip, 0, fmod(360 + f, 360), 0xff, 0x80);
      ESP_ERROR_CHECK(led_strip_refresh(led_strip));
      return;
    }
    case ATTACH: {
      const float sw = 0.70; // Stabilize at @ three quarters
      if (t0 < sw) {
       const float acc = 1 - pow(t0, 3);
       const float t1 = BW(seed, (int)(acc * (duration * sw)) + 1);
       int f = sin(M_PI * 2 * t1) * 0x80;
       led_strip_set_pixel(led_strip, 0, 0x80 + f, 0 , 0x80 - f);
      } else {
       const float t2 = BW(seed, (int)(duration * (1 - sw) / 3));
       int f = (1 + sin(M_PI * 2 * t2) / 2) * 0x30;
       led_strip_set_pixel(led_strip, 0, 0x80 + f, 10, 0x80 + f);
      }
      ESP_ERROR_CHECK(led_strip_refresh(led_strip));
      return;
    }
    case NOTIFY: {
      const float f = sin(M_PI * 2 * fmod(t0 * 4, 1));
      led_strip_set_pixel(led_strip, 0, 0, 0 , f * pow(t0, 3) * 0xff);
      ESP_ERROR_CHECK(led_strip_refresh(led_strip));
      return;
    }
    case INFORM:{
      const float t1 = fmod(t0 * (1 / MAX(pow(t0, 2), 0.0001)), 1);
      const float a = sin(M_PI * 2 * t1) > 0 ? 1 : 0;
      led_strip_set_pixel(led_strip, 0, 0, 0xff * a , 0);
      ESP_ERROR_CHECK(led_strip_refresh(led_strip));
      return;
    }
    case LEAVE:
    case OFFLINE: {
      // const float i = pow(1 - fmod(t0 * 4, 1), 2) * 0xff;
      const uint8_t i = 0x40; // Can't appreciate chaos without order
      led_strip_set_pixel(led_strip, 0, i, i, i);
      ESP_ERROR_CHECK(led_strip_refresh(led_strip));
      return;
    }
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
      delay(150);
    }
    hold = b;

    display_state(&state);
    // ESP_LOGI(TAG, "free: %zu", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if ((state.status == SEEK || state.status == NOTIFY)) {
      uint32_t r = esp_random() ; // Force drift/desync
      const uint16_t delta = xTaskGetTickCount() - mode_start;
      if (delta > (7000 + (r & 2047)) / portTICK_PERIOD_MS) {
        mode_start = xTaskGetTickCount();
        // ESP_LOGW(TAG, "SWAPPING POLARITY %i, r: %08lx", delta, r);
        nan_swap_polarity(&state);
      }
    }
    delay(5);
  }
}
