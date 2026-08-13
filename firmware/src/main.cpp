#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <RTClib.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <driver/gpio.h>

#include "config.h"

struct RoutineItem {
  String id;
  String title;
  uint16_t startMin;
  uint16_t endMin;
  bool completed;
};

static TFT_eSPI tft;
static RTC_DS3231 rtc;
static AsyncWebServer server(80);
static Preferences prefs;
static NimBLECharacteristic *bleStatus = nullptr;

static bool bleConnected = false;
static RoutineItem routine[24];
static size_t routineCount = 0;
static int activeIndex = -1;
static bool rtcAvailable = false;
static bool wifiConnected = false;
static bool setupMode = false;
static bool restartPending = false;
static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
static String attemptedWifiSsid = "";
static bool needsMenuOpen = false;
static bool alertRunning = false;
static uint32_t alertStartedAt = 0;
static bool deviceVibration = true;
static bool deviceSound = true;
static uint8_t routineDaysMask = 0xFF;
static uint32_t deviceAlertMs = 3000;

RTC_DATA_ATTR uint8_t brightnessLevel = 1; // 0=Bajo, 1=Medio, 2=Alto

static void applyBrightness() {
  if (brightnessLevel == 0) ledcWrite(2, 50); // 20%
  else if (brightnessLevel == 1) ledcWrite(2, 140); // 55%
  else ledcWrite(2, 255); // 100%
}

static uint32_t lastDrawAt = 0;
static uint32_t okPressedAt = 0;
static uint32_t menuPressedAt = 0;
static bool okWasDown = false;
static bool menuWasDown = false;
static uint32_t restartAt = 0;

static uint32_t lastInteractionTime = 0;
static bool screenIsAsleep = false;

static void applyNetworkMode();
static void disableNetwork();

static bool previewMode = false;
static uint32_t previewStartedAt = 0;
static int previewIndex = -1;

static String routineName = "Sin rutina";
static String childName = "BuscaTEA";
static bool infoMode = false;
static uint32_t infoStartedAt = 0;
static bool menuLongPressed = false;
static int networkMode = 0;
static int lastDrawnIndex = -2;
static bool lastDrawnNeedsMenu = false;
static uint16_t lastDrawnMinute = 9999;
static bool lastDrawnCompleted = false;
static uint8_t lastDrawnProgress = 255;
static String lastDrawnIp = "";
static int lastDrawnBattery = -1;

static bool tftJpegOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= SCREEN_H) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

static uint16_t parseTimeToMinutes(const String &value) {
  int sep = value.indexOf(':');
  if (sep < 0) return 0;
  int hh = value.substring(0, sep).toInt();
  int mm = value.substring(sep + 1).toInt();
  hh = constrain(hh, 0, 23);
  mm = constrain(mm, 0, 59);
  return hh * 60 + mm;
}

static String minutesToTime(uint16_t value) {
  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02u:%02u", value / 60, value % 60);
  return String(buffer);
}

static float batteryMultiplier = 2.00f;

static uint32_t readBatteryMilliVolts() {
  analogRead(PIN_BATTERY_ADC); // dummy
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogReadMilliVolts(PIN_BATTERY_ADC);
    delay(1);
  }
  uint32_t raw_mv = (sum / 16) * 2;
  // Calibración por hardware: El LED de carga marca 100% (4200mV físicos), 
  // pero el ADC lee ~3952mV debido a la tolerancia de las resistencias. 
  // Factor de corrección: 4200 / 3952 = 1.0627
  return (uint32_t)(raw_mv * 1.0627f);
}

static int batteryPercent() {
  uint32_t millivolts = readBatteryMilliVolts();
  
  struct VoltageMap {
    uint32_t mv;
    int percentage;
  };
  
  static const VoltageMap table[] = {
    {4200, 100},
    {4100, 95},
    {4000, 85},
    {3900, 75},
    {3800, 60},
    {3700, 40},
    {3600, 15},
    {3500, 5},
    {3400, 0}
  };
  
  if (millivolts >= table[0].mv) return 100;
  if (millivolts <= table[8].mv) return 0;
  
  for (int i = 0; i < 8; i++) {
    if (millivolts <= table[i].mv && millivolts >= table[i+1].mv) {
      float frac = (float)(table[i].mv - millivolts) / (table[i].mv - table[i+1].mv);
      return table[i].percentage - frac * (table[i].percentage - table[i+1].percentage);
    }
  }
  return 0;
}

static void tftSleep() {
  ledcWrite(2, 0);
  ledcDetachPin(PIN_LCD_BACKLIGHT);
  pinMode(PIN_LCD_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_LCD_BACKLIGHT, LOW);
  tft.writecommand(0x10); // ST7789_SLPIN
  
  pinMode(TFT_CS, INPUT);
  pinMode(TFT_DC, INPUT);
  pinMode(TFT_RST, INPUT);
  pinMode(TFT_WR, INPUT);
  pinMode(TFT_RD, INPUT);
  pinMode(TFT_D0, INPUT);
  pinMode(TFT_D1, INPUT);
  pinMode(TFT_D2, INPUT);
  pinMode(TFT_D3, INPUT);
  pinMode(TFT_D4, INPUT);
  pinMode(TFT_D5, INPUT);
  pinMode(TFT_D6, INPUT);
  pinMode(TFT_D7, INPUT);
  
  digitalWrite(PIN_POWER_ON, LOW);
  gpio_hold_en((gpio_num_t)PIN_POWER_ON); // Bloqueo hermético del pin en sleep
}

class TFT_eSPI_Hack : public TFT_eSPI {
public:
  void forceBooted() { _booted = true; }
};

static void tftWake() {
  gpio_hold_dis((gpio_num_t)PIN_POWER_ON);
  digitalWrite(PIN_POWER_ON, HIGH);
  delay(120); 
  
  ((TFT_eSPI_Hack*)&tft)->forceBooted();
  tft.init();
  tft.writecommand(0x11); // SLPOUT
  delay(120);
  
  ledcSetup(2, 10000, 8);
  ledcAttachPin(PIN_LCD_BACKLIGHT, 2);
  applyBrightness();
}

static String activeIp() {
  if (wifiConnected) return WiFi.localIP().toString();
  if (setupMode) return WiFi.softAPIP().toString();
  return "0.0.0.0";
}

static String wifiModeName() {
  if (wifiConnected) return "station";
  if (setupMode) return "setup";
  return "offline";
}

static String wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return String(static_cast<int>(status));
  }
}

static void updateBleStatus();
static void drawScreen(bool force = false);

static uint16_t currentMinuteOfDay() {
  if (rtcAvailable) {
    DateTime now = rtc.now();
    return now.hour() * 60 + now.minute();
  }

  uint32_t seconds = millis() / 1000;
  return (seconds / 60) % 1440;
}

static uint16_t currentToneFreq = 0;
static void safeTone(uint16_t freq) {
  if (currentToneFreq != freq) {
    currentToneFreq = freq;
    ledcWriteTone(0, freq);
  }
}

