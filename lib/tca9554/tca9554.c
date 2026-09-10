#include "tca9554.h"
#include "esp_log.h"

static const char *TAG = "TCA9554";

// Запись в регистр TCA9554
static esp_err_t tca9554_write_reg(tca9554_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(dev->i2c_dev, data, sizeof(data), -1);
}

// Инициализация TCA9554
esp_err_t tca9554_init(tca9554_t *dev, i2c_master_bus_handle_t bus, uint8_t addr)
{
    // Настройка I2C устройства
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device");
        return err;
    }
    
    // Настройка всех пинов как выходы (0x00)
    err = tca9554_write_reg(dev, TCA9554_REG_CONFIG, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TCA9554");
        return err;
    }
    
    dev->output_mask = 0x00;
    
    // Выключаем все реле
    err = tca9554_write_reg(dev, TCA9554_REG_OUTPUT, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set initial output");
        return err;
    }
    
    ESP_LOGI(TAG, "TCA9554 initialized at address 0x%02X", addr);
    return ESP_OK;
}

// Установить состояние всех реле
esp_err_t tca9554_set_output(tca9554_t *dev, uint8_t mask)
{
    dev->output_mask = mask;
    return tca9554_write_reg(dev, TCA9554_REG_OUTPUT, mask);
}

// Включить/выключить одно реле (0-7)
esp_err_t tca9554_set_relay(tca9554_t *dev, uint8_t relay_num, uint8_t state)
{
    if (relay_num > 7) return ESP_ERR_INVALID_ARG;
    
    if (state) {
        dev->output_mask |= (1 << relay_num);
    } else {
        dev->output_mask &= ~(1 << relay_num);
    }
    
    return tca9554_write_reg(dev, TCA9554_REG_OUTPUT, dev->output_mask);
}

// Получить текущее состояние реле
uint8_t tca9554_get_output(tca9554_t *dev)
{
    return dev->output_mask;
}