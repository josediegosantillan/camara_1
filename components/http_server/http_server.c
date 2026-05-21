#include "http_server.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "crypto.h"
#include "cam_hal.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi_net.h"
#include "sd_hal.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t server_httpd = NULL;
static uint32_t s_manual_capture_fallback_counter = 0;
static char s_manual_capture_stamp[20] = {0};
static volatile uint32_t s_manual_capture_result_id = 0;
static volatile bool s_manual_capture_last_ok = false;
static char s_manual_capture_last_file[64] = {0};
static char s_manual_capture_last_msg[96] = {0};

// ============================================================================
// CONFIGURACIÓN
// ============================================================================
#define MOUNT_POINT "/sdcard"
#define MAX_FILES 50

// NVS para configuración de movimiento
#define NVS_NAMESPACE_MOTION "motion_cfg"
#define NVS_KEY_EMISSION_TIME "emit_time"
#define NVS_KEY_LIVE_TIME "live_time"
#define NVS_KEY_CAPTURE_MODE "cap_mode"
#define NVS_KEY_VIDEO_DURATION "vid_dur"
#define NVS_NAMESPACE_DEVICE "device_cfg"
#define NVS_KEY_IMAGE_QUALITY "img_quality"
#define NVS_KEY_FRAME_SIZE "frame_size"
#define DEFAULT_EMISSION_TIME 30  // Segundos por defecto
#define DEFAULT_LIVE_TIME 60      // Tiempo de vista en vivo por defecto
#define DEFAULT_VIDEO_DURATION 10 // Duración de video por defecto
#define DEFAULT_IMAGE_QUALITY CAM_HAL_DEFAULT_JPEG_QUALITY
#define DEFAULT_FRAME_SIZE CAM_HAL_DEFAULT_FRAME_SIZE

// ============================================================================
// ESTADO DE DETECCIÓN DE MOVIMIENTO
// ============================================================================
static volatile bool g_motion_detected = false;       // Hay movimiento activo
static volatile int64_t g_motion_end_time = 0;        // Timestamp cuando termina emisión
static volatile int g_emission_time_sec = DEFAULT_EMISSION_TIME; // Tiempo de emisión configurable
static volatile bool g_force_stream = false;          // Forzar stream desde web (vista en vivo manual)
static volatile int64_t g_force_end_time = 0;         // Timestamp cuando termina vista en vivo
static volatile int g_live_time_sec = DEFAULT_LIVE_TIME; // Tiempo de vista en vivo configurable
static volatile bool g_manual_capture_requested = false;
static volatile bool g_capture_in_progress = false;

// Modo de captura (foto vs video)
static volatile capture_mode_t g_capture_mode = CAPTURE_MODE_PHOTO;
static volatile int g_video_duration_sec = DEFAULT_VIDEO_DURATION;
static volatile int g_image_quality = DEFAULT_IMAGE_QUALITY;
static volatile framesize_t g_frame_size = DEFAULT_FRAME_SIZE;

// ============================================================================
// FUNCIONES DE CONFIGURACIÓN NVS
// ============================================================================
static void load_motion_config(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_MOTION, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        int32_t time_val = DEFAULT_EMISSION_TIME;
        err = nvs_get_i32(nvs_handle, NVS_KEY_EMISSION_TIME, &time_val);
        if (err == ESP_OK && time_val >= 5 && time_val <= 300) {
            g_emission_time_sec = time_val;
            ESP_LOGI(TAG, "Tiempo de emisión cargado: %d segundos", g_emission_time_sec);
        }
        
        int32_t live_val = DEFAULT_LIVE_TIME;
        err = nvs_get_i32(nvs_handle, NVS_KEY_LIVE_TIME, &live_val);
        if (err == ESP_OK && live_val >= 10 && live_val <= 600) {
            g_live_time_sec = live_val;
            ESP_LOGI(TAG, "Tiempo de vista en vivo cargado: %d segundos", g_live_time_sec);
        }
        
        // Cargar modo de captura
        int32_t mode_val = CAPTURE_MODE_PHOTO;
        err = nvs_get_i32(nvs_handle, NVS_KEY_CAPTURE_MODE, &mode_val);
        if (err == ESP_OK && (mode_val == CAPTURE_MODE_PHOTO || mode_val == CAPTURE_MODE_VIDEO)) {
            g_capture_mode = (capture_mode_t)mode_val;
            ESP_LOGI(TAG, "Modo de captura: %s", g_capture_mode == CAPTURE_MODE_VIDEO ? "VIDEO" : "FOTO");
        }
        
        // Cargar duración de video
        int32_t vid_dur = DEFAULT_VIDEO_DURATION;
        err = nvs_get_i32(nvs_handle, NVS_KEY_VIDEO_DURATION, &vid_dur);
        if (err == ESP_OK && vid_dur >= 5 && vid_dur <= 60) {
            g_video_duration_sec = vid_dur;
            ESP_LOGI(TAG, "Duración de video: %d segundos", g_video_duration_sec);
        }
        
        nvs_close(nvs_handle);
    }
}

static void save_motion_config(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_MOTION, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_KEY_EMISSION_TIME, g_emission_time_sec);
        nvs_set_i32(nvs_handle, NVS_KEY_LIVE_TIME, g_live_time_sec);
        nvs_set_i32(nvs_handle, NVS_KEY_CAPTURE_MODE, (int32_t)g_capture_mode);
        nvs_set_i32(nvs_handle, NVS_KEY_VIDEO_DURATION, g_video_duration_sec);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Config guardada - Modo: %s, Emisión: %ds, Video: %ds", 
                 g_capture_mode == CAPTURE_MODE_VIDEO ? "VIDEO" : "FOTO",
                 g_emission_time_sec, g_video_duration_sec);
    }
}

static void load_device_config(void) {
    g_image_quality = DEFAULT_IMAGE_QUALITY;
    g_frame_size = DEFAULT_FRAME_SIZE;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_DEVICE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        int32_t quality_val = DEFAULT_IMAGE_QUALITY;
        err = nvs_get_i32(nvs_handle, NVS_KEY_IMAGE_QUALITY, &quality_val);
        if (err == ESP_OK && quality_val >= 10 && quality_val <= 63) {
            g_image_quality = quality_val;
        }

        int32_t frame_val = DEFAULT_FRAME_SIZE;
        err = nvs_get_i32(nvs_handle, NVS_KEY_FRAME_SIZE, &frame_val);
        if (err == ESP_OK) {
            g_frame_size = (framesize_t)frame_val;
        }
        nvs_close(nvs_handle);
    }

    err = cam_hal_set_jpeg_quality(g_image_quality);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo aplicar calidad JPEG guardada (%d): %s",
                 g_image_quality, esp_err_to_name(err));
        g_image_quality = cam_hal_get_jpeg_quality();
    } else {
        ESP_LOGI(TAG, "Calidad JPEG cargada: %d", g_image_quality);
    }

    err = cam_hal_set_frame_size(g_frame_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo aplicar frame size guardado (%d): %s",
                 (int)g_frame_size, esp_err_to_name(err));
        g_frame_size = cam_hal_get_frame_size();
    } else {
        ESP_LOGI(TAG, "Frame size cargado: %s", cam_hal_get_frame_size_name(g_frame_size));
    }
}

static void save_device_config(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_DEVICE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_KEY_IMAGE_QUALITY, g_image_quality);
        nvs_set_i32(nvs_handle, NVS_KEY_FRAME_SIZE, (int32_t)g_frame_size);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Config de dispositivo guardada - Calidad JPEG: %d, Resolucion: %s",
                 g_image_quality, cam_hal_get_frame_size_name(g_frame_size));
    }
}

// Funciones públicas para modo de captura
capture_mode_t http_server_get_capture_mode(void) {
    return g_capture_mode;
}

int http_server_get_video_duration(void) {
    return g_video_duration_sec;
}

// ============================================================================
// FUNCIONES PÚBLICAS DE CONTROL DE MOVIMIENTO
// ============================================================================
void http_server_notify_motion(void) {
    g_motion_detected = true;
    g_motion_end_time = esp_timer_get_time() + ((int64_t)g_emission_time_sec * 1000000);
    ESP_LOGI(TAG, "Movimiento detectado - streaming activo por %d segundos", g_emission_time_sec);
}

bool http_server_is_streaming_active(void) {
    // Actualizar estado basado en tiempo para movimiento
    if (g_motion_detected && esp_timer_get_time() >= g_motion_end_time) {
        g_motion_detected = false;
        ESP_LOGI(TAG, "Tiempo de emisión terminado");
    }
    // Actualizar estado basado en tiempo para vista en vivo
    if (g_force_stream && esp_timer_get_time() >= g_force_end_time) {
        g_force_stream = false;
        ESP_LOGI(TAG, "Tiempo de vista en vivo terminado");
    }
    return g_motion_detected || g_force_stream;
}

int http_server_get_remaining_time(void) {
    if (!g_motion_detected && !g_force_stream) return 0;
    
    // Si está en modo forzado, devolver tiempo restante de live
    if (g_force_stream) {
        int64_t remaining = (g_force_end_time - esp_timer_get_time()) / 1000000;
        return remaining > 0 ? (int)remaining : 0;
    }
    
    // Tiempo restante de movimiento
    int64_t remaining = (g_motion_end_time - esp_timer_get_time()) / 1000000;
    return remaining > 0 ? (int)remaining : 0;
}

int http_server_get_emission_time(void) {
    return g_emission_time_sec;
}

bool http_server_take_manual_capture_request(void) {
    if (!g_manual_capture_requested) {
        return false;
    }

    g_manual_capture_requested = false;
    return true;
}

void http_server_set_capture_in_progress(bool in_progress) {
    g_capture_in_progress = in_progress;
}

bool http_server_is_capture_in_progress(void) {
    return g_capture_in_progress;
}

static void set_manual_capture_result(bool ok, const char *message, const char *filename) {
    s_manual_capture_last_ok = ok;

    if (message) {
        strncpy(s_manual_capture_last_msg, message, sizeof(s_manual_capture_last_msg) - 1);
        s_manual_capture_last_msg[sizeof(s_manual_capture_last_msg) - 1] = '\0';
    } else {
        s_manual_capture_last_msg[0] = '\0';
    }

    if (filename) {
        strncpy(s_manual_capture_last_file, filename, sizeof(s_manual_capture_last_file) - 1);
        s_manual_capture_last_file[sizeof(s_manual_capture_last_file) - 1] = '\0';
    } else {
        s_manual_capture_last_file[0] = '\0';
    }

    s_manual_capture_result_id++;
}

