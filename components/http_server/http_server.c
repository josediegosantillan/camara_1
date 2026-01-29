#include "http_server.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/socket.h>
#include <netinet/tcp.h>

static const char *TAG = "WEB_SERVER";

// Esta es la "etiqueta" que separa cada foto en el stream
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";

// --- MANEJADOR DEL STREAM DE VIDEO (OPTIMIZADO) ---
esp_err_t jpg_stream_httpd_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[128];  // Buffer más grande para headers
    
    // Métricas de FPS para debug
    int64_t last_frame = esp_timer_get_time();
    int frame_count = 0;

    // 1. Enviar la cabecera HTTP diciendo que esto es un stream MJPEG 
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }
    
    // OPTIMIZACIÓN: Deshabilitar Nagle's algorithm para envío inmediato
    int nodelay = 1;
    int sockfd = httpd_req_to_sockfd(req);
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ESP_LOGI(TAG, "Stream iniciado - Cliente conectado");

    // BUCLE INFINITO DE VIDEO
    while(true){
        // 2. Capturar Frame del DMA
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Fallo captura de camara");
            res = ESP_FAIL;
            break;
        }

        // OPTIMIZACIÓN: Combinar boundary + header en un solo envío
        size_t hlen = snprintf(part_buf, sizeof(part_buf), 
            "%s" "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            _STREAM_BOUNDARY, fb->len);
        
        // 3. Enviar header combinado
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }

        // 4. Enviar la foto (Buffer JPEG)
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        // 5. Devolver el frame al driver INMEDIATAMENTE
        esp_camera_fb_return(fb);
        fb = NULL;

        // Si algo falló en el envío (cliente desconectado), salimos
        if(res != ESP_OK){
            ESP_LOGW(TAG, "Cliente desconectado");
            break;
        }

        // Métricas de FPS cada 30 frames
        frame_count++;
        if(frame_count >= 30) {
            int64_t now = esp_timer_get_time();
            float fps = 30.0f * 1000000.0f / (now - last_frame);
            ESP_LOGI(TAG, "Streaming: %.1f FPS | Frame: %u bytes", fps, fb ? fb->len : 0);
            last_frame = now;
            frame_count = 0;
        }
        
        // YIELD mínimo para no bloquear watchdog (1 tick = 10ms default)
        taskYIELD();
    }

    return res;
}

// --- CONFIGURACIÓN DEL SERVIDOR (OPTIMIZADO PARA STREAMING) ---
esp_err_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    
    // OPTIMIZACIONES DE RENDIMIENTO:
    // 1. Aumentar stack para manejar buffers grandes de JPEG
    config.stack_size = 8192;
    
    // 2. Subir prioridad de la tarea HTTP para competir con WiFi
    config.task_priority = tskIDLE_PRIORITY + 5;
    
    // 3. Aumentar tamaño de buffer de envío para chunks más grandes
    config.send_wait_timeout = 5;  // Timeout más corto para detectar desconexiones rápido
    config.recv_wait_timeout = 5;
    
    // 4. Usar LRU purge para limpiar conexiones viejas automáticamente
    config.lru_purge_enable = true;
    
    // 5. Core affinity: Fijar HTTP al Core 1 (Core 0 = WiFi/Sistema)
    config.core_id = 1;

    httpd_handle_t stream_httpd = NULL;

    // Definimos la ruta URL (ej: http://192.168.1.50/stream)
    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = jpg_stream_httpd_handler,
        .user_ctx  = NULL
    };

    ESP_LOGI(TAG, "Arrancando servidor web en puerto: '%d'", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        // Registramos la URL
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error arrancando servidor web!");
    return ESP_FAIL;
}
