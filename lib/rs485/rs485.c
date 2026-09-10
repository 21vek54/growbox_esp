#include "rs485.h"

static const char *TAG = "RS485";

// ============================================
// CRC16
// ============================================
uint16_t rs485_crc16(uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

// ============================================
// Формирование Modbus запроса
// ============================================
void rs485_build_modbus_request(uint8_t *buffer, uint8_t addr, uint8_t func, 
                                 uint16_t reg_addr, uint16_t num_regs)
{
    buffer[0] = addr;
    buffer[1] = func;
    buffer[2] = (reg_addr >> 8) & 0xFF;
    buffer[3] = reg_addr & 0xFF;
    buffer[4] = (num_regs >> 8) & 0xFF;
    buffer[5] = num_regs & 0xFF;
    
    uint16_t crc = rs485_crc16(buffer, 6);
    buffer[6] = crc & 0xFF;
    buffer[7] = (crc >> 8) & 0xFF;
}

// ============================================
// Инициализация RS485
// ============================================
void rs485_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RS485_RE_DE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // ============================================
    // ИНВЕРТИРОВАННОЕ УПРАВЛЕНИЕ:
    // RE/DE = 1 = РЕЖИМ ПРИЁМА (по умолчанию)
    // RE/DE = 0 = РЕЖИМ ПЕРЕДАЧИ
    // ============================================
    gpio_set_level(RS485_RE_DE_PIN, 1);  // По умолчанию ПРИЁМ

    uart_config_t uart_config = {
        .baud_rate = RS485_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_param_config(RS485_UART_NUM, &uart_config);
    uart_set_pin(RS485_UART_NUM, RS485_TX_PIN, RS485_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(RS485_UART_NUM, RS485_BUFFER_SIZE, RS485_BUFFER_SIZE, 0, NULL, 0);

    ESP_LOGI(TAG, "✅ RS485 инициализирован (инвертированное управление)");
    ESP_LOGI(TAG, "   RE/DE = 1 (приём), RE/DE = 0 (передача)");
}

// ============================================
// Отправка запроса (инвертированное управление)
// ============================================
void rs485_send_request(uint8_t *data, size_t len)
{
    // 1. Переключаем в режим передачи (RE/DE = 1)
    gpio_set_level(RS485_RE_DE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    
    // 2. Очищаем буфер
    uart_flush(RS485_UART_NUM);
    
    // 3. Отправляем запрос
    char hex_str[64] = {0};
    for (size_t i = 0; i < len && i < 16; i++) {
        char tmp[4];
        sprintf(tmp, "%02X ", data[i]);
        strcat(hex_str, tmp);
    }
    ESP_LOGI(TAG, "📤 Запрос: %s", hex_str);
    
    uart_write_bytes(RS485_UART_NUM, (const char *)data, len);
    uart_wait_tx_done(RS485_UART_NUM, pdMS_TO_TICKS(100));
    
    // 4. ЖДЁМ, ПОКА ДАТЧИК ОТВЕТИТ (19 мс)
    // Датчик отвечает во время RE/DE = 1, поэтому мы НЕ переключаемся!
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 5. Теперь переключаем в режим приёма, чтобы ESP32 прочитал ответ
    gpio_set_level(RS485_RE_DE_PIN, 0);
}

// ============================================
// Чтение ответа
// ============================================
int rs485_read_response(uint8_t *buffer, size_t max_len, uint32_t timeout_ms)
{
    // Даём время на стабилизацию
    vTaskDelay(pdMS_TO_TICKS(2));
    
    int len = uart_read_bytes(RS485_UART_NUM, buffer, max_len, pdMS_TO_TICKS(timeout_ms));
    
    if (len > 0) {
        char hex_str[64] = {0};
        for (int i = 0; i < len && i < 16; i++) {
            char tmp[4];
            sprintf(tmp, "%02X ", buffer[i]);
            strcat(hex_str, tmp);
        }
        ESP_LOGI(TAG, "📥 Ответ: %s (%d байт)", hex_str, len);
    } else {
        ESP_LOGW(TAG, "⏱️ Таймаут ответа");
    }
    
    return len;
}

// ============================================
// Проверка CRC
// ============================================
bool rs485_verify_crc(uint8_t *response, int len)
{
    if (len < 3) return false;
    
    uint16_t received_crc = (response[len-1] << 8) | response[len-2];
    uint16_t calculated_crc = rs485_crc16(response, len - 2);
    
    return received_crc == calculated_crc;
}

// ============================================
// Чтение датчика
// ============================================
sensor_data_t rs485_read_sensor(void)
{
    sensor_data_t result = {0};
    result.valid = false;
    result.error_code = 0;
    
    ESP_LOGI(TAG, "🔍 Опрос датчика...");
    
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t request[8];
        rs485_build_modbus_request(request, RS485_DEVICE_ADDRESS, 0x03, REG_TEMPERATURE, 2);
        
        rs485_send_request(request, sizeof(request));
        
        uint8_t response[16] = {0};
        int len = rs485_read_response(response, sizeof(response), RS485_TIMEOUT_MS);
        
        if (len >= 7) {
            if (!rs485_verify_crc(response, len)) {
                ESP_LOGW(TAG, "⚠️ Ошибка CRC");
                continue;
            }
            
            if (response[0] == RS485_DEVICE_ADDRESS && response[1] == 0x03) {
                uint16_t temp_raw = (response[3] << 8) | response[4];
                uint16_t hum_raw = (response[5] << 8) | response[6];
                
                result.temperature = temp_raw / 10.0f;
                result.humidity = hum_raw / 10.0f;
                result.valid = true;
                
                ESP_LOGI(TAG, "✅ T: %.1f°C, H: %.1f%%", 
                         result.temperature, result.humidity);
                return result;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGE(TAG, "❌ Все попытки не удались");
    result.error_code = 5;
    return result;
}