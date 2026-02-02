#include <string.h>
#include "wifi_net.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_netif.h"

static const char *TAG = "WIFI_NET";

// --- NVS NAMESPACE PARA WIFI ---
#define NVS_NAMESPACE_WIFI "wifi_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "password"
#define NVS_KEY_MODE "wifi_mode"
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"

// --- CONFIGURACION POR DEFECTO ---
#define DEFAULT_WIFI_SSID      ""
#define DEFAULT_WIFI_PASS      ""

// Configuración del "Modo Isla" (AP Propio)
#define DEFAULT_AP_SSID        "CamaraVigia_AP"
#define DEFAULT_AP_PASS        "seguridad123"
#define MAX_RETRY      5  // Intentos antes de rendirse y levantar AP

static int s_retry_num = 0;
static bool s_is_connected = false;
static bool s_is_ap_mode = false;
static char s_current_ip[16] = "0.0.0.0";
static char s_current_ssid[33] = "";
static char s_current_pass[65] = "";
static char s_ap_ssid[33] = "";
static char s_ap_pass[65] = "";
static wifi_preferred_mode_t s_preferred_mode = WIFI_MODE_STATION;

// Cargar credenciales desde NVS
static void load_wifi_credentials(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        size_t ssid_len = sizeof(s_current_ssid);
        size_t pass_len = sizeof(s_current_pass);
        
        // Cargar credenciales STA
        err = nvs_get_str(nvs_handle, NVS_KEY_SSID, s_current_ssid, &ssid_len);
        if (err != ESP_OK || strlen(s_current_ssid) == 0) {
            strncpy(s_current_ssid, DEFAULT_WIFI_SSID, sizeof(s_current_ssid) - 1);
        }
        
        err = nvs_get_str(nvs_handle, NVS_KEY_PASS, s_current_pass, &pass_len);
        if (err != ESP_OK) {
            strncpy(s_current_pass, DEFAULT_WIFI_PASS, sizeof(s_current_pass) - 1);
        }
        
        // Cargar credenciales AP
        size_t ap_ssid_len = sizeof(s_ap_ssid);
        size_t ap_pass_len = sizeof(s_ap_pass);
        err = nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, s_ap_ssid, &ap_ssid_len);
        if (err != ESP_OK || strlen(s_ap_ssid) == 0) {
            strncpy(s_ap_ssid, DEFAULT_AP_SSID, sizeof(s_ap_ssid) - 1);
        }
        err = nvs_get_str(nvs_handle, NVS_KEY_AP_PASS, s_ap_pass, &ap_pass_len);
        if (err != ESP_OK) {
            strncpy(s_ap_pass, DEFAULT_AP_PASS, sizeof(s_ap_pass) - 1);
        }
        
        // Cargar modo preferido
        int32_t mode_val = WIFI_MODE_STATION;
        err = nvs_get_i32(nvs_handle, NVS_KEY_MODE, &mode_val);
        if (err == ESP_OK && (mode_val == WIFI_MODE_STATION || mode_val == WIFI_MODE_ACCESS_POINT)) {
            s_preferred_mode = (wifi_preferred_mode_t)mode_val;
        }
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Config cargada - Modo: %s, STA SSID: %s, AP SSID: %s", 
                 s_preferred_mode == WIFI_MODE_ACCESS_POINT ? "AP" : "STA",
                 s_current_ssid, s_ap_ssid);
    } else {
        // Usar valores por defecto
        strncpy(s_current_ssid, DEFAULT_WIFI_SSID, sizeof(s_current_ssid) - 1);
        strncpy(s_current_pass, DEFAULT_WIFI_PASS, sizeof(s_current_pass) - 1);
        strncpy(s_ap_ssid, DEFAULT_AP_SSID, sizeof(s_ap_ssid) - 1);
        strncpy(s_ap_pass, DEFAULT_AP_PASS, sizeof(s_ap_pass) - 1);
        ESP_LOGI(TAG, "Usando credenciales por defecto - SSID: %s", s_current_ssid);
    }
}

// Función auxiliar para iniciar modo AP
static void start_ap_mode(void) {
    // Detener WiFi actual antes de cambiar de modo
    esp_wifi_stop();
    
    wifi_config_t ap_config = {0};
    strncpy((char*)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    ap_config.ap.channel = 1;
    strncpy((char*)ap_config.ap.password, s_ap_pass, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = strlen(s_ap_pass) > 0 ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    s_is_ap_mode = true;
    s_is_connected = false;
    snprintf(s_current_ip, sizeof(s_current_ip), "192.168.4.1");
    ESP_LOGI(TAG, "✅ MODO AP ACTIVADO. Red: %s | Contraseña: %s | IP: 192.168.4.1", 
             s_ap_ssid, strlen(s_ap_pass) > 0 ? s_ap_pass : "(abierta)");
}

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
        s_is_connected = false;
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Reintentando conectar al WiFi... (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            // Se acabaron los intentos: PASAR A MODO AP (Generador)
            ESP_LOGE(TAG, "Fallo conexion WiFi. Levantando Access Point propio.");
            start_ap_mode();
        }
    } 
    // Caso 3: Conexión Exitosa
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_is_connected = true;
        s_is_ap_mode = false;
        ESP_LOGI(TAG, "Conectado! IP: %s", s_current_ip);
        s_retry_num = 0; // Reseteamos contador
        
        // Nota: Para liberar memoria de Bluetooth (~150KB), descomenta esta línea
        // en sdkconfig.defaults: CONFIG_BLUEDROID_ENABLED=n
        // O llama a esp_bt_mem_release(ESP_BT_MODE_BTDM) en app_main() después de WiFi
        ESP_LOGI(TAG, "Tip: Desactiva Bluetooth en menuconfig para liberar ~150KB");
    }
}

