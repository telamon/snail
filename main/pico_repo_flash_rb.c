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

#define SLOT_GLYPH 0b10110001
// #define FLAG_TOMB (1 << 1)

/* TODO: use values from partition info instead */
#define SLOT_SIZE 4096
#define MEM_SIZE (0x200000)
#define SLOT_OFFSET(s) ((((s) * SLOT_SIZE)) % MEM_SIZE)

static const char TAG[] = "repo.c";
/*
struct pr_internal {
  const void *mmap_ptr;
  esp_partition_mmap_handle_t mmap_handle;
};
*/

static const esp_partition_t *partition;

/**
 * Understanding flash correctly, when erased
 * all bits are set to 1 - this damages the flash.
 * But pulling 1 to 0 does not.
 */
struct __attribute__((packed)) flash_slot {
  uint8_t glyph;  /* magic */
  uint8_t iflags; /* inverted flags */
  uint64_t decay; /* N-Shares counter */
  uint64_t stored_at; /* Wonky swarmtime */
  uint8_t hops;	  /* TTL */
  uint8_t hash[32]; /* Blake2b | builtin sha256 */
  /* TODO: pad block start to known offset */
  pf_block_t block; /* start of block */
} ;


static esp_err_t pr_get_slot (flash_slot_t *dst, uint16_t idx) {
  // ESP_LOGI(TAG, "pr_get_slot(%p, %i)", dst, idx);
  return esp_partition_read(partition, SLOT_OFFSET(idx), dst, SLOT_SIZE);
}

/**
 * @brief Iteraters through ring buffer
 * @return 0: not done, -1: done, empty space reached, 1: done, starting offset reached.
 */
int pr_iter_next(pr_iterator_t *iter) {
  iter->block = NULL;
  int first_run = iter->offset == 0;
  uint16_t idx = iter->start + iter->offset;
  iter->offset++;
  if (first_run) iter->_tmp = calloc(1, SLOT_SIZE);
  else memset(iter->_tmp, 0, SLOT_SIZE);

  if (!first_run && SLOT_OFFSET(idx) == SLOT_OFFSET(iter->start)) return 1; /* Wrap around completed */

  ESP_ERROR_CHECK(pr_get_slot(iter->_tmp, idx));
  flash_slot_t *slot = iter->_tmp;

  if (slot->glyph != SLOT_GLYPH) return -1; /* End of memory reached */
  iter->meta.flags = ~slot->iflags;
  iter->meta.decay = __builtin_clzll(slot->decay);
  iter->meta.stored_at = slot->stored_at;
  iter->meta.hops = slot->hops;
  iter->meta.hash = slot->hash;
  iter->block = &slot->block;
  return 0;
}

/* To be removed when mmap works. */
void pr_iter_deinit(pr_iterator_t *iter) {
  free(iter->_tmp);
  memset(iter, 0, sizeof(pr_iterator_t));
}

/**
 * @brief Finds a writable slot.
 * failing to find an empty slot then next
 * slot in line for recycling is returned.
 */
static int find_empty_slot () {
  pr_iterator_t iter = {0};
  /* registers for our garbage collection */
  int most_decayed_idx = -1;
  uint8_t most_decayed_value = UINT8_MAX;
  int oldest_block = -1;
  uint64_t oldest_date = UINT64_MAX;

  int exit = 0;
  while (0 == (exit = pr_iter_next(&iter))) {
    if (iter.meta.decay < most_decayed_value) {
      most_decayed_value = iter.meta.decay;
      /* not sure if doing iterators right but raw offset points to next block, not current */
      most_decayed_idx = iter.offset - 1;
    }

    if (iter.meta.decay != 0) { // 0 is synonymous with entombed
      assert(CANONICAL == pf_typeof(iter.block));
      uint64_t date = pf_read_utc(iter.block->net.date);
      if (date < oldest_date) {
	oldest_date = date;
	oldest_block = iter.offset - 1;
      }
    }
  }
  free(iter._tmp); /* I knew this was a bad idea */
  // ESP_LOGI(TAG, "find_empty_slot(search exit: %i), iter: %i, decayed: %i, oldest: %i", exit, iter.offset, most_decayed_idx, oldest_block);
  /* empty space found */
  if (exit == -1) return iter.start + iter.offset - 1;
  /* overwrite first most expired block*/
  if (most_decayed_idx != -1) return iter.start + most_decayed_idx;
  /* overwrite active block by age */
  return iter.start + oldest_block;
}

