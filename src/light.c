#include "light.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "variables.h"
#include "time_sync.h"

static const char *LIGHT_TAG = "LIGHT";

#define MIN_PER_DAY 1440

typedef enum {
    LIGHT_MODE_OFF = 0,
    LIGHT_MODE_ON = 1,
    LIGHT_MODE_AUTO = 2,
} light_mode_t;

static const char *const MODE_NAMES[] = {"off", "on", "auto"};

static SemaphoreHandle_t s_lock;
static light_mode_t s_mode = LIGHT_MODE_OFF;
static uint16_t s_on_min = 8 * 60;
static uint16_t s_duration_min = 12 * 60;
static bool s_applied;

static void save_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open("light", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(LIGHT_TAG, "NVS недоступен, расписание не сохранено");
        return;
    }
    nvs_set_u8(nvs, "mode", (uint8_t)s_mode);
    nvs_set_u16(nvs, "on", s_on_min);
    nvs_set_u16(nvs, "dur", s_duration_min);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void light_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    nvs_handle_t nvs;
    if (nvs_open("light", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t mode;
        uint16_t on_min;
        uint16_t duration_min;
        if (nvs_get_u8(nvs, "mode", &mode) == ESP_OK && mode <= LIGHT_MODE_AUTO) {
            s_mode = (light_mode_t)mode;
        }
        if (nvs_get_u16(nvs, "on", &on_min) == ESP_OK && on_min < MIN_PER_DAY) {
            s_on_min = on_min;
        }
        if (nvs_get_u16(nvs, "dur", &duration_min) == ESP_OK && duration_min <= MIN_PER_DAY) {
            s_duration_min = duration_min;
        }
        nvs_close(nvs);
    }
    ESP_LOGI(LIGHT_TAG, "Режим %s, включение %02u:%02u, %u мин",
             MODE_NAMES[s_mode], s_on_min / 60, s_on_min % 60, s_duration_min);
}

static uint8_t wanted_state(void)
{
    if (s_mode != LIGHT_MODE_AUTO) {
        return s_mode == LIGHT_MODE_ON;
    }
    // Без реального времени расписание неизвестно — лампа выключена
    if (!time_sync_valid() || s_duration_min == 0) {
        return 0;
    }
    if (s_duration_min >= MIN_PER_DAY) {
        return 1;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    int now_min = t.tm_hour * 60 + t.tm_min;
    int since_on = (now_min - s_on_min + MIN_PER_DAY) % MIN_PER_DAY;
    return since_on < s_duration_min;
}

bool light_tick(void)
{
    if (s_lock == NULL) {
        return false;
    }
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t want = wanted_state();
    if (!s_applied || state.relays[LIGHT_RELAY] != want) {
        if (tca9554_set_relay(&tca9554, LIGHT_RELAY, want) == ESP_OK) {
            changed = s_applied && state.relays[LIGHT_RELAY] != want;
            state.relays[LIGHT_RELAY] = want;
            s_applied = true;
            ESP_LOGI(LIGHT_TAG, "Фитолампа %s (%s)", want ? "ВКЛ" : "ВЫКЛ", MODE_NAMES[s_mode]);
        } else {
            ESP_LOGE(LIGHT_TAG, "Ошибка TCA9554, реле %d", LIGHT_RELAY + 1);
        }
    }
    xSemaphoreGive(s_lock);
    return changed;
}

static bool parse_hhmm(const char *s, uint16_t *out)
{
    unsigned h;
    unsigned m;
    char tail;
    if (sscanf(s, "%u:%u%c", &h, &m, &tail) != 2 || h > 23 || m > 59) {
        return false;
    }
    *out = (uint16_t)(h * 60 + m);
    return true;
}

const char *light_apply(const cJSON *cfg)
{
    if (!cJSON_IsObject(cfg) || s_lock == NULL) {
        return "bad light config";
    }
    const cJSON *mode = cJSON_GetObjectItem(cfg, "mode");
    const cJSON *on = cJSON_GetObjectItem(cfg, "on");
    const cJSON *duration = cJSON_GetObjectItem(cfg, "duration_min");

    light_mode_t new_mode;
    uint16_t new_on;
    uint16_t new_duration;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    new_mode = s_mode;
    new_on = s_on_min;
    new_duration = s_duration_min;
    xSemaphoreGive(s_lock);

    if (mode != NULL) {
        if (!cJSON_IsString(mode)) {
            return "mode must be auto|on|off";
        }
        if (strcmp(mode->valuestring, "auto") == 0) {
            new_mode = LIGHT_MODE_AUTO;
        } else if (strcmp(mode->valuestring, "on") == 0) {
            new_mode = LIGHT_MODE_ON;
        } else if (strcmp(mode->valuestring, "off") == 0) {
            new_mode = LIGHT_MODE_OFF;
        } else {
            return "mode must be auto|on|off";
        }
    }
    if (on != NULL && (!cJSON_IsString(on) || !parse_hhmm(on->valuestring, &new_on))) {
        return "on must be HH:MM";
    }
    if (duration != NULL) {
        if (!cJSON_IsNumber(duration) || duration->valueint < 1
            || duration->valueint > MIN_PER_DAY) {
            return "duration_min must be 1..1440";
        }
        new_duration = (uint16_t)duration->valueint;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode = new_mode;
    s_on_min = new_on;
    s_duration_min = new_duration;
    save_config();
    xSemaphoreGive(s_lock);

    ESP_LOGI(LIGHT_TAG, "Новое расписание: %s, %02u:%02u, %u мин",
             MODE_NAMES[new_mode], new_on / 60, new_on % 60, new_duration);
    return NULL;
}

void light_add_json(cJSON *root)
{
    char on[6];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(on, sizeof(on), "%02u:%02u", s_on_min / 60, s_on_min % 60);
    cJSON_AddNumberToObject(root, "light", state.relays[LIGHT_RELAY]);
    cJSON_AddStringToObject(root, "light_mode", MODE_NAMES[s_mode]);
    cJSON_AddStringToObject(root, "light_on", on);
    cJSON_AddNumberToObject(root, "light_duration_min", s_duration_min);
    xSemaphoreGive(s_lock);
}
