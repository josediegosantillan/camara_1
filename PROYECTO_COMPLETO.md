# Vigilante ESP32-CAM - Documentación Completa

## Descripción General
Sistema de cámara de seguridad basado en ESP32-CAM (AI-Thinker) con:
- Streaming MJPEG en tiempo real via WiFi
- Captura de fotos automática por detección de movimiento (PIR)
- Almacenamiento encriptado AES-256 en microSD
- Interfaz web para visualizar/borrar archivos
- Modo AP fallback si no hay WiFi

## Hardware Requerido
- **Placa**: ESP32-CAM AI-Thinker
- **Sensor cámara**: OV2640 (incluido)
- **RAM externa**: PSRAM 4MB (incluido en AI-Thinker)
- **Almacenamiento**: MicroSD (FAT32, cualquier capacidad)
- **Sensor PIR**: Conectado a GPIO 13
- **LEDs IR**: Conectados a GPIO 12 (opcional)

## Entorno de Desarrollo
- **ESP-IDF**: v5.5.1
- **Componentes externos**:
  - `espressif/esp32-camera` v2.1.4
  - `espressif/esp_jpeg` v1.3.1

---

## Estructura del Proyecto

```
camara/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   └── main.c
└── components/
    ├── cam_hal/
    │   ├── CMakeLists.txt
    │   ├── cam_hal.c
    │   └── include/cam_hal.h
    ├── sd_hal/
    │   ├── CMakeLists.txt
    │   ├── sd_hal.c
    │   └── include/sd_hal.h
    ├── wifi_net/
    │   ├── CMakeLists.txt
    │   ├── wifi_net.c
    │   └── include/wifi_net.h
    ├── http_server/
    │   ├── CMakeLists.txt
    │   ├── http_server.c
    │   └── include/http_server.h
    └── crypto/
        ├── CMakeLists.txt
        ├── crypto.c
        └── include/crypto.h
```

---

## Archivos de Configuración

### CMakeLists.txt (raíz)
```cmake
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Vigilante_ESP32)
```

### partitions.csv
```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x5000,
otadata,  data, ota,     ,        0x2000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        2500K,
coredump, data, coredump,,        64K,
```

### sdkconfig.defaults
```ini
# --- MEMORIA RAM Y VELOCIDAD (HARDWARE) ---
CONFIG_ESP32_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_MODE_OCT=n
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_SPIRAM_CACHE_WORKAROUND=y
CONFIG_OV2640_SUPPORT=y
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y

# --- OPTIMIZACIÓN DE LA PILA TCP/IP ---
CONFIG_LWIP_TCP_WND_DEFAULT=32768
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=32768
CONFIG_LWIP_MAX_ACTIVE_TCP=16
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_SO_RCVBUF=y
CONFIG_LWIP_IRAM_OPTIMIZATION=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y

# --- OPTIMIZACIÓN DEL SERVIDOR HTTP ---
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_ERR_RESP_NO_DELAY=y
```

---

## Componente: cam_hal (Cámara)

### cam_hal/CMakeLists.txt
```cmake
idf_component_register(SRCS "cam_hal.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp32-camera driver)
```

### cam_hal/include/cam_hal.h
```c
#pragma once
#include "esp_err.h"

esp_err_t camera_init_hardware(void);
```

### cam_hal/cam_hal.c
```c
#include "cam_hal.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "sdkconfig.h"

static const char *TAG = "CAM_HAL";

// Pines AI-Thinker
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

esp_err_t camera_init_hardware(void) {
    camera_config_t config = {0};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    
    config.xclk_freq_hz = 24000000;  // 24MHz para más FPS
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA;  // 640x480
    config.jpeg_quality = 12;  // 12-15 buena calidad

#if CONFIG_SPIRAM
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 3;  // Triple buffer para streaming fluido
#else
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 2;
#endif
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo al iniciar camara: 0x%x", err);
        return err;
    }
    return ESP_OK;
}
```

---

## Componente: sd_hal (MicroSD)

### sd_hal/CMakeLists.txt
```cmake
idf_component_register(SRCS "sd_hal.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fatfs sdmmc esp_vfs_fat)
```

### sd_hal/include/sd_hal.h
```c
#pragma once
#include "esp_err.h"

esp_err_t sd_card_init(void);
```

### sd_hal/sd_hal.c
```c
#include "sd_hal.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "SD_HAL";

esp_err_t sd_card_init(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;  // Modo 1-bit para liberar GPIO4

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Fallo al montar: Verifique que la tarjeta este puesta");
        } else {
            ESP_LOGE(TAG, "Fallo al iniciar SD (%s)", esp_err_to_name(ret));
        }
    }
    return ret;
}
```

---

## Componente: wifi_net (WiFi)

### wifi_net/CMakeLists.txt
```cmake
idf_component_register(SRCS "wifi_net.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi esp_event nvs_flash)
```

### wifi_net/include/wifi_net.h
```c
#pragma once

void wifi_net_init(void);
```