static void buzz(uint16_t freq, uint16_t durationMs) {
  safeTone(freq);
  delay(durationMs);
  safeTone(0);
}

static void startTransitionAlert() {
  if (screenIsAsleep) {
    screenIsAsleep = false;
    tftWake();
    tft.setRotation(1);
    drawScreen(true);
  }

  alertRunning = true;
  alertStartedAt = millis();
  lastInteractionTime = millis();
  if (deviceVibration) {
    digitalWrite(PIN_VIBRATION, HIGH);
  }
  if (deviceSound) {
  }
}

static void updateAlert() {
  if (!alertRunning) return;
  uint32_t elapsed = millis() - alertStartedAt;
  
    if (elapsed >= deviceAlertMs) {
    digitalWrite(PIN_VIBRATION, LOW);
    if (deviceSound) safeTone(0);
    alertRunning = false;
    return;
  }
  
  if (deviceSound) {
    uint32_t t = elapsed % 1600;
    if (t < 600) {
      uint32_t cycle = t % 150;
      if (cycle < 100) safeTone(4000);
      else safeTone(0);
    } else {
      safeTone(0);
    }
  }
}

static bool loadRoutine() {
  routineCount = 0;
  routineName = "Sin rutina";
  File file = LittleFS.open(ROUTINE_PATH, "r");
  if (!file) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) return false;

  routineName = doc["name"] | "Sin rutina";
  childName = doc["child"] | "BuscaTEA";
  deviceVibration = doc["vibration"] | true;
  deviceSound = doc["sound"] | true;
  routineDaysMask = 0;
  JsonArray dArr = doc["days"].as<JsonArray>();
  if (dArr.isNull() || dArr.size() == 0) {
    routineDaysMask = 0xFF;
  } else {
    for (JsonVariant v : dArr) {
      routineDaysMask |= (1 << v.as<int>());
    }
  }
  int durationSec = doc["alertDuration"] | 3;
  deviceAlertMs = durationSec * 1000;

  JsonArray items = doc["items"].as<JsonArray>();
  for (JsonObject item : items) {
    if (routineCount >= 24) break;
    routine[routineCount].id = item["id"] | "";
    routine[routineCount].title = item["title"] | routine[routineCount].id;
    routine[routineCount].startMin = parseTimeToMinutes(item["start"] | "00:00");
    routine[routineCount].endMin = parseTimeToMinutes(item["end"] | "00:00");
    routine[routineCount].completed = false;
    routineCount++;
  }
  return routineCount > 0;
}

static int findActiveRoutineIndex(uint16_t minute) {
  if (rtcAvailable) {
    DateTime now = rtc.now();
    int dayW = now.dayOfTheWeek();
    int mappedDay = (dayW == 0) ? 7 : dayW;
    if (!(routineDaysMask & (1 << mappedDay))) {
      return -1;
    }
  }
  for (size_t i = 0; i < routineCount; i++) {

    if (routine[i].startMin <= routine[i].endMin) {
      if (minute >= routine[i].startMin && minute < routine[i].endMin) return i;
    } else {
      if (minute >= routine[i].startMin || minute < routine[i].endMin) return i;
    }
  }
  return -1;
}

static uint8_t progressFor(const RoutineItem &item, uint16_t minute) {
  int start = item.startMin;
  int end = item.endMin;
  int now = minute;

  if (end <= start) end += 1440;
  if (now < start) now += 1440;

  int total = max(1, end - start);
  int elapsed = constrain(now - start, 0, total);
  return (elapsed * 100) / total;
}

static void drawBatteryIcon(int x, int y, int w, int h, int percent, bool charging) {
  uint16_t color = tft.color565(16, 185, 129);
  if (charging) {
    color = TFT_YELLOW;
  } else if (percent < 20) {
    color = tft.color565(239, 68, 68);
  } else if (percent < 50) {
    color = tft.color565(249, 115, 22);
  }
  
  tft.drawRect(x, y, w - 2, h, color);
  tft.fillRect(x + w - 2, y + 2, 2, h - 4, color);
  
  if (charging) {
    tft.fillRect(x + 2, y + 2, w - 6, h - 4, color);
    tft.drawLine(x + w/2 - 2, y + 2, x + w/2, y + h/2, TFT_BLACK);
    tft.drawLine(x + w/2, y + h/2, x + w/2 - 2, y + h/2, TFT_BLACK);
    tft.drawLine(x + w/2 - 2, y + h/2, x + w/2 + 2, y + h - 2, TFT_BLACK);
  } else {
    int fillW = ((w - 4) * percent) / 100;
    if (fillW > 0) {
      tft.fillRect(x + 2, y + 2, fillW, h - 4, color);
    }
  }
}

static void drawWifiIcon(int x, int y, int rssi, bool apMode, bool connected) {
  if (!connected) {
    if (apMode) {
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(tft.color565(249, 115, 22), tft.color565(15, 23, 42));
      tft.drawString("AP", x, y + 4, 1);
    } else {
      uint16_t gray = tft.color565(80, 80, 90);
      tft.fillRect(x, y + 6, 2, 2, gray);
      tft.fillRect(x + 4, y + 4, 2, 4, gray);
      tft.fillRect(x + 8, y + 2, 2, 6, gray);
      tft.fillRect(x + 12, y, 2, 8, gray);
      tft.drawLine(x - 2, y, x + 14, y + 8, tft.color565(239, 68, 68));
    }
  } else {
    int bars = 1;
    if (rssi >= -55) bars = 4;
    else if (rssi >= -70) bars = 3;
    else if (rssi >= -85) bars = 2;
    
    uint16_t barColor = tft.color565(16, 185, 129);
    uint16_t inactiveColor = tft.color565(50, 50, 60);
    
    tft.fillRect(x, y + 6, 2, 2, (bars >= 1) ? barColor : inactiveColor);
    tft.fillRect(x + 4, y + 4, 2, 4, (bars >= 2) ? barColor : inactiveColor);
    tft.fillRect(x + 8, y + 2, 2, 6, (bars >= 3) ? barColor : inactiveColor);
    tft.fillRect(x + 12, y, 2, 8, (bars >= 4) ? barColor : inactiveColor);
  }
}

static void drawBleIcon(int x, int y, bool connected) {
  uint16_t color = connected ? tft.color565(59, 130, 246) : tft.color565(80, 80, 90);
  tft.drawLine(x + 3, y, x + 3, y + 8, color);
  tft.drawLine(x + 3, y, x + 6, y + 2, color);
  tft.drawLine(x + 6, y + 2, x + 3, y + 4, color);
  tft.drawLine(x + 3, y + 4, x + 6, y + 6, color);
  tft.drawLine(x + 6, y + 6, x + 3, y + 8, color);
  tft.drawLine(x + 3, y + 2, x, y, color);
  tft.drawLine(x + 3, y + 6, x, y + 8, color);
}

