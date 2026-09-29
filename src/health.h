#ifndef HEALTH_H
#define HEALTH_H

#include <stdint.h>

// Вызывать первым в app_main: NVS, причина старта, счётчик загрузок, Task WDT
void health_init(void);

// Вызывать раз в секунду: перезагрузка, если долго нет Wi-Fi или отправки в MQTT
void health_check(void);

const char *health_reset_reason(void);
uint32_t health_boot_count(void);

#endif
