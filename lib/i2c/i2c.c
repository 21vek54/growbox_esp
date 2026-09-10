#include "i2c.h"
#include "tca9554.h"

// Определяем переменные здесь (ТОЛЬКО ОДИН РАЗ)
i2c_master_bus_handle_t i2c_bus;
i2c_master_bus_config_t bus_config;

// ============================================
// I2C и TCA9554 инициализация
// ============================================
void init_i2c(void)
{
    bus_config.i2c_port = I2C_MASTER_NUM;
    bus_config.sda_io_num = I2C_MASTER_SDA_IO;
    bus_config.scl_io_num = I2C_MASTER_SCL_IO;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    
    ESP_LOGI("I2C", "✅ I2C инициализирован");
}