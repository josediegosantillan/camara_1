#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Inicializa el módulo crypto (carga/genera clave desde NVS)
esp_err_t crypto_init(void);

// Encripta datos en memoria
// output debe tener espacio para input_len + 16 bytes (padding + IV)
// Retorna el tamaño real del output
int crypto_encrypt(const uint8_t *input, size_t input_len, 
                   uint8_t *output, size_t output_max_len);

// Desencripta datos en memoria
// Retorna el tamaño real del output (datos originales)
int crypto_decrypt(const uint8_t *input, size_t input_len,
                   uint8_t *output, size_t output_max_len);

// Guarda archivo encriptado en SD
esp_err_t crypto_save_file(const char *filename, const uint8_t *data, size_t len);

// Lee y desencripta archivo de SD
// Retorna puntero a datos desencriptados (debe liberarse con free())
// out_len contiene el tamaño de los datos
uint8_t* crypto_load_file(const char *filename, size_t *out_len);
