#include "health.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mqtt_log.h"
#include "wifi_handler.h"

static const char *HEALTH_TAG = "HEALTH";

// Задача, не отметившаяся у Task WDT дольше этого срока, вызывает panic → перезагрузку
#define TASK_WDT_TIMEOUT_MS    60000
#define WIFI_LOST_RESTART_S    (10 * 60)
#define NO_PUBLISH_RESTART_S   (15 * 60)

#define RESTART_MAGIC 0x48454C54u

typedef enum {
    RESTART_CAUSE_NONE = 0,
    RESTART_CAUSE_WIFI_LOST,
    RESTART_CAUSE_NO_PUBLISH,
} restart_cause_t;

// RTC-память переживает программную перезагрузку, но не отключение питания
static RTC_NOINIT_ATTR uint32_t s_restart_magic;
static RTC_NOINIT_ATTR uint32_t s_restart_cause;

static const char *s_reset_reason = "unknown";
static uint32_t s_boot_count;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_EXT:      return "ext";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    default:               return "other";
    }
}

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void count_boot(void)
{
    nvs_handle_t nvs;
    if (nvs_open("health", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_get_u32(nvs, "boots", &s_boot_count);
    s_boot_count++;
    nvs_set_u32(nvs, "boots", s_boot_count);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void init_task_wdt(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = TASK_WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&cfg);
    }
    ESP_ERROR_CHECK(err);
}

void health_init(void)
{
    init_nvs();

    esp_reset_reason_t reason = esp_reset_reason();
    s_reset_reason = reset_reason_name(reason);
    if (reason == ESP_RST_SW && s_restart_magic == RESTART_MAGIC) {
        if (s_restart_cause == RESTART_CAUSE_WIFI_LOST) {
            s_reset_reason = "wifi_lost";
        } else if (s_restart_cause == RESTART_CAUSE_NO_PUBLISH) {
            s_reset_reason = "no_publish";
        }
    }
    s_restart_magic = 0;
    s_restart_cause = RESTART_CAUSE_NONE;

    count_boot();
    init_task_wdt();

    ESP_LOGW(HEALTH_TAG, "Старт: причина=%s, загрузка №%lu",
             s_reset_reason, (unsigned long)s_boot_count);
}

static void restart_with(restart_cause_t cause, const char *why)
{
    ESP_LOGE(HEALTH_TAG, "Перезагрузка: %s", why);
    s_restart_magic = RESTART_MAGIC;
    s_restart_cause = cause;
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void health_check(void)
{
    if (wifi_offline_s() >= WIFI_LOST_RESTART_S) {
        restart_with(RESTART_CAUSE_WIFI_LOST, "нет Wi-Fi дольше 10 минут");
    }
    if (mqtt_log_since_publish_s() >= NO_PUBLISH_RESTART_S) {
        restart_with(RESTART_CAUSE_NO_PUBLISH, "нет отправки в MQTT дольше 15 минут");
    }
}

const char *health_reset_reason(void)
{
    return s_reset_reason;
}

uint32_t health_boot_count(void)
{
    return s_boot_count;
}