static void drawSoundIcon(int x, int y, bool enabled) {
  uint16_t color = enabled ? tft.color565(186, 195, 210) : tft.color565(80, 80, 90);
  tft.fillRect(x, y + 2, 3, 4, color);
  tft.drawLine(x + 3, y + 2, x + 6, y, color);
  tft.drawLine(x + 3, y + 5, x + 6, y + 7, color);
  tft.fillRect(x + 6, y, 1, 8, color);
  if (enabled) {
    tft.drawPixel(x + 8, y + 2, color);
    tft.drawPixel(x + 9, y + 3, color);
    tft.drawPixel(x + 9, y + 4, color);
    tft.drawPixel(x + 8, y + 5, color);
  }
}

static void drawVibrationIcon(int x, int y, bool enabled) {
  uint16_t color = enabled ? tft.color565(186, 195, 210) : tft.color565(80, 80, 90);
  tft.drawRect(x + 2, y + 1, 6, 8, color);
  if (enabled) {
    tft.drawPixel(x, y + 2, color);
    tft.drawPixel(x + 1, y + 3, color);
    tft.drawPixel(x + 1, y + 5, color);
    tft.drawPixel(x, y + 6, color);
    tft.drawPixel(x + 9, y + 2, color);
    tft.drawPixel(x + 8, y + 3, color);
    tft.drawPixel(x + 8, y + 5, color);
    tft.drawPixel(x + 9, y + 6, color);
  }
}

static void drawRoundRectGradientH(int x, int y, int w, int h, int r, uint16_t colorStart, uint16_t colorEnd) {
  uint8_t rS = (colorStart >> 11) & 0x1F;
  uint8_t gS = (colorStart >> 5) & 0x3F;
  uint8_t bS = colorStart & 0x1F;
  
  uint8_t rE = (colorEnd >> 11) & 0x1F;
  uint8_t gE = (colorEnd >> 5) & 0x3F;
  uint8_t bE = colorEnd & 0x1F;
  
  for (int i = 0; i < w; i++) {
    float t = (float)i / (w - 1);
    uint8_t rC = rS + t * (rE - rS);
    uint8_t gC = gS + t * (gE - gS);
    uint8_t bC = bS + t * (bE - bS);
    uint16_t color = (rC << 11) | (gC << 5) | bC;
    
    int dy = 0;
    if (i < r) {
      int dx = r - i;
      dy = sqrt(r * r - dx * dx);
      tft.drawFastVLine(x + i, y + r - dy, 2 * dy + (h - 2 * r), color);
    } else if (i >= w - r) {
      int dx = i - (w - r);
      dy = sqrt(r * r - dx * dx);
      tft.drawFastVLine(x + i, y + r - dy, 2 * dy + (h - 2 * r), color);
    } else {
      tft.drawFastVLine(x + i, y, h, color);
    }
  }
}
static bool isCharging() {
  static uint32_t lastCheckMillis = 0;
  static uint32_t lastVolts = 0;
  static bool chargingState = false;
  
  uint32_t currentVolts = readBatteryMilliVolts();
  uint32_t now = millis();
  
  // Inicialización en el primer arranque
  if (lastVolts == 0) {
    lastVolts = currentVolts;
    lastCheckMillis = now;
    // Si al arrancar el voltaje es inusualmente alto (por encima del 100% natural), asumimos que está enchufado
    if (currentVolts > 4250) chargingState = true; 
    return chargingState;
  }
  
  // Revisamos la tendencia cada 2 segundos
  if (now - lastCheckMillis > 2000) {
    int32_t delta = (int32_t)currentVolts - (int32_t)lastVolts;
    
    // Idea 1: Salto brusco hacia arriba (>50mV en 2 seg) = Acaban de enchufar el cable
    if (delta > 50) {
      chargingState = true;
    } 
    // Bajón brusco rápido (>-50mV) = Acaban de quitar el cable
    else if (delta < -50) {
      chargingState = false;
    }
    
    // Seguridad: Si el voltaje es mecánicamente superior al tope natural de la batería, está enchufado
    if (currentVolts > 4250) {
      chargingState = true;
    }
    
    // Idea 2: Si hay conexión de datos USB por puerto serie con un PC, sabemos seguro que está enchufado
    if (Serial) {
      chargingState = true;
    }
    
    lastVolts = currentVolts;
    lastCheckMillis = now;
  }
  
  return chargingState;
}

static void drawHeader(uint16_t minute) {
  tft.fillRect(0, 0, SCREEN_W, 22, tft.color565(15, 23, 42));
  tft.drawFastHLine(0, 22, SCREEN_W, tft.color565(51, 65, 85));

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, tft.color565(15, 23, 42));
  tft.drawString(minutesToTime(minute), 10, 11, 2);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  tft.drawString(childName, 95, 11, 2);

  int bat = batteryPercent();
  bool charging = isCharging();
  
  tft.setTextDatum(MR_DATUM);
  if (charging) {
    tft.setTextColor(TFT_YELLOW, tft.color565(15, 23, 42));
    tft.drawString("CHG", 292, 11, 2);
  } else {
    tft.setTextColor(TFT_WHITE, tft.color565(15, 23, 42));
    tft.drawString(String(bat), 292, 11, 2);
  }

  drawBatteryIcon(296, 4, 20, 10, bat, charging);
  int rssi = wifiConnected ? WiFi.RSSI() : -100;
  drawWifiIcon(250, 7, rssi, setupMode, wifiConnected);
  drawBleIcon(230, 7, bleConnected);
  drawVibrationIcon(210, 7, deviceVibration);
  drawSoundIcon(190, 7, deviceSound);
}

static void drawBorderProgress(int x, int y, int w, int h, int thickness, uint8_t progress, uint16_t activeColor, uint16_t inactiveColor) {
  for (int t = 0; t < thickness; t++) {
    tft.drawRect(x - t, y - t, w + 2 * t, h + 2 * t, inactiveColor);
  }

  int topProgress = constrain(progress, 0, 25);
  for (int t = 0; t < thickness; t++) {
    int topW = (topProgress * (w + 2 * t)) / 25;
    tft.drawFastHLine(x - t, y - t, topW, activeColor);
  }

  if (progress > 25) {
    int rightProgress = constrain(progress - 25, 0, 25);
    for (int t = 0; t < thickness; t++) {
      int rightH = (rightProgress * (h + 2 * t)) / 25;
      tft.drawFastVLine(x + w - 1 + t, y - t, rightH, activeColor);
    }
  }

  if (progress > 50) {
    int bottomProgress = constrain(progress - 50, 0, 25);
    for (int t = 0; t < thickness; t++) {
      int bottomW = (bottomProgress * (w + 2 * t)) / 25;
      tft.drawFastHLine(x + w - 1 + t - bottomW, y + h - 1 + t, bottomW, activeColor);
    }
  }

  if (progress > 75) {
    int leftProgress = constrain(progress - 75, 0, 25);
    for (int t = 0; t < thickness; t++) {
      int leftH = (leftProgress * (h + 2 * t)) / 25;
      tft.drawFastVLine(x - t, y + h - 1 + t - leftH, leftH, activeColor);
    }
  }
}

