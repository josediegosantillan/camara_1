idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES cam_hal sd_hal wifi_net http_server crypto expander_hal esp32-camera)
                    