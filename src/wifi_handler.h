#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include "variables.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Объявляем функцию
void wifi_init_sta(void);

#endif