### wifi_net/wifi_net.c
```c
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

// CONFIGURACIÓN - Cambiar estos valores
#define WIFI_SSID      "TU_SSID_WIFI"
#define WIFI_PASS      "TU_PASSWORD_WIFI"
#define AP_SSID        "ESP32_CAM"
#define AP_PASS        "seguridad123"
#define MAX_RETRY      5

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Reintentando conectar al WiFi... (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
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
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
    }
}

void wifi_net_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Sistema WiFi Iniciado. Intentando conectar a %s...", WIFI_SSID);
}
```

---

## Componente: crypto (Encriptación AES-256)

### crypto/CMakeLists.txt
```cmake
idf_component_register(SRCS "crypto.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mbedtls nvs_flash esp_partition)
```

### crypto/include/crypto.h
```c
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t crypto_init(void);
int crypto_encrypt(const uint8_t *input, size_t input_len, 
                   uint8_t *output, size_t output_max_len);
int crypto_decrypt(const uint8_t *input, size_t input_len,
                   uint8_t *output, size_t output_max_len);
esp_err_t crypto_save_file(const char *filename, const uint8_t *data, size_t len);
uint8_t* crypto_load_file(const char *filename, size_t *out_len);
```

### crypto/crypto.c
```c
#include "crypto.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "mbedtls/aes.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "CRYPTO";
static uint8_t aes_key[32];
static bool crypto_initialized = false;

#define NVS_NAMESPACE "crypto"
#define NVS_KEY_NAME "aes_key"

static esp_err_t generate_random_key(uint8_t *key, size_t len) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "esp32_cam_crypto";
    
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)pers, strlen(pers));
    if (ret != 0) return ESP_FAIL;
    
    ret = mbedtls_ctr_drbg_random(&ctr_drbg, key, len);
    
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t crypto_init(void) {
    if (crypto_initialized) return ESP_OK;
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    
    size_t key_len = sizeof(aes_key);
    err = nvs_get_blob(nvs_handle, NVS_KEY_NAME, aes_key, &key_len);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Generando nueva clave AES-256...");
        if (generate_random_key(aes_key, sizeof(aes_key)) != ESP_OK) {
            nvs_close(nvs_handle);
            return ESP_FAIL;
        }
        err = nvs_set_blob(nvs_handle, NVS_KEY_NAME, aes_key, sizeof(aes_key));
        if (err == ESP_OK) err = nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Clave AES-256 generada y guardada en NVS");
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "Clave AES-256 cargada desde NVS");
    }
    
    nvs_close(nvs_handle);
    crypto_initialized = (err == ESP_OK);
    return err;
}

static size_t add_pkcs7_padding(uint8_t *data, size_t data_len, size_t block_size) {
    size_t padding = block_size - (data_len % block_size);
    for (size_t i = 0; i < padding; i++) {
        data[data_len + i] = (uint8_t)padding;
    }
    return data_len + padding;
}

static size_t remove_pkcs7_padding(uint8_t *data, size_t data_len) {
    if (data_len == 0) return 0;
    uint8_t padding = data[data_len - 1];
    if (padding > 16 || padding == 0) return data_len;
    return data_len - padding;
}

int crypto_encrypt(const uint8_t *input, size_t input_len,
                   uint8_t *output, size_t output_max_len) {
    if (!crypto_initialized) return -1;
    
    size_t padded_len = ((input_len / 16) + 1) * 16;
    size_t total_len = 16 + padded_len;
    
    if (output_max_len < total_len) return -1;
    
    uint8_t iv[16];
    if (generate_random_key(iv, 16) != ESP_OK) return -1;
    memcpy(output, iv, 16);
    
    uint8_t *padded_data = malloc(padded_len);
    if (!padded_data) return -1;
    
    memcpy(padded_data, input, input_len);
    add_pkcs7_padding(padded_data, input_len, 16);
    
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, aes_key, 256);
    
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    
    int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded_len,
                                    iv_copy, padded_data, output + 16);
    
    mbedtls_aes_free(&aes);
    free(padded_data);
    
    return (ret == 0) ? (int)total_len : -1;
}

int crypto_decrypt(const uint8_t *input, size_t input_len,
                   uint8_t *output, size_t output_max_len) {
    if (!crypto_initialized || input_len < 32) return -1;
    
    size_t cipher_len = input_len - 16;
    if (output_max_len < cipher_len) return -1;
    
    uint8_t iv[16];
    memcpy(iv, input, 16);
    
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, aes_key, 256);
    
    int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipher_len,
                                    iv, input + 16, output);
    
    mbedtls_aes_free(&aes);
    
    if (ret != 0) return -1;
    return (int)remove_pkcs7_padding(output, cipher_len);
}

esp_err_t crypto_save_file(const char *filename, const uint8_t *data, size_t len) {
    if (!crypto_initialized) return ESP_FAIL;
    
    size_t enc_size = 16 + ((len / 16) + 1) * 16;
    uint8_t *enc_data = heap_caps_malloc(enc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc_data) enc_data = malloc(enc_size);
    if (!enc_data) return ESP_ERR_NO_MEM;
    
    int enc_len = crypto_encrypt(data, len, enc_data, enc_size);
    if (enc_len < 0) { free(enc_data); return ESP_FAIL; }
    
    char filepath[320];
    snprintf(filepath, sizeof(filepath), "/sdcard/%s.enc", filename);
    
    FILE *f = fopen(filepath, "wb");
    if (!f) { free(enc_data); return ESP_FAIL; }
    
    uint32_t orig_size = (uint32_t)len;
    fwrite(&orig_size, sizeof(orig_size), 1, f);
    size_t written = fwrite(enc_data, 1, enc_len, f);
    fclose(f);
    free(enc_data);
    
    return (written == (size_t)enc_len) ? ESP_OK : ESP_FAIL;
}

uint8_t* crypto_load_file(const char *filename, size_t *out_len) {
    if (!crypto_initialized) return NULL;
    
    char filepath[320];
    if (strstr(filename, ".enc") != NULL) {
        snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);
    } else {
        snprintf(filepath, sizeof(filepath), "/sdcard/%s.enc", filename);
    }
    
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;
    
    uint32_t orig_size;
    if (fread(&orig_size, sizeof(orig_size), 1, f) != 1) { fclose(f); return NULL; }
    
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, sizeof(orig_size), SEEK_SET);
    
    size_t enc_size = file_size - sizeof(orig_size);
    uint8_t *enc_data = heap_caps_malloc(enc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc_data) enc_data = malloc(enc_size);
    if (!enc_data) { fclose(f); return NULL; }
    
    if (fread(enc_data, 1, enc_size, f) != enc_size) { free(enc_data); fclose(f); return NULL; }
    fclose(f);
    
    uint8_t *dec_data = heap_caps_malloc(orig_size + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dec_data) dec_data = malloc(orig_size + 16);
    if (!dec_data) { free(enc_data); return NULL; }
    
    int dec_len = crypto_decrypt(enc_data, enc_size, dec_data, orig_size + 16);
    free(enc_data);
    
    if (dec_len < 0) { free(dec_data); return NULL; }
    
    *out_len = (size_t)dec_len;
    return dec_data;
}
```

