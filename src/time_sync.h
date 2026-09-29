#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Запуск SNTP и часового пояса (после wifi_init_sta)
void time_sync_start(void);

// Часы показывают реальное время (синхронизировано сейчас или сохранено в RTC)
bool time_sync_valid(void);

// "synced" — получено по NTP после загрузки, "rtc" — пережило перезагрузку, "none" — неизвестно
const char *time_sync_status(void);

// Локальное время ISO 8601 с поясом; пустая строка, если время неизвестно
void time_sync_format(char *buf, size_t len);

#endif
