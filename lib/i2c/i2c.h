#ifndef I2C_H
#define I2C_H

#include "driver/i2c_master.h"
#include "esp_log.h"

#define I2C_MASTER_NUM         0
#define I2C_MASTER_SCL_IO      41
#define I2C_MASTER_SDA_IO      42
#define I2C_MASTER_FREQ_HZ     100000

// Убираем определения переменных из заголовочного файла
// Используем extern, чтобы объявить их как внешние

extern i2c_master_bus_handle_t i2c_bus;
extern i2c_master_bus_config_t bus_config;

void init_i2c(void);

#endif