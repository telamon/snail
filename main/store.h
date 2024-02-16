/**
 * THIS FILE IS JUNK ATM.
 */

/* #include "driver/sdspi_host.h" */
#include <stdint.h>
#include "esp_err.h"
esp_err_t storage_init(void);
esp_err_t storage_deinit(void);

typedef uint8_t public_key_t[32];
typedef uint8_t block_id_t[64]; /* Synonymous with Signature */
typedef uint8_t block_hash_t[32];

/**
 * Synonymous with Node.
 * Sector0 contains the block-descriptor
 * Sector1-3 contains actual block
 */
struct block_descriptor {
  uint8_t flags; /* DELETED|LEFT|RIGHT */
  uint16_t size;
  uint16_t date;
  uint32_t date_recv; /* Date Received | there is an RTC but no sync */
  uint16_t hops; /* Inverse TTL */
  public_key_t author;
  block_id_t id; /* PicoBlocks use a non-malleable Signature as BlockID, not hash */
  block_id_t parentId; /* PicoDAG parent. unrelated to storage nodes. */
  uint32_t children[2]; /* Sector ptr */
}; /* 512-(16+16+32+16+32+64+64) = 144bytes left for indexing */


/**
 * Wishlist:
 * list_blocks()
 * read_block()
 * write_block()
 * delete_block()
 *
 * Is it faster to use sd-cards without FS?
 * update: yes it is, see below
 *
 * I know to little to do this right now.
 * First step is to get a solid on connectivity.
 * Need to benchmark and unglitch NAN or SW-AP
 *
 * # 24-02-16
 * There are a few flash-friendly filesystems out there.
 * Everytime a sector in flash is erased it gets slightly damaged.
 * So most solutions to this is "log-based" writes where all entries
 * where all entries just go into the next sector, treating the memory as a ring-buffer.
 * And second type is "copy-on-write", where each edit copies the entry to a new location
 * and marks the previous as obsolete.
 * They both come with tradeoffs and strengths.
 *
 * https://github.com/armink/FlashDB/blob/master/demos/esp32_spi_flash/main/main.c
 * https://github.com/littlefs-project/littlefs
 *
 * None of them is a perfect fit. littlefs is nice while Flashdb is the right amount of little.
 * (does TSDB support unordered inserts?)
 * Further since our model is cryptographically signed blocks we already know
 * that that no modification will be done to the bulk of the data.
 * What we essentially could use is a wear-leveling indexing system.
 *
 *
 * Good news is that we have about 1-2MB free builtin flash for storage in the prototype.
 * So external memory is not required.
 */
int store_list_blocks();
