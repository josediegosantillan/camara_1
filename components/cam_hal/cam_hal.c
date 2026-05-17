#include "cam_hal.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

static const char *TAG = "CAM_HAL";
static int s_jpeg_quality = CAM_HAL_DEFAULT_JPEG_QUALITY;
static framesize_t s_frame_size = CAM_HAL_DEFAULT_FRAME_SIZE;
static bool s_flash_led_ready = false;
static bool s_flash_led_on = false;

#define FLASH_LED_GPIO GPIO_NUM_4

// Pines AI-Thinker [cite: 84-100]
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

static esp_err_t ensure_flash_led_ready(void) {
    if (s_flash_led_ready) {
        return ESP_OK;
    }

    esp_err_t err = gpio_reset_pin(FLASH_LED_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo resetear GPIO del LED flash: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_set_direction(FLASH_LED_GPIO, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo configurar GPIO del LED flash: %s", esp_err_to_name(err));
        return err;
    }

    gpio_set_level(FLASH_LED_GPIO, 0);
    s_flash_led_ready = true;
    s_flash_led_on = false;
    return ESP_OK;
}

static bool is_supported_frame_size(framesize_t frame_size) {
    switch (frame_size) {
        case FRAMESIZE_QQVGA:
        case FRAMESIZE_QVGA:
        case FRAMESIZE_CIF:
        case FRAMESIZE_HVGA:
        case FRAMESIZE_VGA:
        case FRAMESIZE_SVGA:
        case FRAMESIZE_XGA:
        case FRAMESIZE_HD:
        case FRAMESIZE_SXGA:
        case FRAMESIZE_UXGA:
            return true;
        default:
            return false;
    }
}

esp_err_t camera_init_hardware(void) {
    camera_config_t config = {0};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    
    // OPTIMIZACIÓN: Subir XCLK a 24MHz para más FPS
    // (Si hay ruido visual, bajar a 20MHz)
    config.xclk_freq_hz = 24000000;
    
    config.pixel_format = PIXFORMAT_JPEG;
    
    // Resolución VGA (640x480) - Balance velocidad/calidad
    // Opciones más rápidas: FRAMESIZE_HVGA (480x320) o FRAMESIZE_CIF (400x296)
    config.frame_size = s_frame_size;

    // JPEG Quality: 12-15 = buena calidad, 18-25 = más velocidad
    // Valor óptimo para streaming fluido
    config.jpeg_quality = s_jpeg_quality;
    
#if CONFIG_SPIRAM
    // OPTIMIZACIÓN: 3 buffers en PSRAM para pipeline DMA sin esperas
    // El driver captura en uno mientras envías otro
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 3;
#else
    // Sin PSRAM: 2 buffers máximo por limitación de DRAM
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 2;
#endif
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo al iniciar camara: 0x%x", err);
        return err;
    }

    ensure_flash_led_ready();
    return ESP_OK;
}

esp_err_t cam_hal_set_jpeg_quality(int quality) {
    if (quality < 10 || quality > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        return ESP_ERR_INVALID_STATE;
    }

    if (sensor->set_quality(sensor, quality) != 0) {
        ESP_LOGE(TAG, "No se pudo aplicar calidad JPEG=%d", quality);
        return ESP_FAIL;
    }

    s_jpeg_quality = quality;
    ESP_LOGI(TAG, "Calidad JPEG aplicada: %d", s_jpeg_quality);
    return ESP_OK;
}

int cam_hal_get_jpeg_quality(void) {
    return s_jpeg_quality;
}

esp_err_t cam_hal_set_frame_size(framesize_t frame_size) {
    if (!is_supported_frame_size(frame_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        return ESP_ERR_INVALID_STATE;
    }

    if (sensor->set_framesize(sensor, frame_size) != 0) {
        ESP_LOGE(TAG, "No se pudo aplicar frame size=%d", (int)frame_size);
        return ESP_FAIL;
    }

    s_frame_size = frame_size;
    ESP_LOGI(TAG, "Frame size aplicado: %s (%d)", cam_hal_get_frame_size_name(s_frame_size), (int)s_frame_size);
    return ESP_OK;
}

framesize_t cam_hal_get_frame_size(void) {
    return s_frame_size;
}

const char *cam_hal_get_frame_size_name(framesize_t frame_size) {
    switch (frame_size) {
        case FRAMESIZE_QQVGA: return "QQVGA";
        case FRAMESIZE_QVGA: return "QVGA";
        case FRAMESIZE_CIF: return "CIF";
        case FRAMESIZE_HVGA: return "HVGA";
        case FRAMESIZE_VGA: return "VGA";
        case FRAMESIZE_SVGA: return "SVGA";
        case FRAMESIZE_XGA: return "XGA";
        case FRAMESIZE_HD: return "HD";
        case FRAMESIZE_SXGA: return "SXGA";
        case FRAMESIZE_UXGA: return "UXGA";
        default: return "DESCONOCIDA";
    }
}

esp_err_t cam_hal_set_flash_led(bool on) {
    esp_err_t err = ensure_flash_led_ready();
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(FLASH_LED_GPIO, on ? 1 : 0);
    s_flash_led_on = on;
    ESP_LOGI(TAG, "LED flash %s", on ? "encendido" : "apagado");
    return ESP_OK;
}

bool cam_hal_get_flash_led(void) {
    return s_flash_led_on;
}
