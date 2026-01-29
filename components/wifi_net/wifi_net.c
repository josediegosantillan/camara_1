#include <string.h>
#include "wifi_net.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"

static const char *TAG = "WIFI_NET";

// --- CONFIGURACION DE RED ---
// Cambiá esto por los datos de tu casa
#define WIFI_SSID      "Personal-830"
#define WIFI_PASS      "EMFGqcga5U"
// Configuración del "Modo Isla" (AP Propio)
#define AP_SSID        "Casa_V"
#define AP_PASS        "seguridad123"
#define MAX_RETRY      5  // Intentos antes de rendirse y levantar AP

static int s_retry_num = 0;

// Manejador de Eventos (El "Cerebro" de la conexión)
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    // Caso 1: Arrancando el WiFi
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    // Caso 2: Se cortó o no conecta
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Reintentando conectar al WiFi... (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            // Se acabaron los intentos: PASAR A MODO AP (Generador)
            ESP_LOGE(TAG, "Fallo conexion WiFi. Levantando Access Point propio.");
            
            wifi_config_t ap_config = {
                .ap = {
                    .ssid = AP_SSID,
                    .ssid_len = strlen(AP_SSID),
                    .channel = 1,
                    .password = AP_PASS,
                    .max_connection = 4,
                    .authmode = WIFI_AUTH_WPA_WPA2_PSK
                },
            };
            if (strlen(AP_PASS) == 0) {
                ap_config.ap.authmode = WIFI_AUTH_OPEN;
            }
            
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
            ESP_LOGI(TAG, "MODO ISLA ACTIVADO. Conectate a: %s", AP_SSID);
        }
    } 
    // Caso 3: Conexión Exitosa
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0; // Reseteamos contador
    }
}

void wifi_net_init(void)
{
    // 1. Inicializar la pila TCP/IP (LwIP)
    ESP_ERROR_CHECK(esp_netif_init());
    
    // 2. Crear Loop de eventos
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 3. Crear las interfaces (Estación y AP)
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // 4. Configuración inicial del driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 5. Registrar el manejador de eventos (El electricista de guardia)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));

    // 6. Configurar credenciales de Casa (Station)
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            /* Authmode threshold resetea seguridad si es necesaria */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    // 7. Arrancar en modo Station (Cliente)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Sistema WiFi Iniciado. Intentando conectar a %s...", WIFI_SSID);
}
