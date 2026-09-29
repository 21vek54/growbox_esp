#ifndef MQTT_LOG_H
#define MQTT_LOG_H

#include <stdint.h>
#include "cJSON.h"

void mqtt_log_start(void);
void mqtt_log_feed_sample(int moisture, int moisture2, int temperature, int humidity);

// Сколько секунд прошло с последней успешной отправки (или со старта)
uint32_t mqtt_log_since_publish_s(void);

// Опубликовать мгновенный снимок в termo/home/rastishka/snapshot (из mqtt_log_service)
void mqtt_log_request_snapshot(const char *reason);

// Событие в termo/home/rastishka/event (без retain) + снимок. Забирает владение ev;
// добавляет time и uptime. Можно звать из любой задачи
void mqtt_log_event(cJSON *ev);

// Главный цикл, раз в секунду: отправить события и запрошенный снимок
void mqtt_log_service(void);

#endif
