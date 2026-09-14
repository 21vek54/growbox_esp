#ifndef ADC_SENSOR_H
#define ADC_SENSOR_H

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"          // Для калибровки (если нужно)
#include "esp_adc/adc_cali_scheme.h"    // Для схемы калибровки
#include "soc/soc_caps.h"
#include "driver/gpio.h"
// ============================================
// Настройки ADC для двух датчиков
// ============================================
#define ADC_SENSOR_1_GPIO       GPIO_NUM_1
#define ADC_SENSOR_1_CHANNEL    ADC_CHANNEL_0

#define ADC_SENSOR_2_GPIO       GPIO_NUM_2
#define ADC_SENSOR_2_CHANNEL    ADC_CHANNEL_1

// ============================================
// Структура калибровки
// ============================================
typedef struct {
    int dry_value;      // Значение в сухой почве (воздух)
    int wet_value;      // Значение во влажной почве (вода)
} soil_calibration_t;

// ============================================
// Функции
// ============================================
void adc_sensor_init(void);

// Для датчика 1 (GPIO1)
int adc_sensor_1_read_raw(void);
int adc_sensor_1_read_mv(void);
int adc_sensor_1_read_percent(soil_calibration_t *cal);

// Для датчика 2 (GPIO2)
int adc_sensor_2_read_raw(void);
int adc_sensor_2_read_mv(void);
int adc_sensor_2_read_percent(soil_calibration_t *cal);

// Калибровка
void adc_sensor_calibrate(soil_calibration_t *cal, int sensor_num);

// Стандартная калибровка (можно менять)
extern soil_calibration_t soil_cal_1;
extern soil_calibration_t soil_cal_2;

// Хэндл для юнита ADC (глобальная переменная)
extern adc_oneshot_unit_handle_t adc1_handle;

#endif