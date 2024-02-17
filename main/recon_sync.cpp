#include "recon_sync.h"
#include "negentropy.h"
#include "negentropy/types.h"
#include "negentropy/storage/Vector.h"
#include "esp_log.h"
#include "sha-256.h" // <-- use Blake2b from Monocypher instead?
#include "esp_random.h"
#include "string.h"
#include <assert.h>

/****
 *
 *  After some quick calculation,
 *  negentropy is overkill for synchronizing a store of 2MB
 *  Maybe i't possible build a BTee store over FlashDB
 *  but prototype version might as well just use no
 *  FS and do a simple 4k aligned ringbuffer of available
 *  flash.
 *
 *
 ***********************************************************/
#define HASH2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[30], (a)[31]
#define HASHSTR "%02x%02x %02x%02x..%02x%02x"
#define MAX_FRAME_SIZE 4096
#define ID_SIZE 32
static const char* TAG = "recon";
static uint8_t *buffer = NULL;

static auto storage = negentropy::storage::Vector();
static negentropy::Negentropy<negentropy::storage::Vector> *ne = NULL;

enum MSG_TYPES {
  T_OK = 0,
  T_RECONCILE,
  T_BLOCK,
  T_BLOCK_REQ
};

static pwire_ret_t recon_onopen(pwire_event_t *ev) {
  ESP_LOGI(TAG, "pwire_onopen initiator: %i", ev->initiator);
  if (buffer != NULL) {
    ESP_LOGE(TAG, "memory still in use");
    abort();
  }
  /* Initialize link-state */
  buffer = (uint8_t*)calloc(1, 4098);

  ne = new Negentropy<negentropy::storage::Vector>(storage, 4096);
  if (ev->initiator) {
    std::string msg = ne->initiate();
    ESP_LOGI(TAG, "ngn_init() first msg size: %zu", msg.length());
    buffer[0] = T_RECONCILE;
    memcpy(buffer + 1, msg.data(), msg.length());
    ev->message = buffer;
    ev->size = msg.length() + 1;
  }
  return PW_REPLY;
}

static std::vector<std::string> have;
static std::vector<std::string> need;
static std::optional<std::string> reply;
static pwire_ret_t recon_ondata(pwire_event_t *ev) {
  ESP_LOGI(TAG, "pwire_data initiator: %i, msg-length: %" PRIu32, ev->initiator, ev->size);
  if (!ev->size) return PW_CLOSE;
  enum MSG_TYPES type = (enum MSG_TYPES) ev->message[0];
  std::string_view msg(reinterpret_cast<const char*>(ev->message + 1), ev->size);

  if (ev->initiator) {
    switch (type) {
      case T_RECONCILE: {
	reply = ne->reconcile(msg, have, need);
	if (!reply) {
	  // assert(have.empty());
	  // assert(need.empty();
	  return PW_CLOSE;
	}
      case T_OK:

	if (!have.empty()) {
	  auto& hash = have.back();
	  ESP_LOGI(TAG, "HAVE --> " HASHSTR, HASH2STR(hash.data()));
	  buffer[0] = T_BLOCK;
	  int n = pico_repo.read_block(buffer + 1, (uint8_t*)hash.data());
	  if (n < 0) {
	    ESP_LOGE(TAG, "read_block failed! (%i)", n);
	    return PW_CLOSE;
	  }
	  have.pop_back();
	  ev->size = ID_SIZE + n;
	  ev->message = buffer;
	}

	if (!need.empty()) {
	  auto& hash = need.back();
	  ESP_LOGI(TAG, "NEED <-- " HASHSTR, HASH2STR(hash.data()));
	  buffer[0] = T_BLOCK_REQ;
	  memcpy(buffer + 1, hash.data(), ID_SIZE);
	  need.pop_back();
	  ev->size = ID_SIZE + 1;
	  ev->message = buffer;
	  return PW_REPLY;
	}

	/* Do next reconcilliation round */
	buffer[0] = T_RECONCILE;
	memcpy(buffer + 1, reply->data(), reply->length());
	ev->message = buffer;
	ev->size = reply->length() + 1;
	return PW_REPLY;
      }
    }
  } else {
    switch (type) {
      case T_RECONCILE: {
	std::string reply = ne->reconcile(msg);
	if (reply.empty()) {
	  ESP_LOGI(TAG, "ngn_reconcile(I%i): reconcilliation complete!", ev->initiator);
	  return PW_CLOSE; // ALL DONE
	}
	ESP_LOGI(TAG, "ngn_reconcile(I%i) reply: %zu", ev->initiator, reply.length());

	buffer[0] = T_RECONCILE;
	memcpy(buffer + 1, reply.data(), reply.length());
	ev->message = buffer;
	ev->size = reply.length() + 1;
	return PW_REPLY;
      }
    }
  }
  abort(); // unreachable
}

static void recon_onclose(pwire_event_t *ev) {
  ESP_LOGI(TAG, "pwire_onclose initiator: %i", ev->initiator);
  if (buffer == NULL) {
    ESP_LOGE(TAG, "expected memory is gone");
    abort();
  }
  free(buffer);
  buffer = NULL;
  delete ne;
}

pwire_handlers_t wire_io = {
  .on_open = recon_onopen,
  .on_data = recon_ondata,
  .on_close = recon_onclose
};

static void init_dummy_store(bool initiator) {
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
}

pwire_handlers_t *recon_init_io(void) {
  init_dummy_store(true); // TODO: require param store ptr
  return &wire_io;
}


struct RepoWrapper : negentropy::StorageBase {
  uint64_t size() {
    return 0;
  }

  const negentropy::Item &getItem(size_t i) {

  }

  void iterate(size_t begin, size_t end, std::function<bool(const negentropy::Item &, size_t)> cb) {

  }

  size_t findLowerBound(size_t begin, size_t end, const negentropy::Bound &value) {

  }

  negentropy::Fingerprint fingerprint(size_t begin, size_t end) {

  }
};
