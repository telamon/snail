#include "recon_sync.h"
#include "negentropy.h"
#include "negentropy/types.h"
#include "negentropy/storage/Vector.h"
#include "esp_log.h"
#include "sha-256.h" // <-- use Blake2b from Monocypher instead?
#include "esp_random.h"
#include "string.h"

#define HASH2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[30], (a)[31]
#define HASHSTR "%02x%02x %02x%02x..%02x%02x"
static const char* TAG = "recon";
static uint8_t *buffer = NULL;

static pwire_ret_t recon_onopen(pwire_event_t *ev) {
  ESP_LOGI(TAG, "pwire_onopen initiator: %i", ev->initiator);
  if (buffer != NULL) {
    ESP_LOGE(TAG, "memory still in use");
    abort();
  }
  buffer = (uint8_t*)calloc(1, 4096);
  if (ev->initiator) {
    buffer[0] = 0xFE;
    buffer[1] = 0xED;
    ev->message = buffer;
    ev->size = 2;
  }
  return PW_REPLY;
}

static pwire_ret_t recon_ondata(pwire_event_t *ev) {
  ESP_LOGI(TAG, "pwire_data initiator: %i, msg-length: %"PRIu32, ev->initiator, ev->size);
  if (ev->initiator) return PW_CLOSE;
  buffer[0] = 0xBE;
  buffer[1] = 0xEF;
  ev->message = buffer;
  ev->size = 2;
  return PW_REPLY;
}

static void recon_onclose(pwire_event_t *ev) {
  ESP_LOGI(TAG, "pwire_onclose initiator: %i", ev->initiator);
  if (buffer == NULL) {
    ESP_LOGE(TAG, "expected memory is gone");
    abort();
  }
  free(buffer);
  buffer = NULL;
}

pwire_handlers_t wire_io = {
  .on_open = recon_onopen,
  .on_data = recon_ondata,
  .on_close = recon_onclose
};

pwire_handlers_t *recon_init_io(void) {
  return &wire_io;
}

static auto storage = negentropy::storage::Vector();
static bool store_loaded = false;

static void init_storage(bool initiator) {
  if (store_loaded) return;
  uint8_t hash[16][32];
  for (int i = 0; i < 16; i++) {
    char v[12];
    if (i < 10) {
      sprintf(v, "id:%i", i);
    } else {
      uint32_t r = esp_random();
      sprintf(v, "id:%i", (int)(r & 0xfff));
    }
    calc_sha_256(hash[i], v, 12);
    std::string_view hview(reinterpret_cast<const char*>(hash[i]), 32);
    if (
        i < 2 ||
        i >= 10 ||
        (initiator && (i % 2)) ||
        (!initiator && !(i % 2))
       ) storage.insert(i, hview);
    ESP_LOGI(TAG, "Block spawned %i " HASHSTR, i, HASH2STR(hash[i]));

  }
  // for (const auto &item : myItems) {
  //   storage.insert(timestamp, id);
  // }
  storage.seal();
  store_loaded = true;
}

ngn_state* ngn_init (bool initiator, char *buffer, unsigned short* in_out) {
  init_storage(initiator);
  auto state = new ngn_state();
  state->initiator = initiator;
  state->buffer = buffer;
  state->buffer_cap = *in_out;
  *in_out = 0;
  state->ne = new Negentropy<negentropy::storage::Vector>(storage, state->buffer_cap);
  auto ne = static_cast<Negentropy<negentropy::storage::Vector>*>(state->ne);
  ESP_LOGI(TAG, "ngn_init(%i, cap: %i)", initiator, state->buffer_cap);

  if (initiator) {
    std::string msg = ne->initiate();
    ESP_LOGI(TAG, "ngn_init() first msg size: %zu", msg.length());
    memcpy(buffer, msg.data(), msg.length());
    *in_out = msg.length();
  }
  return state;
}

void ngn_deinit(ngn_state* handle) {
  delete static_cast<Negentropy<negentropy::storage::Vector>*>(handle->ne);
  delete handle;
}

static std::vector<std::string> have;
static std::vector<std::string> need;

int ngn_reconcile(ngn_state* handle, const unsigned short m_size, unsigned char* io_type) {
  auto ne = static_cast<Negentropy<negentropy::storage::Vector>*>(handle->ne);
  std::string_view msg(reinterpret_cast<const char*>(handle->buffer), m_size);
  unsigned char type = *io_type;
  // Client Recon
  if (handle->initiator) {
    /* WIP
    if (need.size()) {
      *io_type = 2; // BLOCK_REQ
      auto id = need.at(need.size() - 1);
      memcpy(handle->buffer, id.data(), negentropy::ID_SIZE);
      need.pop_back();
      return negentropy::ID_SIZE;
    }
    if (have.size()) {
    }
    */
    std::optional<std::string> reply = ne->reconcile(msg, have, need);

    // TODO: Stash have, need, reply;
    // Alternate Have/Need until both lists empty;
    // Transmit reply.
    // Repeat until have,needs and reply is empty.

    for (const auto &item: have) {
      ESP_LOGI(TAG, "HAVE --> " HASHSTR, HASH2STR(item.data()));
    }
    for (const auto &item: need) {
      ESP_LOGI(TAG, "NEED <-- " HASHSTR, HASH2STR(item.data()));
    }

    if (!reply) {
      ESP_LOGI(TAG, "ngn_recon: reconcilliation complete!");
      return 0;
    }

    *io_type = 1;
    ESP_LOGI(TAG, "ngn_reconcile(init: %i) reply: %zu", handle->initiator, reply->length());
    memcpy(handle->buffer, reply->data(), reply->length());
    return reply->length();
  }

  // Server Recon
  else {
    std::string reply = ne->reconcile(msg);

    if (reply.empty()) {
      ESP_LOGI(TAG, "ngn_reconcile(init: %i): reconcilliation complete!", handle->initiator);
      return 0;
    }

    ESP_LOGI(TAG, "ngn_reconcile(init: %i) reply: %zu", handle->initiator, reply.length());
    memcpy(handle->buffer, reply.data(), reply.length());
    *io_type = 1;
    return reply.length();
  }
}
