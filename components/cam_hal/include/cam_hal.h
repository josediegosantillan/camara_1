#pragma once
#include "esp_err.h"
#include "esp_camera.h"
#include <stdbool.h>

#define CAM_HAL_DEFAULT_JPEG_QUALITY 12
#define CAM_HAL_DEFAULT_FRAME_SIZE FRAMESIZE_VGA

esp_err_t camera_init_hardware(void);
esp_err_t cam_hal_set_jpeg_quality(int quality);
int cam_hal_get_jpeg_quality(void);
esp_err_t cam_hal_set_frame_size(framesize_t frame_size);
framesize_t cam_hal_get_frame_size(void);
const char *cam_hal_get_frame_size_name(framesize_t frame_size);
esp_err_t cam_hal_set_flash_led(bool on);
bool cam_hal_get_flash_led(void);
