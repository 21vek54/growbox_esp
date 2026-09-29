#include "time_sync.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TIME_TAG = "TIME";

// POSIX TZ: UTC+7 (Новосибирск), без перехода на летнее время
#define TIME_ZONE "<+07>-7"
// Любая дата раньше 2024 года означает, что часы не выставлены
#define TIME_VALID_AFTER 1704067200

static volatile bool s_synced;

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    char buf[32];
    time_sync_format(buf, sizeof(buf));
    ESP_LOGI(TIME_TAG, "Время синхронизировано: %s", buf);
}

void time_sync_start(void)
{
    setenv("TZ", TIME_ZONE, 1);
    tzset();

    // sdkconfig: CONFIG_LWIP_SNTP_MAX_SERVERS=1 — больше серверов esp_netif_sntp_init отвергнет
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TIME_TAG, "SNTP не запущен: %s", esp_err_to_name(err));
    }
}

bool time_sync_valid(void)
{
    return time(NULL) > TIME_VALID_AFTER;
}

const char *time_sync_status(void)
{
    if (s_synced) {
        return "synced";
    }
    return time_sync_valid() ? "rtc" : "none";
}

void time_sync_format(char *buf, size_t len)
{
    if (len == 0) {
        return;
    }
    buf[0] = '\0';
    if (!time_sync_valid()) {
        return;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S%z", &t);
}
