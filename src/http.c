#include "http.h"

// ============================================
// SPIFFS инициализация
// ============================================
void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка монтирования SPIFFS (%s)", esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка получения информации SPIFFS");
    } else {
        ESP_LOGI(TAG, "SPIFFS: всего %d байт, используется %d байт", total, used);
    }
}

// ============================================
// HTTP обработчики
// ============================================

// Главная страница
static esp_err_t root_get_handler(httpd_req_t *req)
{
    FILE* f = fopen("/spiffs/index.html", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Не удалось открыть index.html");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = malloc(size + 1);
    if (buffer == NULL) {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t read_size = fread(buffer, 1, size, f);
    buffer[read_size] = '\0';
    fclose(f);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, buffer, read_size);
    
    free(buffer);
    return ESP_OK;
}

// API статус
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

// API управление реле
static esp_err_t api_relay_post_handler(httpd_req_t *req)
{
    char buffer[128];
    int ret = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
    
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buffer[ret] = '\0';

    cJSON *json = cJSON_Parse(buffer);
    if (json == NULL) {
        ESP_LOGE(TAG, "Ошибка парсинга JSON: %s", buffer);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *relay_item = cJSON_GetObjectItem(json, "relay");
    cJSON *state_item = cJSON_GetObjectItem(json, "state");
    
    if (!cJSON_IsNumber(relay_item) || !cJSON_IsNumber(state_item)) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int relay_num = relay_item->valueint;
    int new_state = state_item->valueint;
    
    if (relay_num >= 0 && relay_num < 8) {
        state.relays[relay_num] = new_state;
        esp_err_t err = tca9554_set_relay(&tca9554, relay_num, new_state);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Реле %d %s", relay_num + 1, new_state ? "ВКЛ" : "ВЫКЛ");
        } else {
            ESP_LOGE(TAG, "Ошибка управления реле %d", relay_num + 1);
        }
    }

    cJSON_Delete(json);

    cJSON *response_json = cJSON_CreateObject();
    cJSON_AddBoolToObject(response_json, "success", true);
    char *response = cJSON_Print(response_json);
    cJSON_Delete(response_json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    
    free(response);
    return ESP_OK;
}

// API управление всеми реле
static esp_err_t api_relay_all_post_handler(httpd_req_t *req)
{
    char buffer[128];
    int ret = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
    
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buffer[ret] = '\0';

    cJSON *json = cJSON_Parse(buffer);
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *state_item = cJSON_GetObjectItem(json, "state");
    if (!cJSON_IsNumber(state_item)) {
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int new_state = state_item->valueint;
    uint8_t mask = 0;
    
    for (int i = 0; i < 8; i++) {
        state.relays[i] = new_state;
        if (new_state) {
            mask |= (1 << i);
        }
    }
    
    esp_err_t err = tca9554_set_output(&tca9554, mask);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Все реле %s", new_state ? "ВКЛ" : "ВЫКЛ");
    }

    cJSON_Delete(json);

    cJSON *response_json = cJSON_CreateObject();
    cJSON_AddBoolToObject(response_json, "success", true);
    char *response = cJSON_Print(response_json);
    cJSON_Delete(response_json);

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

        httpd_uri_t uri_api_relay = {
            .uri       = "/api/relay",
            .method    = HTTP_POST,
            .handler   = api_relay_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_api_relay);

        httpd_uri_t uri_api_relay_all = {
            .uri       = "/api/relay/all",
            .method    = HTTP_POST,
            .handler   = api_relay_all_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_api_relay_all);

        ESP_LOGI(TAG, "✅ Веб-сервер запущен!");
    } else {
        ESP_LOGE(TAG, "❌ Ошибка запуска веб-сервера");
    }
}