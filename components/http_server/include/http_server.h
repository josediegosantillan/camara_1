#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

// Iniciar servidor web
esp_err_t start_webserver(void);

// Detener servidor web
esp_err_t stop_webserver(void);
