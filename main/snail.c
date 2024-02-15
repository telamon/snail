#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "snail.h"
#include "store.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "picofeed.h"
#include "string.h"

static struct snail_state state = {0};

#define TAG "snail.c"
#define BTN GPIO_NUM_39

#ifdef PROTO_NAN
#include "nanr.h"
#endif
#ifdef PROTO_SWAP
#include "swap.h"
#endif

#ifdef DISPLAY_LED
// WS2812
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "math.h"

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
// #define MAX(a, b) ((a)>(b)?(a):(b)) - redefined?
void display_state (struct snail_state* state) {
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
    case NOTIFY: {
      const float f = sin(M_PI * 2 * fmod(t0 * 4, 1));
      led_strip_set_pixel(led_strip, 0, 0, 0 , f * pow(t0, 3) * 0xff);
      ESP_ERROR_CHECK(led_strip_refresh(led_strip));
      return;
    }
    case ATTACH: {
      const float sw = 0.70; // Stabilize at @ three quarters
      if (t0 < sw) {
       const float acc = 1 - pow(t0, 3);
       const float t1 = BW(seed, (int)(acc * (duration * sw)) + 1);
       int f = sin(M_PI * 2 * t1) * 0x40;
       led_strip_set_pixel(led_strip, 0, 0x80 + f, 0 , 0x80 - f);
      } else {
       const float t2 = BW(seed, (int)(duration * (1 - sw) / 3));
       int f = (1 + sin(M_PI * 2 * t2) / 2) * 0x30;
       led_strip_set_pixel(led_strip, 0, 0x80 + f, 10, 0x80 + f);
      }
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

void init_POP01(void) {
  /* Does it run?; TODO: call after wifi init */
  pico_keypair_t pair = {0};
  pico_crypto_keypair(&pair);
  char pkstr[64];
  for (int i = 0; i < 32; i++) sprintf(pkstr+i*2, "%02x", pair.pk[i]);
  ESP_LOGI(TAG, "secret initialized:\n %s",pkstr);
  char msg[] = "Let S.N.A.I.L terraform the infospheres";
  pico_feed_t feed = {0};
  pico_feed_init(&feed);
  pico_feed_append(&feed, (uint8_t*)msg, strlen(msg), pair);
}

/* The main task drives optional UI
 * and wifi NAN discovery.
 */
void app_main(void) {
  /* Initialization code */
  ESP_LOGI(TAG, "snail.c main()");
  init_display();

  display_state(&state);

  /*storage_init();*/
  /*storage_deinit();*/

#ifdef PROTO_NAN
  nanr_discovery_start();
#endif
  /* SoftAP Swapping */
#ifdef PROTO_SWAP
  uint64_t start = esp_timer_get_time();
  swap_init();
  uint32_t delta = (esp_timer_get_time() - start) / 1000;
  ESP_LOGI(TAG, "Swap init + seek %"PRIu32" ms", delta);
  // state.status = SEEK;
#endif

  init_POP01();

  /* Hookup button */
  ESP_ERROR_CHECK(gpio_set_direction(BTN, GPIO_MODE_INPUT));
  ESP_ERROR_CHECK(gpio_pullup_en(BTN));
  int hold = gpio_get_level(BTN);
  TickType_t pressedAt = 000000000;
  TickType_t mode_start = xTaskGetTickCount();
  while (1) {
    int b = gpio_get_level(BTN); // TODO: attempt interrupts
    if (hold != b) {
      if (!b) pressedAt = xTaskGetTickCount();
      else {
        uint16_t holdTime = xTaskGetTickCount() - pressedAt;
        // Long Press
        if (holdTime > 100) {
          ESP_ERROR_CHECK(swap_gateway_enable(!swap_gateway_is_enabled()));
#ifdef PROTO_NAN
          nanr_unpublish();
          nanr_unsubscribe();
#endif
        } else { // Short
          // swap_polarity(); polarity swapping was due to a limitation in esp-nan.
        }
        ESP_LOGW(TAG, "Button was held %i", holdTime);
        delay(150);
      }
    }
    hold = b;

    display_state(&state);
    // ESP_LOGI(TAG, "free: %zu", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if ((state.status == SEEK || state.status == NOTIFY)) {
      uint32_t r = esp_random() ; // Force drift/desync
      const uint16_t delta = xTaskGetTickCount() - mode_start;
      if (delta > (10000 + (r & 2047)) / portTICK_PERIOD_MS) {
        mode_start = xTaskGetTickCount();
        // ESP_LOGW(TAG, "SWAPPING POLARITY %i, r: %08lx", delta, r);
        // Disabled for manual testing.
        swap_polarity();
      }
    }
    delay(5);
  }
}

void swap_polarity() {
#ifdef PROTO_NAN
  nanr_swap_polarity();
#else
  // if (state.status == SEEK) snail_transition(NOTIFY);
  // else state.status = SEEK;
#endif
}

void snail_inform_complete(const int exit_code) {
#ifdef PROTO_NAN
  nanr_inform_complete(exit_code);
#endif
#ifdef PROTO_SWAP
  swap_deauth(exit_code);
#endif
  // snail_transition(LEAVE);
}

const char* status_str(peer_status s) {
  switch (s) {
    case SEEK: return "SEEK";
    case NOTIFY: return "NOTIFY";
    case ATTACH: return "ATTACH";
    case INFORM: return "INFORM";
    case LEAVE: return "LEAVE";
    case OFFLINE: return "OFFLINE";
    default: return "unknown";
  }
}

void snail_transition (peer_status target) {
  int ret = validate_transition(state.status, target);
  ESP_LOGI(TAG, "Status change: %s => %s, v: %i", status_str(state.status), status_str(target), ret);
  if (ret != 0) {
    ESP_LOGE(TAG, "Invalid tansition: %s => %s", status_str(state.status), status_str(target));
    abort();
  }
  state.status = target;
  // TODO: Dispatch blocked listner
}

peer_status snail_current_status(void) {
  return state.status;
}

int snail_transition_valid(peer_status to) {
  return validate_transition(snail_current_status(), to);
}

/**
 * @brief State Transition Matrix
 * @returns 0: valid, -1: invalid source state, 1: invalid target state
 */
int validate_transition(peer_status from, peer_status to) {
  switch (from) {
    case OFFLINE:
      switch (to) {
        case NOTIFY:
        case SEEK:
        case LEAVE:
          return 0;
        default: return 1;
      }
    case SEEK:
      switch (to) {
        case NOTIFY:
        case ATTACH:
          return 0;
        default: return 1;
      }
    case NOTIFY:
      switch (to) {
        case SEEK:
        case ATTACH:
          return 0;
        default: return 1;
      }
    case ATTACH:
      switch (to) {
        case INFORM:
        case LEAVE:
          return 0;
        default: return 1;
      }
    case INFORM:
      return to == LEAVE ? 0 : 1;
    case LEAVE:
      switch (to) {
        case SEEK:
        case NOTIFY:
          return 0;
        default: return 1;
      }
    default: return -1;
  }
}
