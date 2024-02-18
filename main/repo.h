#ifndef PICOREPO_H
#define PICOREPO_H
#include <stdint.h>
#include "picofeed.h"

/****
 *
 * Soul successor to pico-repo operating over NAND-flash
 *
 *******************/
typedef enum {
  PR_ERROR_UNSUPPORTED_BLOCK_TYPE = -1,
  PR_ERROR_INVALID_BLOCK = -2,
  PR_ERROR_BLOCK_TOO_LARGE = -3
} pr_error_t;

typedef struct {
  int start;
  int offset;

  /* Consider moving to struct block_metadata_t */
  uint8_t meta_flags;
  uint64_t meta_decay; /* N-hops / TTL */
  uint64_t meta_stored_at; /* This timestamp is funny, because we dont' have a clock (uses swarm-time) */
  uint8_t meta_hops;
  const uint8_t *meta_hash;
  const pf_block_t *block;
} pr_iterator_t;

typedef struct pr_internal pr_internal;
typedef struct pico_repo_t pico_repo_t;

/**
 * Storage types should implement this interface
 */
struct pico_repo_t{
  pr_internal *_state;

  /**
   * @brief Iterate forward though all storage slots
   * @param repo Initalized repository
   * @param iter Empty iterator, modify iter.start to begin from a different index
   * @return 0: not done, -1: done, empty space reached, 1: done - looped back to starting offset.
   */
  int (*next) (const pico_repo_t *repo, pr_iterator_t *iter);

  /**
   * @brief Writes block to storage
   * @param repo Initalized repository
   * @param n_hops number of hops as received over wire. (distance traveled)
   * @return slot-id or pr_error_t when result is < 0;
   */
  int (*write_block) (const pico_repo_t *repo, const uint8_t *block_bytes, uint8_t n_hops);
  int (*read_block_size) (uint8_t id[32]);
  int (*read_block) (pico_signature_t id);
};

int pr_init(pico_repo_t *repo);
void pr_deinit(pico_repo_t *repo);

#endif
