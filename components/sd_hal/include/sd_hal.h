#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t sd_card_init(void);
esp_err_t sd_card_format(void);
bool sd_card_is_mounted(void);
esp_err_t sd_card_reinit(void);
