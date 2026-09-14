#include "arduino_ota.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "esp_rom_md5.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "variables.h"

static const char *OTA_TAG = "ESPOTA";

#define OTA_CMD_FLASH 0
#define OTA_CMD_AUTH  200
#define OTA_CHUNK_SIZE 1024
#define OTA_RX_TIMEOUT_MS 10000

static void md5_hex(const uint8_t *data, size_t len, char out[33])
{
    uint8_t digest[16];
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, data, (uint32_t)len);
    esp_rom_md5_final(digest, &ctx);
    for (int i = 0; i < 16; i++) {
        sprintf(out + i * 2, "%02x", digest[i]);
    }
    out[32] = '\0';
}

static void udp_reply(int udp, const struct sockaddr_in *to, const char *msg)
{
    sendto(udp, msg, strlen(msg), 0, (const struct sockaddr *)to, sizeof(*to));
}

static bool auth_ok(const char *nonce, const char *cnonce, const char *response)
{
    char pass_md5[33];
    char expected[33];
    char challenge[128];

    md5_hex((const uint8_t *)OTA_PASS, strlen(OTA_PASS), pass_md5);
    snprintf(challenge, sizeof(challenge), "%s:%s:%s", pass_md5, nonce, cnonce);
    md5_hex((const uint8_t *)challenge, strlen(challenge), expected);
    return strcasecmp(expected, response) == 0;
}

static bool run_update(const char *host_ip, uint16_t host_port, uint32_t image_size, const char *expect_md5)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        ESP_LOGE(OTA_TAG, "Нет OTA-слота. Нужна таблица с ota_0/ota_1");
        return false;
    }

    ESP_LOGI(OTA_TAG, "Пишем в %s (0x%lx, %lu байт)",
             part->label, (unsigned long)part->address, (unsigned long)image_size);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return false;
    }

    int tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (tcp < 0) {
        ESP_LOGE(OTA_TAG, "socket() failed");
        esp_ota_abort(handle);
        return false;
    }

    struct timeval tv = {
        .tv_sec = OTA_RX_TIMEOUT_MS / 1000,
        .tv_usec = 0,
    };
    setsockopt(tcp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(host_port);
    inet_aton(host_ip, &dest.sin_addr);

    if (connect(tcp, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(OTA_TAG, "Не удалось подключиться к %s:%u", host_ip, host_port);
        close(tcp);
        esp_ota_abort(handle);
        return false;
    }

    md5_context_t md5;
    esp_rom_md5_init(&md5);

    uint8_t *buf = malloc(OTA_CHUNK_SIZE);
    if (buf == NULL) {
        close(tcp);
        esp_ota_abort(handle);
        return false;
    }

    uint32_t received = 0;
    bool ok = true;
    while (received < image_size) {
        size_t want = image_size - received;
        if (want > OTA_CHUNK_SIZE) {
            want = OTA_CHUNK_SIZE;
        }

        size_t got = 0;
        while (got < want) {
            int n = recv(tcp, buf + got, want - got, 0);
            if (n <= 0) {
                ESP_LOGE(OTA_TAG, "Обрыв загрузки на %lu/%lu",
                         (unsigned long)received, (unsigned long)image_size);
                ok = false;
                break;
            }
            got += (size_t)n;
        }
        if (!ok) {
            break;
        }

        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(OTA_TAG, "esp_ota_write: %s", esp_err_to_name(err));
            ok = false;
            break;
        }

        esp_rom_md5_update(&md5, buf, (uint32_t)got);
        received += got;

        char ack[16];
        int ack_len = snprintf(ack, sizeof(ack), "%u", (unsigned)got);
        if (send(tcp, ack, ack_len, 0) < 0) {
            ESP_LOGE(OTA_TAG, "Не удалось отправить ACK");
            ok = false;
            break;
        }
    }

    uint8_t digest[16];
    char got_md5[33];
    esp_rom_md5_final(digest, &md5);
    for (int i = 0; i < 16; i++) {
        sprintf(got_md5 + i * 2, "%02x", digest[i]);
    }
    got_md5[32] = '\0';
    free(buf);

    if (!ok || strcasecmp(got_md5, expect_md5) != 0) {
        ESP_LOGE(OTA_TAG, "MD5 не совпал (ожидали %s, получили %s)", expect_md5, got_md5);
        close(tcp);
        esp_ota_abort(handle);
        return false;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_end: %s", esp_err_to_name(err));
        close(tcp);
        return false;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        close(tcp);
        return false;
    }

    send(tcp, "OK", 2, 0);
    close(tcp);
    ESP_LOGI(OTA_TAG, "Прошивка принята (%lu байт), перезагрузка...", (unsigned long)received);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return true;
}

