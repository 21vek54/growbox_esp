#ifndef WATER_H
#define WATER_H

#include "cJSON.h"

// Нормально закрытый клапан полива на реле 2 (индекс 1 у TCA9554)
#define WATER_RELAY 1

// Загрузить настройки и счётчики из NVS (после health_init, до mqtt_log_start)
void water_init(void);

// Раз в секунду из главного цикла, после tca9554_init
void water_tick(void);

// {"mode":"auto|off","start":35,"target":50,"dose_s":20,"soak_min":20,"max_doses":6,
//  "now":20,"stop":true,"reset":true}; поля необязательны. NULL — успех, иначе текст ошибки
const char *water_apply(const cJSON *cfg);

// valve, water_* поля состояния и настроек
void water_add_json(cJSON *root);

#endif