void wifi_net_init(void)
{
    // 0. Cargar credenciales desde NVS
    load_wifi_credentials();
    
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

    // 6. Siempre intentar conectar a WiFi primero (si hay credenciales)
    if (strlen(s_current_ssid) > 0) {
        ESP_LOGI(TAG, "Intentando conectar a WiFi: %s...", s_current_ssid);
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, s_current_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char*)wifi_config.sta.password, s_current_pass, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        // No hay credenciales WiFi, iniciar en modo AP
        ESP_LOGI(TAG, "No hay WiFi configurado, iniciando en modo AP...");
        start_ap_mode();
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

// Intentar conectar a WiFi (llamado manualmente desde la web)
esp_err_t wifi_net_try_connect(void) {
    if (strlen(s_current_ssid) == 0) {
        ESP_LOGW(TAG, "No hay SSID configurado para conectar");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Intentando conectar a WiFi: %s...", s_current_ssid);
    
    // Detener WiFi actual
    esp_wifi_stop();
    
    // Resetear contador de reintentos
    s_retry_num = 0;
    s_is_connected = false;
    s_is_ap_mode = false;
    
    // Configurar modo STA
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, s_current_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, s_current_pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    return ESP_OK;
}

// Cambiar a modo AP inmediatamente (sin reiniciar)
esp_err_t wifi_net_switch_to_ap(void) {
    if (strlen(s_ap_ssid) == 0) {
        ESP_LOGW(TAG, "No hay SSID del AP configurado");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Cambiando a modo AP: %s", s_ap_ssid);
    
    // Detener WiFi actual
    esp_wifi_stop();
    
    // Resetear estado de conexión
    s_is_connected = false;
    s_is_ap_mode = true;
    s_retry_num = 0;
    
    // Configurar y arrancar modo AP
    wifi_config_t ap_config = {0};
    strncpy((char*)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    strncpy((char*)ap_config.ap.password, s_ap_pass, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.authmode = strlen(s_ap_pass) > 0 ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    snprintf(s_current_ip, sizeof(s_current_ip), "192.168.4.1");
    ESP_LOGI(TAG, "✅ MODO AP ACTIVADO. Red: %s | IP: 192.168.4.1", s_ap_ssid);
    
    return ESP_OK;
}

// ============================================================================
// FUNCIONES PÚBLICAS PARA CONFIGURACIÓN WIFI
// ============================================================================

esp_err_t wifi_net_set_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_PASS, password ? password : "");
    }
    
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Credenciales WiFi guardadas - SSID: %s (se aplicarán tras reinicio)", ssid);
    }
    
    return err;
}

void wifi_net_get_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len) {
    if (ssid && ssid_len > 0) {
        strncpy(ssid, s_current_ssid, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }
    if (password && pass_len > 0) {
        strncpy(password, s_current_pass, pass_len - 1);
        password[pass_len - 1] = '\0';
    }
}

bool wifi_net_is_connected(void) {
    return s_is_connected;
}

void wifi_net_get_ip(char *ip_str, size_t len) {
    if (ip_str && len > 0) {
        strncpy(ip_str, s_current_ip, len - 1);
        ip_str[len - 1] = '\0';
    }
}

bool wifi_net_is_ap_mode(void) {
    return s_is_ap_mode;
}

void wifi_net_get_ap_ssid(char *ssid, size_t len) {
    if (ssid && len > 0) {
        strncpy(ssid, s_ap_ssid, len - 1);
        ssid[len - 1] = '\0';
    }
}

esp_err_t wifi_net_set_preferred_mode(wifi_preferred_mode_t mode) {
    if (mode != WIFI_MODE_STATION && mode != WIFI_MODE_ACCESS_POINT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_set_i32(nvs_handle, NVS_KEY_MODE, (int32_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        s_preferred_mode = mode;
        ESP_LOGI(TAG, "Modo WiFi guardado: %s (se aplicará tras reinicio)", 
                 mode == WIFI_MODE_ACCESS_POINT ? "AP (Red propia)" : "STA (Red externa)");
    }
    
    return err;
}

wifi_preferred_mode_t wifi_net_get_preferred_mode(void) {
    return s_preferred_mode;
}

esp_err_t wifi_net_set_ap_credentials(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_AP_PASS, password ? password : "");
    }
    
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
        strncpy(s_ap_pass, password ? password : "", sizeof(s_ap_pass) - 1);
        ESP_LOGI(TAG, "Credenciales AP guardadas - SSID: %s", ssid);
    }
    
    return err;
}

void wifi_net_get_ap_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len) {
    if (ssid && ssid_len > 0) {
        strncpy(ssid, s_ap_ssid, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }
    if (password && pass_len > 0) {
        strncpy(password, s_ap_pass, pass_len - 1);
        password[pass_len - 1] = '\0';
    }
}
