#include "repo.h"
#include "negentropy.h"
#include "negentropy/storage/Vector.h"
#include "esp_log.h"
#include "sha-256.h"

#define HASH2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[30], (a)[31]
#define HASHSTR "%02x%02x %02x%02x..%02x%02x"
static const char* TAG = "NEGEN";

static auto storage = negentropy::storage::Vector();
static bool store_loaded = false;

static void init_storage(bool initiator) {
  if (store_loaded) return;
  uint8_t hash[10][32];
  for (int i = 0; i < 10; i++) {
    char v[12];
    sprintf(v, "id:%i", i);
    calc_sha_256(hash[i], v, 12);
    std::string_view hview(reinterpret_cast<const char*>(hash[i]), 32);
    if (
        i < 2 ||
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

int ngn_reconcile(ngn_state* handle, const unsigned short m_size) {
  auto ne = static_cast<Negentropy<negentropy::storage::Vector>*>(handle->ne);
  std::string_view msg(reinterpret_cast<const char*>(handle->buffer), m_size);

  // Client Recon
  if (handle->initiator) {
    std::vector<std::string> have, need;
    std::optional<std::string> reply = ne->reconcile(msg, have, need);

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
    return reply.length();
  }
}
