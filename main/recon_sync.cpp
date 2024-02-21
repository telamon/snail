#include "recon_sync.h"
#include "negentropy.h"
#include "negentropy/storage/BTreeMem.h"
#include "esp_log.h"
#include "picofeed.h"
#include "pwire.h"
#include "string.h"
#include "monocypher.h"
#include <assert.h>
#include <cstdint>

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

#define T_OK	    0
#define T_RECONCILE 0b0001
#define T_EXCHANGE  0b0010
#define T_GIVE_SET  0b0100
#define T_WANT_SET  0b1000

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

struct __attribute__((packed)) exchange_packet {
  uint8_t type;
  uint8_t want[32];
  uint8_t offer_hops;
  uint8_t block_bytes[0];
};


/* Process given block and store to flash */
static int accept_incoming_block(const pwire_event_t *ev) {
  struct exchange_packet *x = (struct exchange_packet*) ev->message;
  if (!(x->type & (T_GIVE_SET | T_EXCHANGE))) return 0;

  uint16_t expected_block_size = ev->size - sizeof(struct exchange_packet);
  pf_block_t *block = (pf_block_t*)x->block_bytes;
  pf_block_type_t btype = pf_typeof(block);
  if (btype != CANONICAL) {
    ESP_LOGE(TAG, "Unsupported block type %i", btype);
    return -1;
  }
  uint16_t block_size = pf_sizeof(block);
  if (expected_block_size != block_size) {
    ESP_LOGE(TAG, "Invalid block received, expected: %i, got: %i", expected_block_size, block_size);
    return -1;
  }
  ++x->offer_hops; // Receiver increments hop count
  int slot_id = pr_write_block(x->block_bytes, x->offer_hops);
  if (slot_id < 0) {
    ESP_LOGE(TAG, "Failed to store block, error: %i", slot_id);
    return -1;
  }
  // Unecessary rehash - block is hashed already by prepo
  uint8_t hash[32];
  crypto_blake2b(hash, 32, x->block_bytes, block_size);
  // Update index
  if (x->offer_hops < PR_MAX_HOPS) storage.insert(pf_read_utc(block->net.date), std::string_view((const char*)hash, 32));
  ESP_LOGI(TAG, "Block accepted " HASHSTR, HASH2STR(hash));
  // TODO: READJUST SYSTEM CLOCK/SWARM-TIME: time = time + (time - block-time) / 2
  return 0;
}

static uint16_t resolve_requested_block(struct exchange_packet *out, const uint8_t *hash) {
  uint16_t block_size = 0;
  pr_iterator_t iter{};
  while (!pr_iter_next(&iter)) {
    if (0 != memcmp(iter.meta.hash, hash, ID_SIZE)) continue;
    out->offer_hops = iter.meta.hops;
    block_size = pf_sizeof(iter.block);
    memcpy(out->block_bytes, iter.block, block_size); /* TODO: boundary check? */
    out->type |= T_GIVE_SET;
    // TODO: pr_decay(iter.slot_idx, 1);
    break;
  }
  pr_iter_deinit(&iter);
  if (!(out->type & T_GIVE_SET)) {
    ESP_LOGW(TAG, "Couldn't resolve requested block");
    // storage.erase(0, hash); // TODO: implement
  }
  return block_size;
}


