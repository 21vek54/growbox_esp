#ifndef MQTT_LOG_H
#define MQTT_LOG_H

void mqtt_log_start(void);
void mqtt_log_feed_sample(int moisture, int moisture2, int temperature, int humidity);

#endif
