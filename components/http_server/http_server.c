#include "http_server.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "crypto.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t server_httpd = NULL;

// ============================================================================
// CONFIGURACIÓN
// ============================================================================
#define MOUNT_POINT "/sdcard"
#define MAX_FILES 50

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
// PÁGINA HTML PRINCIPAL (Ultra ligera, sin frameworks)
// ============================================================================
static const char* HTML_INDEX = 
"<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Vigilante ESP32</title><style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;padding:10px}"
"h1{color:#0f0;font-size:1.2em;margin-bottom:10px}"
".btn{background:#16213e;border:1px solid #0f3460;color:#eee;padding:8px 12px;margin:3px;cursor:pointer;border-radius:4px;text-decoration:none;display:inline-block;font-size:0.9em}"
".btn:hover{background:#0f3460}.btn-danger{background:#a00;border-color:#f00}"
".btn-danger:hover{background:#c00}"
"#stream{width:100%;max-width:640px;border:2px solid #0f3460;margin:10px 0}"
".files{margin-top:15px}.file{background:#16213e;padding:8px;margin:5px 0;border-radius:4px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap}"
".file-name{flex:1;min-width:150px;word-break:break-all}.file-info{color:#888;font-size:0.8em;margin:0 10px}"
".file-actions{display:flex;gap:5px}"
".tab{display:inline-block;padding:10px 15px;cursor:pointer;background:#16213e;border-radius:4px 4px 0 0}"
".tab.active{background:#0f3460}.panel{display:none;padding:15px;background:#16213e;border-radius:0 4px 4px 4px}"
".panel.active{display:block}.status{background:#0a0;padding:5px 10px;border-radius:4px;margin:5px 0;font-size:0.85em}"
"</style></head><body>"
"<h1>🎥 camara vigia :)</h1>"
"<div><span class='tab active' onclick='showTab(0)'>📹 Stream</span><span class='tab' onclick='showTab(1)'>📁 Archivos</span></div>"
"<div class='panel active' id='p0'><img id='stream' src='/stream' alt='Video Stream'><br>"
"<button class='btn' onclick='location.reload()'>🔄 Reconectar</button></div>"
"<div class='panel' id='p1'><div class='status' id='status'>Cargando...</div>"
"<button class='btn' onclick='loadFiles()'>🔄 Actualizar</button>"
"<button class='btn btn-danger' onclick='deleteAll()'>🗑️ Borrar Todo</button>"
"<div class='files' id='files'></div></div>"
"<script>"
"function showTab(n){document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',i==n));"
"document.querySelectorAll('.panel').forEach((p,i)=>p.classList.toggle('active',i==n));if(n==1)loadFiles();}"
"function loadFiles(){fetch('/api/files').then(r=>r.json()).then(d=>{"
"document.getElementById('status').textContent='Encontrados: '+d.count+' archivos ('+formatSize(d.total_size)+')';"
"let h='';d.files.forEach(f=>{"
"h+='<div class=\"file\"><span class=\"file-name\">'+f.name+'</span>';"
"h+='<span class=\"file-info\">'+formatSize(f.size)+' | '+formatDate(f.mtime)+'</span>';"
"h+='<div class=\"file-actions\"><a class=\"btn\" href=\"/file?name='+encodeURIComponent(f.name)+'\" target=\"_blank\">👁️</a>';"
"h+='<button class=\"btn btn-danger\" onclick=\"deleteFile(\\''+f.name+'\\');\">🗑️</button></div></div>';});"
"document.getElementById('files').innerHTML=h||'<p>No hay archivos</p>';}).catch(e=>"
"document.getElementById('status').textContent='Error: '+e);}"
"function deleteFile(n){if(confirm('¿Borrar '+n+'?'))fetch('/api/delete?name='+encodeURIComponent(n),{method:'DELETE'})"
".then(r=>r.ok?loadFiles():alert('Error al borrar'));}"
"function deleteAll(){if(confirm('¿BORRAR TODOS los archivos?'))fetch('/api/delete_all',{method:'DELETE'})"
".then(r=>r.ok?loadFiles():alert('Error'));}"
"function formatSize(b){if(b<1024)return b+'B';if(b<1048576)return(b/1024).toFixed(1)+'KB';return(b/1048576).toFixed(1)+'MB';}"
"function formatDate(t){let d=new Date(t*1000);return d.toLocaleDateString()+' '+d.toLocaleTimeString();}"
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
// HANDLER: STREAM MJPEG (OPTIMIZADO)
// ============================================================================
static esp_err_t stream_handler(httpd_req_t *req) {
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
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Fallo camara");
            res = ESP_FAIL;
            break;
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
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se puede abrir SD");
        return ESP_FAIL;
    }

    file_info_t *files = malloc(sizeof(file_info_t) * MAX_FILES);
    if (!files) {
        closedir(dir);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Sin memoria");
        return ESP_FAIL;
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

    // Verificar si es archivo encriptado (.enc)
    const char *ext = strrchr(filename, '.');
    bool is_encrypted = (ext && strcasecmp(ext, ".enc") == 0);
    
    if (is_encrypted) {
        // Desencriptar y enviar
        size_t dec_len = 0;
        uint8_t *dec_data = crypto_load_file(filename, &dec_len);
        
        if (!dec_data) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error desencriptando");
            return ESP_FAIL;
        }
        
        // Los archivos encriptados son JPEGs
        httpd_resp_set_type(req, "image/jpeg");
        
        // Header para descarga con nombre original (sin .enc)
        char disp_header[128];
        char orig_name[64];
        strncpy(orig_name, filename, sizeof(orig_name) - 1);
        char *enc_ext = strstr(orig_name, ".enc");
        if (enc_ext) {
            strcpy(enc_ext, ".jpg");  // Renombrar a .jpg
        }
        snprintf(disp_header, sizeof(disp_header), "inline; filename=\"%s\"", orig_name);
        httpd_resp_set_hdr(req, "Content-Disposition", disp_header);
        
        httpd_resp_send(req, (const char*)dec_data, dec_len);
        free(dec_data);
        return ESP_OK;
    }
    
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

    // Seguridad
    if (strstr(filename, "..") || filename[0] == '/') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nombre invalido");
        return ESP_FAIL;
    }

    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);
    
    if (remove(filepath) == 0) {
        ESP_LOGI(TAG, "Archivo borrado: %s", filename);
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error al borrar");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// ============================================================================
// HANDLER: BORRAR TODOS LOS ARCHIVOS
// ============================================================================
static esp_err_t delete_all_handler(httpd_req_t *req) {
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se puede abrir SD");
        return ESP_FAIL;
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// Handler para favicon (evita warnings 404)
static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ============================================================================
// INICIAR SERVIDOR
// ============================================================================
esp_err_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.task_priority = tskIDLE_PRIORITY + 5;
    config.stack_size = 8192;
    config.core_id = 1;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Iniciando servidor en puerto %d", config.server_port);

    if (httpd_start(&server_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando servidor");
        return ESP_FAIL;
    }

    // Handler para favicon (evita 404)
    httpd_uri_t uri_favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler };

    // Registrar endpoints
    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    httpd_uri_t uri_files = { .uri = "/api/files", .method = HTTP_GET, .handler = files_handler };
    httpd_uri_t uri_file = { .uri = "/file", .method = HTTP_GET, .handler = file_handler };
    httpd_uri_t uri_delete = { .uri = "/api/delete", .method = HTTP_DELETE, .handler = delete_handler };
    httpd_uri_t uri_delete_all = { .uri = "/api/delete_all", .method = HTTP_DELETE, .handler = delete_all_handler };

    httpd_register_uri_handler(server_httpd, &uri_favicon);
    httpd_register_uri_handler(server_httpd, &uri_index);
    httpd_register_uri_handler(server_httpd, &uri_stream);
    httpd_register_uri_handler(server_httpd, &uri_files);
    httpd_register_uri_handler(server_httpd, &uri_file);
    httpd_register_uri_handler(server_httpd, &uri_delete);
    httpd_register_uri_handler(server_httpd, &uri_delete_all);

    ESP_LOGI(TAG, "Servidor listo - Endpoints: /, /stream, /api/files, /file, /api/delete");
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
