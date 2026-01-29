#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// --- IMPORTAMOS NUESTROS MÓDULOS ---
#include "cam_hal.h"
#include "sd_hal.h"
#include "wifi_net.h"
#include "http_server.h"

static const char *TAG = "MAIN_APP";

// --- DEFINICIÓN DE PERIFÉRICOS DE LOGICA ---
// Estos no son del driver de cámara, son de TU aplicación.
#define PIR_SENSOR_GPIO   GPIO_NUM_13
#define IR_LEDS_GPIO      GPIO_NUM_12 

// Configuración de GPIOs auxiliares
void peripheral_init(void) {
    // 1. Configurar PIR como Entrada con resistencia Pull-Down
    // (Por si el sensor queda flotando, que no dispare falsos)
    gpio_config_t pir_conf = {};
    pir_conf.intr_type = GPIO_INTR_DISABLE;
    pir_conf.mode = GPIO_MODE_INPUT;
    pir_conf.pin_bit_mask = (1ULL << PIR_SENSOR_GPIO);
    pir_conf.pull_down_en = 1;
    pir_conf.pull_up_en = 0;
    gpio_config(&pir_conf);

    // 2. Configurar LEDs IR como Salida
    // Arrancan apagados (0)
    gpio_reset_pin(IR_LEDS_GPIO);
    gpio_set_direction(IR_LEDS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(IR_LEDS_GPIO, 0);

    ESP_LOGI(TAG, "Perifericos (PIR/IR) configurados.");
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

    // 2. INICIALIZAR PERIFÉRICOS (PIR y LEDs)
    peripheral_init();

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
    }

    // 5. INICIALIZAR RED (WiFi + AP Fallback)
    wifi_net_init();

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
        // Leemos el sensor PIR
        int movimiento = gpio_get_level(PIR_SENSOR_GPIO);

        if (movimiento) {
            ESP_LOGI(TAG, "¡MOVIMIENTO DETECTADO! (PIR ACTIVO)");
            
            // Acá iría la lógica de encender IR y Grabar
            // Por ahora solo prendemos el LED IR para probar hardware
            gpio_set_level(IR_LEDS_GPIO, 1);
        } else {
            // Apagamos IR si no hay movimiento
            gpio_set_level(IR_LEDS_GPIO, 0);
        }

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
