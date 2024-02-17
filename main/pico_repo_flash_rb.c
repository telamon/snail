#include "picofeed.h"
#include "repo.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "memory.h"
#define ESP_PARTITION_SUBTYPE_DATA_PiC0 87
#define ESP_PARTITION_LABEL_PiC0 "PiC0"
#define SLOT_SIZE 2048
// 2 Mega Bytes
#define MEM_SIZE (SLOT_SIZE * 1024)
#define SLOT_GLYPH 0b10110001
#define FLAG_TOMB (1 << 1)

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
  uint8_t glyph;
  uint8_t iflags; /* inverted flags */
  /* Lol this is a waste of space but removes need to erase flash
   * and have a secondary "updatable" filesystem/registry */
  uint64_t decay;
  uint64_t stored_at;
  uint8_t hash[32];
  pf_block_t block;
} flash_slot_t;
/*
typedef struct {
  size_t start;
  size_t offset;
  flash_slot_t slot;
} pr_slot_iterator_t;
*/
#define SLOT2ADDR(s) ((((s) * SLOT_SIZE)) % MEM_SIZE)
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
  iter->stored_at = slot->stored_at;
  iter->block = &slot->block;
  return 0;
}
/**
 * @brief Finds an writable slot.
 * failing to find an empty slot then next
 * slot in line for recycling is returned.
 */
const void *find_empty_slot (const pico_repo_t *repo) {
  pr_iterator_t iter = {0};
  /* register for our garbage collection */
  int oldest_tomb = -1;
  uint64_t oldest_tomb_date = 0;
  int oldest_block = -1;
  uint64_t oldest_date = 0;
  int most_decayed = -1;
  uint8_t most_decayed_value = 0;
  int least_decayed = -1;
  uint8_t least_decayed_value = 0;

  int exit = 0;
  while (0 == (exit = next_slot(repo, &iter))) {
    if (iter.meta_flags & FLAG_TOMB) {
      // TODO increment tomb registers
    } else {
      // TODO increment decayed
    }
  }
  /* empty space found */
  if (exit == -1) return repo->_state->mmap_ptr + SLOT2ADDR(iter.start + iter.offset);
  /* overwrite expired block by age*/
  if (oldest_tomb != -1) return repo->_state->mmap_ptr + SLOT2ADDR(iter.start + oldest_tomb);
  /* overwrite active block by age */
  return repo->_state->mmap_ptr + SLOT2ADDR(iter.start + oldest_block);
}

int prf_write_block(const pico_repo_t *repo, const uint8_t *bytes) {
  pf_block_t *block = (pf_block_t*)bytes;
  // assert(pf_typeof(block) == PF_BLOCK_CANONICAL)
  void *slot = find_empty_slot(repo);
  const int size = pf_sizeof(block);
  // TODO: Slot struct
  memcpy(slot, block, size);
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
  repo->next_block = prf_next_block;
  return 0;
}

int pr_deinit(pico_repo_t *repo) {
  esp_partition_munmap(repo->_state->mmap_handle);
  repo->_state->mmap_ptr = NULL;
  repo->_state->mmap_handle = 0;

  /* free internal state */
  free(repo->_state);
}
