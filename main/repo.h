#ifndef PICOREPO_H
#define PICOREPO_H
#include <stdint.h>
#include "picofeed.h"

/****
 *
 * Soul Successor to pico-repo operating over NAND-flash
 *
 *******************/

typedef struct {
  int start;
  int offset;
  uint8_t meta_flags;
  uint64_t meta_decay; /* N-hops / TTL */
  uint64_t stored_at; /* This timestamp is funny, because we dont' have a clock (uses swarm-time) */
  const pf_block_t *block;
} pr_iterator_t;

typedef struct pr_internal pr_internal;

typedef struct {
  pr_internal *_state;
  int (*read_block_size) (uint8_t id[32]);
  int (*read_block) (pico_signature_t id);
  int (*write_block) (uint8_t *dst, pico_signature_t id);
  int (*next_block) (pr_iterator_t *iter);
} pico_repo_t;

int pr_init(pico_repo_t *repo);
int pr_deinit(pico_repo_t *repo);

#endif
