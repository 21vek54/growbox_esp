#ifndef TCA9554_H
#define TCA9554_H

#include <stdint.h>
#include "driver/i2c_master.h"

// Регистры TCA9554
#define TCA9554_REG_INPUT      0x00
#define TCA9554_REG_OUTPUT     0x01
#define TCA9554_REG_POLARITY   0x02
#define TCA9554_REG_CONFIG     0x03

// Структура для хранения состояния драйвера
typedef struct {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t output_mask;      // Текущее состояние реле (бит 0 = реле 1)
} tca9554_t;

// Инициализация TCA9554
esp_err_t tca9554_init(tca9554_t *dev, i2c_master_bus_handle_t bus, uint8_t addr);

// Установить состояние всех реле (mask: бит 0 = реле 1, бит 7 = реле 8)
esp_err_t tca9554_set_output(tca9554_t *dev, uint8_t mask);

// Включить одно реле (номер 0-7)
esp_err_t tca9554_set_relay(tca9554_t *dev, uint8_t relay_num, uint8_t state);

// Получить текущее состояние реле
uint8_t tca9554_get_output(tca9554_t *dev);

#endif