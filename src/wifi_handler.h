#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include "variables.h"

#define WIFI_CONNECTED_BIT BIT0

// Объявляем функцию
void wifi_init_sta(void);

// Сколько секунд нет IP-адреса (0 — подключено)
uint32_t wifi_offline_s(void);

#endif
