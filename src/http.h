#ifndef HTTP_H
#define HTTP_H

#include "variables.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

void init_spiffs(void);
void start_webserver(void);


#endif