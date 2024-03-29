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
#include "recon_sync.h"
#include "sys/time.h"

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
#define MAX(a, b) ((a)>(b)?(a):(b)) // - redefined?
void display_state (struct snail_state* state) {
  const TickType_t seed = xTaskGetTickCount();
  /* Global Animation Duration */
  const uint16_t duration = pdMS_TO_TICKS(5000);
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
      led_strip_set_pixel(led_strip, 0, 0, 0 , f * pow(t0, 2) * 0xff);
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
  pr_iterator_t iter = {0};
  int n_blocks = 0;
  while (!pr_iter_next(&iter)) {
    int bsize = pf_block_body_size(iter.block);
    char *txt = calloc(1, bsize + 1);
    memcpy(txt, pf_block_body(iter.block), bsize);
    ESP_LOGI(TAG, "Slot%i: body: %s", n_blocks, txt);
    free(txt);
    n_blocks++;
  }
  pr_iter_deinit(&iter);
  ESP_LOGI(TAG, "init_POP01, %i blocks exist", n_blocks);
  if (n_blocks > 0) return; // Skip past initial block


  /* Does it run?; TODO: call after wifi init */
  pico_keypair_t pair = {0};
  pico_crypto_keypair(&pair);
  char pkstr[64];
  for (int i = 0; i < 32; i++) sprintf(pkstr+i*2, "%02x", pair.pk[i]);
  ESP_LOGI(TAG, "secret initialized:\n %s",pkstr);
  for (int i = 0; i < 7; i++) {
    char msg[] = " :Hello neighbourhood";
    msg[0] = i;
    pico_feed_t feed = {0};
    pf_init(&feed);
    pf_append(&feed, (uint8_t*)msg, strlen(msg), pair);
    int res = pr_write_block((const uint8_t*)pf_get(&feed, 0), 0);
    ESP_LOGI(TAG, "repo_write_block exit: %i", res);
    assert(res >= 0);
    pf_deinit(&feed);
  }
}

/* The main task drives optional UI
 * and wifi NAN discovery.
 */
void app_main(void) {
  /* Initialization */
  uint64_t start = esp_timer_get_time();
  ESP_LOGI(TAG, "snail.c main()");
  init_display();

  display_state(&state);
  pr_init();
  init_POP01();
  pwire_handlers_t *wire_io = recon_init_io();
#ifdef PROTO_NAN
  nanr_discovery_start(); /* desired but broken */
#endif

#ifdef PROTO_SWAP
  swap_init(wire_io);
#endif

  /* Hookup button */
  ESP_ERROR_CHECK(gpio_set_direction(BTN, GPIO_MODE_INPUT));
  ESP_ERROR_CHECK(gpio_pullup_en(BTN));
  int hold = gpio_get_level(BTN);
  TickType_t pressedAt = 0;
  TickType_t mode_start = xTaskGetTickCount();

  ESP_LOGI(TAG, "System ready, higher init took %"PRIu32" ms", (uint32_t)((esp_timer_get_time() - start) / 1000));
  while (1) {
    int b = gpio_get_level(BTN); // TODO: attempt interrupts
    if (hold != b) {
      if (!b) pressedAt = xTaskGetTickCount();
      else {
        uint16_t holdTime = xTaskGetTickCount() - pressedAt;
        // Long Press
        if (holdTime > 1000) {
	  ESP_LOGI(TAG, "Purging flash");
	  pr_purge_flash();
	  ESP_LOGI(TAG, "Restarting...");
	  abort();
#ifdef PROTO_NAN
          nanr_unpublish();
          nanr_unsubscribe();
#endif
        } else { // Short
          ESP_ERROR_CHECK(swap_gateway_enable(!swap_gateway_is_enabled()));
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
      if (delta > pdMS_TO_TICKS(10000 + (r & 2047))) {
        mode_start = xTaskGetTickCount();
      }
    }
    delay(5);
  }
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

void bump_time(uint64_t utc_millis) {
  uint64_t pop8 = pf_utc_to_pop8(utc_millis);
  if (pop8 < state.pop8_block_time) return;
  state.pop8_block_time = pop8;

  /* Alternative; bumb systemclock */
  struct timeval tv;
  tv.tv_sec = utc_millis / 1000;
  tv.tv_sec = (utc_millis % 1000) * 100;
  int err = settimeofday(&tv, NULL);
  if (err < 0) ESP_LOGE(TAG, "bump_time: Error %i", err);
  ESP_LOGI(TAG, "System-time bumped to: %"PRIu64, utc_millis);
}

uint64_t snail_current_pop8(void) {
  return state.pop8_block_time;
}
