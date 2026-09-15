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

static const char *MQTT_TAG = "MQTT_LOG";

#define PUBLISH_INTERVAL_US (60 * 1000000LL)

static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static int64_t s_last_publish_us;

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

    state.uptime = (uint32_t)(esp_timer_get_time() / 1000000);

    cJSON_AddNumberToObject(root, "moisture", avg_m);
    cJSON_AddNumberToObject(root, "moisture2", avg_m2);
    cJSON_AddNumberToObject(root, "temperature", avg_t);
    cJSON_AddNumberToObject(root, "humidity", avg_h);
    cJSON_AddNumberToObject(root, "samples", samples);
    cJSON_AddNumberToObject(root, "uptime", state.uptime);
    cJSON_AddStringToObject(root, "ip", state.ip);
    cJSON_AddStringToObject(root, "version", VERSION);

    cJSON *relays = cJSON_CreateArray();
    for (int i = 0; i < 8; i++) {
        cJSON_AddItemToArray(relays, cJSON_CreateNumber(state.relays[i]));
    }
    cJSON_AddItemToObject(root, "relays", relays);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
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
    if (msg_id < 0) {
        ESP_LOGW(MQTT_TAG, "Publish не отправлен");
    } else {
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
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(MQTT_TAG, "Подключено к брокеру %s", MQTT_HOST);
        break;
    case MQTT_EVENT_DISCONNECTED:
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
    reset_accumulator();
    s_last_publish_us = esp_timer_get_time();

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
