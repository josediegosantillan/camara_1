idf_component_register(
    SRCS "crypto.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls nvs_flash esp_partition
)