static bool is_valid_capture_stamp(const char *stamp) {
    if (!stamp || strlen(stamp) != 14 ||
        stamp[2] != '_' || stamp[5] != '_' || stamp[8] != '_' || stamp[11] != '_') {
        return false;
    }

    for (int i = 0; i < 14; i++) {
        if (i == 2 || i == 5 || i == 8 || i == 11) {
            continue;
        }
        if (stamp[i] < '0' || stamp[i] > '9') {
            return false;
        }
    }

    return true;
}

static void build_manual_capture_filename(char *filename, size_t filename_len, const char *preferred_stamp) {
    if (is_valid_capture_stamp(preferred_stamp)) {
        snprintf(filename, filename_len, "IMG_%s.jpg", preferred_stamp);
        return;
    }

    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year >= (2024 - 1900)) {
        strftime(filename, filename_len, "IMG_%d_%m_%y_%H_%M.jpg", &timeinfo);
        return;
    }

    snprintf(filename, filename_len, "IMG_NOCLK_%04lu.jpg",
             (unsigned long)(s_manual_capture_fallback_counter++ % 10000));
}

static esp_err_t save_manual_capture_frame(const uint8_t *data, size_t len) {
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "Captura manual ignorada: SD no montada");
        set_manual_capture_result(false, "microSD no montada", NULL);
        return ESP_ERR_INVALID_STATE;
    }

    char filename[64];
    char filepath[128];
    char preferred_stamp[sizeof(s_manual_capture_stamp)] = {0};

    if (s_manual_capture_stamp[0] != '\0') {
        strncpy(preferred_stamp, s_manual_capture_stamp, sizeof(preferred_stamp) - 1);
        s_manual_capture_stamp[0] = '\0';
    }

    build_manual_capture_filename(filename, sizeof(filename), preferred_stamp);

    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);

    for (int suffix = 1; suffix <= 99; suffix++) {
        struct stat st;
        if (stat(filepath, &st) != 0) {
            break;
        }

        time_t now = 0;
        struct tm timeinfo = {0};
        if (is_valid_capture_stamp(preferred_stamp)) {
            snprintf(filename, sizeof(filename), "IMG_%s_%02d.jpg", preferred_stamp, suffix);
        } else {
            time(&now);
            localtime_r(&now, &timeinfo);
            if (timeinfo.tm_year >= (2024 - 1900)) {
                strftime(filename, sizeof(filename), "IMG_%d_%m_%y_%H_%M", &timeinfo);
                snprintf(filename + strlen(filename), sizeof(filename) - strlen(filename), "_%02d.jpg", suffix);
            } else {
                snprintf(filename, sizeof(filename), "IMG_NOCLK_%04lu_%02d.jpg",
                         (unsigned long)(s_manual_capture_fallback_counter % 10000), suffix);
            }
        }
        snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);
    }

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo crear captura manual %s (errno=%d)", filepath, errno);
        set_manual_capture_result(false, "no se pudo crear el archivo en la SD", NULL);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Escritura incompleta de captura manual: %u/%u bytes",
                 (unsigned)written, (unsigned)len);
        set_manual_capture_result(false, "escritura incompleta en la SD", filename);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Captura manual guardada: %s (%u bytes)", filename, (unsigned)len);
    set_manual_capture_result(true, "guardado", filename);
    return ESP_OK;
}

// Boundary para MJPEG
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";

// ============================================================================
// ESTRUCTURA PARA LISTA DE ARCHIVOS
// ============================================================================
typedef struct {
    char name[64];
    size_t size;
    time_t mtime;
} file_info_t;

