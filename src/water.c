#include "water.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "variables.h"
#include "soil.h"
#include "time_sync.h"
#include "mqtt_log.h"

static const char *WATER_TAG = "WATER";

// Жёсткий предел открытия клапана, что бы ни пришло в настройках
#define DOSE_MAX_S        120
// Рост влажности, который считается откликом датчика на полив, %
#define MIN_RISE          3
#define NO_EFFECT_DOSES   3
#define US_PER_MIN        (60LL * 1000000)
// Датчики расходятся дольше часа, а более сухой ниже порога — пробная порция
#define DISAGREE_TEST_US  (60 * US_PER_MIN)
#define TEST_REPEAT_US    (12 * 60 * US_PER_MIN)

typedef enum { MODE_OFF = 0, MODE_AUTO = 1 } water_mode_t;
typedef enum { ST_IDLE = 0, ST_DOSING, ST_SOAKING } water_state_t;
typedef enum { KIND_AUTO = 0, KIND_MANUAL, KIND_TEST } cycle_kind_t;

static const char *const MODE_NAMES[] = {"off", "auto"};
static const char *const STATE_NAMES[] = {"idle", "dosing", "soaking"};
static const char *const KIND_NAMES[] = {"auto", "manual", "test"};

static SemaphoreHandle_t s_lock;

static struct {
    water_mode_t mode;
    uint8_t start;
    uint8_t target;
    uint16_t dose_s;
    uint16_t soak_min;
    uint8_t max_doses;
} s_cfg = {MODE_OFF, 35, 50, 20, 20, 6};

static struct {
    water_state_t st;
    cycle_kind_t kind;
    bool valve;
    int64_t opened_us;
    int64_t until_us;
    uint8_t cycle_doses;
    int base_value;
    int base_pct[SOIL_SENSORS];
    bool base_ok[SOIL_SENSORS];
    uint8_t doses_today;
    uint32_t day_key;
    uint32_t last_water;
    bool fault;
    bool limit_announced;
    int64_t disagree_since_us;
    int64_t last_test_us;
    uint16_t req_now_s;
    bool req_stop;
    bool req_reset;
} s_rt;

