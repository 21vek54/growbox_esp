#ifndef ADC_SENSOR_H
#define ADC_SENSOR_H

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"

// ============================================
// Настройки ADC
// ============================================
#define ADC_SENSOR_GPIO         GPIO_NUM_1
#define ADC_SENSOR_CHANNEL      ADC_CHANNEL_0    // GPIO1 = ADC1_CHANNEL0

// ============================================
// Структура калибровки
// ============================================
typedef struct {
    int dry_value;      // Значение в сухой почве (воздух)
    int wet_value;      // Значение в влажной почве (вода)
} soil_calibration_t;

// ============================================
// Функции
// ============================================
void adc_sensor_init(void);
int adc_sensor_read_raw(void);
int adc_sensor_read_mv(void);
int adc_sensor_read_percent(soil_calibration_t *cal);
void adc_sensor_calibrate(soil_calibration_t *cal);

// Стандартная калибровка (можно менять)
extern soil_calibration_t soil_cal;

#endif