#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

// Iniciar servidor web
esp_err_t start_webserver(void);

// Detener servidor web
esp_err_t stop_webserver(void);

// ============================================================================
// CONTROL DE DETECCIÓN DE MOVIMIENTO
// ============================================================================

// Notificar al servidor que se detectó movimiento (llamar desde main.c)
void http_server_notify_motion(void);

// Verificar si el streaming está activo (por movimiento o forzado)
bool http_server_is_streaming_active(void);

// Obtener tiempo restante de streaming (en segundos)
int http_server_get_remaining_time(void);

// Obtener tiempo de emisión configurado (en segundos)
int http_server_get_emission_time(void);

// ============================================================================
// MODO DE CAPTURA (FOTO vs VIDEO)
// ============================================================================
typedef enum {
    CAPTURE_MODE_PHOTO = 0,   // Capturar fotos individuales
    CAPTURE_MODE_VIDEO = 1    // Grabar video (secuencia de frames)
} capture_mode_t;

// Obtener modo de captura actual
capture_mode_t http_server_get_capture_mode(void);

// Obtener duración de video configurada (en segundos)
int http_server_get_video_duration(void);
