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

// ============================================
// Глобальные переменные
// ============================================

tca9554_t tca9554;

system_state_t state = {
    .relays = {0, 0, 0, 0, 0, 0, 0, 0},
    .moisture = 0,
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
        // 1. Читаем влажность почвы (аналоговый датчик)
        int moisture_percent = adc_sensor_read_percent(NULL);
        if (moisture_percent >= 0) {
            state.moisture = moisture_percent;
            ESP_LOGI("SENSORS", "💧 Влажность почвы: %d%% (raw: %d)", 
                     moisture_percent, adc_sensor_read_raw());
        } else {
            ESP_LOGE("SENSORS", "❌ Ошибка чтения влажности почвы");
        }
        
        // 2. Читаем температуру и влажность воздуха (RS485)
        sensor_data_t sensor = rs485_read_sensor();
        if (sensor.valid) {
            state.temperature = (int)sensor.temperature;
            state.humidity = (int)sensor.humidity;
            ESP_LOGI("SENSORS", "🌡️ T: %.1f°C, 💧 H: %.1f%%", 
                     sensor.temperature, sensor.humidity);
        } else {
            ESP_LOGW("SENSORS", "⚠️ Ошибка RS485 (код: %d)", sensor.error_code);
        }
        
        // Обновляем uptime
        state.uptime = (uint32_t)(esp_timer_get_time() / 1000000);
        
        vTaskDelay(pdMS_TO_TICKS(5000));  // Опрос каждые 5 секунд
    }
}

// ============================================
// Главная функция
// ============================================
void app_main(void)
{
    wifi_init_sta();
    init_i2c();
    rs485_init();
    adc_sensor_init();          // <-- Инициализация ADC
    init_spiffs();
    start_webserver();
    
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));
    ESP_ERROR_CHECK(tca9554_init(&tca9554, i2c_bus, TCA9554_ADDR));
    ESP_LOGI(TAG, "✅ TCA9554 инициализирован");
    
    // Запускаем задачу опроса датчиков (вместо rs485_task)
    xTaskCreate(sensors_task, "sensors_task", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "✅ Система запущена!");
    ESP_LOGI(TAG, "🌐 Откройте: http://%s", state.ip);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}