// ============================================================================
// PÁGINA HTML PRINCIPAL (Con control de movimiento, WiFi y modo captura)
// ============================================================================
static const char* HTML_INDEX =
"<!DOCTYPE html><html lang='es'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Camara Vigia</title><style>"
":root{--bg:#0d1117;--sf:#161b22;--sf2:#1c2128;--br:#30363d;--ac:#58a6ff;--gr:#3fb950;--rd:#f85149;--yl:#d29922;--tx:#e6edf3;--mt:#8b949e;--sw:640px;--sh:360px}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:var(--bg);color:var(--tx);padding:12px}"
"h1{color:var(--ac);font-size:1.3em;margin-bottom:12px}"
".tabs{display:flex;gap:2px;border-bottom:2px solid var(--br);overflow-x:auto;margin-bottom:0}"
".tab{padding:10px 14px;cursor:pointer;color:var(--mt);font-size:.88em;white-space:nowrap;border-bottom:2px solid transparent;margin-bottom:-2px;transition:color .2s,border-color .2s}"
".tab:hover{color:var(--tx)}.tab.active{color:var(--ac);border-bottom-color:var(--ac)}"
".panel{display:none;padding:16px;background:var(--sf);border:1px solid var(--br);border-top:none;border-radius:0 0 6px 6px;animation:fadeIn .15s}"
".panel.active{display:block}"
".btn{background:var(--sf2);border:1px solid var(--br);color:var(--tx);padding:7px 14px;margin:3px 2px;cursor:pointer;border-radius:6px;font-size:.88em;transition:all .15s;display:inline-flex;align-items:center;gap:5px;text-decoration:none}"
".btn:hover{background:#21262d;border-color:#8b949e}"
".btn-success{background:#1a4731;border-color:var(--gr);color:#3fb950}.btn-success:hover{background:#1f5c3c}"
".btn-danger{background:#3d1515;border-color:var(--rd);color:#f85149}.btn-danger:hover{background:#5c1a1a}"
".btn-primary{background:#1c3353;border-color:var(--ac);color:var(--ac)}.btn-primary:hover{background:#1f3a5f}"
".btn-group{display:flex;flex-wrap:wrap;gap:4px;margin:10px 0}"
".status{padding:6px 12px;border-radius:20px;margin:6px 0;font-size:.85em;font-weight:600;display:inline-flex;align-items:center;gap:6px}"
".status-on{background:#1a4731;color:#3fb950;border:1px solid #1f5c3c}"
".status-off{background:#3d1515;color:#f85149;border:1px solid #5c1a1a}"
".status-warn{background:#3d2f00;color:#d29922;border:1px solid #5c4400}"
".card{background:var(--sf2);padding:14px 16px;border-radius:8px;margin:10px 0;border:1px solid var(--br);box-shadow:0 1px 3px rgba(0,0,0,.3)}"
".card-title{font-size:.95em;font-weight:600;margin-bottom:10px}"
"#stream{width:100%;max-width:var(--sw);border-radius:6px;border:1px solid var(--br);display:block;margin:10px 0}"
"#stream-placeholder{width:100%;max-width:var(--sw);height:var(--sh);border-radius:6px;border:1px solid var(--br);margin:10px 0;display:flex;align-items:center;justify-content:center;background:#0a0e16;flex-direction:column;gap:8px;color:var(--mt)}"
"#stream-timer{font-size:.85em;color:var(--mt);margin:2px 0 6px}"
"#stream-timer b{color:var(--ac)}"
".progress-bar{width:100%;max-width:var(--sw);height:4px;background:var(--br);border-radius:2px;margin:0 0 10px;overflow:hidden}"
".progress-fill{height:100%;background:var(--ac);border-radius:2px;transition:width 1s linear}"
".config-row{display:flex;align-items:center;gap:10px;margin:10px 0;flex-wrap:wrap}"
".config-row label{min-width:140px;color:var(--mt);font-size:.9em}"
".config-row input,.config-row select{padding:7px 10px;border-radius:6px;border:1px solid var(--br);background:var(--sf);color:var(--tx);font-size:.9em;outline:none;transition:border-color .2s}"
".config-row input:focus,.config-row select:focus{border-color:var(--ac)}"
".config-row input[type=number]{width:88px}.config-row select{min-width:180px}"
".hint{color:var(--mt);font-size:.8em;margin-top:4px}"
".radio-group{display:flex;gap:16px;margin:8px 0;flex-wrap:wrap}"
".radio-group label{display:flex;align-items:center;gap:5px;cursor:pointer;font-size:.9em}"
".file{background:var(--sf2);padding:10px 12px;margin:6px 0;border-radius:6px;border:1px solid var(--br);display:flex;align-items:center;flex-wrap:wrap;gap:6px;transition:border-color .15s}"
".file:hover{border-color:#4a5568}"
".file-name{flex:1;min-width:140px;word-break:break-all;cursor:pointer;font-size:.9em}"
".file-name:hover{color:var(--ac)}"
".file-info{color:var(--mt);font-size:.78em}.file-actions{display:flex;gap:4px;flex-shrink:0}"
".capture-status{display:none;max-width:var(--sw);margin:10px 0;padding:12px 14px;border-radius:6px;font-size:.9em;font-weight:600}"
".capture-status.show{display:block}"
".capture-status.saving{border:1px solid #d29922;background:#3d2f00;color:#f0c040}"
".capture-status.ok{border:1px solid var(--gr);background:#1a4731;color:#56d364}"
".capture-status.error{border:1px solid var(--rd);background:#3d1515;color:#ff8080}"
".pass-container{position:relative;display:inline-flex;align-items:center}"
".pass-toggle{position:absolute;right:8px;background:none;border:none;color:var(--mt);cursor:pointer;font-size:1em;padding:0}"
".pass-toggle:hover{color:var(--tx)}"
"#viewer-modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.96);z-index:1000;align-items:center;justify-content:center;flex-direction:column}"
"#viewer-modal.show{display:flex}"
"#viewer-close{position:absolute;top:16px;right:24px;font-size:1.5em;color:#fff;cursor:pointer;z-index:1001;background:rgba(255,255,255,.1);border-radius:50%;width:40px;height:40px;display:flex;align-items:center;justify-content:center}"
"#viewer-close:hover{background:rgba(255,255,255,.2)}"
"#viewer-img{max-width:90vw;max-height:78vh;border-radius:4px;border:1px solid var(--br)}"
"#viewer-title{color:#ccc;font-size:.95em;margin-bottom:10px;max-width:90vw;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
"#viewer-nav{display:flex;gap:12px;margin-top:14px}"
"hr{border:none;border-top:1px solid var(--br);margin:14px 0}"
"@keyframes fadeIn{from{opacity:0;transform:translateY(-4px)}to{opacity:1;transform:none}}"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}"
"</style></head><body>"
"<h1>🎥 Camara Vigia</h1>"
"<div class='tabs'>"
"<span class='tab active' onclick='showTab(0)'>📹 Stream</span>"
"<span class='tab' onclick='showTab(1)'>⚙️ Captura</span>"
"<span class='tab' onclick='showTab(2)'>🖥 Config</span>"
"<span class='tab' onclick='showTab(3)'>📶 WiFi</span>"
"<span class='tab' onclick='showTab(4)'>📁 Archivos</span>"
"</div>"
"<div class='panel active' id='p0'>"
"<div style='display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:8px'>"
"<div class='status status-off' id='stream-status'>Verificando...</div>"
"</div>"
"<div id='stream-timer' style='display:none'>Tiempo restante: <b id='timer-val'>--</b>s</div>"
"<div id='progress-container' style='display:none'><div class='progress-bar'><div class='progress-fill' id='progress-fill'></div></div></div>"
"<div id='stream-container'></div>"
"<div class='btn-group'>"
"<button class='btn btn-success' id='btn-live' onclick='forceStream()'>🔴 Ver en Vivo</button>"
"<button class='btn btn-primary' id='btn-capture' onclick='captureNow()' disabled>📸 Capturar</button>"
"<button class='btn' onclick='stopForce()'>⏹ Detener</button>"
"<button class='btn' onclick='checkStatus()'>🔄 Actualizar</button>"
"</div>"
"<div id='capture-status' class='capture-status'></div>"
"</div>"
"<div class='panel' id='p1'>"
"<div class='card'>"
"<div class='card-title'>📷 Modo de Captura</div>"
"<div class='radio-group'>"
"<label><input type='radio' name='cap-mode' value='0' checked> 📸 Foto</label>"
"<label><input type='radio' name='cap-mode' value='1'> 🎬 Video</label>"
"</div>"
"<div id='video-opts' style='display:none'>"
"<div class='config-row'><label>Duracion video:</label><input type='number' id='vid-dur' min='5' max='60' value='10'><span class='hint'>seg (5-60)</span></div>"
"</div>"
"</div>"
"<div class='card'>"
"<div class='card-title'>⏱ Tiempo tras Movimiento</div>"
"<div class='config-row'><label>Segundos:</label><input type='number' id='emit-time' min='5' max='300' value='30'></div>"
"<p class='hint'>Duracion del stream al detectar movimiento PIR. (5-300s)</p>"
"</div>"
"<div class='card'>"
"<div class='card-title'>🔴 Tiempo Vista en Vivo</div>"
"<div class='config-row'><label>Segundos:</label><input type='number' id='live-time' min='10' max='600' value='60'></div>"
"<p class='hint'>Duracion maxima del stream manual. (10-600s)</p>"
"</div>"
"<div style='text-align:center;margin:14px 0'><button class='btn btn-success' onclick='saveConfig()'>💾 Guardar Configuracion</button></div>"
"<div class='card'><div class='card-title'>📊 Estado Actual</div><div id='config-status'>Cargando...</div></div>"
"</div>"
"<div class='panel' id='p2'>"
"<div class='card'>"
"<div class='card-title'>🖼 Calidad de Imagen</div>"
"<div class='config-row'><label>Perfil JPEG:</label>"
"<select id='img-quality'>"
"<option value='10'>Muy alta</option><option value='12'>Alta</option>"
"<option value='18'>Media</option><option value='24'>Baja</option>"
"<option value='30'>Muy baja</option>"
"</select></div>"
"<div class='config-row'><label>Resolucion:</label>"
"<select id='frame-size'>"
"<option value='5'>QVGA (320x240)</option><option value='9'>CIF (400x296)</option>"
"<option value='7'>HVGA (480x320)</option><option value='10'>VGA (640x480)</option>"
"<option value='11'>SVGA (800x600)</option><option value='12'>XGA (1024x768)</option>"
"<option value='13'>HD (1280x720)</option>"
"</select></div>"
"<p class='hint'>Menor valor JPEG = mejor calidad. Mayor resolucion = mas detalle.</p>"
"</div>"
"<div class='card'>"
"<div class='card-title'>📐 Tamano de Vista en Vivo</div>"
"<div class='config-row'><label>Tamano:</label>"
"<select id='live-view-size' onchange='previewLiveViewSize()'>"
"<option value='sm'>Compacta (360px)</option><option value='md'>Mediana (480px)</option>"
"<option value='lg'>Grande (640px)</option><option value='xl'>Extra grande (800px)</option>"
"<option value='full'>Ancho completo</option>"
"</select></div>"
"</div>"
"<div class='card'>"
"<div class='card-title'>💡 LED Incorporado</div>"
"<div class='status status-off' id='led-status'>Cargando...</div>"
"<div class='btn-group'>"
"<button class='btn btn-success' onclick='setLed(true)'>💡 Encender LED</button>"
"<button class='btn btn-danger' onclick='setLed(false)'>Apagar LED</button>"
"</div>"
"</div>"
"<div style='text-align:center;margin:14px 0'><button class='btn btn-success' onclick='saveDeviceConfig()'>💾 Guardar Configuracion</button></div>"
"<div class='card'><div class='card-title'>📊 Estado Actual</div><div id='device-status'>Cargando...</div></div>"
"</div>"
"<div class='panel' id='p3'>"
"<div class='status status-off' id='wifi-status'>Cargando...</div>"
"<div class='card'>"
"<div class='card-title'>📡 Modo de Red</div>"
"<div class='radio-group'>"
"<label><input type='radio' name='wifi-mode' value='0'> 📶 WiFi (conectar a red)</label>"
"<label><input type='radio' name='wifi-mode' value='1'> 📡 AP (crear red propia)</label>"
"</div>"
"</div>"
"<div id='sta-config' class='card'>"
"<div class='card-title'>📶 Configurar WiFi (Estacion)</div>"
"<div class='config-row'><label>SSID:</label><input type='text' id='wifi-ssid' maxlength='32' placeholder='Nombre de red WiFi'></div>"
"<div class='config-row'><label>Contrasena:</label><div class='pass-container'><input type='password' id='wifi-pass' maxlength='64' placeholder='Contrasena WiFi' style='padding-right:36px'><button type='button' class='pass-toggle' onclick='togglePass(\"wifi-pass\")'>👁</button></div></div>"
"</div>"
"<div id='ap-config' class='card' style='display:none'>"
"<div class='card-title'>📡 Configurar Access Point</div>"
"<div class='config-row'><label>Nombre de Red:</label><input type='text' id='ap-ssid' maxlength='32' placeholder='Nombre del AP'></div>"
"<div class='config-row'><label>Contrasena:</label><div class='pass-container'><input type='password' id='ap-pass' maxlength='64' placeholder='Contrasena (min 8 chars)' style='padding-right:36px'><button type='button' class='pass-toggle' onclick='togglePass(\"ap-pass\")'>👁</button></div></div>"
"<p class='hint'>IP del dispositivo en modo AP: 192.168.4.1</p>"
"<button class='btn btn-danger' onclick='resetApCredentials()' style='margin-top:8px'>🔄 Restablecer a valores por defecto</button>"
"</div>"
"<div class='btn-group'>"
"<button class='btn btn-success' onclick='saveWifi()'>💾 Guardar WiFi</button>"
"<button class='btn btn-primary' onclick='tryConnectWifi()'>📶 Conectar a WiFi</button>"
"<button class='btn btn-danger' onclick='restartDevice()'>🔄 Reiniciar</button>"
"</div>"
"<p class='hint' style='color:var(--yl)'>Guarda la config primero, luego presiona Conectar.</p>"
"<div class='card'><div class='card-title'>✅ Datos Guardados</div><div id='wifi-saved-data' style='background:var(--bg);padding:10px;border-radius:4px;border-left:3px solid var(--gr)'></div></div>"
"<div class='card'><div class='card-title'>Informacion Actual</div><div id='wifi-info'>Cargando...</div></div>"
"</div>"
"<div class='panel' id='p4'>"
"<div style='display:flex;align-items:center;flex-wrap:wrap;gap:6px;margin-bottom:10px'>"
"<div class='status' id='files-status'>Cargando...</div>"
"</div>"
"<div class='btn-group'>"
"<button class='btn' onclick='loadFiles()'>🔄 Actualizar</button>"
"<button class='btn' onclick='mountSd()'>💾 Montar SD</button>"
"<button class='btn btn-danger' onclick='deleteAll()'>🗑 Borrar Todo</button>"
"</div>"
"<div class='card'>"
"<div class='card-title'>Formatear microSD (FAT32)</div>"
"<div class='status status-warn'>ADVERTENCIA: Esto borra TODOS los archivos. No desconectes la camara durante el formateo.</div>"
"<p class='hint' style='margin:6px 0'>Usar solo cuando la tarjeta tenga errores o antes de un nuevo ciclo.</p>"
"<button class='btn btn-danger' onclick='formatSd()' style='margin-top:6px'>Formatear microSD</button>"
"<div class='status' id='format-result' style='display:none;margin-top:8px'></div>"
"</div>"
"<div class='files' id='files'></div>"
"</div>"
"<div id='viewer-modal'>"
"<span id='viewer-close' onclick='closeViewer()'>&times;</span>"
"<div id='viewer-title'></div>"
"<div id='viewer-content'><img id='viewer-img' src='' alt='Visor'></div>"
"<div id='viewer-nav'>"
"<button class='btn' onclick='viewerPrev()'>← Anterior</button>"
"<button class='btn' onclick='viewerDownload()'>⬇ Descargar</button>"
"<button class='btn' onclick='viewerNext()'>Siguiente →</button>"
"</div>"
"</div>"
"<script>"
"let streamActive=false,forceMode=false,statusInterval=null;"
"let viewerFiles=[],viewerIndex=0;"
"let deviceLedOn=false;"
"let captureResultSeen=0;"
"let frameSizeLabels={5:'QVGA',7:'HVGA',9:'CIF',10:'VGA',11:'SVGA',12:'XGA',13:'HD'};"
"let liveViewSizeLabels={sm:'Compacta',md:'Mediana',lg:'Grande',xl:'Extra grande',full:'Ancho completo'};"
"document.querySelectorAll('input[name=cap-mode]').forEach(r=>r.addEventListener('change',e=>{"
"document.getElementById('video-opts').style.display=e.target.value=='1'?'block':'none';}));"
"function showTab(n){document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',i==n));"
"document.querySelectorAll('.panel').forEach((p,i)=>p.classList.toggle('active',i==n));"
"if(n==0)checkStatus();if(n==1)loadConfig();if(n==2)loadDeviceConfig();if(n==3)loadWifi();if(n==4)loadFiles();}"
"function qualityLabel(q){q=parseInt(q,10);if(q<=10)return'Muy alta';if(q<=12)return'Alta';if(q<=18)return'Media';if(q<=24)return'Baja';return'Muy baja';}"
"function frameSizeLabel(v){v=parseInt(v,10);return frameSizeLabels[v]||('Personalizada ('+v+')');}"
"function liveViewSizeLabel(v){return liveViewSizeLabels[v]||liveViewSizeLabels.lg;}"
"function applyLiveViewSize(v){"
"let presets={sm:['360px','220px'],md:['480px','270px'],lg:['640px','360px'],xl:['800px','450px'],full:['100%','min(56vw,520px)']};"
"let key=presets[v]?v:'lg';"
"document.documentElement.style.setProperty('--sw',presets[key][0]);"
"document.documentElement.style.setProperty('--sh',presets[key][1]);"
"let sel=document.getElementById('live-view-size');if(sel)sel.value=key;"
"return key;}"
"function previewLiveViewSize(){"
"let sel=document.getElementById('live-view-size');"
"let key=applyLiveViewSize(sel?sel.value:'lg');"
"try{localStorage.setItem('live-view-size',key);}catch(e){}"
"let status=document.getElementById('device-status');"
"if(status&&status.dataset.imageQuality&&status.dataset.frameSize){updateDeviceStatus({image_quality:parseInt(status.dataset.imageQuality,10),frame_size:parseInt(status.dataset.frameSize,10),led_on:status.dataset.ledOn==='true'});}"
"}"
"function loadLiveViewSize(){"
"let saved='lg';try{saved=localStorage.getItem('live-view-size')||'lg';}catch(e){}"
"return applyLiveViewSize(saved);}"
"function showIdleStream(){"
"let container=document.getElementById('stream-container');"
"let st=document.getElementById('stream-status');"
"if(st){st.className='status status-off';st.textContent='Sin transmision - Esperando movimiento...';}"
"if(container){container.innerHTML='<div id=\"stream-placeholder\"><span style=\"font-size:3em\">📷</span><p>Camara en espera</p><p style=\"color:var(--mt);font-size:.85em\">El video se activara cuando el sensor detecte movimiento</p></div>';}"
"let timer=document.getElementById('stream-timer');if(timer)timer.style.display='none';"
"let prog=document.getElementById('progress-container');if(prog)prog.style.display='none';}"
"function closeStreamImage(){"
"let img=document.getElementById('stream');"
"if(img){img.src='';img.removeAttribute('src');}}"
"function updateCaptureButton(active,busy,mode){"
"let btn=document.getElementById('btn-capture');"
"if(!btn)return;"
"btn.textContent='📸 Capturar';"
"btn.disabled=!active||busy;"
"btn.style.opacity=btn.disabled?'0.5':'1';"
"btn.title=!active?'Disponible solo con streaming activo':(busy?'Hay una captura en curso':'');}"
"function setCaptureNotice(kind,msg){"
"let el=document.getElementById('capture-status');"
"if(!el)return;el.className='capture-status show '+kind;el.textContent=msg;}"
"function refreshCaptureNotice(d){"
"if(d.manual_pending||d.capture_busy){setCaptureNotice('saving','Guardando captura en la microSD...');return;}"
"if(d.capture_result_id&&d.capture_result_id!==captureResultSeen){"
"captureResultSeen=d.capture_result_id;"
"if(d.capture_last_ok){setCaptureNotice('ok','Guardado: '+(d.capture_last_file||'imagen JPG'));loadFiles();}"
"else{setCaptureNotice('error','Error al guardar: '+(d.capture_last_msg||'sin detalle'));}}}"
"function checkStatus(){let ctrl=new AbortController();setTimeout(()=>ctrl.abort(),2000);"
"fetch('/api/motion/status',{signal:ctrl.signal,cache:'no-store'}).then(r=>r.json()).then(d=>{"
"streamActive=d.active;"
"let st=document.getElementById('stream-status');"
"let container=document.getElementById('stream-container');"
"let timer=document.getElementById('stream-timer');"
"let timerVal=document.getElementById('timer-val');"
"let prog=document.getElementById('progress-container');"
"let fill=document.getElementById('progress-fill');"
"if(d.active){"
"let modeStr=d.is_live?' (Vista en vivo)':' (Movimiento)';"
"st.className='status status-on';st.textContent='Transmitiendo'+modeStr;"
"if(timer){timer.style.display='';if(timerVal)timerVal.textContent=d.remaining;}"
"if(prog)prog.style.display='';"
"if(fill&&d.emission_time>0)fill.style.width=Math.max(0,Math.min(100,100*d.remaining/d.emission_time))+'%';"
"if(!container.innerHTML||container.innerHTML.indexOf('placeholder')>-1){"
"container.innerHTML='<img id=\"stream\" src=\"/stream?t='+Date.now()+'\" alt=\"Video\">';}"
"}else{"
"st.className='status status-off';st.textContent='Sin transmision - Esperando movimiento...';"
"if(timer)timer.style.display='none';"
"if(prog)prog.style.display='none';"
"container.innerHTML='<div id=\"stream-placeholder\"><span style=\"font-size:3em\">📷</span><p>Camara en espera</p><p style=\"color:var(--mt);font-size:.85em\">El video se activara cuando el sensor detecte movimiento</p></div>';}"
"updateCaptureButton(d.active,!!d.capture_busy||!!d.manual_pending,d.capture_mode);"
"refreshCaptureNotice(d);"
"}).catch(e=>{if(e.name!=='AbortError')console.error(e);});}"
"function forceStream(){fetch('/api/motion/force',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.ok){forceMode=true;checkStatus();}});}"
"function captureStamp(){let d=new Date();let p=n=>String(n).padStart(2,'0');return p(d.getDate())+'_'+p(d.getMonth()+1)+'_'+String(d.getFullYear()).slice(-2)+'_'+p(d.getHours())+'_'+p(d.getMinutes());}"
"function captureNow(){"
"if(!streamActive){showToast('Activa Ver en vivo o espera movimiento','#a00');return;}"
"let btn=document.getElementById('btn-capture');"
"if(btn){btn.disabled=true;btn.style.opacity='0.5';}"
"setCaptureNotice('saving','Pausando video y guardando captura...');"
"closeStreamImage();"
"setTimeout(()=>{"
"fetch('/api/capture/manual',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'epoch='+Math.floor(Date.now()/1000)+'&stamp='+captureStamp()}).then(r=>r.json()).then(d=>{"
"if(d.ok){setCaptureNotice('ok','Guardado: '+(d.file||'imagen JPG'));loadFiles();}"
"else{showToast('Error: '+(d.error||'No se pudo capturar'),'#a00');}"
"}).catch(()=>{setCaptureNotice('error','Error de conexion con la camara');}).finally(()=>setTimeout(checkStatus,600));"
"},250);}"
"function loadConfig(){fetch('/api/motion/config').then(r=>r.json()).then(d=>{"
"document.getElementById('emit-time').value=d.emission_time;"
"document.getElementById('live-time').value=d.live_time;"
"document.getElementById('vid-dur').value=d.video_duration||10;"
"document.querySelectorAll('input[name=cap-mode]').forEach(r=>r.checked=(r.value==d.capture_mode));"
"document.getElementById('video-opts').style.display=d.capture_mode==1?'block':'none';"
"let modeStr=d.capture_mode==1?'Video ('+d.video_duration+'s)':'Foto';"
"document.getElementById('config-status').innerHTML="
"'<p>Modo: <b>'+modeStr+'</b></p>'"
"+'<p>Movimiento: <b>'+d.emission_time+'</b>s | En vivo: <b>'+d.live_time+'</b>s</p>'"
"+'<p>Estado: '+(d.active?'<span style=\"color:var(--gr)\">Transmitiendo</span>':'<span style=\"color:var(--rd)\">En espera</span>')+'</p>';});}"
"function saveConfig(){let t=parseInt(document.getElementById('emit-time').value);"
"let l=parseInt(document.getElementById('live-time').value);"
"let m=document.querySelector('input[name=cap-mode]:checked').value;"
"let v=parseInt(document.getElementById('vid-dur').value);"
"if(t<5||t>300){showToast('Tiempo movimiento: 5-300s','#a00');return;}"
"if(l<10||l>600){showToast('Tiempo en vivo: 10-600s','#a00');return;}"
"if(m==1&&(v<5||v>60)){showToast('Duracion video: 5-60s','#a00');return;}"
"fetch('/api/motion/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'time='+t+'&live='+l+'&mode='+m+'&vdur='+v}).then(r=>r.json()).then(d=>{if(d.ok){"
"showToast('Configuracion guardada','#0a0');}loadConfig();});}"
"function updateDeviceStatus(d){"
"deviceLedOn=!!d.led_on;"
"let qualitySel=document.getElementById('img-quality');"
"if(qualitySel&&!qualitySel.querySelector('option[value=\"'+d.image_quality+'\"]')){let opt=document.createElement('option');opt.value=String(d.image_quality);opt.textContent='Personalizada ('+d.image_quality+')';qualitySel.appendChild(opt);}"
"if(qualitySel)qualitySel.value=String(d.image_quality);"
"let frameSel=document.getElementById('frame-size');"
"if(frameSel&&!frameSel.querySelector('option[value=\"'+d.frame_size+'\"]')){let opt=document.createElement('option');opt.value=String(d.frame_size);opt.textContent=frameSizeLabel(d.frame_size);frameSel.appendChild(opt);}"
"if(frameSel)frameSel.value=String(d.frame_size);"
"let led=document.getElementById('led-status');"
"if(led){led.className='status '+(deviceLedOn?'status-on':'status-off');led.textContent=deviceLedOn?'LED encendido':'LED apagado';}"
"let liveViewSize=loadLiveViewSize();"
"let status=document.getElementById('device-status');"
"if(status){status.dataset.imageQuality=String(d.image_quality);status.dataset.frameSize=String(d.frame_size);status.dataset.ledOn=String(!!d.led_on);"
"status.innerHTML='<p>Calidad JPEG: <b>'+qualityLabel(d.image_quality)+'</b> (valor '+d.image_quality+')</p><p>Resolucion: <b>'+frameSizeLabel(d.frame_size)+'</b></p><p>Vista en vivo: <b>'+liveViewSizeLabel(liveViewSize)+'</b></p><p>LED: <b>'+(deviceLedOn?'Encendido':'Apagado')+'</b></p>';}}"
"function loadDeviceConfig(){fetch('/api/device/config').then(r=>r.json()).then(d=>updateDeviceStatus(d));}"
"function saveDeviceConfig(){let q=parseInt(document.getElementById('img-quality').value,10);let s=parseInt(document.getElementById('frame-size').value,10);"
"if(q<10||q>63){showToast('Calidad JPEG invalida','#a00');return;}"
"fetch('/api/device/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'quality='+q+'&size='+s})"
".then(r=>r.json()).then(d=>{if(d.ok){updateDeviceStatus(d);showToast('Configuracion guardada','#0a0');}else{showToast('No se pudo guardar','#a00');}})"
".catch(()=>showToast('Error de conexion','#a00'));}"
"function setLed(on){fetch('/api/device/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'led='+(on?1:0)})"
".then(r=>r.json()).then(d=>{if(d.ok){updateDeviceStatus(d);showToast(on?'LED encendido':'LED apagado',on?'#0a0':'#444');}else{showToast('No se pudo cambiar el LED','#a00');}})"
".catch(()=>showToast('Error de conexion','#a00'));}"
"function wifiSignalLabel(rssi){"
"if(!rssi)return'Sin datos';if(rssi>=-55)return'Excelente';if(rssi>=-67)return'Buena';if(rssi>=-75)return'Regular';return'Mala';}"
"function loadWifi(){fetch('/api/wifi/status').then(r=>r.json()).then(d=>{"
"let st=document.getElementById('wifi-status');"
"if(d.connected){st.className='status status-on';st.textContent='Conectado a: '+d.ssid;}"
"else if(d.ap_mode){st.className='status status-warn';st.textContent='Modo AP: '+d.ap_ssid;}"
"else{st.className='status status-off';st.textContent='Desconectado';}"
"if(d.connected&&d.rssi){st.textContent+=' | Senal: '+wifiSignalLabel(d.rssi)+' ('+d.rssi+' dBm)';}"
"document.getElementById('wifi-ssid').value=d.ssid||'';"
"document.getElementById('ap-ssid').value=d.ap_ssid||'';"
"document.querySelectorAll('input[name=wifi-mode]').forEach(r=>r.checked=(r.value==d.preferred_mode));"
"updateWifiForm(d.preferred_mode);"
"let modeStr=d.preferred_mode==0?'WiFi (Estacion)':'Access Point';"
"let savedSsid=d.preferred_mode==0?d.ssid:d.ap_ssid;"
"document.getElementById('wifi-saved-data').innerHTML="
"'<p><b>Modo:</b> '+modeStr+'</p>'"
"+'<p><b>SSID guardado:</b> '+(savedSsid||'(no configurado)')+'</p>'"
"+'<p style=\"color:var(--yl)\">Los datos estan guardados en la memoria del dispositivo</p>';"
"document.getElementById('wifi-info').innerHTML="
"'<p>IP: <b>'+d.ip+'</b></p>'"
"+'<p>Modo actual: <b>'+(d.ap_mode?'Access Point':'Estacion')+'</b></p>'"
"+'<p>Modo preferido: <b>'+(d.preferred_mode==1?'AP (Red propia)':'WiFi (Red externa)')+'</b></p>'"
"+'<p>'+(d.ap_mode?'AP SSID: <b>'+d.ap_ssid+'</b>':'WiFi SSID: <b>'+d.ssid+'</b>')+'</p>';"
"if(d.connected&&d.rssi){document.getElementById('wifi-info').innerHTML+='<p>Senal WiFi: <b>'+wifiSignalLabel(d.rssi)+'</b> ('+d.rssi+' dBm)</p>';} });}"
"function updateWifiForm(mode){document.getElementById('sta-config').style.display=mode==0?'block':'none';"
"document.getElementById('ap-config').style.display=mode==1?'block':'none';}"
"document.querySelectorAll('input[name=wifi-mode]').forEach(r=>r.addEventListener('change',e=>updateWifiForm(e.target.value)));"
"function saveWifi(){let mode=document.querySelector('input[name=wifi-mode]:checked').value;"
"let s,p;"
"if(mode=='0'){s=document.getElementById('wifi-ssid').value.trim();p=document.getElementById('wifi-pass').value;}"
"else{s=document.getElementById('ap-ssid').value.trim();p=document.getElementById('ap-pass').value;}"
"if(!s||s.length<1||s.length>32){showToast('SSID invalido (1-32 caracteres)','#a00');return;}"
"if(p.length>0&&p.length<8){showToast('Contrasena muy corta (min 8)','#a00');return;}"
"fetch('/api/wifi/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'mode='+mode+'&ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)}).then(r=>r.json()).then(d=>{"
"if(d.ok){showToast('Configuracion guardada','#0a0');loadWifi();}"
"else{showToast('Error guardando','#a00');}});}"
"function restartDevice(){if(confirm('Reiniciar el dispositivo?')){"
"fetch('/api/restart',{method:'POST'}).then(()=>showToast('Reiniciando...','#06a'));}}"
"function togglePass(id){let inp=document.getElementById(id);if(inp.type==='password'){inp.type='text';}else{inp.type='password';}}"
"function resetApCredentials(){if(confirm('Restaurar credenciales AP a valores por defecto?\\n\\nSSID: CamaraVigia_AP\\nContrasena: seguridad123')){"
"fetch('/api/wifi/reset_ap',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.ok){"
"showToast('Credenciales AP reseteadas','#0a0');loadWifi();}"
"else{showToast('Error reseteando','#a00');}});}}"
"function tryConnectWifi(){showToast('Intentando conectar a WiFi...','#06a');"
"fetch('/api/wifi/connect',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.ok){"
"showConnectingBanner();checkWifiConnection(0);}"
"else{showToast('Error: '+d.error,'#a00');}});}"
"function showConnectingBanner(){let b=document.createElement('div');b.id='connect-banner';"
"b.style.cssText='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.9);display:flex;flex-direction:column;align-items:center;justify-content:center;z-index:10000';"
"b.innerHTML='<div style=\"font-size:4em;animation:pulse 1s infinite\">📶</div><h2 style=\"color:#fff;margin:20px\">Conectando a WiFi...</h2><p id=\"connect-status\" style=\"color:#888\">Intento 1 de 10</p><p style=\"color:#666;font-size:.8em\">Por favor espera...</p>';"
"document.body.appendChild(b);}"
"function checkWifiConnection(attempt){if(attempt>=10){"
"document.getElementById('connect-banner').remove();"
"showToast('No se pudo conectar. Verifica las credenciales.','#a00');loadWifi();return;}"
"document.getElementById('connect-status').textContent='Intento '+(attempt+1)+' de 10';"
"fetch('/api/wifi/status').then(r=>r.json()).then(d=>{if(d.connected){"
"document.getElementById('connect-banner').innerHTML='<div style=\"font-size:4em\">✅</div><h2 style=\"color:var(--gr);margin:20px\">CONECTADO!</h2><p style=\"color:#fff;font-size:1.2em\">Red: <b>'+d.ssid+'</b></p><p style=\"color:var(--gr);font-size:1.5em\">IP: '+d.ip+'</p><p style=\"color:#888;margin-top:20px\">Esta ventana se cerrara en 3 segundos...</p>';"
"setTimeout(()=>{document.getElementById('connect-banner').remove();loadWifi();},3000);}"
"else{setTimeout(()=>checkWifiConnection(attempt+1),2000);}});}"
"function showToast(msg,bg){let toast=document.createElement('div');"
"toast.style.cssText='position:fixed;top:20px;left:50%;transform:translateX(-50%);background:'+(bg||'#0a0')+';color:#fff;padding:14px 22px;border-radius:8px;z-index:9999;font-size:.9em;white-space:pre-line;text-align:center;box-shadow:0 4px 20px rgba(0,0,0,.5);animation:fadeIn .2s';"
"toast.textContent=msg;document.body.appendChild(toast);"
"setTimeout(()=>{toast.style.opacity='0';toast.style.transition='opacity 0.4s';setTimeout(()=>toast.remove(),400);},3000);}"
"function setFormatResult(msg,cls){let el=document.getElementById('format-result');"
"if(!el)return;el.style.display='flex';el.className='status '+cls;el.textContent=msg;}"
"function formatSd(){"
"if(!confirm('FORMATEAR microSD?\\n\\nSe borraran TODOS los archivos.\\nNo desconectes la camara durante el proceso.'))return;"
"setFormatResult('Formateando microSD...','status-warn');"
"fetch('/api/format_sd',{method:'POST'}).then(r=>r.json()).then(d=>{"
"if(d&&d.ok){setFormatResult('Formateo completado','status-on');loadFiles();return;}"
"let err=(d&&d.error)?d.error:'no se pudo formatear';"
"if(err==='ESP_ERR_INVALID_STATE')err='No se puede formatear durante streaming/captura';"
"if(err==='ESP_ERR_TIMEOUT')err='Tiempo agotado. Revisa microSD y conexiones.';"
"if(err==='ESP_ERR_NOT_SUPPORTED')err='MKFS no habilitado en FATFS';"
"setFormatResult('Error: '+err,'status-off');"
"}).catch(()=>{setFormatResult('Error de conexion','status-off');});}"
"function loadFiles(){fetch('/api/files').then(r=>r.json()).then(d=>{"
"if(d.error){"
"document.getElementById('files-status').className='status status-off';"
"document.getElementById('files-status').textContent=d.error;"
"document.getElementById('files').innerHTML='';viewerFiles=[];return;}"
"document.getElementById('files-status').className='status status-on';"
"document.getElementById('files-status').textContent=d.count+' archivos ('+formatSize(d.total_size)+')';"
"viewerFiles=d.files;"
"let h='';d.files.forEach((f,i)=>{"
"let icon=f.name.startsWith('VID_')?'🎬':'📷';"
"h+='<div class=\"file\"><span class=\"file-name\" onclick=\"openViewer('+i+')\">'+icon+' '+f.name+'</span>';"
"h+='<span class=\"file-info\">'+formatSize(f.size)+' | '+formatDate(f.mtime)+'</span>';"
"h+='<div class=\"file-actions\"><button class=\"btn\" onclick=\"openViewer('+i+')\">👁</button>';"
"h+='<a class=\"btn\" href=\"/file?name='+encodeURIComponent(f.name)+'\" download>⬇</a>';"
"h+='<button class=\"btn btn-danger\" onclick=\"deleteFile(\\''+f.name+'\\');\">🗑</button></div></div>';});"
"document.getElementById('files').innerHTML=h||'<p style=\"color:var(--mt);padding:10px\">No hay archivos guardados</p>';}).catch(()=>{"
"document.getElementById('files-status').className='status status-off';"
"document.getElementById('files-status').textContent='Error de conexion';});}"
"function openViewer(idx){viewerIndex=idx;let f=viewerFiles[idx];if(!f)return;"
"document.getElementById('viewer-title').textContent=f.name+' ('+formatSize(f.size)+')';"
"document.getElementById('viewer-img').src='/file?name='+encodeURIComponent(f.name);"
"document.getElementById('viewer-modal').classList.add('show');}"
"function closeViewer(){document.getElementById('viewer-modal').classList.remove('show');document.getElementById('viewer-img').src='';}"
"function viewerPrev(){if(viewerIndex>0)openViewer(viewerIndex-1);}"
"function viewerNext(){if(viewerIndex<viewerFiles.length-1)openViewer(viewerIndex+1);}"
"function viewerDownload(){let f=viewerFiles[viewerIndex];if(f)window.open('/file?name='+encodeURIComponent(f.name),'_blank');}"
"document.addEventListener('keydown',e=>{if(document.getElementById('viewer-modal').classList.contains('show')){"
"if(e.key==='Escape')closeViewer();if(e.key==='ArrowLeft')viewerPrev();if(e.key==='ArrowRight')viewerNext();}});"
"function deleteFile(n){if(confirm('Borrar '+n+'?'))fetch('/api/delete?name='+encodeURIComponent(n),{method:'DELETE'})"
".then(r=>r.json()).then(d=>{if(d&&d.ok){loadFiles();closeViewer();showToast('Archivo borrado','#444');}else{showToast('Error: '+(d.error||'No se pudo borrar'),'#a00');}}).catch(()=>showToast('Error de conexion con la camara','#a00'));}"
"function mountSd(){if(streamActive){showToast('Detener transmision antes de montar SD','#a60');return;}"
"document.getElementById('files-status').textContent='Montando SD...';"
"fetch('/api/sd/reinit',{method:'POST'}).then(r=>r.json()).then(d=>{if(d&&d.ok){showToast('SD montada','#0a0');loadFiles();}"
"else{showToast('Error: '+(d.error||'No se pudo montar'),'#a00');document.getElementById('files-status').textContent=d.error||'No se pudo montar';}}).catch(()=>{showToast('Error de conexion con la camara','#a00');document.getElementById('files-status').textContent='Error de conexion';});}"
"function deleteAll(){if(confirm('BORRAR TODOS los archivos?'))fetch('/api/delete_all',{method:'DELETE'})"
".then(r=>r.json()).then(d=>{if(d&&d.ok){loadFiles();showToast('Borrados '+d.deleted+' archivos','#444');}else{showToast('Error: '+(d.error||'No se pudo borrar'),'#a00');}}).catch(()=>showToast('Error de conexion con la camara','#a00'));}"
"function formatSize(b){if(b<1024)return b+'B';if(b<1048576)return(b/1024).toFixed(1)+'KB';return(b/1048576).toFixed(1)+'MB';}"
"function formatDate(t){let d=new Date(t*1000);return d.toLocaleDateString()+' '+d.toLocaleTimeString();}"
"function stopForce(){forceMode=false;streamActive=false;closeStreamImage();showIdleStream();"
"setTimeout(()=>{fetch('/api/motion/stop',{method:'POST'}).then(r=>r.json()).then(()=>setTimeout(checkStatus,150)).catch(()=>setTimeout(checkStatus,300));},250);}"
"loadLiveViewSize();checkStatus();statusInterval=setInterval(checkStatus,1000);"
"</script></body></html>";