static int findNextRoutineIndex(uint16_t minute) {
  if (routineCount == 0) return -1;
  if (rtcAvailable) {
    DateTime now = rtc.now();
    int dayW = now.dayOfTheWeek();
    int mappedDay = (dayW == 0) ? 7 : dayW;
    if (!(routineDaysMask & (1 << mappedDay))) return -1;
  }
  if (activeIndex >= 0 && (size_t)(activeIndex + 1) < routineCount) {
    return activeIndex + 1;
  }
  for (size_t i = 0; i < routineCount; i++) {
    if (routine[i].startMin > minute) {
      return i;
    }
  }
  return 0;
}

static bool drawPictogramImage(const RoutineItem &item, int picX, int picY, int picW, int picH) {
  String jpgPath = "/pictos/" + item.id + ".jpg";
  String jpegPath = "/pictos/" + item.id + ".jpeg";
  String path = LittleFS.exists(jpgPath) ? jpgPath : jpegPath;
  if (!LittleFS.exists(path)) return false;

  tft.fillRect(picX, picY, picW, picH, TFT_WHITE);

  uint16_t jpgW = 0;
  uint16_t jpgH = 0;
  TJpgDec.getFsJpgSize(&jpgW, &jpgH, path, LittleFS);
  uint8_t scale = 1;
  while ((jpgW / scale > (picW - 6) || jpgH / scale > (picH - 6)) && scale < 8) {
    scale *= 2;
  }
  TJpgDec.setJpgScale(scale);

  int16_t drawW = jpgW / scale;
  int16_t drawH = jpgH / scale;
  int16_t x = picX + max(0, (picW - drawW) / 2);
  int16_t y = picY + max(0, (picH - drawH) / 2);
  TJpgDec.drawFsJpg(x, y, path, LittleFS);
  return true;
}

static void drawPictogramPlaceholder(const RoutineItem &item, int picX, int picY, int picW, int picH) {
  tft.fillRect(picX, picY, picW, picH, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.drawString(item.id.substring(0, 10), picX + picW / 2, picY + picH / 2, 2);
}

static void drawRightPanel(const RoutineItem &item, uint16_t minute, int cx, uint16_t activeProgressColor) {
  tft.fillRect(150, 23, 170, 147, tft.color565(15, 23, 42));
  
  bool twoLines = tft.textWidth(item.title, 4) > 160;

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  String timeRange = minutesToTime(item.startMin) + " - " + minutesToTime(item.endMin);
  tft.drawString(timeRange, cx, twoLines ? 34 : 40, 2);
  
  tft.setTextColor(TFT_WHITE, tft.color565(15, 23, 42));
  if (!twoLines) {
    tft.drawString(item.title, cx, 66, 4);
  } else {
    int splitIdx = -1;
    int minDiff = 999;
    for (int i = 0; i < (int)item.title.length(); i++) {
      if (item.title[i] == ' ') {
        int diff = abs(i - (int)item.title.length() / 2);
        if (diff < minDiff) {
          minDiff = diff;
          splitIdx = i;
        }
      }
    }
    if (splitIdx > 0) {
      String l1 = item.title.substring(0, splitIdx);
      String l2 = item.title.substring(splitIdx + 1);
      tft.drawString(l1, cx, 54, 4);
      tft.drawString(l2, cx, 77, 4);
    } else {
      tft.drawString(item.title, cx, 66, 4);
    }
  }
  
  int badgeY = twoLines ? 94 : 80;
  int remainingMin = item.endMin - minute;
  if (remainingMin < 0) remainingMin += 1440;
  String remainingStr = "Quedan " + String(remainingMin) + " min";
  tft.fillRoundRect(cx - 55, badgeY, 110, 18, 9, tft.color565(30, 41, 59));
  tft.setTextColor(tft.color565(56, 189, 248), tft.color565(30, 41, 59));
  tft.drawString(remainingStr, cx, badgeY + 9, 2);
  
  int cy = twoLines ? 136 : 126;
  if (item.completed) {
    tft.fillCircle(cx, cy, 16, activeProgressColor);
    for (int i = -1; i <= 1; i++) {
      tft.drawLine(cx - 7, cy + i, cx - 2, cy + 5 + i, TFT_WHITE);
      tft.drawLine(cx - 2, cy + 5 + i, cx + 7, cy - 5 + i, TFT_WHITE);
      tft.drawLine(cx - 7 + i, cy, cx - 2 + i, cy + 5, TFT_WHITE);
      tft.drawLine(cx - 2 + i, cy + 5, cx + 7 + i, cy - 5, TFT_WHITE);
    }
  } else {
    int bx = cx - 55;
    int by = twoLines ? 120 : 110;
    int bw = 110;
    int bh = 32;
    drawRoundRectGradientH(bx, by, bw, bh, 16, tft.color565(2, 132, 199), tft.color565(3, 105, 161));
    tft.setTextColor(TFT_WHITE);
    tft.drawString("OK", cx, by + bh / 2, 2);
  }
}

static void drawActiveScreen(uint16_t minute) {
  const RoutineItem &item = routine[activeIndex];
  uint8_t progress = progressFor(item, minute);

  constexpr int picX = 12;
  constexpr int picY = 28;
  constexpr int picW = 134;
  constexpr int picH = 134;
  
  constexpr int gap = 3;
  constexpr int borderX = picX - gap;
  constexpr int borderY = picY - gap;
  constexpr int borderW = picW + 2 * gap;
  constexpr int borderH = picH + 2 * gap;
  constexpr int picThickness = 3;

  constexpr int cx = 235;

  uint16_t activeProgressColor = tft.color565(14, 165, 233);
  uint16_t inactiveProgressColor = tft.color565(30, 41, 59);

  if (activeIndex != lastDrawnIndex || lastDrawnNeedsMenu) {
    tft.fillScreen(tft.color565(15, 23, 42));
    drawHeader(minute);
    if (!drawPictogramImage(item, picX, picY, picW, picH)) {
      drawPictogramPlaceholder(item, picX, picY, picW, picH);
    }

    drawBorderProgress(borderX, borderY, borderW, borderH, picThickness, progress, activeProgressColor, inactiveProgressColor);
    drawRightPanel(item, minute, cx, activeProgressColor);
  } else {
    if (minute != lastDrawnMinute || activeIp() != lastDrawnIp || batteryPercent() != lastDrawnBattery) {
      drawHeader(minute);
    }
    if (progress != lastDrawnProgress) {
      drawBorderProgress(borderX, borderY, borderW, borderH, picThickness, progress, activeProgressColor, inactiveProgressColor);
    }
    if (item.completed != lastDrawnCompleted || minute != lastDrawnMinute) {
      drawRightPanel(item, minute, cx, activeProgressColor);
    }
  }

  lastDrawnIndex = activeIndex;
  lastDrawnMinute = minute;
  lastDrawnCompleted = item.completed;
  lastDrawnProgress = progress;
  lastDrawnIp = activeIp();
  lastDrawnBattery = batteryPercent();
}

static void drawInfoScreen(uint16_t minute) {
  tft.fillScreen(tft.color565(15, 23, 42));
  
  tft.fillRect(0, 0, SCREEN_W, 22, tft.color565(15, 23, 42));
  tft.drawFastHLine(0, 22, SCREEN_W, tft.color565(51, 65, 85));
  
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tft.color565(186, 195, 210), tft.color565(15, 23, 42));
  tft.drawString("ESTADO DEL DISPOSITIVO", 160, 11, 2);

  tft.setTextDatum(ML_DATUM);
  
  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  tft.drawString("IP:", 20, 35, 2);
  tft.setTextColor(tft.color565(14, 165, 233), tft.color565(15, 23, 42));
  tft.drawString(activeIp(), 20, 55, 2);
  
  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  tft.drawString("BATERIA:", 20, 80, 2);
  tft.setTextColor(TFT_WHITE, tft.color565(15, 23, 42));
  String batStr = String(batteryPercent()) + "% (" + String(readBatteryMilliVolts()) + "mV)";
  tft.drawString(batStr, 20, 100, 2);

  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  tft.drawString("RUTINA:", 20, 125, 2);
  tft.setTextColor(TFT_WHITE, tft.color565(15, 23, 42));
  tft.drawString(routineName.substring(0, 18), 20, 145, 2);

  tft.drawFastVLine(160, 32, 106, tft.color565(51, 65, 85));

  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  tft.drawString("AVISOS:", 180, 45, 2);
  
  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  tft.drawString("Vibra:", 180, 68, 2);
  if (deviceVibration) {
    tft.setTextColor(tft.color565(16, 185, 129), tft.color565(15, 23, 42));
    tft.drawString("SI", 240, 68, 2);
  } else {
    tft.setTextColor(tft.color565(239, 68, 68), tft.color565(15, 23, 42));
    tft.drawString("NO", 240, 68, 2);
  }
  
  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  tft.drawString("Sonido:", 180, 88, 2);
  if (deviceSound) {
    tft.setTextColor(tft.color565(16, 185, 129), tft.color565(15, 23, 42));
    tft.drawString("SI", 245, 88, 2);
  } else {
    tft.setTextColor(tft.color565(239, 68, 68), tft.color565(15, 23, 42));
    tft.drawString("NO", 245, 88, 2);
  }

  int completedCount = 0;
  for (size_t i = 0; i < routineCount; i++) {
    if (routine[i].completed) completedCount++;
  }
  
  tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
  tft.drawString("TAREAS:", 180, 115, 2);
  tft.setTextColor(tft.color565(249, 115, 22), tft.color565(15, 23, 42));
  String progressStr = String(completedCount) + " / " + String(routineCount);
  tft.drawString(progressStr, 180, 135, 2);

  tft.setTextDatum(MC_DATUM);
  tft.drawFastHLine(0, 148, SCREEN_W, tft.color565(51, 65, 85));
  tft.setTextColor(tft.color565(100, 116, 139), tft.color565(15, 23, 42));
  tft.drawString("Cualquier boton para salir", 160, 159, 2);
}

