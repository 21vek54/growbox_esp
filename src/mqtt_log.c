#include "mqtt_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "variables.h"
#include "secrets.h"
#include "health.h"
#include "light.h"
#include "soil.h"
#include "water.h"
#include "time_sync.h"

static const char *MQTT_TAG = "MQTT_LOG";

#ifndef MQTT_CMD_TOPIC
#define MQTT_CMD_TOPIC "termo/home/rastishka/cmd"
#endif
#ifndef MQTT_SNAPSHOT_TOPIC
#define MQTT_SNAPSHOT_TOPIC "termo/home/rastishka/snapshot"
#endif
#ifndef MQTT_EVENT_TOPIC
#define MQTT_EVENT_TOPIC "termo/home/rastishka/event"
#endif

#define EVENT_QUEUE_LEN 8

#define PUBLISH_INTERVAL_US (60 * 1000000LL)

static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static int64_t s_last_publish_us;
static volatile bool s_connected;
static volatile uint32_t s_last_publish_ok_s;

// Снимок публикует главный цикл: из обработчика MQTT нельзя брать s_lock
// (sensors_task держит его во время publish)
static SemaphoreHandle_t s_reply_lock;
static struct {
    bool pending;
    char reason[12];
    char req_id[40];
    char error[48];
} s_reply;

// Отдельный замок: события шлют модули, которые сами держат свои замки
static SemaphoreHandle_t s_event_lock;
static char *s_events[EVENT_QUEUE_LEN];
static int s_event_count;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static struct {
    int64_t sum_moisture;
    int64_t sum_moisture2;
    int64_t sum_temperature;
    int64_t sum_humidity;
    int count;
} s_acc;

static void reset_accumulator(void)
{
    s_acc.sum_moisture = 0;
    s_acc.sum_moisture2 = 0;
    s_acc.sum_temperature = 0;
    s_acc.sum_humidity = 0;
    s_acc.count = 0;
}

static void add_common(cJSON *root)
{
    char now[32];
    time_sync_format(now, sizeof(now));
    state.uptime = (uint32_t)(esp_timer_get_time() / 1000000);

    cJSON_AddNumberToObject(root, "uptime", state.uptime);
    cJSON_AddStringToObject(root, "ip", state.ip);
    cJSON_AddStringToObject(root, "version", VERSION);
    cJSON_AddStringToObject(root, "reset_reason", health_reset_reason());
    cJSON_AddNumberToObject(root, "boot_count", health_boot_count());
    if (now[0] != '\0') {
        cJSON_AddStringToObject(root, "time", now);
    } else {
        cJSON_AddNullToObject(root, "time");
    }
    cJSON_AddStringToObject(root, "time_status", time_sync_status());
    light_add_json(root);
    soil_add_json(root);
    water_add_json(root);

    cJSON *relays = cJSON_CreateArray();
    for (int i = 0; i < 8; i++) {
        cJSON_AddItemToArray(relays, cJSON_CreateNumber(state.relays[i]));
    }
    cJSON_AddItemToObject(root, "relays", relays);
}

