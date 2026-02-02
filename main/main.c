#include <stdio.h>
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

static const char TAG[] = "MAIN_APP";
static bool sd_available = false;
static uint32_t photo_counter = 0;

// NVS para persistir el contador
#define NVS_NAMESPACE_PHOTO "photos"
#define NVS_KEY_COUNTER "counter"

// Carga el contador desde NVS (sobrevive reinicios)
static void load_photo_counter(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_PHOTO, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_get_u32(nvs_handle, NVS_KEY_COUNTER, &photo_counter);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Contador de fotos recuperado: %lu", (unsigned long)photo_counter);
        } else {
            photo_counter = 0;
            ESP_LOGI(TAG, "Contador de fotos iniciando en 0");
        }
        nvs_close(nvs_handle);
    } else {
        photo_counter = 0;
    }
}

static void heap_integrity_check(const char *stage) {
    if (!heap_caps_check_integrity_all(true)) {
        esp_rom_printf("HEAP CORRUPT at %s\n", stage);
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
    
    // Generar nombre único
    char filename[32];
    snprintf(filename, sizeof(filename), "IMG_%08lu", (unsigned long)photo_counter++);
    
    // Guardar encriptado
    esp_err_t ret = crypto_save_file(filename, fb->buf, fb->len);
    
    esp_camera_fb_return(fb);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Foto guardada: %s.enc", filename);
        // Persistir el contador para sobrevivir reinicios
        save_photo_counter();
    } else {
        ESP_LOGE(TAG, "Error guardando foto encriptada");
        photo_counter--;  // Revertir si falló
    }
}

// Captura video (secuencia de frames JPEG) y lo guarda como archivo MJPEG
static void capture_encrypted_video(int duration_sec) {
    if (!sd_available) return;
    
    ESP_LOGI(TAG, "Iniciando captura de video por %d segundos...", duration_sec);
    
    // Crear archivo temporal para acumular frames
    char filename[32];
    snprintf(filename, sizeof(filename), "VID_%08lu", (unsigned long)photo_counter++);
    
    // Buffer para acumular el video (MJPEG sin contenedor)
    // Cada frame JPEG se concatena con un separador
    size_t total_size = 0;
    size_t buffer_capacity = 512 * 1024;  // 512KB inicial
    uint8_t *video_buffer = heap_caps_malloc(buffer_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (!video_buffer) {
        ESP_LOGE(TAG, "No hay memoria para buffer de video");
        photo_counter--;
        return;
    }
    
    // Boundary para MJPEG
    const char *boundary = "\r\n--frame\r\n";
    const size_t boundary_len = strlen(boundary);
    
    int64_t start_time = esp_timer_get_time();
    int64_t end_time = start_time + ((int64_t)duration_sec * 1000000);
    int frame_count = 0;
    
    while (esp_timer_get_time() < end_time) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Frame perdido");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // Verificar si hay espacio
        size_t needed = boundary_len + fb->len + 32;
        if (total_size + needed > buffer_capacity) {
            // Expandir buffer si es posible
            size_t new_capacity = buffer_capacity + 256 * 1024;
            uint8_t *new_buffer = heap_caps_realloc(video_buffer, new_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (new_buffer) {
                video_buffer = new_buffer;
                buffer_capacity = new_capacity;
            } else {
                ESP_LOGW(TAG, "No se puede expandir buffer, terminando video");
                esp_camera_fb_return(fb);
                break;
            }
        }
        
        // Agregar boundary y frame
        memcpy(video_buffer + total_size, boundary, boundary_len);
        total_size += boundary_len;
        memcpy(video_buffer + total_size, fb->buf, fb->len);
        total_size += fb->len;
        frame_count++;
        
        esp_camera_fb_return(fb);
        
        // ~10 FPS para no saturar
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "Video capturado: %d frames, %zu bytes", frame_count, total_size);
    
    // Guardar encriptado
    if (total_size > 0) {
        esp_err_t ret = crypto_save_file(filename, video_buffer, total_size);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Video guardado: %s.enc", filename);
            save_photo_counter();
        } else {
            ESP_LOGE(TAG, "Error guardando video encriptado");
            photo_counter--;
        }
    } else {
        photo_counter--;
    }
    
    heap_caps_free(video_buffer);
}

// --- DEFINICIÓN DE PERIFÉRICOS DE LOGICA ---
// TODOS DESACTIVADOS - ESP32-CAM tiene GPIOs muy limitados
#define PIR_SENSOR_GPIO   GPIO_NUM_13  // No usado
#define IR_LEDS_GPIO      GPIO_NUM_4   // No usado
#define FLASH_LED_GPIO    GPIO_NUM_4   // No usado

