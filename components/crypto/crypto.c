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

// Clave AES-256 (32 bytes)
static uint8_t aes_key[32];
static bool crypto_initialized = false;

// Namespace NVS para la clave
#define NVS_NAMESPACE "crypto"
#define NVS_KEY_NAME "aes_key"

// Genera clave aleatoria usando el RNG del ESP32
static esp_err_t generate_random_key(uint8_t *key, size_t len) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "esp32_cam_crypto";
    
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "Error inicializando DRBG: %d", ret);
        return ESP_FAIL;
    }
    
    ret = mbedtls_ctr_drbg_random(&ctr_drbg, key, len);
    
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t crypto_init(void) {
    if (crypto_initialized) return ESP_OK;
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Abrir NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    // Intentar leer clave existente
    size_t key_len = sizeof(aes_key);
    err = nvs_get_blob(nvs_handle, NVS_KEY_NAME, aes_key, &key_len);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // No existe clave, generar una nueva
        ESP_LOGI(TAG, "Generando nueva clave AES-256...");
        
        if (generate_random_key(aes_key, sizeof(aes_key)) != ESP_OK) {
            nvs_close(nvs_handle);
            return ESP_FAIL;
        }
        
        // Guardar la clave
        err = nvs_set_blob(nvs_handle, NVS_KEY_NAME, aes_key, sizeof(aes_key));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error guardando clave: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            return err;
        }
        
        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error en commit NVS: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            return err;
        }
        
        ESP_LOGI(TAG, "Clave AES-256 generada y guardada en NVS");
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "Clave AES-256 cargada desde NVS");
    } else {
        ESP_LOGE(TAG, "Error leyendo clave: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    nvs_close(nvs_handle);
    crypto_initialized = true;
    return ESP_OK;
}

// Padding PKCS7
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
    if (!crypto_initialized) {
        ESP_LOGE(TAG, "Crypto no inicializado");
        return -1;
    }
    
    // Calcular tamaño con padding
    size_t padded_len = ((input_len / 16) + 1) * 16;
    size_t total_len = 16 + padded_len;  // IV (16) + datos cifrados
    
    if (output_max_len < total_len) {
        ESP_LOGE(TAG, "Buffer muy pequeño: necesita %u, tiene %u", 
                 (unsigned)total_len, (unsigned)output_max_len);
        return -1;
    }
    
    // Generar IV aleatorio
    uint8_t iv[16];
    if (generate_random_key(iv, 16) != ESP_OK) {
        ESP_LOGE(TAG, "Error generando IV");
        return -1;
    }
    
    // Copiar IV al inicio del output
    memcpy(output, iv, 16);
    
    // Preparar datos con padding
    uint8_t *padded_data = malloc(padded_len);
    if (!padded_data) {
        ESP_LOGE(TAG, "Sin memoria para padding");
        return -1;
    }
    
    memcpy(padded_data, input, input_len);
    add_pkcs7_padding(padded_data, input_len, 16);
    
    // Encriptar con AES-256-CBC
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, aes_key, 256);
    
    // CBC necesita IV mutable
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    
    int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded_len,
                                    iv_copy, padded_data, output + 16);
    
    mbedtls_aes_free(&aes);
    free(padded_data);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "Error en AES encrypt: %d", ret);
        return -1;
    }
    
    return (int)total_len;
}

int crypto_decrypt(const uint8_t *input, size_t input_len,
                   uint8_t *output, size_t output_max_len) {
    if (!crypto_initialized) {
        ESP_LOGE(TAG, "Crypto no inicializado");
        return -1;
    }
    
    if (input_len < 32) {  // Mínimo: 16 IV + 16 datos
        ESP_LOGE(TAG, "Datos muy cortos para desencriptar");
        return -1;
    }
    
    size_t cipher_len = input_len - 16;  // Sin IV
    
    if (output_max_len < cipher_len) {
        ESP_LOGE(TAG, "Buffer muy pequeño");
        return -1;
    }
    
    // Extraer IV
    uint8_t iv[16];
    memcpy(iv, input, 16);
    
    // Desencriptar
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, aes_key, 256);
    
    int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipher_len,
                                    iv, input + 16, output);
    
    mbedtls_aes_free(&aes);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "Error en AES decrypt: %d", ret);
        return -1;
    }
    
    // Remover padding
    size_t real_len = remove_pkcs7_padding(output, cipher_len);
    return (int)real_len;
}

