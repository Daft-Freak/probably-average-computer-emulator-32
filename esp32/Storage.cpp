#include "esp_err.h"
#include "esp_vfs_fat.h"

#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"

#include "diskio_impl.h"
#include "diskio_sdmmc.h"

#include "config.h"

static sdmmc_host_t sd_host;
static sdmmc_card_t sd_card;

void init_storage()
{
    FATFS *fs = nullptr;

#ifdef SD_SPI
    // int spi bus
    spi_host_device_t spi_slot = SDSPI_DEFAULT_HOST;
    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = SD_SPI_MOSI_PIN;
    bus_config.miso_io_num = SD_SPI_MISO_PIN;
    bus_config.sclk_io_num = SD_SPI_SCK_PIN;
    ESP_ERROR_CHECK(spi_bus_initialize(spi_slot, &bus_config, SDSPI_DEFAULT_DMA));

    // init sdspi host
    ESP_ERROR_CHECK(sdspi_host_init());

    sdspi_dev_handle_t sdspi_handle;
    sdspi_device_config_t sdspi_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    sdspi_config.gpio_cs = gpio_num_t(SD_SPI_CS_PIN);
    sdspi_config.host_id = spi_slot;
    ESP_ERROR_CHECK(sdspi_host_init_device(&sdspi_config, &sdspi_handle));

    // prepare to init sdmmc host
    sd_host = SDSPI_HOST_DEFAULT();
    sd_host.slot = spi_slot;
#elif defined(SD_SDMMC)
    // init sdmmc
    ESP_ERROR_CHECK(sdmmc_host_init());

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

#ifdef SD_SDMMC_1BIT
    slot_config.width = 1;
#endif

    ESP_ERROR_CHECK(sdmmc_host_init_slot(0, &slot_config));

    sd_host = SDMMC_HOST_DEFAULT();
    sd_host.slot = 0;
#endif

#ifdef SD_LDO_ID
    // setup LDO
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = SD_LDO_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = nullptr;

    ESP_ERROR_CHECK(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle));
    sd_host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

#ifdef SD_HIGH_SPEED
    sd_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#endif

    // init card
    if(sdmmc_card_init(&sd_host, &sd_card) != ESP_OK)
    {
        printf("failed to init sd card\n");
        return;
    }
    
    // mount fs
    BYTE pdrv;
    ESP_ERROR_CHECK(ff_diskio_get_drive(&pdrv));

    ff_diskio_register_sdmmc(pdrv, &sd_card);

    esp_vfs_fat_conf_t vfs_config = {};
    vfs_config.base_path = "";
    vfs_config.fat_drive = "";
    vfs_config.max_files = 2;

    ESP_ERROR_CHECK(esp_vfs_fat_register_cfg(&vfs_config, &fs));

    f_mount(fs, "", 0);
}
