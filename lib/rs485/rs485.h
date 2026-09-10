#ifndef RS485_H
#define RS485_H

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"

// ============================================
// Настройки RS485
// ============================================
#define RS485_UART_NUM          UART_NUM_1
#define RS485_TX_PIN            GPIO_NUM_17
#define RS485_RX_PIN            GPIO_NUM_18
#define RS485_RE_DE_PIN         GPIO_NUM_45    // RE и DE закорочены вместе
#define RS485_BAUDRATE          9600
#define RS485_BUFFER_SIZE       512
#define RS485_TIMEOUT_MS        1000
#define RS485_DEVICE_ADDRESS    0x01

// ============================================
// Modbus регистры
// ============================================
#define REG_TEMPERATURE         0x0000
#define REG_HUMIDITY            0x0001

// ============================================
// Структура данных
// ============================================
typedef struct {
    float temperature;
    float humidity;
    uint8_t raw_data[16];
    bool valid;
    int error_code;
} sensor_data_t;

// ============================================
// Функции
// ============================================
void rs485_init(void);
void rs485_send_request(uint8_t *data, size_t len);
int rs485_read_response(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);
sensor_data_t rs485_read_sensor(void);
void rs485_build_modbus_request(uint8_t *buffer, uint8_t addr, uint8_t func, 
                                 uint16_t reg_addr, uint16_t num_regs);
uint16_t rs485_crc16(uint8_t *data, size_t len);

#endif