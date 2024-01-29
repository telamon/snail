/**
 * Exposes SDMMC function for reading/writing blocks
 */

/* #include "driver/sdspi_host.h" */
#include <stdint.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"

#define SDMODE_SPI
#ifdef SDMODE_SPI
#define SD_CLK GPIO_NUM_22
#define SD_MISO GPIO_NUM_19
#define SD_MOSI GPIO_NUM_23
#define SD_CS GPIO_NUM_33
#endif

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
typedef struct {
  uint8_t flags; /* DELETED|LEFT|RIGHT */
  uint16_t size;
  uint16_t date;
  uint32_t date_recv; /* Date Received | there is an RTC but no sync */
  uint16_t hops; /* Inverse TTL */
  public_key_t author;
  block_id_t id; /* PicoBlocks use a non-malleable Signature as BlockID, not hash */
  block_id_t parentId; /* PicoDAG parent. unrelated to storage nodes. */
  block_hash_t children[4];
} BlockDescriptor; /* 512-(16+16+32+16+32+64+64+4*32) = 144bytes left for indexing */

/**
 * Wishlist:
 * list_blocks()
 * read_block()
 * write_block()
 * delete_block()
 *
 * Is it faster to use sd-cards without FS?
 */
