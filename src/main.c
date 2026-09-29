#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_task_wdt.h"
#include "variables.h"
#include "i2c.h"
#include "http.h"
#include "wifi_handler.h"
#include "rs485.h"
#include "adc_sensor.h"
#include "arduino_ota.h"
#include "mqtt_log.h"
#include "health.h"
#include "light.h"
#include "soil.h"
#include "water.h"
#include "time_sync.h"

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
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1) {
        esp_task_wdt_reset();

        // 1–2. Датчики влажности почвы (GPIO1, GPIO2): медиана и проверка исправности
        soil_sample();
        soil_reading_t soil;
        soil_get(&soil);
        if (soil.percent[0] >= 0) {
            state.moisture = soil.percent[0];
        }
        if (soil.percent[1] >= 0) {
            state.moisture2 = soil.percent[1];
        }
        ESP_LOGI("SENSORS", "💧 Почва %d%% (%s) | 1: %d%% raw %d %s | 2: %d%% raw %d %s",
                 soil.value, soil.source,
                 soil.percent[0], soil.raw[0], soil.ok[0] ? "ok" : (soil.fault[0] ? soil.fault[0] : "?"),
                 soil.percent[1], soil.raw[1], soil.ok[1] ? "ok" : (soil.fault[1] ? soil.fault[1] : "?"));
        
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
    health_init();
    light_init();
    soil_init();
    water_init();
    wifi_init_sta();
    time_sync_start();
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

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    while (1) {
        esp_task_wdt_reset();
        health_check();
        if (light_tick()) {
            mqtt_log_request_snapshot("light");
        }
        water_tick();
        mqtt_log_service();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}