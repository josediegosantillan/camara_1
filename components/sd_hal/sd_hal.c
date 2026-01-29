#include "sd_hal.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "SD_HAL";

esp_err_t sd_card_init(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // No formatear si falla, avisar
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";

    // Configuración Host
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    
    // TRUCO DEL VIEJO LOBO: Forzar 1-bit para liberar GPIO 4 [cite: 161, 167]
    host.flags = SDMMC_HOST_FLAG_1BIT; 

    // Configuración Slot
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // Confirmamos ancho de bus 1 bit [cite: 170]

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Fallo al montar: Verifique que la tarjeta este puesta");
        } else {
            ESP_LOGE(TAG, "Fallo al iniciar SD (%s)", esp_err_to_name(ret));
        }
    }
    return ret;
}