static void drawIdleScreen(uint16_t minute) {
  bool forceRedraw = (activeIndex != lastDrawnIndex || lastDrawnNeedsMenu);

  if (forceRedraw) {
    tft.fillScreen(tft.color565(15, 23, 42));
    drawHeader(minute);
  } else if (minute != lastDrawnMinute || activeIp() != lastDrawnIp || batteryPercent() != lastDrawnBattery) {
    drawHeader(minute);
  }

  if (forceRedraw || minute != lastDrawnMinute) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(tft.color565(14, 165, 233), tft.color565(15, 23, 42));
    tft.drawString(minutesToTime(minute), 160, 96, 8);
  }

  lastDrawnIndex = activeIndex;
  lastDrawnMinute = minute;
  lastDrawnIp = activeIp();
  lastDrawnBattery = batteryPercent();
}

static void drawScreen(bool force) {
  if (!previewMode && !force && millis() - lastDrawAt < DISPLAY_REFRESH_MS) return;
  lastDrawAt = millis();

  if (force) {
    lastDrawnIndex = -2;
    lastDrawnNeedsMenu = false;
    lastDrawnMinute = 9999;
    lastDrawnCompleted = false;
    lastDrawnProgress = 255;
    lastDrawnIp = "";
  }

  if (infoMode) {
    if (millis() - infoStartedAt >= 300000) {
      infoMode = false;
      disableNetwork();
      drawScreen(true);
      return;
    }

    uint16_t minute = currentMinuteOfDay();
    if (lastDrawnIndex != -4) {
      drawInfoScreen(minute);
      lastDrawnIndex = -4;
    } else {
      if (minute != lastDrawnMinute || activeIp() != lastDrawnIp || batteryPercent() != lastDrawnBattery) {
        drawInfoScreen(minute);
        lastDrawnMinute = minute;
        lastDrawnIp = activeIp();
        lastDrawnBattery = batteryPercent();
      }
    }
    return;
  }

  if (previewMode) {
    if (millis() - previewStartedAt >= 3000) {
      previewMode = false;
      drawScreen(true);
      return;
    }

    if (lastDrawnIndex != -3) {
      tft.fillScreen(tft.color565(15, 23, 42));
      uint16_t minute = currentMinuteOfDay();
      drawHeader(minute);

      if (previewIndex >= 0 && (size_t)previewIndex < routineCount) {
        const RoutineItem &nextItem = routine[previewIndex];
        
        int picX = 30;
        int picY = 50;
        int picW = 80;
        int picH = 80;
        if (!drawPictogramImage(nextItem, picX, picY, picW, picH)) {
          drawPictogramPlaceholder(nextItem, picX, picY, picW, picH);
        }

        int textX = 205;
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
        tft.drawString("SIGUIENTE TAREA", textX, 50, 2);

        tft.setTextColor(TFT_WHITE, tft.color565(15, 23, 42));
        tft.drawString(nextItem.title, textX, 85, 4);

        String timeRange = minutesToTime(nextItem.startMin) + " - " + minutesToTime(nextItem.endMin);
        tft.setTextColor(tft.color565(14, 165, 233), tft.color565(15, 23, 42));
        tft.drawString(timeRange, textX, 125, 4);
      } else {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, tft.color565(15, 23, 42));
        tft.drawString("Fin de la rutina", 160, 85, 4);
        tft.setTextColor(tft.color565(148, 163, 184), tft.color565(15, 23, 42));
        tft.drawString("No hay mas tareas hoy", 160, 120, 2);
      }
      lastDrawnIndex = -3;
    } else {
      uint16_t minute = currentMinuteOfDay();
      if (minute != lastDrawnMinute || activeIp() != lastDrawnIp || batteryPercent() != lastDrawnBattery) {
        drawHeader(minute);
        lastDrawnMinute = minute;
        lastDrawnIp = activeIp();
        lastDrawnBattery = batteryPercent();
      }
    }
    return;
  }

  uint16_t minute = currentMinuteOfDay();
  
  static uint16_t lastMinuteDraw = 9999;
  if (lastMinuteDraw != 9999 && minute < lastMinuteDraw) {
    for (size_t i = 0; i < routineCount; i++) routine[i].completed = false;
  }
  lastMinuteDraw = minute;

  int nextActive = findActiveRoutineIndex(minute);
  if (nextActive != activeIndex) {
    activeIndex = nextActive;
    if (activeIndex >= 0 && !routine[activeIndex].completed) startTransitionAlert();
  }

  if (activeIndex >= 0) {
    drawActiveScreen(minute);
  } else {
    drawIdleScreen(minute);
  }
}

