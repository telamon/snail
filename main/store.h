/**
 * Exposes SDMMC function for reading/writing blocks
 */

/* #include "driver/sdspi_host.h" */
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define SDMODE_SPI
#ifdef SDMODE_SPI
#define SD_CLK GPIO_NUM_22
#define SD_MISO GPIO_NUM_19
#define SD_MOSI GPIO_NUM_23
#define SD_CS GPIO_NUM_33
#endif

esp_err_t storage_init(void);
esp_err_t storage_deinit(void);