static void ota_task(void *arg)
{
    (void)arg;

    int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp < 0) {
        ESP_LOGE(OTA_TAG, "Не удалось создать UDP-сокет");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(OTA_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(udp, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(OTA_TAG, "bind(%u) failed", OTA_PORT);
        close(udp);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(OTA_TAG, "Слушаю ArduinoOTA на UDP %u, пароль задан", OTA_PORT);

    char packet[160];
    char nonce[33] = {0};
    bool wait_auth = false;
    uint16_t pending_port = 0;
    uint32_t pending_size = 0;
    char pending_md5[33] = {0};
    char pending_ip[16] = {0};

    while (1) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        int n = recvfrom(udp, packet, sizeof(packet) - 1, 0,
                         (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        packet[n] = '\0';

        char from_ip[16];
        inet_ntoa_r(from.sin_addr, from_ip, sizeof(from_ip));

        if (!wait_auth) {
            int cmd = -1;
            int port = 0;
            unsigned int size = 0;
            char md5[40] = {0};
            if (sscanf(packet, "%d %d %u %32s", &cmd, &port, &size, md5) < 4) {
                continue;
            }
            if (cmd != OTA_CMD_FLASH || port <= 0 || size == 0 || strlen(md5) != 32) {
                ESP_LOGW(OTA_TAG, "Отклонено приглашение cmd=%d size=%u", cmd, size);
                continue;
            }

            pending_port = (uint16_t)port;
            pending_size = size;
            memcpy(pending_md5, md5, 33);
            strncpy(pending_ip, from_ip, sizeof(pending_ip) - 1);

            char seed[40];
            snprintf(seed, sizeof(seed), "%llu", (unsigned long long)esp_timer_get_time());
            md5_hex((const uint8_t *)seed, strlen(seed), nonce);

            char auth[48];
            snprintf(auth, sizeof(auth), "AUTH %s", nonce);
            udp_reply(udp, &from, auth);
            wait_auth = true;
            ESP_LOGI(OTA_TAG, "Приглашение от %s, ждём AUTH", from_ip);
            continue;
        }

        int cmd = -1;
        char cnonce[40] = {0};
        char response[40] = {0};
        if (sscanf(packet, "%d %32s %32s", &cmd, cnonce, response) < 3 || cmd != OTA_CMD_AUTH) {
            ESP_LOGW(OTA_TAG, "Ожидался AUTH, сброс");
            wait_auth = false;
            continue;
        }

        if (!auth_ok(nonce, cnonce, response)) {
            udp_reply(udp, &from, "Authentication Failed");
            ESP_LOGW(OTA_TAG, "Неверный пароль OTA");
            wait_auth = false;
            continue;
        }

        udp_reply(udp, &from, "OK");
        wait_auth = false;
        ESP_LOGI(OTA_TAG, "AUTH OK, качаем %lu байт с %s:%u",
                 (unsigned long)pending_size, pending_ip, pending_port);
        run_update(pending_ip, pending_port, pending_size, pending_md5);
    }
}

void arduino_ota_start(void)
{
    xTaskCreate(ota_task, "espota", 8192, NULL, 5, NULL);
}

void arduino_ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(OTA_TAG, "Запущено из %s @ 0x%lx",
                 running->label, (unsigned long)running->address);
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(OTA_TAG, "mark_valid: %s", esp_err_to_name(err));
    }
}