static void handleButtons() {
  uint32_t now = millis();
  
  static bool okDown = false;
  static uint32_t lastOkEdge = 0;
  bool rawOk = (digitalRead(PIN_BUTTON_OK) == LOW);
  if (rawOk != okDown) {
    if (now - lastOkEdge > 50) {
      okDown = rawOk;
      lastOkEdge = now;
    }
  }
  
  static bool menuDown = false;
  static uint32_t lastMenuEdge = 0;
  bool rawMenu = (digitalRead(PIN_BUTTON_MENU) == LOW);
  if (rawMenu != menuDown) {
    if (now - lastMenuEdge > 50) {
      menuDown = rawMenu;
      lastMenuEdge = now;
    }
  }

  static uint32_t bothPressedAt = 0;
  static bool bothWereDown = false;
  static bool okLongPressHandled = false;

  if (okDown && menuDown) {
    if (!bothWereDown) {
      bothPressedAt = now;
      bothWereDown = true;
    } else if (now - bothPressedAt >= 5000) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Apagando...", 160, 85, 4);
      delay(1000);
      
      tftSleep();
      
      Serial.flush();
      Serial.end();
      
      esp_sleep_enable_ext1_wakeup((1ULL << PIN_BUTTON_OK) | (1ULL << PIN_BUTTON_MENU), ESP_EXT1_WAKEUP_ANY_LOW);
      esp_deep_sleep_start();
    }
    okWasDown = okDown;
    menuWasDown = menuDown;
    return;
  } else {
    bothWereDown = false;
  }

  bool anyButtonPressed = (okDown && !okWasDown) || (menuDown && !menuWasDown);
  if (anyButtonPressed) {
    lastInteractionTime = now;
    if (screenIsAsleep) {
      screenIsAsleep = false;
      tftWake();
      tft.setRotation(1);
      drawScreen(true);
      okWasDown = true;
      menuWasDown = true;
      if (okDown) {
        okPressedAt = now;
        okLongPressHandled = true;
      }
      if (menuDown) menuPressedAt = now;
      return;
    }
  }

  if (okDown) {
    if (!okWasDown) {
      okPressedAt = now;
      okLongPressHandled = false;
    } else if (now - okPressedAt >= 1000) {
      okLongPressHandled = true;
      okPressedAt = now; // Reiniciamos para que cicle si sigue pulsando
      
      brightnessLevel = (brightnessLevel + 1) % 3;
      
      buzz(1000, 50); // Feedback táctil leve
      
      // ledcWriteTone (usado en buzz) corrompe el timer PWM en ESP32S3, 
      // así que reiniciamos el canal de la luz trasera después de pitar.
      ledcSetup(2, 10000, 8);
      ledcAttachPin(PIN_LCD_BACKLIGHT, 2);
      applyBrightness();
      
      lastInteractionTime = now;
      
      // Popup visual de feedback
      tft.fillRoundRect(60, 60, 200, 50, 8, TFT_DARKGREY);
      tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(brightnessLevel == 0 ? "Brillo: Bajo" : (brightnessLevel == 1 ? "Brillo: Medio" : "Brillo: Alto"), 160, 85, 4);
      delay(400);
      drawScreen(true);
    }
  } else if (okWasDown) {
    if (!okLongPressHandled) {
      if (infoMode) {
        infoMode = false;
        disableNetwork();
        drawScreen(true);
      } else if (previewMode) {
        previewMode = false;
        drawScreen(true);
      } else if (activeIndex >= 0 && !routine[activeIndex].completed) {
        routine[activeIndex].completed = true;
        
        if (alertRunning) {
          alertRunning = false;
          digitalWrite(PIN_VIBRATION, LOW);
          safeTone(0);
        }
        
        if (deviceSound) {
          buzz(523, 80); // Do
          delay(20);
          buzz(659, 80); // Mi
          delay(20);
          buzz(784, 80); // Sol
          delay(20);
          buzz(1046, 200); // Do agudo
        }

        drawScreen(true);
      }
      // Si no hay tarea pendiente, no hace nada (no cambia brillo)
    }
  }

  if (menuDown) {
    if (!menuWasDown) {
      menuPressedAt = now;
      menuLongPressed = false;
    } else if (now - menuPressedAt >= 3000) {
      if (!menuLongPressed) {
        menuLongPressed = true;
        menuPressedAt = now; // Reiniciamos por si sigue pulsando
        
        if (!infoMode) {
          // Entrar en menú (AP por defecto)
          previewMode = false;
          infoMode = true;
          infoStartedAt = now;
          networkMode = 0;
          applyNetworkMode();
          drawScreen(true);
          buzz(1000, 100);
        } else {
          // Salir del menú
          infoMode = false;
          disableNetwork();
          drawScreen(true);
          buzz(1000, 100);
        }
      }
    }
  } else if (menuWasDown) {
    if (!menuLongPressed) {
      if (infoMode) {
        // Ciclado de modos de red
        networkMode = (networkMode + 1) % 3;
        applyNetworkMode();
        
        buzz(1000, 50);
        
        // Popup visual
        tft.fillRoundRect(40, 60, 240, 50, 8, TFT_DARKGREY);
        tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
        tft.setTextDatum(MC_DATUM);
        String mStr = networkMode == 0 ? "Red: Dispositivo" : (networkMode == 1 ? "Red: Bluetooth" : "Red: Casa (WiFi)");
        tft.drawString(mStr, 160, 85, 4);
        delay(400);
        drawScreen(true);
      } else {
        // Modo preview normal
        previewMode = true;
        previewStartedAt = now;
        previewIndex = findNextRoutineIndex(currentMinuteOfDay());
        drawScreen(true);
      }
    }
  }

  okWasDown = okDown;
  menuWasDown = menuDown;
}

