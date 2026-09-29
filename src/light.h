#ifndef LIGHT_H
#define LIGHT_H

#include <stdbool.h>
#include "cJSON.h"

// Фитолампа на реле 1 (индекс 0 у TCA9554)
#define LIGHT_RELAY 0

// Загрузить расписание из NVS (после health_init, до mqtt_log_start)
void light_init(void);

// Раз в секунду, после tca9554_init: привести реле к расписанию.
// true — лампа переключилась
bool light_tick(void);

// Применить {"mode":"auto|on|off","on":"HH:MM","duration_min":N}; поля необязательны.
// NULL — успех, иначе текст ошибки
const char *light_apply(const cJSON *cfg);

// light, light_mode, light_on, light_duration_min
void light_add_json(cJSON *root);

#endif