---

## Componente: http_server (Servidor Web)

### http_server/CMakeLists.txt
```cmake
idf_component_register(SRCS "http_server.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_server esp32-camera esp_timer crypto)
```

### http_server/include/http_server.h
```c
#pragma once
#include "esp_err.h"

esp_err_t start_webserver(void);
esp_err_t stop_webserver(void);
```

### http_server/http_server.c
El código completo está en el archivo del proyecto. Incluye:
- Página HTML embebida con interfaz responsive
- Streaming MJPEG optimizado con TCP_NODELAY
- API REST: `/api/files`, `/file`, `/api/delete`, `/api/delete_all`
- Desencriptación automática de archivos `.enc`

---

## main/main.c

### main/CMakeLists.txt
```cmake
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES cam_hal sd_hal wifi_net http_server crypto esp32-camera)
```

### Flujo Principal
1. Inicializa NVS
2. Configura periféricos (PIR en GPIO13, LEDs IR en GPIO12)
3. Inicializa cámara
4. Monta SD card
5. Inicializa encriptación (genera clave AES-256 única)
6. Inicia WiFi (STA → AP fallback)
7. Inicia servidor web
8. Loop: detecta movimiento → captura foto encriptada

---

## Endpoints Web

| Endpoint | Método | Descripción |
|----------|--------|-------------|
| `/` | GET | Página web principal |
| `/stream` | GET | Stream MJPEG en vivo |
| `/api/files` | GET | Lista JSON de archivos |
| `/file?name=X` | GET | Descarga archivo (desencripta .enc) |
| `/api/delete?name=X` | DELETE | Borra un archivo |
| `/api/delete_all` | DELETE | Borra todos los archivos |

---

## Conexión y Uso

1. **Primera vez**: El ESP32 intenta conectar al WiFi configurado
2. **Si falla**: Crea AP propio (`ESP32_CAM` / `seguridad123`)
3. **IP en modo AP**: `192.168.4.1`
4. **Acceso**: Abrir navegador → `http://192.168.4.1`

---

## Seguridad

- Fotos guardadas como `.enc` con AES-256-CBC
- Clave generada aleatoriamente y almacenada en NVS (flash interno)
- IV aleatorio por archivo
- Si extraen la SD, los archivos son ilegibles
- Desencriptación solo via interfaz web del ESP32

---

## Optimizaciones Implementadas

| Área | Configuración | Efecto |
|------|--------------|--------|
| XCLK | 24MHz | +FPS |
| Frame buffers | 3 en PSRAM | Pipeline sin esperas |
| JPEG quality | 12 | Balance calidad/velocidad |
| TCP buffers | 32KB | Menos fragmentación |
| TCP_NODELAY | Habilitado | Baja latencia streaming |
| Core affinity | Server en Core 1 | No interfiere con cámara |
| GRAB_LATEST | Habilitado | Siempre frame más reciente |

---

## Compilación

```bash
# Configurar ESP-IDF
. $IDF_PATH/export.sh

# Compilar
idf.py build

# Flash
idf.py -p COM8 flash

# Monitor
idf.py -p COM8 monitor
```
