#include "wifi_handler.h"

#define WIFI_RETRY_MIN_MS   2000
#define WIFI_RETRY_MAX_MS   60000
#define WIFI_START_WAIT_MS  30000

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;
static esp_timer_handle_t s_retry_timer;
static volatile bool s_online;
static volatile uint32_t s_offline_since_s;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    uint32_t delay_ms = WIFI_RETRY_MIN_MS;
    for (int i = 0; i < s_retry_num && delay_ms < WIFI_RETRY_MAX_MS; i++) {
        delay_ms *= 2;
    }
    if (delay_ms > WIFI_RETRY_MAX_MS) {
        delay_ms = WIFI_RETRY_MAX_MS;
    }
    s_retry_num++;

    ESP_LOGW(TAG, "Отключено, переподключение через %lu с (попытка %d)",
             (unsigned long)(delay_ms / 1000), s_retry_num);
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

// ============================================
// Wi-Fi обработчик
// ============================================
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_online) {
            s_offline_since_s = now_s();
            s_online = false;
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        schedule_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(state.ip, sizeof(state.ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "✅ Подключено! IP: %s", state.ip);
        s_retry_num = 0;
        s_online = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

uint32_t wifi_offline_s(void)
{
    if (s_online) {
        return 0;
    }
    return now_s() - s_offline_since_s;
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "❌ Не удалось создать Event Group");
        return;
    }

    const esp_timer_create_args_t retry_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    s_offline_since_s = now_s();
    s_online = false;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold = {
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Подключение к Wi-Fi...");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_START_WAIT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ Подключено к Wi-Fi!");
    } else {
        ESP_LOGW(TAG, "⚠️ Wi-Fi пока нет, продолжаем запуск, переподключение в фоне");
    }
}