static pwire_ret_t initiator_ondata(pwire_event_t *ev) {
  uint8_t type = ev->message[0];
  /* TODO: validate in order RECONCILE / EXCHANGE messaging */
  /*if (type == T_RECONCILE && last_msg != T_RECONCILE) {
    ESP_LOGE(TAG, "Unexpected recon packet received, disconnecting.");
    return PW_CLOSE;
  }*/

  /* Process incoming data */
  if (type == T_RECONCILE) { /* We sent an T_RECONCILE msg during open, expect T_RECONCILE msg */
    std::string_view msg(reinterpret_cast<const char*>(ev->message + 1), ev->size - 1);
    reply = ne->reconcile(msg, have, need);
    ESP_LOGI(TAG, "INIT RECON_RSP - mlen: %i, have: %i, need: %i", msg.size(), have.size(), need.size());
  } else if ((type & 0b11) == T_EXCHANGE){
    accept_incoming_block(ev);
    /* Initiator does not process T_WANT_SET */
  } else {
    ESP_LOGE(TAG, "I: Connection dropped, unknown type %i", type);
    return PW_CLOSE;
  }

  /* Prepare outgoing data */
  if (have.empty() && need.empty()) {
    /* We're in sync, and have/need should be satisfied, bye! */
    if (!reply.has_value()) {
      ESP_LOGI(TAG, "All empty, no reply, recon exit.");
      return PW_CLOSE;
    }
    /* ask for more if we're empty */
    std::string msg = reply.value();
    ESP_LOGI(TAG, "Recon continues %zu", msg.size());
    buffer[0] = T_RECONCILE;
    memcpy(buffer + 1, msg.data(), msg.size());
    ev->message = buffer;
    ev->size = msg.length() + 1;
    return PW_REPLY;
  }

  /* Define an exchange message, each roundtrip gives and requests 1 block. */
  struct exchange_packet *x = (struct exchange_packet*) buffer;
  memset(x, 0, sizeof(struct exchange_packet));
  x->type = T_EXCHANGE;

  if (!need.empty()) {
    auto& hash = need.back();
    ESP_LOGI(TAG, "NEED <-- " HASHSTR, HASH2STR(hash.data()));
    memcpy(x->want, hash.data(), ID_SIZE);
    x->type |= T_WANT_SET;
    need.pop_back();
  }

  int block_size = 0;
  if (!have.empty()) {
    auto& hash = need.back();
    ESP_LOGI(TAG, "HAVE --> " HASHSTR, HASH2STR(hash.data()));
    block_size = resolve_requested_block(x, (const uint8_t*)hash.data());
    need.pop_back();
  }

  ev->size = sizeof(struct exchange_packet) + block_size;
  ev->message = buffer;
  return PW_REPLY;
}

static pwire_ret_t recon_ondata(pwire_event_t *ev) {
  ESP_LOGI(TAG, "pwire_data initiator: %i, msg-length: %" PRIu32, ev->initiator, ev->size);
  if (!ev->size) return PW_CLOSE;
  /* Fork-off into initiator handler */
  if (ev->initiator) return initiator_ondata(ev);

  /* Proceed as non-initiator */
  uint8_t type = ev->message[0];
  if (type == T_RECONCILE) {
    std::string_view msg(reinterpret_cast<const char*>(ev->message + 1), ev->size - 1);
    std::string reply = ne->reconcile(msg);
    // if (reply.empty()) ESP_LOGI(TAG, "ngn_reconcile(I%i): reconcilliation complete?", ev->initiator);
    // return PW_CLOSE; // Hang-on, client decides when done right?
    ESP_LOGI(TAG, "ngn_reconcile(I%i) reply: %zu", ev->initiator, reply.length());
    buffer[0] = T_RECONCILE;
    memcpy(buffer + 1, reply.data(), reply.length());
    ev->message = buffer;
    ev->size = reply.length() + 1;
    return PW_REPLY;
  } else if ((type & 0b11) != T_EXCHANGE) {
    ESP_LOGE(TAG, "S: Connection dropped, unknown type %i", type);
    return PW_CLOSE;
  }

  accept_incoming_block(ev); // TODO: don't ignore error?

  struct exchange_packet *x_out = (struct exchange_packet*) buffer;
  memset(x_out, 0, sizeof(struct exchange_packet));
  x_out->type = T_EXCHANGE; // Server always replies with T_EXCHANGE even when empty.
  int block_size = 0;

  /* non-initiator responds to block requests */
  if (type & T_WANT_SET) {
    const struct exchange_packet *x_in = (const struct exchange_packet*) ev->message;
    block_size = resolve_requested_block(x_out, x_in->want);
  }

  ev->size = sizeof(struct exchange_packet) + block_size;
  ev->message = buffer;
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
  delete ne;
}

pwire_handlers_t wire_io = {
  .on_open = recon_onopen,
  .on_data = recon_ondata,
  .on_close = recon_onclose
};

pwire_handlers_t *recon_init_io() {
  /* Build in-mem index of all blocks on boot */
  ESP_LOGI(TAG, "Indexing block repo...");
  pr_iterator_t iter{};
  int i = 0;
  while (!pr_iter_next(&iter)) {
    if (iter.meta.hops >= PR_MAX_HOPS) continue;
    storage.insert(pf_read_utc(iter.block->net.date), std::string_view((const char*)iter.meta.hash, 32));
    int bsize = pf_block_body_size(iter.block);
    char *txt = (char*)calloc(1, bsize + 1);
    memcpy(txt, pf_block_body(iter.block), bsize);
    ESP_LOGI(TAG, "Slot%i: body: %s, "HASHSTR, i, txt, HASH2STR(iter.meta.hash));
    free(txt);
    ++i;
  };
  ESP_LOGI(TAG, "Done! %i blocks discovered", i);
  pr_iter_deinit(&iter);
  return &wire_io;
}