// ============================================================================
// COMPARADOR PARA ORDENAR POR FECHA (MÁS RECIENTE PRIMERO)
// ============================================================================
static int compare_by_date(const void *a, const void *b) {
    file_info_t *fa = (file_info_t *)a;
    file_info_t *fb = (file_info_t *)b;
    // Orden descendente (más reciente primero)
    return (fb->mtime - fa->mtime);
}

// ============================================================================
// HANDLER: PÁGINA PRINCIPAL
// ============================================================================
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_INDEX, strlen(HTML_INDEX));
    return ESP_OK;
}

// ============================================================================
// HANDLER: STREAM MJPEG (Con control de movimiento)
// ============================================================================
static esp_err_t stream_handler(httpd_req_t *req) {
    // Verificar si el streaming está permitido
    if (!http_server_is_streaming_active()) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Stream inactivo - esperando movimiento", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[128];
    
    // TCP_NODELAY para baja latencia
    int sockfd = httpd_req_to_sockfd(req);
    int nodelay = 1;
    if (sockfd != -1) {
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK) return res;

    ESP_LOGI(TAG, "Stream iniciado");

    while(true) {
        // Verificar si debemos seguir transmitiendo
        if (!http_server_is_streaming_active()) {
            ESP_LOGI(TAG, "Stream detenido - tiempo de emisión expirado");
            break;
        }
        
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Fallo camara");
            res = ESP_FAIL;
            break;
        }

        if (http_server_take_manual_capture_request()) {
            g_capture_in_progress = true;
            save_manual_capture_frame(fb->buf, fb->len);
            g_capture_in_progress = false;
        }

        size_t hlen = snprintf(part_buf, sizeof(part_buf), 
            "%sContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", 
            _STREAM_BOUNDARY, fb->len);
        
        if(res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
        if(res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

        esp_camera_fb_return(fb);
        fb = NULL;

        if(res != ESP_OK) break;
    }
    
    ESP_LOGI(TAG, "Stream terminado");
    return res;
}

// ============================================================================
// HANDLER: LISTAR ARCHIVOS (JSON)
// ============================================================================
static esp_err_t files_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Solicitud de lista de archivos recibida");
    
    // Verificar primero si la SD está montada
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "SD no montada - respondiendo error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"count\":0,\"total_size\":0,\"files\":[],\"error\":\"💾 Tarjeta SD no montada. Verifica que esté insertada.\"}");
        return ESP_OK;
    }
    
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "No se pudo abrir %s - SD no disponible", MOUNT_POINT);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"count\":0,\"total_size\":0,\"files\":[],\"error\":\"💾 Error al acceder a la tarjeta SD.\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Directorio %s abierto correctamente", MOUNT_POINT);

    file_info_t *files = malloc(sizeof(file_info_t) * MAX_FILES);
    if (!files) {
        closedir(dir);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"count\":0,\"total_size\":0,\"files\":[],\"error\":\"⚠️ Sin memoria disponible\"}");
        return ESP_OK;
    }

    int count = 0;
    size_t total_size = 0;
    struct dirent *entry;
    struct stat st;
    char filepath[320];

    while ((entry = readdir(dir)) != NULL && count < MAX_FILES) {
        // Ignorar directorios y archivos ocultos
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.') continue;
        
        snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, entry->d_name);
        if (stat(filepath, &st) == 0) {
            strncpy(files[count].name, entry->d_name, sizeof(files[count].name) - 1);
            files[count].size = st.st_size;
            files[count].mtime = st.st_mtime;
            total_size += st.st_size;
            count++;
        }
    }
    closedir(dir);

    // Ordenar por fecha (más reciente primero)
    qsort(files, count, sizeof(file_info_t), compare_by_date);

    // Generar JSON
    httpd_resp_set_type(req, "application/json");
    
    char *json = malloc(4096);
    if (!json) {
        free(files);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
        return ESP_FAIL;
    }

    int pos = snprintf(json, 4096, "{\"count\":%d,\"total_size\":%zu,\"files\":[", count, total_size);
    
    for (int i = 0; i < count && pos < 3900; i++) {
        pos += snprintf(json + pos, 4096 - pos, 
            "%s{\"name\":\"%s\",\"size\":%zu,\"mtime\":%ld}",
            i > 0 ? "," : "", files[i].name, files[i].size, (long)files[i].mtime);
    }
    
    pos += snprintf(json + pos, 4096 - pos, "]}");
    
    httpd_resp_send(req, json, pos);
    
    free(json);
    free(files);
    return ESP_OK;
}

