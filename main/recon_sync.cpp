#include "recon_sync.h"
#include "negentropy.h"
#include "negentropy/storage/BTreeMem.h"
#include "esp_log.h"
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

static pico_repo_t *repo;

enum MSG_TYPES {
  T_OK = 0,
  T_RECONCILE,
  T_BLOCK,
  T_BLOCK_REQ
};
static auto storage = negentropy::storage::BTreeMem(); /* One global index */
static negentropy::Negentropy<negentropy::storage::BTreeMem> *ne = NULL;

static pwire_ret_t recon_onopen(pwire_event_t *ev) {
  ESP_LOGI(TAG, "pwire_onopen initiator: %i", ev->initiator);
  if (buffer != NULL) {
    ESP_LOGE(TAG, "memory still in use");
    abort();
  }
  /* Initialize link-state */
  buffer = (uint8_t*)calloc(1, 4098);

  ne = new Negentropy<negentropy::storage::BTreeMem>(storage, 4096);
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
	return PW_CLOSE;
      };
      case T_OK: {
	if (!have.empty()) {
	  auto& hash = have.back();
	  ESP_LOGI(TAG, "HAVE --> " HASHSTR, HASH2STR(hash.data()));
	  buffer[0] = T_BLOCK;

	  //int n = pico_repo.read_block(buffer + 1, (uint8_t*)hash.data());
	  //if (n < 0) {
	    //ESP_LOGE(TAG, "read_block failed! (%i)", n);
	    //return PW_CLOSE;
	  //}
	  have.pop_back();
	  ev->size = ID_SIZE + 1;  // n;
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

      default: {
	ESP_LOGW(TAG, "initiator message type not implemented: %i", type);
	return PW_CLOSE;
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
      default: {
	ESP_LOGW(TAG, "reciever message type not implemented: %i", type);
	return PW_CLOSE;
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

pwire_handlers_t *recon_init_io(pico_repo_t *block_repository) {
  repo = block_repository;
  /* Build in-mem index of all blocks on boot */
  ESP_LOGI(TAG, "Indexing block repo...");
  pr_iterator_t iter{};
  int i = 0;
  while (!pr_next_slot(repo, &iter)) {
    storage.insert(pf_read_utc(iter.block->net.date), std::string_view((const char*)iter.meta.hash, 32));
    int bsize = pf_block_body_size(iter.block);
    char *txt = (char*)calloc(1, bsize + 1);
    memcpy(txt, pf_block_body(iter.block), bsize);
    ESP_LOGI(TAG, "Slot%i: body: %s", i, txt);
    free(txt);
    ++i;
  };
  ESP_LOGI(TAG, "Done! %i blocks discovered", i);
  return &wire_io;
}
