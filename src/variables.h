#ifndef VARIABLE_H
#define VARIABLE_H

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "tca9554.h"

// ============================================
// Wi-Fi настройки
// ============================================
#define WIFI_SSID      "21VEK"
#define WIFI_PASS      "vek2121vek"

// ============================================
// TCA9554 настройки
// ============================================
#define TCA9554_ADDR           0x20

static const char *TAG = "MAIN";

extern tca9554_t tca9554;

// Состояние системы
typedef struct {
    uint8_t relays[8];
    int moisture;
    int temperature;
    int humidity;
    uint32_t uptime;
    char ip[16];
} system_state_t;

// Объявляем внешнюю переменную состояния
extern system_state_t state;

#endif