// ============================================================================
// HANDLER: DESCARGAR/VER ARCHIVO
// ============================================================================
static esp_err_t file_handler(httpd_req_t *req) {
    char filepath[320];
    char query[64] = {0};
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Falta nombre");
        return ESP_FAIL;
    }

    char filename[64] = {0};
    if (httpd_query_key_value(query, "name", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Parametro invalido");
        return ESP_FAIL;
    }

    // Seguridad: evitar path traversal
    if (strstr(filename, "..") || filename[0] == '/') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nombre invalido");
        return ESP_FAIL;
    }

    // Detectar extensión del archivo
    const char *ext = strrchr(filename, '.');
    
    // Archivo normal (no encriptado)
    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);
    
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Archivo no encontrado");
        return ESP_FAIL;
    }

    // Detectar tipo MIME
    if (ext && strcasecmp(ext, ".jpg") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
    } else if (ext && strcasecmp(ext, ".avi") == 0) {
        httpd_resp_set_type(req, "video/x-msvideo");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    // Enviar archivo en chunks
    char *buf = malloc(4096);
    if (buf) {
        size_t read_len;
        while ((read_len = fread(buf, 1, 4096, f)) > 0) {
            httpd_resp_send_chunk(req, buf, read_len);
        }
        httpd_resp_send_chunk(req, NULL, 0);
        free(buf);
    }
    
    fclose(f);
    return ESP_OK;
}