static char *build_payload(int samples)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    int avg_m = samples > 0 ? (int)(s_acc.sum_moisture / samples) : state.moisture;
    int avg_m2 = samples > 0 ? (int)(s_acc.sum_moisture2 / samples) : state.moisture2;
    int avg_t = samples > 0 ? (int)(s_acc.sum_temperature / samples) : state.temperature;
    int avg_h = samples > 0 ? (int)(s_acc.sum_humidity / samples) : state.humidity;

    cJSON_AddNumberToObject(root, "moisture", avg_m);
    cJSON_AddNumberToObject(root, "moisture2", avg_m2);
    cJSON_AddNumberToObject(root, "temperature", avg_t);
    cJSON_AddNumberToObject(root, "humidity", avg_h);
    cJSON_AddNumberToObject(root, "samples", samples);
    add_common(root);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *build_snapshot(const char *reason, const char *req_id, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "moisture", state.moisture);
    cJSON_AddNumberToObject(root, "moisture2", state.moisture2);
    cJSON_AddNumberToObject(root, "temperature", state.temperature);
    cJSON_AddNumberToObject(root, "humidity", state.humidity);
    add_common(root);
    cJSON_AddStringToObject(root, "reason", reason);
    if (req_id[0] != '\0') {
        cJSON_AddStringToObject(root, "req_id", req_id);
    }
    cJSON_AddBoolToObject(root, "ok", error[0] == '\0');
    if (error[0] != '\0') {
        cJSON_AddStringToObject(root, "error", error);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void request_snapshot(const char *reason, const char *req_id, const char *error)
{
    if (s_reply_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    // Не затирать ожидающий ответ на команду событием без req_id
    if (!(s_reply.pending && s_reply.req_id[0] != '\0' && req_id[0] == '\0')) {
        snprintf(s_reply.reason, sizeof(s_reply.reason), "%s", reason);
        snprintf(s_reply.req_id, sizeof(s_reply.req_id), "%s", req_id);
        snprintf(s_reply.error, sizeof(s_reply.error), "%s", error);
    }
    s_reply.pending = true;
    xSemaphoreGive(s_reply_lock);
}

static void handle_cmd(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(MQTT_TAG, "Команда: не JSON");
        return;
    }
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const char *req_id = cJSON_IsString(id) ? id->valuestring : "";
    const char *error = NULL;
    const cJSON *light = cJSON_GetObjectItem(root, "light");
    if (light != NULL) {
        error = light_apply(light);
    }
    const cJSON *water = cJSON_GetObjectItem(root, "water");
    if (water != NULL && error == NULL) {
        error = water_apply(water);
    }
    ESP_LOGI(MQTT_TAG, "Команда %s: %s", req_id, error ? error : "ok");
    request_snapshot("cmd", req_id, error ? error : "");
    cJSON_Delete(root);
}

static void try_publish(void)
{
    if (s_client == NULL || s_acc.count <= 0) {
        return;
    }

    int samples = s_acc.count;
    char *payload = build_payload(samples);
    if (payload == NULL) {
        ESP_LOGE(MQTT_TAG, "Не удалось собрать JSON");
        return;
    }

    int msg_id = esp_mqtt_client_publish(s_client, MQTT_TOPIC, payload,
                                         0, 0, 1);
    if (msg_id < 0 || !s_connected) {
        ESP_LOGW(MQTT_TAG, "Publish не отправлен");
    } else {
        s_last_publish_ok_s = now_s();
        ESP_LOGI(MQTT_TAG, "Опубликовано %s (%d samples)", MQTT_TOPIC, samples);
    }
    free(payload);
    reset_accumulator();
    s_last_publish_us = esp_timer_get_time();
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        esp_mqtt_client_subscribe(s_client, MQTT_CMD_TOPIC, 1);
        request_snapshot("connect", "", "");
        ESP_LOGI(MQTT_TAG, "Подключено к брокеру %s", MQTT_HOST);
        break;
    case MQTT_EVENT_DATA:
        if (event->current_data_offset == 0 && event->data_len == event->total_data_len
            && event->topic_len == (int)strlen(MQTT_CMD_TOPIC)
            && strncmp(event->topic, MQTT_CMD_TOPIC, event->topic_len) == 0) {
            handle_cmd(event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(MQTT_TAG, "Отключено от брокера");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(MQTT_TAG, "Ошибка MQTT");
        break;
    default:
        break;
    }
}

void mqtt_log_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_reply_lock = xSemaphoreCreateMutex();
    s_event_lock = xSemaphoreCreateMutex();
    reset_accumulator();
    s_last_publish_us = esp_timer_get_time();
    s_last_publish_ok_s = now_s();

    char uri[64];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .session.keepalive = 60,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(MQTT_TAG, "Клиент MQTT → %s, топик %s", uri, MQTT_TOPIC);
}

uint32_t mqtt_log_since_publish_s(void)
{
    return now_s() - s_last_publish_ok_s;
}

void mqtt_log_request_snapshot(const char *reason)
{
    request_snapshot(reason, "", "");
}

void mqtt_log_event(cJSON *ev)
{
    if (ev == NULL) {
        return;
    }
    if (s_event_lock == NULL) {
        cJSON_Delete(ev);
        return;
    }
    char now[32];
    time_sync_format(now, sizeof(now));
    if (now[0] != '\0') {
        cJSON_AddStringToObject(ev, "time", now);
    } else {
        cJSON_AddNullToObject(ev, "time");
    }
    cJSON_AddNumberToObject(ev, "uptime", (double)now_s());
    const cJSON *name = cJSON_GetObjectItem(ev, "event");
    char reason[sizeof(s_reply.reason)];
    snprintf(reason, sizeof(reason), "%s", cJSON_IsString(name) ? name->valuestring : "event");
    char *json = cJSON_PrintUnformatted(ev);
    cJSON_Delete(ev);
    if (json == NULL) {
        return;
    }

    xSemaphoreTake(s_event_lock, portMAX_DELAY);
    if (s_event_count == EVENT_QUEUE_LEN) {
        free(s_events[0]);
        memmove(&s_events[0], &s_events[1], sizeof(s_events[0]) * (EVENT_QUEUE_LEN - 1));
        s_event_count--;
    }
    s_events[s_event_count++] = json;
    xSemaphoreGive(s_event_lock);

    request_snapshot(reason, "", "");
}

static void publish_events(void)
{
    for (;;) {
        xSemaphoreTake(s_event_lock, portMAX_DELAY);
        if (s_event_count == 0) {
            xSemaphoreGive(s_event_lock);
            return;
        }
        char *json = s_events[0];
        memmove(&s_events[0], &s_events[1], sizeof(s_events[0]) * (s_event_count - 1));
        s_event_count--;
        xSemaphoreGive(s_event_lock);

        esp_mqtt_client_publish(s_client, MQTT_EVENT_TOPIC, json, 0, 1, 0);
        ESP_LOGI(MQTT_TAG, "Событие %s", json);
        free(json);
    }
}

void mqtt_log_service(void)
{
    if (s_client == NULL || !s_connected || s_reply_lock == NULL) {
        return;
    }
    publish_events();

    char reason[sizeof(s_reply.reason)];
    char req_id[sizeof(s_reply.req_id)];
    char error[sizeof(s_reply.error)];

    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    if (!s_reply.pending) {
        xSemaphoreGive(s_reply_lock);
        return;
    }
    memcpy(reason, s_reply.reason, sizeof(reason));
    memcpy(req_id, s_reply.req_id, sizeof(req_id));
    memcpy(error, s_reply.error, sizeof(error));
    s_reply.pending = false;
    xSemaphoreGive(s_reply_lock);

    char *payload = build_snapshot(reason, req_id, error);
    if (payload == NULL) {
        return;
    }
    esp_mqtt_client_publish(s_client, MQTT_SNAPSHOT_TOPIC, payload, 0, 1, 1);
    ESP_LOGI(MQTT_TAG, "Снимок %s (%s)", MQTT_SNAPSHOT_TOPIC, reason);
    free(payload);
}

void mqtt_log_feed_sample(int moisture, int moisture2, int temperature, int humidity)
{
    if (s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_acc.sum_moisture += moisture;
    s_acc.sum_moisture2 += moisture2;
    s_acc.sum_temperature += temperature;
    s_acc.sum_humidity += humidity;
    s_acc.count++;

    int64_t now = esp_timer_get_time();
    if (now - s_last_publish_us >= PUBLISH_INTERVAL_US) {
        try_publish();
    }

    xSemaphoreGive(s_lock);
}
