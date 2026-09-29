#include "http.h"

static esp_err_t root_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "name", "rastishka");
    cJSON_AddStringToObject(root, "version", VERSION);
    cJSON_AddStringToObject(root, "ota", "espota");
    char *response = cJSON_Print(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    return ESP_OK;
}

// API статус (только чтение; реле управляются прошивкой через TCA9554)
static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    state.uptime = (uint32_t)(esp_timer_get_time() / 1000000);
    
    cJSON_AddNumberToObject(root, "moisture", state.moisture);      // Влажность почвы
     cJSON_AddNumberToObject(root, "moisture2", state.moisture2);       // Влажность почвы 2
    cJSON_AddNumberToObject(root, "temperature", state.temperature); // Температура
    cJSON_AddNumberToObject(root, "humidity", state.humidity);      // Влажность воздуха
    cJSON_AddNumberToObject(root, "uptime", state.uptime);
    cJSON_AddStringToObject(root, "ip", state.ip);
    cJSON_AddStringToObject(root, "version", VERSION);
    
    // Состояние реле
    cJSON *relays = cJSON_CreateArray();
    for (int i = 0; i < 8; i++) {
        cJSON_AddItemToArray(relays, cJSON_CreateNumber(state.relays[i]));
    }
    cJSON_AddItemToObject(root, "relays", relays);

    char *response = cJSON_Print(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    
    free(response);
    return ESP_OK;
}

// ============================================
// Запуск веб-сервера
// ============================================
void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_api_status = {
            .uri       = "/api/status",
            .method    = HTTP_GET,
            .handler   = api_status_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_api_status);

        ESP_LOGI(TAG, "✅ Веб-сервер запущен!");
    } else {
        ESP_LOGE(TAG, "❌ Ошибка запуска веб-сервера");
    }
}
