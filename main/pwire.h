#ifndef P_WIRE
#define P_WIRE
#include <stdint.h>

typedef void (*reply_t) (uint8_t data, uint32_t length, int close);

typedef enum {
  PW_REPLY = 0,
  PW_NOOP,
  PW_CLOSE
} pwire_ret_t;

typedef struct {
  int initiator;
  uint8_t *message;
  uint32_t size;
} pwire_event_t;

typedef pwire_ret_t (*on_open_cb) (pwire_event_t *event);
typedef pwire_ret_t (*on_data_cb) (pwire_event_t *event);
typedef void (*on_close_cb) (pwire_event_t *event);

typedef struct {
  on_open_cb on_open;
  on_data_cb on_data;
  on_close_cb on_close;
} pwire_handlers_t;

// typedef pwire_handlers_t* (*pwire_spawn_wire_cb) (void);
#endif
