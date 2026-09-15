#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "variables.h"
#include "i2c.h"
#include "http.h"
#include "wifi_handler.h"
#include "rs485.h"
#include "adc_sensor.h"
#include "arduino_ota.h"
#include "mqtt_log.h"

// ============================================
// Глобальные переменные
// ============================================

tca9554_t tca9554;

system_state_t state = {
    .relays = {0, 0, 0, 0, 0, 0, 0, 0},
    .moisture = 0,
    .moisture2 = 0,   // <-- НОВОЕ
    .temperature = 0,
    .humidity = 0,
    .uptime = 0,
    .ip = {0}
};

// ============================================
// Задача для опроса датчиков
// ============================================
void sensors_task(void *pvParameters)
{
    while (1) {
        // 1. Датчик влажности почвы 1 (GPIO1)
        int moisture1 = adc_sensor_1_read_percent(NULL);
        if (moisture1 >= 0) {
            state.moisture = moisture1;
            ESP_LOGI("SENSORS", "💧 Датчик 1: %d%% (raw: %d)", 
                     moisture1, adc_sensor_1_read_raw());
        }
        
        // 2. Датчик влажности почвы 2 (GPIO2)
        int moisture2 = adc_sensor_2_read_percent(NULL);
        if (moisture2 >= 0) {
            state.moisture2 = moisture2;
            ESP_LOGI("SENSORS", "💧 Датчик 2: %d%% (raw: %d)", 
                     moisture2, adc_sensor_2_read_raw());
        }
        
        // 3. Температура и влажность воздуха (RS485)
        sensor_data_t sensor = rs485_read_sensor();
        if (sensor.valid) {
            state.temperature = (int)sensor.temperature;
            state.humidity = (int)sensor.humidity;
            ESP_LOGI("SENSORS", "🌡️ T: %.1f°C, 💧 H: %.1f%%", 
                     sensor.temperature, sensor.humidity);
        }
        
        state.uptime = (uint32_t)(esp_timer_get_time() / 1000000);

        mqtt_log_feed_sample(state.moisture, state.moisture2,
                             state.temperature, state.humidity);
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============================================
// Главная функция
// ============================================
void app_main(void)
{
    wifi_init_sta();
    mqtt_log_start();
    init_i2c();
    rs485_init();
    adc_sensor_init();
    arduino_ota_mark_valid();
    start_webserver();
    arduino_ota_start();
    
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));
    ESP_ERROR_CHECK(tca9554_init(&tca9554, i2c_bus, TCA9554_ADDR));
    ESP_LOGI(TAG, "✅ TCA9554 инициализирован");
    
    xTaskCreate(sensors_task, "sensors_task", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "✅ Система запущена! API: http://%s/api/status", state.ip);
    ESP_LOGI(TAG, "📡 OTA (espota) UDP %d, пароль из OTA_PASS", OTA_PORT);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}