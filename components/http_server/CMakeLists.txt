idf_component_register(SRCS "http_server.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_server esp32-camera esp_timer crypto wifi_net nvs_flash sd_hal)
                    
