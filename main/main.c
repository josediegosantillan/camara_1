#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

// --- IMPORTAMOS NUESTROS MÓDULOS ---
#include "cam_hal.h"
#include "sd_hal.h"
#include "wifi_net.h"
#include "http_server.h"
#include "crypto.h"
#include "expander_hal.h"  // <--- NUEVO: Control del PCF8574

static const char TAG[] = "MAIN_APP";
static bool sd_available = false;
static bool expander_available = false;
static uint32_t photo_counter = 0;
static TickType_t last_expander_retry = 0;

#define EXPANDER_RETRY_MS 10000

#define NVS_NAMESPACE_PHOTO "photos"
#define NVS_KEY_COUNTER "counter"

// Carga el contador desde NVS
static void load_photo_counter(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_PHOTO, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_get_u32(nvs_handle, NVS_KEY_COUNTER, &photo_counter);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Contador de fotos recuperado: %lu", (unsigned long)photo_counter);
        } else {
            photo_counter = 0;
        }
        nvs_close(nvs_handle);
    } else {
        photo_counter = 0;
    }
}

// Guarda el contador en NVS
static void save_photo_counter(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_PHOTO, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_u32(nvs_handle, NVS_KEY_COUNTER, photo_counter);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

// Captura foto encriptada y la guarda en SD
static void capture_encrypted_photo(void) {
    if (!sd_available) return;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Error capturando foto");
        return;
    }
    char filename[32];
    snprintf(filename, sizeof(filename), "IMG_%08lu", (unsigned long)photo_counter++);
    esp_err_t ret = crypto_save_file(filename, fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Foto guardada: %s.enc", filename);
        save_photo_counter();
    } else {
        ESP_LOGE(TAG, "Error guardando foto");
        photo_counter--; 
    }
}

// Captura video MJPEG encriptado
static void capture_encrypted_video(int duration_sec) {
    if (!sd_available) return;
    ESP_LOGI(TAG, "Iniciando captura de video (%ds)...", duration_sec);
    char filename[32];
    snprintf(filename, sizeof(filename), "VID_%08lu", (unsigned long)photo_counter++);
    
    size_t total_size = 0;
    size_t buffer_capacity = 512 * 1024; 
    uint8_t *video_buffer = heap_caps_malloc(buffer_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (!video_buffer) {
        ESP_LOGE(TAG, "Sin memoria RAM para video");
        photo_counter--;
        return;
    }
    
    const char *boundary = "\r\n--frame\r\n";
    const size_t boundary_len = strlen(boundary);
    int64_t end_time = esp_timer_get_time() + ((int64_t)duration_sec * 1000000);
    
    while (esp_timer_get_time() < end_time) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) continue;
        
        if (total_size + fb->len + boundary_len + 32 > buffer_capacity) {
            size_t new_capacity = buffer_capacity + 256 * 1024;
            uint8_t *new_buffer = heap_caps_realloc(video_buffer, new_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (new_buffer) {
                video_buffer = new_buffer;
                buffer_capacity = new_capacity;
            } else {
                esp_camera_fb_return(fb);
                break;
            }
        }
        memcpy(video_buffer + total_size, boundary, boundary_len);
        total_size += boundary_len;
        memcpy(video_buffer + total_size, fb->buf, fb->len);
        total_size += fb->len;
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (total_size > 0 && crypto_save_file(filename, video_buffer, total_size) == ESP_OK) {
        ESP_LOGI(TAG, "Video guardado: %s.enc", filename);
        save_photo_counter();
    } else {
        photo_counter--;
    }
    heap_caps_free(video_buffer);
}

void app_main(void)
{
    ESP_LOGI(TAG, "--- ARRANQUE DEL SISTEMA VIGILANTE CON EXPANSOR ---");

    // 1. Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_photo_counter();

    // 2. Inicializar Cámara (Esto también habilita el bus I2C)
    if (camera_init_hardware() != ESP_OK) {
        ESP_LOGE(TAG, "Fallo critico de Camara. Reiniciando...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    // 3. Inicializar Expansor PCF8574 (Bus I2C compartido)
    expander_available = (expander_init() == ESP_OK);
    if (!expander_available) {
        ESP_LOGW(TAG, "Expansor PCF8574 no disponible. Se deshabilita PIR/IR por I2C.");
    }
    last_expander_retry = xTaskGetTickCount();

    // 4. Inicializar SD (Modo 1-bit para liberar GPIO4 [cite: 161, 167])
    if (sd_card_init() != ESP_OK) {
        sd_available = false;
    } else {
        sd_available = true;
        crypto_init();
    }

    // 5. Red y Servidor Web
    wifi_net_init();
    start_webserver();

    ESP_LOGI(TAG, "--- VIGILANDO ---");

    TickType_t last_health_check = xTaskGetTickCount();
    
    while(1) {
        // --- LÓGICA DE DETECCIÓN POR PCF8574 ---
        if (expander_available && expander_read_pir()) {
            ESP_LOGI(TAG, "¡MOVIMIENTO DETECTADO!");
            
            // Activar LEDs IR (Conectados al P0 del expansor)
            expander_set_ir(true);
            
            // Notificar al server para activar stream MJPEG [cite: 205]
            http_server_notify_motion();
            
            if (sd_available) {
                capture_mode_t mode = http_server_get_capture_mode();
                if (mode == CAPTURE_MODE_VIDEO) {
                    capture_encrypted_video(http_server_get_video_duration());
                } else {
                    capture_encrypted_photo();
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            }
            
            // Mantener luz IR activa un tiempo
            vTaskDelay(pdMS_TO_TICKS(5000));
            expander_set_ir(false);
        }

        // Salud del sistema cada 10 segundos
        if (xTaskGetTickCount() - last_health_check >= pdMS_TO_TICKS(10000)) {
            ESP_LOGI(TAG, "[INFO] Heap: %lu | PSRAM: %lu", 
                     esp_get_free_heap_size(), 
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_health_check = xTaskGetTickCount();
        }

        // Reintentar deteccion del PCF8574 si no estaba presente
        if (!expander_available &&
            (xTaskGetTickCount() - last_expander_retry >= pdMS_TO_TICKS(EXPANDER_RETRY_MS))) {
            last_expander_retry = xTaskGetTickCount();
            if (expander_init() == ESP_OK) {
                expander_available = true;
                ESP_LOGI(TAG, "Expansor PCF8574 detectado. PIR/IR habilitados.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500)); 
    }
}
