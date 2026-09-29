#include "soil.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "adc_sensor.h"
#include "mqtt_log.h"

static const char *SOIL_TAG = "SOIL";

#define SOIL_READS        7
// Сырые значения вне окна — обрыв, замыкание или пропало питание датчика
#define SOIL_RAW_MIN      500
#define SOIL_RAW_MAX      3500
// Разброс внутри одной серии чтений — вход висит в воздухе
#define SOIL_NOISE_MAX    300
#define SOIL_STREAK       3
#define SOIL_MAX_DIFF     20
#define SOIL_STUCK_CLEAR  5

typedef struct {
    int raw;
    int percent;
    bool known;
    bool healthy;
    const char *fault;
    uint8_t bad;
    uint8_t good;
    bool stuck;
    int stuck_at;
    int announced;      // -1 — ещё не сообщали, 0 — неисправен, 1 — исправен
} probe_t;

static probe_t s_probe[SOIL_SENSORS];
static SemaphoreHandle_t s_lock;

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static int read_raw(int idx)
{
    return idx == 0 ? adc_sensor_1_read_raw() : adc_sensor_2_read_raw();
}

static int raw_to_percent(int raw, const soil_calibration_t *cal)
{
    int clamped = raw;
    if (clamped > cal->dry_value) clamped = cal->dry_value;
    if (clamped < cal->wet_value) clamped = cal->wet_value;
    int percent = 100 - (int)((float)(clamped - cal->wet_value)
                              / (cal->dry_value - cal->wet_value) * 100);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}

static bool probe_ok(const probe_t *p)
{
    return p->known && p->healthy && !p->stuck;
}

static const char *probe_fault(const probe_t *p)
{
    return p->stuck ? "stuck" : p->fault;
}

static void emit(const char *event, int idx, const char *fault, int raw, int percent)
{
    cJSON *ev = cJSON_CreateObject();
    if (ev == NULL) {
        return;
    }
    cJSON_AddStringToObject(ev, "event", event);
    cJSON_AddNumberToObject(ev, "sensor", idx + 1);
    if (fault != NULL) {
        cJSON_AddStringToObject(ev, "fault", fault);
    }
    cJSON_AddNumberToObject(ev, "raw", raw);
    cJSON_AddNumberToObject(ev, "percent", percent);
    mqtt_log_event(ev);
}

void soil_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < SOIL_SENSORS; i++) {
        s_probe[i] = (probe_t){.raw = -1, .percent = -1, .announced = -1};
    }
}

