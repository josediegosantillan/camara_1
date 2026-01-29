#include "cam_hal.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "sdkconfig.h"

static const char *TAG = "CAM_HAL";

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
    config.frame_size = FRAMESIZE_VGA; 

    // JPEG Quality: 12-15 = buena calidad, 18-25 = más velocidad
    // Valor óptimo para streaming fluido
    config.jpeg_quality = 12;
    
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
    return ESP_OK;
}