esp_err_t crypto_save_file(const char *filename, const uint8_t *data, size_t len) {
    if (!crypto_initialized) {
        ESP_LOGE(TAG, "Crypto no inicializado");
        return ESP_FAIL;
    }
    
    // Calcular tamaño encriptado
    size_t enc_size = 16 + ((len / 16) + 1) * 16;
    
    // Buffer para datos encriptados (usar PSRAM si está disponible)
    uint8_t *enc_data = heap_caps_malloc(enc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc_data) {
        enc_data = malloc(enc_size);
    }
    
    if (!enc_data) {
        ESP_LOGE(TAG, "Sin memoria para encriptar");
        return ESP_ERR_NO_MEM;
    }
    
    // Encriptar
    int enc_len = crypto_encrypt(data, len, enc_data, enc_size);
    if (enc_len < 0) {
        free(enc_data);
        return ESP_FAIL;
    }
    
    // Construir nombre de archivo .enc
    char filepath[320];
    snprintf(filepath, sizeof(filepath), "/sdcard/%s.enc", filename);
    
    // Guardar
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "No se puede crear archivo: %s", filepath);
        free(enc_data);
        return ESP_FAIL;
    }
    
    // Escribir header con tamaño original (para desencriptar)
    uint32_t orig_size = (uint32_t)len;
    fwrite(&orig_size, sizeof(orig_size), 1, f);
    
    // Escribir datos encriptados
    size_t written = fwrite(enc_data, 1, enc_len, f);
    fclose(f);
    free(enc_data);
    
    if (written != (size_t)enc_len) {
        ESP_LOGE(TAG, "Error escribiendo archivo");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Archivo encriptado guardado: %s (%u -> %d bytes)", 
             filename, (unsigned)len, enc_len);
    return ESP_OK;
}

uint8_t* crypto_load_file(const char *filename, size_t *out_len) {
    if (!crypto_initialized) {
        ESP_LOGE(TAG, "Crypto no inicializado");
        return NULL;
    }
    
    char filepath[320];
    
    // Verificar si el nombre ya tiene .enc
    if (strstr(filename, ".enc") != NULL) {
        snprintf(filepath, sizeof(filepath), "/sdcard/%s", filename);
    } else {
        snprintf(filepath, sizeof(filepath), "/sdcard/%s.enc", filename);
    }
    
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "No se puede abrir: %s", filepath);
        return NULL;
    }
    
    // Leer tamaño original
    uint32_t orig_size;
    if (fread(&orig_size, sizeof(orig_size), 1, f) != 1) {
        ESP_LOGE(TAG, "Error leyendo header");
        fclose(f);
        return NULL;
    }
    
    // Obtener tamaño del archivo
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, sizeof(orig_size), SEEK_SET);
    
    size_t enc_size = file_size - sizeof(orig_size);
    
    // Leer datos encriptados
    uint8_t *enc_data = heap_caps_malloc(enc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc_data) {
        enc_data = malloc(enc_size);
    }
    
    if (!enc_data) {
        ESP_LOGE(TAG, "Sin memoria para leer archivo");
        fclose(f);
        return NULL;
    }
    
    if (fread(enc_data, 1, enc_size, f) != enc_size) {
        ESP_LOGE(TAG, "Error leyendo datos");
        free(enc_data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    
    // Desencriptar
    uint8_t *dec_data = heap_caps_malloc(orig_size + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dec_data) {
        dec_data = malloc(orig_size + 16);
    }
    
    if (!dec_data) {
        ESP_LOGE(TAG, "Sin memoria para desencriptar");
        free(enc_data);
        return NULL;
    }
    
    int dec_len = crypto_decrypt(enc_data, enc_size, dec_data, orig_size + 16);
    free(enc_data);
    
    if (dec_len < 0) {
        free(dec_data);
        return NULL;
    }
    
    *out_len = (size_t)dec_len;
    ESP_LOGI(TAG, "Archivo desencriptado: %s (%u bytes)", filename, (unsigned)dec_len);
    return dec_data;
}
