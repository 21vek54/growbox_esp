# 🌱 growbox_esp — Растишка

Прошивка умного гроубокса на **ESP32-S3** (PlatformIO + ESP-IDF): датчики, реле, HTTP API и обновление по воздуху (ArduinoOTA / espota).

![Превью растения](docs/plant-preview.png)

## Возможности

- Температура и влажность воздуха (RS485)
- Влажность почвы (два датчика ADC)
- Управление реле через TCA9554 (фитолампа, полив)
- HTTP API для статуса и реле
- OTA по Wi‑Fi: `pio run -e ota -t upload` (протокол ArduinoOTA / espota)
- MQTT на Mosquitto SZBOX: средняя телеметрия раз в минуту (`termo/home/rastishka/status`)

## Железо

| Параметр | Значение |
|----------|----------|
| Плата | `esp32-s3-devkitc-1` |
| Flash | 16 MB |
| PSRAM | 8 MB |
| Framework | ESP-IDF |

## Быстрый старт

```bash
# Первая прошивка — только USB (меняет таблицу разделов)
pio run -e esp32-s3-devkitc-1 -t upload

# Монитор — в логе будет IP
pio device monitor
```

Дальше по воздуху. В `platformio.ini` в секции `[env:ota]` укажите IP платы и тот же пароль, что `OTA_PASS` в `src/variables.h` (сейчас `rastishka-ota`):

```bash
pio run -e ota -t upload
```

Статус: `http://<IP>/api/status`

### MQTT (средние раз в минуту)

1. Скопируйте `src/secrets.h.example` → `src/secrets.h` и укажите пароль пользователя `esp` на брокере `192.168.1.92`.
2. После прошивки подписка:

```bash
mosquitto_sub -h 192.168.1.92 -u esp -P '...' -t "termo/home/rastishka/#" -v
```

Сообщения с retain, примерно раз в 60 с; в JSON есть `samples` (число усреднённых опросов за минуту).

## Структура

```
src/           — main, HTTP, Wi‑Fi, ArduinoOTA
lib/           — ADC, I2C, RS485, TCA9554
docs/          — превью UI
```

## HTTP API

| Метод | Путь | Описание |
|-------|------|----------|
| `GET` | `/` | JSON: имя, версия, ota |
| `GET` | `/api/status` | JSON: температура, влажность, почва, реле, IP, версия |
| `POST` | `/api/relay` | `{"relay":0\|1,"state":0\|1}` |
| `POST` | `/api/relay/all` | `{"state":0\|1}` |

## Лицензия

Проект для совместной работы. См. репозиторий: https://github.com/21vek54/growbox_esp
