#include "adc_sensor.h"

static const char *TAG = "ADC_SENSOR";

soil_calibration_t soil_cal_1 = {
    .dry_value = 3000,
    .wet_value = 1000
};

soil_calibration_t soil_cal_2 = {
    .dry_value = 3000,
    .wet_value = 1000
};

// Глобальная переменная для хэндла ADC
adc_oneshot_unit_handle_t adc1_handle;

// ============================================
// Инициализация ADC (New API)
// ============================================
void adc_sensor_init(void)
{
    // 1. Инициализация юнита ADC
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    // 2. Настройка каналов для датчиков
    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT, // 12 бит по умолчанию для S3
        .atten = ADC_ATTEN_DB_12,         // Для диапазона до 3.3В
    };
    
    // Настраиваем канал для Датчика 1 (GPIO1 -> ADC1_CHANNEL_0)
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_SENSOR_1_CHANNEL, &chan_config));
    
    // Настраиваем канал для Датчика 2 (GPIO2 -> ADC1_CHANNEL_1)
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_SENSOR_2_CHANNEL, &chan_config));

    ESP_LOGI(TAG, "✅ ADC инициализирован (New API)");
    ESP_LOGI(TAG, "   Датчик 1: GPIO%d, канал %d", ADC_SENSOR_1_GPIO, ADC_SENSOR_1_CHANNEL);
    ESP_LOGI(TAG, "   Датчик 2: GPIO%d, канал %d", ADC_SENSOR_2_GPIO, ADC_SENSOR_2_CHANNEL);
}

// ============================================
// ДАТЧИК 1 (GPIO1)
// ============================================
int adc_sensor_1_read_raw(void)
{
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_SENSOR_1_CHANNEL, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка чтения ADC датчика 1: %s", esp_err_to_name(ret));
        return -1;
    }
    return raw;
}

int adc_sensor_1_read_mv(void)
{
    int raw = adc_sensor_1_read_raw();
    if (raw < 0) return -1;
    // Приблизительное преобразование, лучше использовать калибровку
    return (raw * 3300) / 4095;
}

int adc_sensor_1_read_percent(soil_calibration_t *cal)
{
    int raw = adc_sensor_1_read_raw();
    if (raw < 0) return -1;
    
    if (cal == NULL) cal = &soil_cal_1;
    
    int clamped = raw;
    if (clamped > cal->dry_value) clamped = cal->dry_value;
    if (clamped < cal->wet_value) clamped = cal->wet_value;
    
    int percent = 100 - (int)((float)(clamped - cal->wet_value) / 
                              (cal->dry_value - cal->wet_value) * 100);
    
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    return percent;
}

// ============================================
// ДАТЧИК 2 (GPIO2)
// ============================================
int adc_sensor_2_read_raw(void)
{
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_SENSOR_2_CHANNEL, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка чтения ADC датчика 2: %s", esp_err_to_name(ret));
        return -1;
    }
    return raw;
}

int adc_sensor_2_read_mv(void)
{
    int raw = adc_sensor_2_read_raw();
    if (raw < 0) return -1;
    return (raw * 3300) / 4095;
}

int adc_sensor_2_read_percent(soil_calibration_t *cal)
{
    int raw = adc_sensor_2_read_raw();
    if (raw < 0) return -1;
    
    if (cal == NULL) cal = &soil_cal_2;
    
    int clamped = raw;
    if (clamped > cal->dry_value) clamped = cal->dry_value;
    if (clamped < cal->wet_value) clamped = cal->wet_value;
    
    int percent = 100 - (int)((float)(clamped - cal->wet_value) / 
                              (cal->dry_value - cal->wet_value) * 100);
    
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    return percent;
}

// ============================================
// Калибровка (остается без изменений)
// ============================================
void adc_sensor_calibrate(soil_calibration_t *cal, int sensor_num)
{
    if (cal == NULL) return;
    
    ESP_LOGI(TAG, "🔧 Калибровка датчика %d", sensor_num);
    ESP_LOGI(TAG, "  1. Поместите датчик в СУХУЮ почву/воздух");
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    int dry_raw = (sensor_num == 1) ? adc_sensor_1_read_raw() : adc_sensor_2_read_raw();
    if (dry_raw > 0) {
        cal->dry_value = dry_raw;
        ESP_LOGI(TAG, "  ✅ Сухой: %d", dry_raw);
    }
    
    ESP_LOGI(TAG, "  2. Поместите датчик во ВЛАЖНУЮ почву/воду");
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    int wet_raw = (sensor_num == 1) ? adc_sensor_1_read_raw() : adc_sensor_2_read_raw();
    if (wet_raw > 0) {
        cal->wet_value = wet_raw;
        ESP_LOGI(TAG, "  ✅ Влажный: %d", wet_raw);
    }
    
    ESP_LOGI(TAG, "✅ Калибровка датчика %d: сухой=%d, влажный=%d", 
             sensor_num, cal->dry_value, cal->wet_value);
}