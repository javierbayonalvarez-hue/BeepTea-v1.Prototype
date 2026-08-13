#pragma once

#include <Arduino.h>

#if defined(ARDUINO_SEEED_XIAO_ESP32S3)
static constexpr uint16_t SCREEN_W = 320;
static constexpr uint16_t SCREEN_H = 240;

static constexpr uint8_t PIN_VIBRATION = 43;      // D6
static constexpr uint8_t PIN_BUZZER = 44;         // D7
static constexpr uint8_t PIN_BUTTON_OK = 1;       // D0
static constexpr uint8_t PIN_BUTTON_MENU = 2;     // D1
static constexpr uint8_t PIN_POWER_ON = 15;       // Unused GPIO on XIAO (acting as dummy)
static constexpr uint8_t PIN_BATTERY_ADC = 4;     // D3 / A3
static constexpr uint8_t PIN_LCD_BACKLIGHT = 8;   // D9

static constexpr uint8_t PIN_I2C_SDA = 5;         // D4
static constexpr uint8_t PIN_I2C_SCL = 6;         // D5
#else
static constexpr uint16_t SCREEN_W = 320;
static constexpr uint16_t SCREEN_H = 170;

static constexpr uint8_t PIN_VIBRATION = 1;
static constexpr uint8_t PIN_BUZZER = 2;
static constexpr uint8_t PIN_BUTTON_OK = 0;
static constexpr uint8_t PIN_BUTTON_MENU = 14;
static constexpr uint8_t PIN_POWER_ON = 15;
static constexpr uint8_t PIN_BATTERY_ADC = 4;
static constexpr uint8_t PIN_LCD_BACKLIGHT = 38;

static constexpr uint8_t PIN_I2C_SDA = 17;
static constexpr uint8_t PIN_I2C_SCL = 18;
#endif


static constexpr uint32_t LONG_PRESS_MS = 900;
static constexpr uint32_t DISPLAY_REFRESH_MS = 1000;
static constexpr uint32_t ALERT_PATTERN_MS = 1400;

static constexpr const char *ROUTINE_PATH = "/rutina.json";
static constexpr const char *DEVICE_NAME = "buscatea-demo";
static constexpr const char *AP_SSID = "BeepTea Setup";
static constexpr const char *AP_PASSWORD = "btea1234";
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// Credenciales opcionales para pruebas. Si no hay Wi-Fi guardada por BLE/API,
// el firmware intentara conectarse a esta red antes de abrir BuscaTea-Setup.
static constexpr const char *DEFAULT_WIFI_SSID = "";
static constexpr const char *DEFAULT_WIFI_PASSWORD = "";
static constexpr bool FORCE_DEFAULT_WIFI = false;

static constexpr const char *BLE_SERVICE_UUID = "7d2f6a40-8f4f-4a9f-9f25-2f5a6f0b1000";
static constexpr const char *BLE_STATUS_UUID = "7d2f6a40-8f4f-4a9f-9f25-2f5a6f0b1001";
static constexpr const char *BLE_WIFI_UUID = "7d2f6a40-8f4f-4a9f-9f25-2f5a6f0b1002";