static String contentTypeFor(const String &filename) {
  if (filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css")) return "text/css";
  if (filename.endsWith(".js")) return "application/javascript";
  if (filename.endsWith(".json")) return "application/json";
  if (filename.endsWith(".png")) return "image/png";
  if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  return "application/octet-stream";
}

static void setupWebServer() {
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(204);
      return;
    }
    request->send(404, "text/plain", "No encontrado");
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["name"] = DEVICE_NAME;
    doc["mode"] = wifiModeName();
    doc["battery"] = batteryPercent();
    doc["ip"] = activeIp();
    doc["mdns"] = String("http://") + DEVICE_NAME + ".local";
    doc["ssid"] = wifiConnected ? WiFi.SSID() : "";
    doc["attemptedSsid"] = attemptedWifiSsid;
    doc["wifiStatus"] = wifiStatusName(lastWifiStatus);
    doc["wifiStatusCode"] = static_cast<int>(lastWifiStatus);
    doc["rssi"] = wifiConnected ? WiFi.RSSI() : 0;
    doc["routineItems"] = routineCount;
    doc["active"] = activeIndex >= 0 ? routine[activeIndex].title : "";
    String body;
    serializeJson(doc, body);
    request->send(200, "application/json", body);
  });

  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    int count = WiFi.scanNetworks(false, true);
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
      JsonObject network = networks.add<JsonObject>();
      network["ssid"] = WiFi.SSID(i);
      network["rssi"] = WiFi.RSSI(i);
      network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    String body;
    serializeJson(doc, body);
    request->send(200, "application/json", body);
  });

  server.on("/api/wifi/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid", true)) {
      request->send(400, "text/plain", "Falta SSID");
      return;
    }
    String ssid = request->getParam("ssid", true)->value();
    String password = request->hasParam("password", true)
                          ? request->getParam("password", true)->value()
                          : "";
    ssid.trim();
    if (ssid.isEmpty()) {
      request->send(400, "text/plain", "SSID vacio");
      return;
    }

    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", password);
    prefs.end();
    request->send(200, "text/plain", "Wi-Fi guardada. Reiniciando...");
    restartPending = true;
    restartAt = millis() + 700;
  });

  server.on("/api/wifi/forget", HTTP_POST, [](AsyncWebServerRequest *request) {
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    request->send(200, "text/plain", "Wi-Fi olvidada. Reiniciando...");
    restartPending = true;
    restartAt = millis() + 700;
  });

  server.on("/api/battery/calibrate", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasParam("mult")) {
      float mult = request->getParam("mult")->value().toFloat();
      if (mult > 0.0f) {
         batteryMultiplier = mult;
         prefs.begin("system", false);
         prefs.putFloat("batMult", batteryMultiplier);
         prefs.end();
         request->send(200, "text/plain", "OK");
      } else {
         request->send(400, "text/plain", "Invalid multiplier");
      }
    } else {
      request->send(400, "text/plain", "Missing mult param");
    }
  });

  server.on("/api/routine", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, ROUTINE_PATH, "application/json");
  });

  server.on("/api/time", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!rtcAvailable || !request->hasParam("epoch", true)) {
      request->send(400, "text/plain", "RTC no disponible o fecha no recibida");
      return;
    }
    time_t utc_epoch = request->getParam("epoch", true)->value().toInt();
    struct tm tm_local;
    localtime_r(&utc_epoch, &tm_local);
    DateTime local_dt(tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday, tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    rtc.adjust(local_dt);
    drawScreen(true);
    request->send(200, "text/plain", "OK");
  });

  server.on(
      "/upload", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        loadRoutine();
        drawScreen(true);
        request->send(200, "text/plain", "OK");
      },
      [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data,
         size_t len, bool final) {
        String path = filename == "rutina.json" ? ROUTINE_PATH : "/pictos/" + filename;
        if (!index) {
          if (path.startsWith("/pictos/") && !LittleFS.exists("/pictos")) LittleFS.mkdir("/pictos");
          request->_tempFile = LittleFS.open(path, "w");
        }
        if (request->_tempFile) request->_tempFile.write(data, len);
        if (final && request->_tempFile) request->_tempFile.close();
      });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.begin();
}

static bool connectSavedWifi() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String password = prefs.getString("pass", "");
  prefs.end();

  if (FORCE_DEFAULT_WIFI || ssid.isEmpty()) {
    ssid = DEFAULT_WIFI_SSID;
    password = DEFAULT_WIFI_PASSWORD;
  }

  if (ssid.isEmpty() || password == "CAMBIA_ESTA_CLAVE") return false;

  attemptedWifiSsid = ssid;
  Serial.printf("Intentando Wi-Fi SSID: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(ssid.c_str(), password.c_str());

  uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    lastWifiStatus = WiFi.status();
    delay(250);
  }

  lastWifiStatus = WiFi.status();
  wifiConnected = WiFi.status() == WL_CONNECTED;
  Serial.printf("Resultado Wi-Fi: %s (%d), IP: %s\n",
                wifiStatusName(lastWifiStatus).c_str(),
                static_cast<int>(lastWifiStatus),
                WiFi.localIP().toString().c_str());
  return wifiConnected;
}

static void startSetupAp() {
  setupMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
}

static void setupWifi() {
  wifiConnected = connectSavedWifi();
  if (!wifiConnected) startSetupAp();
  if (MDNS.begin(DEVICE_NAME)) {
    MDNS.addService("http", "tcp", 80);
  }
}

static String buildStatusJson() {
  JsonDocument doc;
  doc["name"] = DEVICE_NAME;
  doc["mode"] = wifiModeName();
  doc["ip"] = activeIp();
  doc["mdns"] = String("http://") + DEVICE_NAME + ".local";
  doc["ssid"] = wifiConnected ? WiFi.SSID() : "";
  doc["attemptedSsid"] = attemptedWifiSsid;
  doc["wifiStatus"] = wifiStatusName(lastWifiStatus);
  doc["battery"] = batteryPercent();
  String body;
  serializeJson(doc, body);
  return body;
}

static void updateBleStatus() {
  if (!bleStatus) return;
  String body = buildStatusJson();
  bleStatus->setValue(reinterpret_cast<const uint8_t *>(body.c_str()), body.length());
  bleStatus->notify();
}

class WifiProvisionCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) override {
    std::string raw = characteristic->getValue();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, raw.c_str());
    if (err) return;

    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";
    ssid.trim();
    if (ssid.isEmpty()) return;

    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", password);
    prefs.end();

    updateBleStatus();
    restartPending = true;
    restartAt = millis() + 900;
  }
};



class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) override {
    bleConnected = true;
    drawScreen(true);
  }
  void onDisconnect(NimBLEServer* pServer) override {
    bleConnected = false;
    drawScreen(true);
  }
};

static MyServerCallbacks myServerCallbacks;
static WifiProvisionCallbacks wifiProvisionCallbacks;

static bool bleInitialized = false;