static void save_cfg(void)
{
    nvs_handle_t nvs;
    if (nvs_open("water", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(WATER_TAG, "NVS недоступен, настройки не сохранены");
        return;
    }
    nvs_set_u8(nvs, "mode", (uint8_t)s_cfg.mode);
    nvs_set_u8(nvs, "start", s_cfg.start);
    nvs_set_u8(nvs, "target", s_cfg.target);
    nvs_set_u16(nvs, "dose", s_cfg.dose_s);
    nvs_set_u16(nvs, "soak", s_cfg.soak_min);
    nvs_set_u8(nvs, "maxd", s_cfg.max_doses);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// Счётчик за сутки в NVS: перезагрузка не должна обнулять лимит
static void save_counters(void)
{
    nvs_handle_t nvs;
    if (nvs_open("water", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_u32(nvs, "day", s_rt.day_key);
    nvs_set_u8(nvs, "doses", s_rt.doses_today);
    nvs_set_u32(nvs, "last", s_rt.last_water);
    nvs_set_u8(nvs, "fault", s_rt.fault ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void water_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_rt.last_test_us = -TEST_REPEAT_US;

    nvs_handle_t nvs;
    if (nvs_open("water", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        if (nvs_get_u8(nvs, "mode", &u8) == ESP_OK && u8 <= MODE_AUTO) s_cfg.mode = (water_mode_t)u8;
        if (nvs_get_u8(nvs, "start", &u8) == ESP_OK) s_cfg.start = u8;
        if (nvs_get_u8(nvs, "target", &u8) == ESP_OK) s_cfg.target = u8;
        if (nvs_get_u16(nvs, "dose", &u16) == ESP_OK && u16 >= 1 && u16 <= DOSE_MAX_S) s_cfg.dose_s = u16;
        if (nvs_get_u16(nvs, "soak", &u16) == ESP_OK && u16 >= 1) s_cfg.soak_min = u16;
        if (nvs_get_u8(nvs, "maxd", &u8) == ESP_OK && u8 >= 1) s_cfg.max_doses = u8;
        if (nvs_get_u32(nvs, "day", &u32) == ESP_OK) s_rt.day_key = u32;
        if (nvs_get_u8(nvs, "doses", &u8) == ESP_OK) s_rt.doses_today = u8;
        if (nvs_get_u32(nvs, "last", &u32) == ESP_OK) s_rt.last_water = u32;
        if (nvs_get_u8(nvs, "fault", &u8) == ESP_OK) s_rt.fault = u8 != 0;
        nvs_close(nvs);
    }
    ESP_LOGI(WATER_TAG, "Режим %s, старт <%u%% → %u%%, порция %u с, пауза %u мин, лимит %u/сут%s",
             MODE_NAMES[s_cfg.mode], s_cfg.start, s_cfg.target, s_cfg.dose_s,
             s_cfg.soak_min, s_cfg.max_doses, s_rt.fault ? ", АВАРИЯ" : "");
}

static uint32_t current_day_key(void)
{
    if (time_sync_valid()) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        return (uint32_t)((t.tm_year + 1900) * 1000 + t.tm_yday);
    }
    return 1000000u + (uint32_t)(esp_timer_get_time() / (24LL * 60 * US_PER_MIN));
}

static void roll_day(void)
{
    uint32_t key = current_day_key();
    if (key != s_rt.day_key) {
        s_rt.day_key = key;
        s_rt.doses_today = 0;
        s_rt.limit_announced = false;
        save_counters();
    }
}

static bool set_valve(bool open)
{
    if (tca9554_set_relay(&tca9554, WATER_RELAY, open ? 1 : 0) != ESP_OK) {
        ESP_LOGE(WATER_TAG, "Ошибка TCA9554, реле %d", WATER_RELAY + 1);
        return false;
    }
    state.relays[WATER_RELAY] = open ? 1 : 0;
    s_rt.valve = open;
    ESP_LOGI(WATER_TAG, "Клапан %s", open ? "ОТКРЫТ" : "закрыт");
    return true;
}

static cJSON *event_new(const char *name, int soil)
{
    cJSON *ev = cJSON_CreateObject();
    if (ev == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(ev, "event", name);
    cJSON_AddStringToObject(ev, "kind", KIND_NAMES[s_rt.kind]);
    if (soil >= 0) {
        cJSON_AddNumberToObject(ev, "soil", soil);
    } else {
        cJSON_AddNullToObject(ev, "soil");
    }
    return ev;
}

static void start_dose(uint16_t dose_s, int64_t now)
{
    if (!set_valve(true)) {
        s_rt.st = ST_IDLE;
        return;
    }
    s_rt.st = ST_DOSING;
    s_rt.opened_us = now;
    s_rt.until_us = now + (int64_t)dose_s * 1000000;
}

static void begin_cycle(cycle_kind_t kind, uint16_t dose_s, const soil_reading_t *r, int64_t now)
{
    s_rt.kind = kind;
    s_rt.cycle_doses = 0;
    s_rt.base_value = r->value;
    for (int i = 0; i < SOIL_SENSORS; i++) {
        s_rt.base_pct[i] = r->percent[i];
        s_rt.base_ok[i] = r->ok[i];
    }
    ESP_LOGI(WATER_TAG, "Полив (%s): почва %d%%, порция %u с", KIND_NAMES[kind], r->value, dose_s);
    cJSON *ev = event_new("water_start", r->value);
    if (ev != NULL) {
        cJSON_AddNumberToObject(ev, "dose_s", dose_s);
        mqtt_log_event(ev);
    }
    start_dose(dose_s, now);
}

static void finish_dose(int64_t now)
{
    set_valve(false);
    if (s_rt.doses_today < UINT8_MAX) s_rt.doses_today++;
    if (s_rt.cycle_doses < UINT8_MAX) s_rt.cycle_doses++;
    s_rt.last_water = time_sync_valid() ? (uint32_t)time(NULL) : 0;
    save_counters();
    s_rt.st = ST_SOAKING;
    s_rt.until_us = now + (int64_t)s_cfg.soak_min * US_PER_MIN;
}

static void finish_cycle(const char *result, int soil_after)
{
    s_rt.st = ST_IDLE;
    ESP_LOGI(WATER_TAG, "Полив завершён (%s): %u порц., %d%% → %d%%",
             result, s_rt.cycle_doses, s_rt.base_value, soil_after);
    cJSON *ev = event_new("water_done", soil_after);
    if (ev != NULL) {
        cJSON_AddStringToObject(ev, "result", result);
        cJSON_AddNumberToObject(ev, "doses", s_rt.cycle_doses);
        cJSON_AddNumberToObject(ev, "soil_before", s_rt.base_value);
        mqtt_log_event(ev);
    }
}

static void announce_limit(int soil)
{
    if (s_rt.limit_announced) {
        return;
    }
    s_rt.limit_announced = true;
    cJSON *ev = event_new("water_limit", soil);
    if (ev != NULL) {
        cJSON_AddNumberToObject(ev, "doses", s_rt.doses_today);
        cJSON_AddNumberToObject(ev, "max_doses", s_cfg.max_doses);
        mqtt_log_event(ev);
    }
}

static void idle_step(const soil_reading_t *r, int64_t now)
{
    if (s_cfg.mode != MODE_AUTO || s_rt.fault || r->value < 0) {
        s_rt.disagree_since_us = 0;
        return;
    }
    bool dry = r->value < s_cfg.start;
    bool test_due = false;
    if (strcmp(r->source, "wetter") == 0) {
        if (s_rt.disagree_since_us == 0) {
            s_rt.disagree_since_us = now;
        }
        int drier = r->percent[0] < r->percent[1] ? r->percent[0] : r->percent[1];
        test_due = now - s_rt.disagree_since_us >= DISAGREE_TEST_US
                   && now - s_rt.last_test_us >= TEST_REPEAT_US
                   && drier < s_cfg.start;
    } else {
        s_rt.disagree_since_us = 0;
    }
    if (!dry && !test_due) {
        return;
    }
    if (s_rt.doses_today >= s_cfg.max_doses) {
        announce_limit(r->value);
        return;
    }
    if (dry) {
        begin_cycle(KIND_AUTO, s_cfg.dose_s, r, now);
    } else {
        s_rt.last_test_us = now;
        begin_cycle(KIND_TEST, s_cfg.dose_s, r, now);
    }
}

static void evaluate(const soil_reading_t *r, int64_t now)
{
    bool measured[SOIL_SENSORS];
    bool responded[SOIL_SENSORS];
    bool any = false;
    for (int i = 0; i < SOIL_SENSORS; i++) {
        measured[i] = s_rt.base_ok[i] && s_rt.base_pct[i] >= 0 && r->percent[i] >= 0;
        responded[i] = measured[i] && r->percent[i] - s_rt.base_pct[i] >= MIN_RISE;
        any = any || responded[i];
    }
    // Полив дошёл до одного датчика, а другой стоит — второй неисправен
    if (measured[0] && measured[1] && responded[0] != responded[1]) {
        soil_mark_stuck(responded[0] ? 1 : 0);
    }

    soil_reading_t now_r;
    soil_get(&now_r);

    if (s_rt.kind != KIND_AUTO) {
        finish_cycle(KIND_NAMES[s_rt.kind], now_r.value);
        return;
    }
    if (s_cfg.mode != MODE_AUTO) {
        finish_cycle("stopped", now_r.value);
    } else if (now_r.value >= s_cfg.target) {
        finish_cycle("target", now_r.value);
    } else if (!any && s_rt.cycle_doses >= NO_EFFECT_DOSES) {
        s_rt.fault = true;
        s_rt.st = ST_IDLE;
        save_counters();
        ESP_LOGE(WATER_TAG, "АВАРИЯ: %u порций без роста влажности", s_rt.cycle_doses);
        cJSON *ev = event_new("water_fault", now_r.value);
        if (ev != NULL) {
            cJSON_AddStringToObject(ev, "fault", "no_effect");
            cJSON_AddNumberToObject(ev, "doses", s_rt.cycle_doses);
            mqtt_log_event(ev);
        }
    } else if (now_r.value < 0) {
        finish_cycle("no_sensors", now_r.value);
    } else if (s_rt.doses_today >= s_cfg.max_doses) {
        // water_done с result=limit уже сообщает о лимите
        s_rt.limit_announced = true;
        finish_cycle("limit", now_r.value);
    } else {
        start_dose(s_cfg.dose_s, now);
    }
}

void water_tick(void)
{
    if (s_lock == NULL) {
        return;
    }
    soil_reading_t r;
    soil_get(&r);
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    roll_day();

    if (s_rt.req_reset) {
        s_rt.req_reset = false;
        s_rt.fault = false;
        s_rt.limit_announced = false;
        soil_clear_stuck();
        save_counters();
        ESP_LOGI(WATER_TAG, "Аварии сброшены");
    }
    if (s_rt.req_stop) {
        s_rt.req_stop = false;
        if (s_rt.st != ST_IDLE) {
            set_valve(false);
            finish_cycle("stopped", r.value);
        }
    }
    if (s_rt.req_now_s > 0) {
        uint16_t dose_s = s_rt.req_now_s;
        s_rt.req_now_s = 0;
        if (s_rt.st != ST_DOSING && s_rt.doses_today < s_cfg.max_doses) {
            begin_cycle(KIND_MANUAL, dose_s, &r, now);
        }
    }

    switch (s_rt.st) {
    case ST_IDLE:
        idle_step(&r, now);
        break;
    case ST_DOSING:
        if (now >= s_rt.until_us || now - s_rt.opened_us >= (int64_t)DOSE_MAX_S * 1000000) {
            finish_dose(now);
        }
        break;
    case ST_SOAKING:
        if (now >= s_rt.until_us) {
            evaluate(&r, now);
        }
        break;
    }
    // Клапан открыт только во время порции; при ошибке закрытия — повтор каждую секунду
    if (s_rt.valve && s_rt.st != ST_DOSING) {
        set_valve(false);
    }
    xSemaphoreGive(s_lock);
}

static const char *read_uint(const cJSON *cfg, const char *key, int lo, int hi,
                             int *out, const char *err)
{
    const cJSON *item = cJSON_GetObjectItem(cfg, key);
    if (item == NULL) {
        return NULL;
    }
    if (!cJSON_IsNumber(item) || item->valueint < lo || item->valueint > hi) {
        return err;
    }
    *out = item->valueint;
    return NULL;
}

const char *water_apply(const cJSON *cfg)
{
    if (!cJSON_IsObject(cfg) || s_lock == NULL) {
        return "bad water config";
    }
    const cJSON *mode = cJSON_GetObjectItem(cfg, "mode");
    const cJSON *now_item = cJSON_GetObjectItem(cfg, "now");
    int mode_v = -1;
    int start = -1;
    int target = -1;
    int dose = -1;
    int soak = -1;
    int maxd = -1;
    int now_s = -1;
    const char *err;

    if (mode != NULL) {
        if (cJSON_IsString(mode) && strcmp(mode->valuestring, "auto") == 0) {
            mode_v = MODE_AUTO;
        } else if (cJSON_IsString(mode) && strcmp(mode->valuestring, "off") == 0) {
            mode_v = MODE_OFF;
        } else {
            return "mode must be auto|off";
        }
    }
    if ((err = read_uint(cfg, "start", 5, 90, &start, "start must be 5..90")) != NULL) return err;
    if ((err = read_uint(cfg, "target", 10, 95, &target, "target must be 10..95")) != NULL) return err;
    if ((err = read_uint(cfg, "dose_s", 1, DOSE_MAX_S, &dose, "dose_s must be 1..120")) != NULL) return err;
    if ((err = read_uint(cfg, "soak_min", 1, 240, &soak, "soak_min must be 1..240")) != NULL) return err;
    if ((err = read_uint(cfg, "max_doses", 1, 50, &maxd, "max_doses must be 1..50")) != NULL) return err;
    if (now_item != NULL
        && (err = read_uint(cfg, "now", 0, DOSE_MAX_S, &now_s, "now must be 0..120")) != NULL) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int new_start = start >= 0 ? start : s_cfg.start;
    int new_target = target >= 0 ? target : s_cfg.target;
    if (new_start >= new_target) {
        xSemaphoreGive(s_lock);
        return "start must be below target";
    }
    int new_max = maxd >= 0 ? maxd : s_cfg.max_doses;
    if (now_s >= 0) {
        if (s_rt.st == ST_DOSING) {
            xSemaphoreGive(s_lock);
            return "already watering";
        }
        if (s_rt.doses_today >= new_max) {
            xSemaphoreGive(s_lock);
            return "daily limit reached";
        }
    }

    bool cfg_changed = mode_v >= 0 || start >= 0 || target >= 0 || dose >= 0 || soak >= 0 || maxd >= 0;
    if (mode_v >= 0) s_cfg.mode = (water_mode_t)mode_v;
    s_cfg.start = (uint8_t)new_start;
    s_cfg.target = (uint8_t)new_target;
    if (dose >= 0) s_cfg.dose_s = (uint16_t)dose;
    if (soak >= 0) s_cfg.soak_min = (uint16_t)soak;
    s_cfg.max_doses = (uint8_t)new_max;
    if (cfg_changed) {
        save_cfg();
        s_rt.limit_announced = false;
    }
    if (now_s >= 0) {
        s_rt.req_now_s = now_s > 0 ? (uint16_t)now_s : s_cfg.dose_s;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(cfg, "stop"))) {
        s_rt.req_stop = true;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(cfg, "reset"))) {
        s_rt.req_reset = true;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(WATER_TAG, "Настройки: %s, <%u%% → %u%%, порция %u с, пауза %u мин, лимит %u",
             MODE_NAMES[s_cfg.mode], s_cfg.start, s_cfg.target, s_cfg.dose_s,
             s_cfg.soak_min, s_cfg.max_doses);
    return NULL;
}

void water_add_json(cJSON *root)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    cJSON_AddNumberToObject(root, "valve", s_rt.valve ? 1 : 0);
    cJSON_AddStringToObject(root, "water_mode", MODE_NAMES[s_cfg.mode]);
    cJSON_AddStringToObject(root, "water_state", STATE_NAMES[s_rt.st]);
    if (s_rt.st != ST_IDLE) {
        cJSON_AddStringToObject(root, "water_cycle", KIND_NAMES[s_rt.kind]);
        int64_t left = (s_rt.until_us - now) / 1000000;
        cJSON_AddNumberToObject(root, "water_left_s", left > 0 ? (double)left : 0);
    }
    cJSON_AddNumberToObject(root, "water_start", s_cfg.start);
    cJSON_AddNumberToObject(root, "water_target", s_cfg.target);
    cJSON_AddNumberToObject(root, "water_dose_s", s_cfg.dose_s);
    cJSON_AddNumberToObject(root, "water_soak_min", s_cfg.soak_min);
    cJSON_AddNumberToObject(root, "water_max_doses", s_cfg.max_doses);
    cJSON_AddNumberToObject(root, "water_doses_today", s_rt.doses_today);
    if (s_rt.last_water > 0) {
        char buf[32];
        time_t t = (time_t)s_rt.last_water;
        struct tm tm_local;
        localtime_r(&t, &tm_local);
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm_local);
        cJSON_AddStringToObject(root, "water_last", buf);
    } else {
        cJSON_AddNullToObject(root, "water_last");
    }
    if (s_rt.fault) {
        cJSON_AddStringToObject(root, "water_fault", "no_effect");
    } else {
        cJSON_AddNullToObject(root, "water_fault");
    }
    xSemaphoreGive(s_lock);
}