// ============================================================================
// HANDLER: BORRAR ARCHIVO
// ============================================================================
static esp_err_t delete_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
    char filepath[320];
    char query[64] = {0};
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Falta nombre de archivo\"}");
        return ESP_OK;
    }

    char filename[64] = {0};
    if (httpd_query_key_value(query, "name", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Parametro invalido\"}");
        return ESP_OK;
    }

    // Seguridad
    if (strstr(filename, "..") || filename[0] == '/') {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Nombre invalido\"}");
        return ESP_OK;
    }

    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);
    
    if (remove(filepath) == 0) {
        ESP_LOGI(TAG, "Archivo borrado: %s", filename);
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        ESP_LOGW(TAG, "No se pudo borrar: %s", filename);
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No se pudo borrar el archivo\"}");
    }
    
    return ESP_OK;
}

// ============================================================================
// HANDLER: BORRAR TODOS LOS ARCHIVOS
// ============================================================================
static esp_err_t delete_all_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "delete_all: No se puede abrir SD");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Tarjeta SD no disponible\",\"deleted\":0}");
        return ESP_OK;
    }

    struct dirent *entry;
    char filepath[320];
    int deleted = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && entry->d_name[0] != '.') {
            snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, entry->d_name);
            if (remove(filepath) == 0) {
                deleted++;
            }
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "Borrados %d archivos", deleted);
    
    char response[64];
    snprintf(response, sizeof(response), "{\"ok\":true,\"deleted\":%d}", deleted);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// ============================================================================