static void sample_one(int idx)
{
    int v[SOIL_READS];
    int n = 0;
    for (int k = 0; k < SOIL_READS; k++) {
        int r = read_raw(idx);
        if (r >= 0) {
            v[n++] = r;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    const char *fault = NULL;
    int raw = -1;
    if (n <= SOIL_READS / 2) {
        fault = "range";
    } else {
        qsort(v, n, sizeof(v[0]), cmp_int);
        raw = v[n / 2];
        if (raw < SOIL_RAW_MIN || raw > SOIL_RAW_MAX) {
            fault = "range";
        } else if (v[n - 1] - v[0] > SOIL_NOISE_MAX) {
            fault = "noise";
        }
    }
    int percent = raw >= 0 ? raw_to_percent(raw, idx == 0 ? &soil_cal_1 : &soil_cal_2) : -1;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    probe_t *p = &s_probe[idx];
    p->raw = raw;
    p->percent = percent;
    if (fault != NULL) {
        p->good = 0;
        if (p->bad < UINT8_MAX) p->bad++;
        if (p->bad >= SOIL_STREAK) {
            p->known = true;
            p->healthy = false;
            p->fault = fault;
        }
    } else {
        p->bad = 0;
        if (p->good < UINT8_MAX) p->good++;
        if (p->good >= SOIL_STREAK) {
            p->known = true;
            p->healthy = true;
            p->fault = NULL;
        }
    }
    if (p->stuck && percent >= 0 && abs(percent - p->stuck_at) >= SOIL_STUCK_CLEAR) {
        p->stuck = false;
    }
    int prev = p->announced;
    int now_ok = p->known ? (probe_ok(p) ? 1 : 0) : -1;
    const char *now_fault = probe_fault(p);
    if (now_ok >= 0) {
        p->announced = now_ok;
    }
    xSemaphoreGive(s_lock);

    if (now_ok == 0 && prev != 0) {
        ESP_LOGW(SOIL_TAG, "Датчик %d неисправен: %s (raw %d)", idx + 1, now_fault, raw);
        emit("soil_fault", idx, now_fault, raw, percent);
    } else if (now_ok == 1 && prev == 0) {
        ESP_LOGI(SOIL_TAG, "Датчик %d снова исправен (raw %d)", idx + 1, raw);
        emit("soil_ok", idx, NULL, raw, percent);
    }
}

void soil_sample(void)
{
    if (s_lock == NULL) {
        return;
    }
    for (int i = 0; i < SOIL_SENSORS; i++) {
        sample_one(i);
    }
}

void soil_get(soil_reading_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SOIL_SENSORS; i++) {
        out->percent[i] = s_probe[i].percent;
        out->raw[i] = s_probe[i].raw;
        out->ok[i] = probe_ok(&s_probe[i]);
        out->fault[i] = probe_fault(&s_probe[i]);
    }
    xSemaphoreGive(s_lock);

    const int a = out->percent[0];
    const int b = out->percent[1];
    if (out->ok[0] && out->ok[1]) {
        if (abs(a - b) <= SOIL_MAX_DIFF) {
            out->source = "avg";
            out->value = (a + b) / 2;
        } else {
            // Лишняя вода опаснее короткой засухи; зависший «влажный» датчик ловит water.c
            out->source = "wetter";
            out->value = a > b ? a : b;
        }
    } else if (out->ok[0]) {
        out->source = "s1";
        out->value = a;
    } else if (out->ok[1]) {
        out->source = "s2";
        out->value = b;
    } else {
        out->source = "none";
        out->value = -1;
    }
}

void soil_mark_stuck(int idx)
{
    if (idx < 0 || idx >= SOIL_SENSORS) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    probe_t *p = &s_probe[idx];
    bool was = p->stuck;
    p->stuck = true;
    p->stuck_at = p->percent;
    p->announced = 0;
    int raw = p->raw;
    int percent = p->percent;
    xSemaphoreGive(s_lock);
    if (!was) {
        ESP_LOGW(SOIL_TAG, "Датчик %d не реагирует на полив — исключён", idx + 1);
        emit("soil_fault", idx, "stuck", raw, percent);
    }
}

void soil_clear_stuck(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SOIL_SENSORS; i++) {
        s_probe[i].stuck = false;
    }
    xSemaphoreGive(s_lock);
}

void soil_add_json(cJSON *root)
{
    if (s_lock == NULL) {
        return;
    }
    soil_reading_t r;
    soil_get(&r);
    if (r.value >= 0) {
        cJSON_AddNumberToObject(root, "soil", r.value);
    } else {
        cJSON_AddNullToObject(root, "soil");
    }
    cJSON_AddStringToObject(root, "soil_source", r.source);
    static const char *const RAW_KEYS[] = {"soil1_raw", "soil2_raw"};
    static const char *const OK_KEYS[] = {"soil1_ok", "soil2_ok"};
    static const char *const FAULT_KEYS[] = {"soil1_fault", "soil2_fault"};
    for (int i = 0; i < SOIL_SENSORS; i++) {
        cJSON_AddNumberToObject(root, RAW_KEYS[i], r.raw[i]);
        cJSON_AddBoolToObject(root, OK_KEYS[i], r.ok[i]);
        if (r.fault[i] != NULL) {
            cJSON_AddStringToObject(root, FAULT_KEYS[i], r.fault[i]);
        } else {
            cJSON_AddNullToObject(root, FAULT_KEYS[i]);
        }
    }
}
