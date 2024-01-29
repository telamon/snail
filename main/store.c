#include "store.h"
#include "sdmmc_cmd.h"
#include <dirent.h>

#define MOUNT_POINT "/sd0"
static const char TAG[] = "store.c";
static const char mount_point[] = MOUNT_POINT;
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t* card;

esp_err_t storage_init (void) {
  esp_err_t ret;
  ESP_LOGI(TAG, "Using SPI peripheral");

  spi_bus_config_t bus_cfg = {
    .sclk_io_num = SD_CLK,
    .miso_io_num = SD_MISO, // D0
    .mosi_io_num = SD_MOSI, // CMD (10k Pullup)
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4000,
  };
  ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize bus.");
    return ret;
  }

  // This initializes the slot without card detect (CD) and write protect (WP) signals.
  // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = SD_CS;
  slot_config.host_id = host.slot;

  ESP_LOGI(TAG, "Initializing SD card");
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
    .allocation_unit_size = 16 * 1024
  };

  ESP_LOGI(TAG, "Mounting filesystem");
  ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem. "
          "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
    } else {
      ESP_LOGE(TAG, "Failed to initialize the card (%s). "
          "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
    }
    return ret;
  }
  ESP_LOGI(TAG, "Filesystem mounted");

  // Card has been initialized, print its properties
  sdmmc_card_print_info(stdout, card);
  //sdmmc_write_sectors(card, &buffer, start_sector, sector_count); // Each sector is 512byte; 1k pBlock = 2sectors.
 
  // Dummy test listing files
  ESP_LOGI(TAG, "Listing root, for fun");
  DIR *d;
  struct dirent *entry;
  d = opendir(MOUNT_POINT);
  if (!d) {
    ESP_LOGE(TAG, "Failed opening dir "MOUNT_POINT);
    return 0;
  }
  while ((entry = readdir(d)) != NULL) {
    ESP_LOGI(TAG, "%s\t [%c]", entry->d_name, entry->d_type);
  }
  closedir(d);
  return ESP_OK;
}

esp_err_t storage_deinit(void) {
  esp_err_t res = esp_vfs_fat_sdcard_unmount(mount_point, card);
  if (res != ESP_OK) ESP_LOGE(TAG, "Failed to Unmount %s", mount_point);
  else ESP_LOGI(TAG, "sdcard %s unmounted", mount_point);
  return spi_bus_free(host.slot);
}
