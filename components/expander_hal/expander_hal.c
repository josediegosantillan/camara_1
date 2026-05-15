#include "expander_hal.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
static const char *TAG = "EXPANDER_HAL";
#define I2C_TIMEOUT_MS     100
#define PCF8574_ADDR       0x20  // Ajusta esto segun los jumpers de tu placa
#define PCF_I2C_PORT       I2C_NUM_0
#define PCF_I2C_SCL_GPIO   14
#define PCF_I2C_SDA_GPIO   15

static i2c_master_bus_handle_t pcf_bus = NULL;
static i2c_master_dev_handle_t pcf_dev = NULL;

esp_err_t expander_init(void) {
    ESP_LOGI(TAG, "Inicializando expansor PCF8574 en direccion 0x%02X", PCF8574_ADDR);

    if (pcf_dev) {
        return ESP_OK;
    }

    if (!pcf_bus) {
        i2c_master_bus_config_t bus_cfg = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = PCF_I2C_PORT,
            .scl_io_num = PCF_I2C_SCL_GPIO,
            .sda_io_num = PCF_I2C_SDA_GPIO,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = 1,
        };

        esp_err_t ret = i2c_new_master_bus(&bus_cfg, &pcf_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo crear el bus I2C para PCF8574. err=0x%x", ret);
            return ret;
        }
    }

    esp_err_t ret = i2c_master_probe(pcf_bus, PCF8574_ADDR, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF8574 no responde en 0x%02X (err=%s). Expansor deshabilitado.",
                 PCF8574_ADDR, esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8574_ADDR,
        .scl_speed_hz = 100000,
    };

    ret = i2c_master_bus_add_device(pcf_bus, &dev_cfg, &pcf_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo registrar el PCF8574 en el bus I2C. err=0x%x", ret);
        return ret;
    }

    return ESP_OK;
}

esp_err_t expander_set_ir(bool state) {
    // 0xFF apaga todo, 0xFE prende P0 (asumiendo logica negativa/transistor)
    uint8_t data = state ? 0xFE : 0xFF;
    if (!pcf_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(pcf_dev, &data, 1, I2C_TIMEOUT_MS);
}

bool expander_read_pir(void) {
    uint8_t data = 0;
    if (!pcf_dev) {
        return false;
    }
    if (i2c_master_receive(pcf_dev, &data, 1, I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    // Retorna true si el pin P1 esta en HIGH
    return (data & (1 << PCF_PIN_PIR));
}
