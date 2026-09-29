#ifndef MQTT_LOG_H
#define MQTT_LOG_H

#include <stdint.h>

void mqtt_log_start(void);
void mqtt_log_feed_sample(int moisture, int moisture2, int temperature, int humidity);

// Сколько секунд прошло с последней успешной отправки (или со старта)
uint32_t mqtt_log_since_publish_s(void);

#endif
