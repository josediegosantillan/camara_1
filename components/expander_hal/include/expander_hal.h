#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Definimos en qué pin del PCF8574 está cada cosa
#define PCF_PIN_IR_LEDS    0  // P0 del PCF8574
#define PCF_PIN_PIR        1  // P1 del PCF8574 (opcional, para liberar el GPIO12)

esp_err_t expander_init(void);
esp_err_t expander_set_ir(bool state);
bool expander_read_pir(void);