pr_error_t pr_write_block(const uint8_t *block_bytes, uint8_t hops) {
  pf_block_t *block = (pf_block_t*)block_bytes;
  if (CANONICAL != pf_typeof(block)) return PR_ERROR_UNSUPPORTED_BLOCK_TYPE;
  if (0 != pf_verify_block(block, block->net.author)) return PR_ERROR_INVALID_BLOCK;
  const size_t block_size = pf_sizeof(block);
  if (block_size + sizeof(flash_slot_t) > SLOT_SIZE) return PR_ERROR_BLOCK_TOO_LARGE; // Not 100% accurate, block-struct is counted twice.
  ESP_LOGI(TAG, "write_block() size: %zu", block_size);

  int slot_idx = find_empty_slot();
  ESP_LOGI(TAG, "Writing to slot %i", slot_idx);

  flash_slot_t *slot = calloc(1, SLOT_SIZE);

  ESP_ERROR_CHECK(pr_get_slot(slot, slot_idx));
  if (slot->glyph != UINT8_MAX) {
    ESP_LOGI(TAG, "erasing slot%i: %zu, +%i", slot_idx, (size_t)SLOT_OFFSET(slot_idx), SLOT_SIZE);
    ESP_ERROR_CHECK(esp_partition_erase_range(partition, SLOT_OFFSET(slot_idx), SLOT_SIZE));
  } else ESP_LOGI(TAG, "Slot seems empty, skipping erase");
  memset(slot, 0xff, SLOT_SIZE);

  slot->glyph = SLOT_GLYPH;
  slot->stored_at = time(NULL); // TODO: format is wrong
  slot->hops = hops;

  // pre-hash the block (block.id is 64 bytes, while hash is 32)
  // should be equal to hash given to crypto_sign as input.
  crypto_blake2b(slot->hash, 32, block_bytes, block_size);
  memcpy(&slot->block, block_bytes, block_size);

  esp_partition_write(partition, SLOT_OFFSET(slot_idx), slot, SLOT_SIZE);
  ESP_LOGI(TAG, "Block flashed @0x%x", SLOT_OFFSET(slot_idx));
  free(slot);
  return slot_idx;
}

void pr_purge_flash() {
  ESP_ERROR_CHECK(esp_partition_erase_range(partition, 0, MEM_SIZE));
}

int pr_init() {
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
  } else {
    ESP_LOGI(TAG, "Partition Found: %s, type: %i, sub: %i, size: %"PRIu32" @%"PRIu32", erz: %"PRIu32" enc: %i",
	part->label,
	part->type,
	part->subtype,
	part->size,
	part->address,
	part->erase_size,
	part->encrypted
    );
  }
  partition = part;
  /*
  repo->_state = calloc(1, sizeof(pr_internal));
  repo->_state->partition = part;

  ESP_ERROR_CHECK(esp_partition_mmap(
      repo->_state->partition,
      0,
      2097152,
      ESP_PARTITION_MMAP_DATA,
      &repo->_state->mmap_ptr,
      &repo->_state->mmap_handle
  ));*/
  return 0;
}

void pr_deinit() {
  /*
  esp_partition_munmap(repo->_state->mmap_handle);
  repo->_state->mmap_ptr = NULL;
  repo->_state->mmap_handle = 0;
  */
  /* free internal state */
  // free(repo->_state);
}

int pr_find_by_hash(pr_iterator_t *iter, uint8_t *hash) {
  int i = 0;
  while (!pr_iter_next(iter)) {
    if (0 == memcmp(iter->meta.hash, hash, 32)) return i;
    i++;
  }
  return -1;
}
