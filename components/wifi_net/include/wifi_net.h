#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Modos de WiFi
typedef enum {
    WIFI_MODE_STATION = 0,  // Conectarse a red externa
    WIFI_MODE_ACCESS_POINT = 1  // Crear red propia (AP)
} wifi_preferred_mode_t;

// Inicializar WiFi
void wifi_net_init(void);

// Configurar nuevas credenciales WiFi (se aplican tras reinicio)
esp_err_t wifi_net_set_credentials(const char *ssid, const char *password);

// Obtener credenciales actuales
void wifi_net_get_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);

// Obtener estado de conexión
bool wifi_net_is_connected(void);

// Obtener IP actual
void wifi_net_get_ip(char *ip_str, size_t len);

// Obtener modo actual (STA o AP)
bool wifi_net_is_ap_mode(void);

// Obtener SSID del AP
void wifi_net_get_ap_ssid(char *ssid, size_t len);

// Configurar modo preferido (se aplica tras reinicio)
esp_err_t wifi_net_set_preferred_mode(wifi_preferred_mode_t mode);

// Obtener modo preferido guardado
wifi_preferred_mode_t wifi_net_get_preferred_mode(void);

// Configurar credenciales del AP propio
esp_err_t wifi_net_set_ap_credentials(const char *ssid, const char *password);

// Obtener credenciales del AP propio
void wifi_net_get_ap_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);

// Intentar conectar a WiFi (inicia intento de conexión manualmente)
esp_err_t wifi_net_try_connect(void);
// Cambiar a modo AP inmediatamente (sin reiniciar)
esp_err_t wifi_net_switch_to_ap(void);