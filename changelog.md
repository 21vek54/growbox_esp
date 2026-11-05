# Changelog — growbox_esp (Растишка)

Версия в прошивке: `#define VERSION` в `src/variables.h` (поле `version` в MQTT/HTTP).

Формат: [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/), версии по смыслу близки к [SemVer](https://semver.org/lang/ru/).

## [0.8] — 2026-11-05

### Changed
- Фитолампа на **NC-контакте** реле 1: расписание считает «лампа горит», инверсия только при записи в TCA9554.
- Поля `light` и `relays[0]` — логическое состояние лампы (`1` = горит), не уровень катушки.

### Fixed
- Вспышка лампы при старте: в `tca9554_init` сначала OUTPUT (катушка реле 1 вкл), потом CONFIG; сразу после init вызывается `light_tick()`.

## [0.7] — 2026-09-29

### Added
- Автополив на реле 2: порции с паузой на впитывание, суточный лимит, ручная порция.
- Контроль исправности датчиков почвы (range / noise / stuck), слияние двух каналов (`soil`, `soil_source`).
- MQTT-топик `termo/home/rastishka/event` (`soil_fault`, `water_*` и др.).
- Команда `water` в `cmd` (режим, пороги, `now` / `stop` / `reset`).

## [0.6] — 2026-09-29

### Added
- Расписание фитолампы на реле 1 (TCA9554), хранение в NVS (`auto` / `on` / `off`).
- Синхронизация времени SNTP (UTC+7), поля `time` / `time_status`.
- MQTT `cmd` и `snapshot` (retain), настройка лампы с сервера.

### Removed
- HTTP-управление реле и веб-UI; остался только `GET /api/status`.

## [0.5] — 2026-09-29

### Added
- Health / watchdog: Task WDT, перезагрузка при долгой потере Wi‑Fi или MQTT.
- В телеметрии: `reset_reason`, `boot_count`.

## [0.4] — 2026-09-15

### Added
- Минутная усреднённая телеметрия в Mosquitto: `termo/home/rastishka/status`.

## [0.3] — 2026-09-14

### Added
- OTA по Wi‑Fi (ArduinoOTA / espota), env `ota` в PlatformIO.

## [0.2b] — 2026-09-14

### Added
- Второй датчик влажности почвы (ADC) и поля `moisture2` в статусе.

## [0.1] — 2026-09-10

### Added
- Стартовая прошивка ESP32-S3 (ESP-IDF): Wi‑Fi, I2C/TCA9554, RS485 (T/H), ADC почвы, HTTP-статус, UI-превью растения.
