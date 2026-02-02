#include "sd_hal.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "SD_HAL";
static const char *MOUNT_POINT = "/sdcard";
static sdmmc_card_t *g_sd_card = NULL;
static bool g_sd_mounted = false;

esp_err_t sd_card_init(void) {
    if (g_sd_mounted) {
        ESP_LOGW(TAG, "SD ya montada");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "=== INICIALIZANDO TARJETA SD ===");
    
    // IMPORTANTE: Asegurar que GPIO 4 (LED flash) no interfiera
    // En ESP32-CAM, GPIO 4 es usado por SD en modo 4-bit pero lo usamos en 1-bit
    gpio_reset_pin(GPIO_NUM_4);
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_4, 0);
    
    // Pequeño delay para estabilizar las líneas después de reset de GPIO
    vTaskDelay(pdMS_TO_TICKS(200));
    
    ESP_LOGI(TAG, "Intentando montar tarjeta SD en modo 1-bit...");
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // No formatear si falla, avisar
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    // Configuración Host - Reducir velocidad para mayor estabilidad
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;  // Modo 1-bit para liberar GPIO 4
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20MHz - más estable que high speed

    // Configuración Slot con pull-ups internos
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // Confirmamos ancho de bus 1 bit
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // Usar pull-ups internos

    ESP_LOGI(TAG, "Montando SD: modo 1-bit, freq=%d kHz", host.max_freq_khz);
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &g_sd_card);
    
    if (ret != ESP_OK) {
        g_sd_card = NULL;
        g_sd_mounted = false;
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Fallo al montar: Verifique que la tarjeta este insertada y formateada FAT32");
        } else if (ret == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "Fallo: Sin memoria para montar SD");
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Timeout: La tarjeta SD no responde - verificar conexiones fisicas");
        } else if (ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGE(TAG, "Respuesta invalida de SD - tarjeta danada o incompatible");
        } else {
            ESP_LOGE(TAG, "Fallo al iniciar SD (%s) - Codigo: 0x%x", esp_err_to_name(ret), ret);
        }
        ESP_LOGE(TAG, "=== CONSEJOS DE DIAGNOSTICO ===");
        ESP_LOGE(TAG, "1. Verificar que la SD este bien insertada");
        ESP_LOGE(TAG, "2. Probar formatear la SD en PC como FAT32 (no exFAT)");
        ESP_LOGE(TAG, "3. Probar con otra tarjeta SD");
        ESP_LOGE(TAG, "4. Verificar que no haya corto en los pines SD");
        return ret;
    }
    
    // Mostrar info de la tarjeta
    ESP_LOGI(TAG, "=== Tarjeta SD montada exitosamente ===");
    ESP_LOGI(TAG, "Nombre: %s", g_sd_card->cid.name);
    ESP_LOGI(TAG, "Capacidad: %llu MB", ((uint64_t)g_sd_card->csd.capacity) * g_sd_card->csd.sector_size / (1024 * 1024));
    ESP_LOGI(TAG, "Velocidad: %d kHz", g_sd_card->max_freq_khz);
    
    g_sd_mounted = true;
    return ESP_OK;
}

esp_err_t sd_card_format(void) {
    esp_err_t ret = ESP_OK;

    if (!g_sd_mounted) {
        ret = sd_card_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo montar SD para formatear");
            return ret;
        }
    }

    // Desmontar volumen FATFS (manteniendo driver activo)
    FRESULT fr = f_mount(NULL, "0:", 0);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "No se pudo desmontar FATFS antes de formatear (fr=%d)", fr);
    }

    size_t work_size = FF_MAX_SS * 2;
    if (work_size < 4096) {
        work_size = 4096;
    }
    uint8_t *work = (uint8_t *)malloc(work_size);
    if (!work) {
        ESP_LOGE(TAG, "Sin memoria para formatear SD");
        ret = ESP_ERR_NO_MEM;
        goto remount;
    }

#if FF_USE_MKFS
    MKFS_PARM opt = {
        .fmt = FM_FAT32,
        .n_fat = 1,
        .align = 0,
        .n_root = 0,
        .au_size = 0
    };

    ESP_LOGI(TAG, "Formateando SD a FAT32...");
    fr = f_mkfs("0:", &opt, work, work_size);
    free(work);
    work = NULL;

    if (fr != FR_OK) {
        ESP_LOGE(TAG, "Error formateando SD (fr=%d)", fr);
        ret = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "Formateo completado");
    }
#else
    free(work);
    work = NULL;
    ESP_LOGE(TAG, "MKFS no habilitado en FATFS");
    ret = ESP_ERR_NOT_SUPPORTED;
#endif

remount:
    if (g_sd_mounted && g_sd_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, g_sd_card);
        g_sd_mounted = false;
        g_sd_card = NULL;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t remount_ret = sd_card_init();
    if (ret == ESP_OK) {
        ret = remount_ret;
    } else if (remount_ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo remontar SD (%s)", esp_err_to_name(remount_ret));
    }

    return ret;
}

bool sd_card_is_mounted(void) {
    return g_sd_mounted;
}

esp_err_t sd_card_reinit(void) {
    ESP_LOGI(TAG, "Reintentando inicializacion de SD...");
    
    // Si ya está montada, primero desmontamos
    if (g_sd_mounted && g_sd_card) {
        ESP_LOGI(TAG, "Desmontando SD actual...");
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, g_sd_card);
        g_sd_mounted = false;
        g_sd_card = NULL;
    }
    
    // Delay para dar tiempo a que se estabilice
    vTaskDelay(pdMS_TO_TICKS(500));
    
    return sd_card_init();
}
