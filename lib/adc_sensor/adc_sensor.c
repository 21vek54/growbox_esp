#include "adc_sensor.h"

static const char *TAG = "ADC_SENSOR";

// Стандартные значения калибровки (подберите под свой датчик)
soil_calibration_t soil_cal = {
    .dry_value = 3000,   // Сухая почва (воздух) — около 3000
    .wet_value = 1000    // Влажная почва (вода) — около 1000
};

// Хендлы
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc_cali_handle;

// ============================================
// Инициализация ADC (ESP-IDF v5.x без ultra_low_power)
// ============================================
void adc_sensor_init(void)
{
    // 1. Настройка ADC Oneshot
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        // .ultra_low_power = false,  // <-- УДАЛИТЬ эту строку!
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    // 2. Настройка канала
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_SENSOR_CHANNEL, &config));

    // 3. Калибровка
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ Калибровка ADC не поддерживается, используем сырые значения");
        adc_cali_handle = NULL;
    }

    ESP_LOGI(TAG, "✅ ADC инициализирован (GPIO%d, канал: %d)", 
             ADC_SENSOR_GPIO, ADC_SENSOR_CHANNEL);
    ESP_LOGI(TAG, "   Калибровка: сухой=%d, влажный=%d", 
             soil_cal.dry_value, soil_cal.wet_value);
}

// ============================================
// Чтение сырого значения (0-4095)
// ============================================
int adc_sensor_read_raw(void)
{
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(adc1_handle, ADC_SENSOR_CHANNEL, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка чтения ADC: %s", esp_err_to_name(ret));
        return -1;
    }
    return raw;
}

// ============================================
// Чтение значения в милливольтах
// ============================================
int adc_sensor_read_mv(void)
{
    if (adc_cali_handle == NULL) {
        // Если калибровка недоступна, возвращаем сырое значение
        return adc_sensor_read_raw();
    }
    
    int raw = adc_sensor_read_raw();
    if (raw < 0) return -1;
    
    int voltage = 0;
    esp_err_t ret = adc_cali_raw_to_voltage(adc_cali_handle, raw, &voltage);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка калибровки ADC: %s", esp_err_to_name(ret));
        return -1;
    }
    return voltage;
}

// ============================================
// Чтение влажности в процентах (0-100%)
// ============================================
int adc_sensor_read_percent(soil_calibration_t *cal)
{
    int raw = adc_sensor_read_raw();
    if (raw < 0) return -1;
    
    // Используем переданную калибровку или стандартную
    if (cal == NULL) {
        cal = &soil_cal;
    }
    
    // Ограничиваем значения диапазоном калибровки
    int clamped = raw;
    if (clamped > cal->dry_value) clamped = cal->dry_value;
    if (clamped < cal->wet_value) clamped = cal->wet_value;
    
    // Преобразуем в проценты (сухо = 0%, влажно = 100%)
    int percent = 100 - (int)((float)(clamped - cal->wet_value) / 
                              (cal->dry_value - cal->wet_value) * 100);
    
    // Ограничиваем 0-100%
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    return percent;
}

// ============================================
// Калибровка датчика
// ============================================
void adc_sensor_calibrate(soil_calibration_t *cal)
{
    if (cal == NULL) cal = &soil_cal;
    
    ESP_LOGI(TAG, "🔧 Калибровка датчика влажности почвы");
    ESP_LOGI(TAG, "  1. Поместите датчик в СУХУЮ почву/воздух");
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    int dry_raw = adc_sensor_read_raw();
    if (dry_raw > 0) {
        cal->dry_value = dry_raw;
        ESP_LOGI(TAG, "  ✅ Сухой: %d", dry_raw);
    }
    
    ESP_LOGI(TAG, "  2. Поместите датчик во ВЛАЖНУЮ почву/воду");
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    int wet_raw = adc_sensor_read_raw();
    if (wet_raw > 0) {
        cal->wet_value = wet_raw;
        ESP_LOGI(TAG, "  ✅ Влажный: %d", wet_raw);
    }
    
    ESP_LOGI(TAG, "✅ Калибровка завершена: сухой=%d, влажный=%d", 
             cal->dry_value, cal->wet_value);
}