// HANDLER: FORMATEAR MICROSD
// ============================================================================
static esp_err_t format_sd_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    if (http_server_is_streaming_active()) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ESP_ERR_INVALID_STATE\"}");
        return ESP_OK;
    }

    esp_err_t ret = sd_card_format();
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }

    char response[96];
    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(ret));
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// ============================================================================
// HANDLER: RECONECTAR/REINICIAR SD
// ============================================================================
static esp_err_t sd_reinit_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
    ESP_LOGI(TAG, "Solicitud de reconexion SD desde web");

    if (http_server_is_streaming_active()) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Detener streaming primero\"}");
        return ESP_OK;
    }

    esp_err_t ret = sd_card_reinit();
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"SD reconectada exitosamente\"}");
        return ESP_OK;
    }

    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(ret));
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// ============================================================================
// HANDLER: ESTADO DE LA SD
// ============================================================================
static esp_err_t sd_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
    bool mounted = sd_card_is_mounted();
    char response[64];
    snprintf(response, sizeof(response), "{\"mounted\":%s}", mounted ? "true" : "false");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// Handler para favicon (evita warnings 404)
static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ============================================================================
// HANDLERS: API DE CONTROL DE MOVIMIENTO
// ============================================================================

// Estado del streaming
static esp_err_t motion_status_handler(httpd_req_t *req) {
    char response[384];
    bool active = http_server_is_streaming_active();
    int remaining = http_server_get_remaining_time();
    
    snprintf(response, sizeof(response), 
        "{\"active\":%s,\"remaining\":%d,\"is_live\":%s,\"emission_time\":%d,\"capture_mode\":%d,"
        "\"manual_pending\":%s,\"capture_busy\":%s,\"capture_result_id\":%lu,"
        "\"capture_last_ok\":%s,\"capture_last_file\":\"%s\",\"capture_last_msg\":\"%s\"}",
        active ? "true" : "false", remaining, 
        g_force_stream ? "true" : "false", g_emission_time_sec, (int)g_capture_mode,
        g_manual_capture_requested ? "true" : "false",
        g_capture_in_progress ? "true" : "false",
        (unsigned long)s_manual_capture_result_id,
        s_manual_capture_last_ok ? "true" : "false",
        s_manual_capture_last_file,
        s_manual_capture_last_msg);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// Obtener/Guardar configuración
static esp_err_t motion_config_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // GET: Devolver configuración actual
        char response[200];
        snprintf(response, sizeof(response), 
            "{\"emission_time\":%d,\"live_time\":%d,\"capture_mode\":%d,\"video_duration\":%d,\"active\":%s}",
            g_emission_time_sec, g_live_time_sec, (int)g_capture_mode, g_video_duration_sec,
            http_server_is_streaming_active() ? "true" : "false");
        
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, response);
    } else {
        // POST: Guardar nueva configuración
        char content[160] = {0};
        int ret = httpd_req_recv(req, content, sizeof(content) - 1);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Sin datos");
            return ESP_FAIL;
        }
        
        // Parsear "time=XX&live=YY&mode=Z&vdur=W"
        char *time_str = strstr(content, "time=");
        char *live_str = strstr(content, "live=");
        char *mode_str = strstr(content, "mode=");
        char *vdur_str = strstr(content, "vdur=");
        
        bool updated = false;
        if (time_str) {
            int new_time = atoi(time_str + 5);
            if (new_time >= 5 && new_time <= 300) {
                g_emission_time_sec = new_time;
                updated = true;
            }
        }
        if (live_str) {
            int new_live = atoi(live_str + 5);
            if (new_live >= 10 && new_live <= 600) {
                g_live_time_sec = new_live;
                updated = true;
            }
        }
        if (mode_str) {
            int new_mode = atoi(mode_str + 5);
            if (new_mode == 0 || new_mode == 1) {
                g_capture_mode = (capture_mode_t)new_mode;
                updated = true;
            }
        }
        if (vdur_str) {
            int new_vdur = atoi(vdur_str + 5);
            if (new_vdur >= 5 && new_vdur <= 60) {
                g_video_duration_sec = new_vdur;
                updated = true;
            }
        }
        
        if (updated) {
            save_motion_config();
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":true}");
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Parametros invalidos");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

// Forzar stream (vista en vivo) con timeout
static esp_err_t motion_force_handler(httpd_req_t *req) {
    g_force_stream = true;
    g_force_end_time = esp_timer_get_time() + ((int64_t)g_live_time_sec * 1000000);
    ESP_LOGI(TAG, "Vista en vivo activada por %d segundos", g_live_time_sec);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t manual_capture_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    if (!http_server_is_streaming_active()) {
        set_manual_capture_result(false, "transmision inactiva", NULL);
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Activa Ver en vivo o espera movimiento\"}");
        return ESP_OK;
    }

    if (!sd_card_is_mounted()) {
        set_manual_capture_result(false, "microSD no montada", NULL);
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"La microSD no esta montada\"}");
        return ESP_OK;
    }

    if (g_capture_in_progress || g_manual_capture_requested) {
        set_manual_capture_result(false, "captura en curso", NULL);
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Ya hay una captura en curso\"}");
        return ESP_OK;
    }

    char content[96] = {0};
    if (req->content_len > 0) {
        size_t to_read = req->content_len < (sizeof(content) - 1) ? req->content_len : (sizeof(content) - 1);
        int ret = httpd_req_recv(req, content, to_read);
        if (ret > 0) {
            content[ret] = '\0';
        }
    }

    char *epoch_str = strstr(content, "epoch=");
    if (epoch_str) {
        long long epoch = atoll(epoch_str + 6);
        if (epoch > 1700000000LL) {
            struct timeval tv = {
                .tv_sec = (time_t)epoch,
                .tv_usec = 0
            };
            settimeofday(&tv, NULL);
        }
    }

    char *stamp_str = strstr(content, "stamp=");
    if (stamp_str) {
        char stamp[sizeof(s_manual_capture_stamp)] = {0};
        stamp_str += 6;
        size_t i = 0;
        while (stamp_str[i] && stamp_str[i] != '&' && i < sizeof(stamp) - 1) {
            stamp[i] = stamp_str[i];
            i++;
        }
        stamp[i] = '\0';

        if (is_valid_capture_stamp(stamp)) {
            strncpy(s_manual_capture_stamp, stamp, sizeof(s_manual_capture_stamp) - 1);
        }
    }

    if (g_force_stream) {
        g_force_end_time = esp_timer_get_time() + ((int64_t)g_live_time_sec * 1000000);
    }

    ESP_LOGI(TAG, "Captura manual solicitada desde la web");
    g_capture_in_progress = true;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        g_capture_in_progress = false;
        s_manual_capture_stamp[0] = '\0';
        set_manual_capture_result(false, "no se pudo obtener frame de la camara", NULL);
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No se pudo obtener imagen de la camara\"}");
        return ESP_OK;
    }

    esp_err_t save_ret = save_manual_capture_frame(fb->buf, fb->len);
    esp_camera_fb_return(fb);
    g_capture_in_progress = false;

    if (save_ret != ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No se pudo guardar en la microSD\"}");
        return ESP_OK;
    }

    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":true,\"file\":\"%s\"}", s_manual_capture_last_file);
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// Detener stream forzado
static esp_err_t motion_stop_handler(httpd_req_t *req) {
    g_force_stream = false;
    g_manual_capture_requested = false;
    g_capture_in_progress = false;
    s_manual_capture_stamp[0] = '\0';
    ESP_LOGI(TAG, "Stream forzado detenido");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ============================================================================
// HANDLERS: API DE CONFIGURACIÓN WIFI
// ============================================================================

// Estado y configuración WiFi actual
static esp_err_t device_config_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        char response[192];
        snprintf(response, sizeof(response),
                 "{\"image_quality\":%d,\"frame_size\":%d,\"frame_size_name\":\"%s\",\"led_on\":%s}",
                 g_image_quality, (int)g_frame_size, cam_hal_get_frame_size_name(g_frame_size),
                 cam_hal_get_flash_led() ? "true" : "false");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, response);
        return ESP_OK;
    }

    char content[96] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Sin datos");
        return ESP_FAIL;
    }

    char *quality_str = strstr(content, "quality=");
    char *size_str = strstr(content, "size=");
    char *led_str = strstr(content, "led=");
    bool updated = false;

    if (quality_str) {
        int new_quality = atoi(quality_str + 8);
        if (new_quality < 10 || new_quality > 63) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Calidad invalida");
            return ESP_FAIL;
        }

        esp_err_t err = cam_hal_set_jpeg_quality(new_quality);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo aplicar la calidad");
            return ESP_FAIL;
        }

        g_image_quality = new_quality;
        updated = true;
    }

    if (size_str) {
        int new_size = atoi(size_str + 5);
        esp_err_t err = cam_hal_set_frame_size((framesize_t)new_size);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Resolucion invalida");
            return ESP_FAIL;
        }

        g_frame_size = (framesize_t)new_size;
        updated = true;
    }

    if (led_str) {
        int led_value = atoi(led_str + 4);
        if (led_value != 0 && led_value != 1) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "LED invalido");
            return ESP_FAIL;
        }

        esp_err_t err = cam_hal_set_flash_led(led_value == 1);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo cambiar el LED");
            return ESP_FAIL;
        }

        updated = true;
    }

    if (!updated) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Parametros invalidos");
        return ESP_FAIL;
    }

    if (quality_str || size_str) {
        save_device_config();
    }

    char response[224];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"image_quality\":%d,\"frame_size\":%d,\"frame_size_name\":\"%s\",\"led_on\":%s}",
             g_image_quality, (int)g_frame_size, cam_hal_get_frame_size_name(g_frame_size),
             cam_hal_get_flash_led() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t wifi_status_handler(httpd_req_t *req) {
    char response[384];
    char ssid[33] = {0};
    char ip[16] = {0};
    char ap_ssid[33] = {0};
    int rssi = wifi_net_get_rssi();
    
    wifi_net_get_credentials(ssid, sizeof(ssid), NULL, 0);
    wifi_net_get_ip(ip, sizeof(ip));
    wifi_net_get_ap_credentials(ap_ssid, sizeof(ap_ssid), NULL, 0);
    
    int preferred_mode = (int)wifi_net_get_preferred_mode();
    
    snprintf(response, sizeof(response), 
        "{\"connected\":%s,\"ap_mode\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"ap_ssid\":\"%s\",\"preferred_mode\":%d,\"rssi\":%d}",
        wifi_net_is_connected() ? "true" : "false",
        wifi_net_is_ap_mode() ? "true" : "false",
        ssid, ip, ap_ssid, preferred_mode, rssi);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

// Guardar nuevas credenciales WiFi
static esp_err_t wifi_config_handler(httpd_req_t *req) {
    char content[200] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Sin datos");
        return ESP_FAIL;
    }
    
    // Buscar mode=, ssid= y pass=
    char *mode_ptr = strstr(content, "mode=");
    char *ssid_ptr = strstr(content, "ssid=");
    char *pass_ptr = strstr(content, "pass=");
    
    if (!ssid_ptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Falta SSID");
        return ESP_FAIL;
    }
    
    // Extraer modo (0=STA, 1=AP)
    int wifi_mode = 0;
    if (mode_ptr) {
        wifi_mode = atoi(mode_ptr + 5);
    }
    
    // Extraer SSID (hasta & o fin)
    char ssid[33] = {0};
    char pass[65] = {0};
    
    ssid_ptr += 5; // Saltar "ssid="
    int i = 0;
    while (*ssid_ptr && *ssid_ptr != '&' && i < 32) {
        if (*ssid_ptr == '%' && ssid_ptr[1] && ssid_ptr[2]) {
            // Decodificar URL encoding
            char hex[3] = {ssid_ptr[1], ssid_ptr[2], 0};
            ssid[i++] = (char)strtol(hex, NULL, 16);
            ssid_ptr += 3;
        } else if (*ssid_ptr == '+') {
            ssid[i++] = ' ';
            ssid_ptr++;
        } else {
            ssid[i++] = *ssid_ptr++;
        }
    }
    
    // Extraer password si existe
    if (pass_ptr) {
        pass_ptr += 5; // Saltar "pass="
        i = 0;
        while (*pass_ptr && *pass_ptr != '&' && i < 64) {
            if (*pass_ptr == '%' && pass_ptr[1] && pass_ptr[2]) {
                char hex[3] = {pass_ptr[1], pass_ptr[2], 0};
                pass[i++] = (char)strtol(hex, NULL, 16);
                pass_ptr += 3;
            } else if (*pass_ptr == '+') {
                pass[i++] = ' ';
                pass_ptr++;
            } else {
                pass[i++] = *pass_ptr++;
            }
        }
    }
    
    esp_err_t err = ESP_OK;
    
    // Guardar modo preferido
    err = wifi_net_set_preferred_mode((wifi_preferred_mode_t)wifi_mode);
    
    // Guardar credenciales según el modo
    if (err == ESP_OK) {
        if (wifi_mode == 1) {
            // Modo AP: guardar credenciales del AP
            err = wifi_net_set_ap_credentials(ssid, pass);
            // Cambiar a modo AP inmediatamente
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(200));
                wifi_net_switch_to_ap();
                ESP_LOGI(TAG, "Red propia activada: %s", ssid);
            }
        } else {
            // Modo STA: guardar credenciales WiFi
            err = wifi_net_set_credentials(ssid, pass);
        }
    }
    
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    return ESP_OK;
}