static void setupBleProvisioning() {
  if (bleInitialized) {
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
      NimBLEDevice::getAdvertising()->start();
    }
    return;
  }
  bleInitialized = true;
  NimBLEDevice::init("BeepTEA BLE");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer *bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&myServerCallbacks);
  NimBLEService *service = bleServer->createService(BLE_SERVICE_UUID);

  bleStatus = service->createCharacteristic(
      BLE_STATUS_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  String initialStatus = buildStatusJson();
  bleStatus->setValue(
      reinterpret_cast<const uint8_t *>(initialStatus.c_str()),
      initialStatus.length());

  NimBLECharacteristic *wifiConfig = service->createCharacteristic(
      BLE_WIFI_UUID,
      NIMBLE_PROPERTY::WRITE);
  wifiConfig->setCallbacks(&wifiProvisionCallbacks);

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
}

static bool networkActive = false;
static bool webServerInitialized = false;

static void applyNetworkMode() {
  disableNetwork();
  networkActive = true;
  
  if (networkMode == 0) {
    // Modo 0: WiFi AP (Directo al dispositivo)
    startSetupAp();
    if (MDNS.begin(DEVICE_NAME)) MDNS.addService("http", "tcp", 80);
    if (!webServerInitialized) {
      setupWebServer();
      webServerInitialized = true;
    }
  } else if (networkMode == 1) {
    // Modo 1: Bluetooth BLE
    setupBleProvisioning();
  } else if (networkMode == 2) {
    // Modo 2: WiFi Local (STA)
    wifiConnected = connectSavedWifi();
    if (MDNS.begin(DEVICE_NAME)) MDNS.addService("http", "tcp", 80);
    if (!webServerInitialized) {
      setupWebServer();
      webServerInitialized = true;
    }
  }
}

static void disableNetwork() {
  if (!networkActive) return;
  networkActive = false;
  wifiConnected = false;
  setupMode = false;
  WiFi.mode(WIFI_OFF);
  if (bleInitialized) {
    if (NimBLEDevice::getAdvertising()->isAdvertising()) {
      NimBLEDevice::getAdvertising()->stop();
    }
  }
}

static void setupTime() {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  rtcAvailable = rtc.begin();
  if (rtcAvailable && rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    pinMode(PIN_BUTTON_OK, INPUT_PULLUP);
    pinMode(PIN_BUTTON_MENU, INPUT_PULLUP);
    
    uint32_t checkStart = millis();
    bool bothInitiallyPressed = false;
    while(millis() - checkStart < 200) {
      if (digitalRead(PIN_BUTTON_OK) == LOW && digitalRead(PIN_BUTTON_MENU) == LOW) {
        bothInitiallyPressed = true;
        break;
      }
      delay(10);
    }
    
    if (!bothInitiallyPressed) {
      esp_sleep_enable_ext1_wakeup((1ULL << PIN_BUTTON_OK) | (1ULL << PIN_BUTTON_MENU), ESP_EXT1_WAKEUP_ANY_LOW);
      esp_deep_sleep_start();
    }
    
    pinMode(PIN_POWER_ON, OUTPUT);
    tftWake();
    
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Mantenga pulsado...", 160, 85, 4);

    bool keepAlive = false;
    uint32_t startWake = millis();
    while (millis() - startWake < 4800) {
      if (digitalRead(PIN_BUTTON_OK) != LOW || digitalRead(PIN_BUTTON_MENU) != LOW) {
        keepAlive = false;
        break;
      }
      keepAlive = true;
      delay(50);
    }

    if (!keepAlive) {
      tftSleep();
      esp_sleep_enable_ext1_wakeup((1ULL << PIN_BUTTON_OK) | (1ULL << PIN_BUTTON_MENU), ESP_EXT1_WAKEUP_ANY_LOW);
      esp_deep_sleep_start();
    }
    
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Encendiendo...", 160, 85, 4);
    delay(1000);
  }

  pinMode(PIN_VIBRATION, OUTPUT);
  pinMode(PIN_POWER_ON, OUTPUT);
  pinMode(PIN_BUTTON_OK, INPUT_PULLUP);
  pinMode(PIN_BUTTON_MENU, INPUT_PULLUP);
  digitalWrite(PIN_POWER_ON, HIGH);
  
  ledcSetup(2, 10000, 8);
  ledcAttachPin(PIN_LCD_BACKLIGHT, 2);
  applyBrightness();
  digitalWrite(PIN_VIBRATION, LOW);
  analogReadResolution(12);

  prefs.begin("system", true);
  batteryMultiplier = prefs.getFloat("batMult", 2.00f);
  prefs.end();

  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BUZZER, 0);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftJpegOutput);

  if (!LittleFS.begin(true)) {
    tft.drawString("Error LittleFS", 160, 85, 2);
    return;
  }

  if (LittleFS.exists("/logo.jpg")) {
    TJpgDec.setJpgScale(1);
    TJpgDec.drawFsJpg(0, 0, "/logo.jpg", LittleFS);
    delay(2500);
  }

  setupTime();
  loadRoutine();
  
  WiFi.mode(WIFI_OFF);
  
  lastInteractionTime = millis();
  drawScreen(true);
}

void loop() {
  handleButtons();
  updateAlert();
  
  if (!screenIsAsleep) {
    drawScreen();
    
    if (!previewMode && !infoMode) {
      uint32_t timeoutMs = (activeIndex >= 0) ? 180000 : 60000;
      if (millis() - lastInteractionTime > timeoutMs) {
        screenIsAsleep = true;
        tftSleep();
      }
    }
  } else {
    Serial.flush();
    Serial.end();
    
    gpio_wakeup_enable((gpio_num_t)PIN_BUTTON_OK, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_BUTTON_MENU, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    
    esp_sleep_enable_timer_wakeup(60000000ULL);
    
    esp_err_t err = esp_light_sleep_start();
    if (err != ESP_OK) {
      Serial.begin(115200);
      Serial.printf("Light sleep failed! err: %d\n", err);
      delay(1000);
    } else {
      Serial.begin(115200);
    }
    
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
      uint16_t minute = currentMinuteOfDay();
      
      static uint16_t lastMinuteSleep = 9999;
      if (lastMinuteSleep != 9999 && minute < lastMinuteSleep) {
        for (size_t i = 0; i < routineCount; i++) routine[i].completed = false;
      }
      lastMinuteSleep = minute;

      int nextActive = findActiveRoutineIndex(minute);
      if (nextActive != activeIndex) {
        screenIsAsleep = false;
        tftWake();
        tft.setRotation(1);
        lastInteractionTime = millis();
        drawScreen(true);
      }
    } else if (cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT1) {
      screenIsAsleep = false;
      tftWake();
      tft.setRotation(1);
      okWasDown = true;
      menuWasDown = true;
      lastInteractionTime = millis();
      drawScreen(true);
    }
  }
  if (restartPending && millis() >= restartAt) {
    ESP.restart();
  }
  static uint32_t lastBleUpdate = 0;
  if (millis() - lastBleUpdate > 5000) {
    lastBleUpdate = millis();
    updateBleStatus();
  }
}
