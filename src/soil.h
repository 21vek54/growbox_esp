#ifndef SOIL_H
#define SOIL_H

#include <stdbool.h>
#include "cJSON.h"

#define SOIL_SENSORS 2

typedef struct {
    int value;                          // итоговая влажность, %; -1 — нет исправных датчиков
    const char *source;                 // avg | wetter | s1 | s2 | none
    int percent[SOIL_SENSORS];          // -1 — нет чтения
    int raw[SOIL_SENSORS];
    bool ok[SOIL_SENSORS];              // исправен и не исключён
    const char *fault[SOIL_SENSORS];    // NULL | range | noise | stuck
} soil_reading_t;

void soil_init(void);

// sensors_task, раз в 5 с: опрос обоих датчиков и проверка исправности
void soil_sample(void);

void soil_get(soil_reading_t *out);

// Датчик не отреагировал на полив, а второй отреагировал — исключить до изменения показаний
void soil_mark_stuck(int idx);
void soil_clear_stuck(void);

// soil, soil_source, soilN_raw, soilN_ok, soilN_fault
void soil_add_json(cJSON *root);

#endif
