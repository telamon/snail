#include "picofeed.h"
#include "repo.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "memory.h"
#include <stdint.h>
#include <time.h>
#include "monocypher.h"
#define ESP_PARTITION_SUBTYPE_DATA_PiC0 87
#define ESP_PARTITION_LABEL_PiC0 "PiC0"
#define SLOT_SIZE 2048
// 2 Mega Bytes
#define MEM_SIZE (SLOT_SIZE * 1024)
#define SLOT_GLYPH 0b10110001
// #define FLAG_TOMB (1 << 1)

static const char TAG[] = "repo.c";
struct pr_internal {
  const esp_partition_t *partition;
  const void *mmap_ptr;
  esp_partition_mmap_handle_t mmap_handle;
};
/**
 * Understanding flash correctly, when erased
 * all bits are set to 1 - this damages the flash.
 * But pulling 1 to 0 does not.
 */
typedef struct {
  uint8_t glyph;  /* magic */
  uint8_t iflags; /* inverted flags */
  uint64_t decay; /* N-Shares counter */
  uint64_t stored_at; /* Wonky swarmtime */
  uint8_t hops;	  /* TTL */
  uint8_t hash[32]; /* Blake2b | builtin sha256 */
  /* TODO: pad block start to known offset */
  pf_block_t block; /* start of block */
} flash_slot_t;

#define SLOT2ADDR(s) ((((s) * SLOT_SIZE)) % MEM_SIZE)

static flash_slot_t *prf_get_slot (const pico_repo_t *repo, uint16_t idx) {
  return (flash_slot_t*) repo->_state->mmap_ptr + SLOT2ADDR(idx);
}

/**
 * @brief Iteraters through ring buffer
 * @return 0: not done, -1: done, empty space reached, 1: done, starting offset reached.
 */
static int next_slot(const pico_repo_t *repo, pr_iterator_t *iter) {
  const void *flash = repo->_state->mmap_ptr;
  iter->block = NULL;
  int first_run = !iter->offset;
  uint32_t vaddr = SLOT2ADDR(iter->start + iter->offset++);
  if (!first_run && vaddr == iter->start * SLOT_SIZE) return 1; /* Wrap around completed */
  const flash_slot_t *slot = flash + vaddr;
  if (slot->glyph != SLOT_GLYPH) return -1; /* End of memory reached */
  iter->meta_flags = ~slot->iflags;
  iter->meta_decay = __builtin_clzll(slot->decay);
  iter->meta_stored_at = slot->stored_at;
  iter->meta_hops = slot->hops;
  iter->meta_hash = slot->hash;
  iter->block = &slot->block;
  return 0;
}

/**
 * @brief Finds a writable slot.
 * failing to find an empty slot then next
 * slot in line for recycling is returned.
 */
static int find_empty_slot (const pico_repo_t *repo) {
  pr_iterator_t iter = {0};
  /* registers for our garbage collection */
  int most_decayed_idx = -1;
  uint8_t most_decayed_value = UINT8_MAX;
  int oldest_block = -1;
  uint64_t oldest_date = UINT64_MAX;

  int exit = 0;
  while (0 == (exit = next_slot(repo, &iter))) {
    if (iter.meta_decay < most_decayed_value) {
      most_decayed_value = iter.meta_decay;
      /* not sure if doing iterators right but raw offset points to next block, not current */
      most_decayed_idx = iter.offset - 1;
    }

    if (iter.meta_decay != 0) { // 0 is synonymous with entombed
      assert(CANONICAL == pf_typeof(iter.block));
      uint64_t date = pf_read_utc(iter.block->net.date);
      if (date < oldest_date) {
	oldest_date = date;
	oldest_block = iter.offset - 1;
      }
    }
  }
  ESP_LOGI(TAG, "find_empty_slot(search exit: %i), decayed: %i, oldest: %i", exit, most_decayed_idx, oldest_block);
  /* empty space found */
  if (exit == -1) return iter.start + iter.offset;
  /* overwrite first most expired block*/
  if (most_decayed_idx != -1) return iter.start + most_decayed_idx;
  /* overwrite active block by age */
  return iter.start + oldest_block;
}

static pr_error_t prf_write_block(const pico_repo_t *repo, const uint8_t *block_bytes, uint8_t hops) {
  pf_block_t *block = (pf_block_t*)block_bytes;
  if (CANONICAL != pf_typeof(block_bytes)) return PR_ERROR_UNSUPPORTED_BLOCK_TYPE;
  if (!pf_verify_block(block, block->net.author)) return PR_ERROR_INVALID_BLOCK;
  const size_t block_size = pf_sizeof(block);
  if (block_size + sizeof(flash_slot_t)) return PR_ERROR_BLOCK_TOO_LARGE; // Not 100% accurate, block-struct is counted twice.
  flash_slot_t *slot = malloc(SLOT_SIZE);
  slot->glyph = SLOT_GLYPH;
  slot->stored_at = time(NULL); // TODO: format is wrong
  slot->hops = hops;
  // pre-hash the block (block.id is 64 bytes, while hash is 32)
  // should be equal to hash given to crypto_sign as input.
  crypto_blake2b(slot->hash, 32, block_bytes, block_size);

  memcpy(&slot->block, block_bytes, block_size);

  int slot_idx = find_empty_slot(repo);
  flash_slot_t *dst_slot = prf_get_slot(repo, slot_idx);
  if (dst_slot->glyph != UINT8_MAX) {
    ESP_ERROR_CHECK(esp_partition_erase_range(repo->_state->partition, SLOT2ADDR(slot_idx), SLOT_SIZE));
    /* Ensure we erased the mmapped region */
    if (true) for (uint16_t i = 0; i < SLOT_SIZE; i++) assert(((uint8_t*)dst_slot)[i] == 0xff);
    else assert(dst_slot->glyph == UINT8_MAX); /* checking glyph should be sufficient */
  }
  memcpy(dst_slot, slot, SLOT_SIZE); // write flash
  free(slot);
  return slot_idx;
}

int pr_init(pico_repo_t *repo) {
  const esp_partition_t *part = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA,
    ESP_PARTITION_SUBTYPE_DATA_PiC0,
    ESP_PARTITION_LABEL_PiC0
  );
  if (part == NULL) {
    ESP_LOGE(TAG, "Could not find 'PiC0' partition, t: %i, sub_t: %i, label: %s",
      ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_PiC0,
      ESP_PARTITION_LABEL_PiC0
    );
    return -1;
  }
  repo->_state = calloc(1, sizeof(pr_internal));
  repo->_state->partition = part;

  ESP_ERROR_CHECK(esp_partition_mmap(
      repo->_state->partition,
      0,
      2097152,
      ESP_PARTITION_MMAP_DATA,
      &repo->_state->mmap_ptr,
      &repo->_state->mmap_handle
  ));
  repo->next = next_slot;
  repo->write_block = prf_write_block;
  return 0;
}

void pr_deinit(pico_repo_t *repo) {
  esp_partition_munmap(repo->_state->mmap_handle);
  repo->_state->mmap_ptr = NULL;
  repo->_state->mmap_handle = 0;

  /* free internal state */
  free(repo->_state);
}
