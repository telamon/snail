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

/* Block metadata */
typedef struct {
  uint8_t flags; /* non-inverted flags */
  uint64_t decay; /* N-hops / TTL */
  uint64_t stored_at; /* we dont' have a clock (use swarm-time) */
  uint8_t hops;
  const uint8_t *hash;
} pr_metadata_t;

typedef struct {
  int start;
  int offset;
  pr_metadata_t meta;
  const pf_block_t *block;
} pr_iterator_t;

typedef struct pr_internal pr_internal;
typedef struct pico_repo_t pico_repo_t;

struct pico_repo_t{
  pr_internal *_state; /* not sure about this */
};

int pr_init(pico_repo_t *repo);
void pr_deinit(pico_repo_t *repo);

/**
 * @brief Iterate forward though all storage slots
 * @param repo Initalized repository
 * @param iter Empty iterator, modify iter.start to begin from a different index
 * @return 0: not done, -1: done, empty space reached, 1: done - looped back to starting offset.
 */
int pr_next_slot (const pico_repo_t *repo, pr_iterator_t *iter);

/**
 * @brief Writes block to storage
 * @param repo Initalized repository
 * @param n_hops number of hops as received over wire. (distance traveled)
 * @return slot-id or pr_error_t when result is < 0;
 */
int pr_write_block (const pico_repo_t *repo, const uint8_t *block_bytes, uint8_t n_hops);

int read_block_size (uint8_t id[32]);
int read_block (pico_signature_t id);

#endif