// Reiniciar dispositivo
static esp_err_t restart_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    
    ESP_LOGI(TAG, "Reiniciando dispositivo por solicitud web...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    
    return ESP_OK;
}

// Resetear credenciales AP a valores por defecto
static esp_err_t reset_ap_handler(httpd_req_t *req) {
    esp_err_t err = wifi_net_set_ap_credentials("CamaraVigia_AP", "seguridad123");
    
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Credenciales AP reseteadas a valores por defecto");
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    return ESP_OK;
}

// Intentar conectar a WiFi
static esp_err_t wifi_connect_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
    esp_err_t err = wifi_net_try_connect();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Intento de conexión WiFi iniciado");
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        ESP_LOGW(TAG, "No se pudo iniciar conexión WiFi");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No hay SSID configurado\"}");
    }
    return ESP_OK;
}

// ============================================================================
// INICIAR SERVIDOR
// ============================================================================
esp_err_t start_webserver(void) {
    // Cargar configuración de movimiento desde NVS
    load_motion_config();
    load_device_config();
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.task_priority = tskIDLE_PRIORITY + 5;
    config.stack_size = 10240;  // Aumentado para operaciones SD
    config.core_id = 1;
    config.max_uri_handlers = 24;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;  // 10 segundos timeout recepción
    config.send_wait_timeout = 10;  // 10 segundos timeout envío

    ESP_LOGI(TAG, "Iniciando servidor en puerto %d", config.server_port);

    if (httpd_start(&server_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando servidor");
        return ESP_FAIL;
    }

    // Handler para favicon (evita 404)
    httpd_uri_t uri_favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler };

    // Registrar endpoints principales
    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    httpd_uri_t uri_files = { .uri = "/api/files", .method = HTTP_GET, .handler = files_handler };
    httpd_uri_t uri_file = { .uri = "/file", .method = HTTP_GET, .handler = file_handler };
    httpd_uri_t uri_delete = { .uri = "/api/delete", .method = HTTP_DELETE, .handler = delete_handler };
    httpd_uri_t uri_delete_all = { .uri = "/api/delete_all", .method = HTTP_DELETE, .handler = delete_all_handler };
    httpd_uri_t uri_format_sd = { .uri = "/api/format_sd", .method = HTTP_POST, .handler = format_sd_handler };
    httpd_uri_t uri_sd_reinit = { .uri = "/api/sd/reinit", .method = HTTP_POST, .handler = sd_reinit_handler };
    httpd_uri_t uri_sd_status = { .uri = "/api/sd/status", .method = HTTP_GET, .handler = sd_status_handler };

    // Endpoints de control de movimiento
    httpd_uri_t uri_motion_status = { .uri = "/api/motion/status", .method = HTTP_GET, .handler = motion_status_handler };
    httpd_uri_t uri_motion_config_get = { .uri = "/api/motion/config", .method = HTTP_GET, .handler = motion_config_handler };
    httpd_uri_t uri_motion_config_post = { .uri = "/api/motion/config", .method = HTTP_POST, .handler = motion_config_handler };
    httpd_uri_t uri_motion_force = { .uri = "/api/motion/force", .method = HTTP_POST, .handler = motion_force_handler };
    httpd_uri_t uri_motion_stop = { .uri = "/api/motion/stop", .method = HTTP_POST, .handler = motion_stop_handler };
    httpd_uri_t uri_manual_capture = { .uri = "/api/capture/manual", .method = HTTP_POST, .handler = manual_capture_handler };
    httpd_uri_t uri_device_config_get = { .uri = "/api/device/config", .method = HTTP_GET, .handler = device_config_handler };
    httpd_uri_t uri_device_config_post = { .uri = "/api/device/config", .method = HTTP_POST, .handler = device_config_handler };

    // Endpoints de WiFi
    httpd_uri_t uri_wifi_status = { .uri = "/api/wifi/status", .method = HTTP_GET, .handler = wifi_status_handler };
    httpd_uri_t uri_wifi_config = { .uri = "/api/wifi/config", .method = HTTP_POST, .handler = wifi_config_handler };
    httpd_uri_t uri_wifi_reset_ap = { .uri = "/api/wifi/reset_ap", .method = HTTP_POST, .handler = reset_ap_handler };
    httpd_uri_t uri_wifi_connect = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_handler };
    httpd_uri_t uri_restart = { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_handler };

    httpd_register_uri_handler(server_httpd, &uri_favicon);
    httpd_register_uri_handler(server_httpd, &uri_index);
    httpd_register_uri_handler(server_httpd, &uri_stream);
    httpd_register_uri_handler(server_httpd, &uri_files);
    httpd_register_uri_handler(server_httpd, &uri_file);
    httpd_register_uri_handler(server_httpd, &uri_delete);
    httpd_register_uri_handler(server_httpd, &uri_delete_all);
    httpd_register_uri_handler(server_httpd, &uri_format_sd);
    httpd_register_uri_handler(server_httpd, &uri_sd_reinit);
    httpd_register_uri_handler(server_httpd, &uri_sd_status);
    httpd_register_uri_handler(server_httpd, &uri_motion_status);
    httpd_register_uri_handler(server_httpd, &uri_motion_config_get);
    httpd_register_uri_handler(server_httpd, &uri_motion_config_post);
    httpd_register_uri_handler(server_httpd, &uri_motion_force);
    httpd_register_uri_handler(server_httpd, &uri_motion_stop);
    httpd_register_uri_handler(server_httpd, &uri_manual_capture);
    httpd_register_uri_handler(server_httpd, &uri_device_config_get);
    httpd_register_uri_handler(server_httpd, &uri_device_config_post);
    httpd_register_uri_handler(server_httpd, &uri_wifi_status);
    httpd_register_uri_handler(server_httpd, &uri_wifi_config);
    httpd_register_uri_handler(server_httpd, &uri_wifi_reset_ap);
    httpd_register_uri_handler(server_httpd, &uri_wifi_connect);
    httpd_register_uri_handler(server_httpd, &uri_restart);

    ESP_LOGI(TAG, "Servidor listo - Modo: %s, Tiempo: %ds", 
             g_capture_mode == CAPTURE_MODE_VIDEO ? "VIDEO" : "FOTO", g_emission_time_sec);
    return ESP_OK;
}

// ============================================================================
// DETENER SERVIDOR
// ============================================================================
esp_err_t stop_webserver(void) {
    if (server_httpd) {
        httpd_stop(server_httpd);
        server_httpd = NULL;
        ESP_LOGI(TAG, "Servidor detenido");
    }
    return ESP_OK;
}