// Configuración de GPIOs auxiliares
void peripheral_init(void) {
    // GPIO 4 LIBRE - No configuramos nada, dejamos el pin sin usar
    // El LED flash puede encenderse brevemente al insertar SD pero se apaga solo
    
    ESP_LOGI(TAG, "Perifericos: Todos desactivados (GPIO4 libre)");
}

void app_main(void)
{
    ESP_LOGI(TAG, "--- ARRANQUE DEL SISTEMA VIGILANTE ESP32 ---");

    // 1. INICIALIZAR NVS (Memoria No Volátil)
    // Vital para que el WiFi funcione y guarde calibraciones.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    heap_integrity_check("post nvs_flash_init");

    // 1.1 CARGAR CONTADOR DE FOTOS (sobrevive reinicios)
    load_photo_counter();
    heap_integrity_check("post load_photo_counter");

    // 2. INICIALIZAR PERIFÉRICOS (PIR y LEDs)
    peripheral_init();

    // Reactivar logs del driver de cámara
    esp_log_level_set("cam_hal", ESP_LOG_INFO);
    esp_log_level_set("camera", ESP_LOG_INFO);
    heap_integrity_check("pre camera_init_hardware");

    // 3. INICIALIZAR CÁMARA
    // Si falla la cámara, el sistema no sirve. Reiniciamos.
    if (camera_init_hardware() != ESP_OK) {
        ESP_LOGE(TAG, "Fallo critico de Camara. Reiniciando en 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    // 4. INICIALIZAR TARJETA SD (Modo 1-bit)
    // Si falla, seguimos igual (quizás solo queremos ver streaming)
    if (sd_card_init() != ESP_OK) {
        ESP_LOGW(TAG, "Sistema funcionando SIN almacenamiento local (SD Fallo o no presente).");
        sd_available = false;
    } else {
        sd_available = true;
        
        // 4.1 INICIALIZAR ENCRIPTACIÓN
        if (crypto_init() != ESP_OK) {
            ESP_LOGE(TAG, "Error inicializando crypto - fotos NO se encriptarán");
        } else {
            ESP_LOGI(TAG, "Encriptación AES-256 activa");
        }
    }

    // 5. INICIALIZAR RED (WiFi + AP Fallback)
    wifi_net_init();

    // Esperar a que el WiFi se establezca (STA o AP)
    ESP_LOGI(TAG, "Esperando conexion de red...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 6. INICIALIZAR SERVIDOR WEB
    // Arranca el servidor MJPEG para ver video por IP
    if (start_webserver() != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el servidor web.");
    } else {
        ESP_LOGI(TAG, "Servidor Web Listo. Esperando conexion de red...");
    }

    ESP_LOGI(TAG, "--- SISTEMA OPERATIVO Y VIGILANDO ---");

    // --- BUCLE PRINCIPAL (El "Sereno") ---
    // OPTIMIZACIÓN: Usar delay más largo ya que el streaming corre en su propia tarea
    // El bucle principal solo monitorea PIR y salud del sistema
    TickType_t last_health_check = xTaskGetTickCount();
    const TickType_t HEALTH_CHECK_INTERVAL = pdMS_TO_TICKS(5000);  // Cada 5 segundos
    
    while(1) {
        // ============================================================
        // SENSOR PIR DESACTIVADO
        // ============================================================
        #if 0  // PIR desactivado - sin GPIO disponible
        int movimiento = gpio_get_level(PIR_SENSOR_GPIO);

        if (movimiento) {
            ESP_LOGI(TAG, "¡MOVIMIENTO DETECTADO! (PIR ACTIVO)");
            gpio_set_level(IR_LEDS_GPIO, 1);
            http_server_notify_motion();
            
            capture_mode_t mode = http_server_get_capture_mode();
            if (mode == CAPTURE_MODE_VIDEO) {
                int duration = http_server_get_video_duration();
                capture_encrypted_video(duration);
            } else {
                capture_encrypted_photo();
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        } else {
            gpio_set_level(IR_LEDS_GPIO, 0);
        }
        #endif
        // ============================================================

        // Monitoreo de salud (Memoria RAM) - Solo cada 5 segundos
        if ((xTaskGetTickCount() - last_health_check) >= HEALTH_CHECK_INTERVAL) {
            ESP_LOGI(TAG, "[SALUD] Heap Libre: %lu bytes | PSRAM Libre: %lu bytes", 
                     esp_get_free_heap_size(), 
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            last_health_check = xTaskGetTickCount();
        }

        // OPTIMIZACIÓN: Delay más largo (500ms) - El PIR no necesita polling rápido
        // Esto libera CPU para el streaming HTTP
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
