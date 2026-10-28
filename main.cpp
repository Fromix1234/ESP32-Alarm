/*
 * ESP32 Smart Alarm Clock
 * TFT ST7789 1.54" 240x320 (portrait, ROT 2)
 *
 * Возможности:
 *  - NTP-часы, дата, день недели;
 *  - до 8 будильников с днями недели и метками;
 *  - веб-панель на телефоне/ПК;
 *  - кнопки A/B + BOOT: список будильников, инфо, snooze, stop, тест;
 *  - плавный рассвет и ночной режим через PWM подсветки (GPIO32).
 */

#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include "secrets.h"
#include "web_page.h"

// ======================= РАСПИНОВКА =======================
//  TFT      -> ESP32
//  VCC      -> 3V3
//  GND      -> GND
#define TFT_CS     5   // CS
#define TFT_DC     2   // DC / A0
#define TFT_RST    4   // RESET
#define TFT_MOSI  23   // SDA / MOSI / DIN
#define TFT_SCLK  18   // SCL / SCK
#define TFT_MISO  -1   // MISO не нужен
#define TFT_BL    32   // BLK / LED через PWM

#define BTN_A     25   // кнопка A -> GND
#define BTN_B     26   // кнопка B -> GND
#define BTN_BOOT   0   // встроенная BOOT

#define TFT_W 240
#define TFT_H 320

#define MAX_ALARMS 8
#define SNOOZE_MIN 5
#define LONG_PRESS_MS 1500
#define INFO_SCREEN_MS 8000
#define LIST_SCREEN_MS 15000
#define ALARM_AUTO_STOP_MS (10UL * 60UL * 1000UL)
#define TEST_ALARM_MS 15000UL
// ==========================================================

// Цвета RGB565
const uint16_t C_BG       = 0x0000;
const uint16_t C_PANEL    = 0x1082;
const uint16_t C_PANEL2   = 0x0841;
const uint16_t C_LINE     = 0x2A69;
const uint16_t C_TEXT     = 0xFFFF;
const uint16_t C_MUTED    = 0x8410;
const uint16_t C_ACCENT   = 0x07FF;
const uint16_t C_ACCENT2  = 0xA81F;
const uint16_t C_GREEN    = 0x07E0;
const uint16_t C_ORANGE   = 0xFD20;
const uint16_t C_RED      = 0xF800;
const uint16_t C_YELLOW   = 0xFFE0;
const uint16_t C_ALARM_BG = 0x3000;
const uint16_t C_SUN      = 0xFD20;
const uint16_t C_SUN_DARK = 0xFBE0;

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);
Preferences prefs;

enum AlarmState { ALARM_IDLE, ALARM_RINGING, ALARM_SNOOZED };
enum Screen { SCREEN_CLOCK, SCREEN_INFO, SCREEN_ALARM, SCREEN_ALARM_LIST };

struct Alarm {
  uint8_t id;
  uint8_t hour;
  uint8_t minute;
  uint8_t days;      // bit0=Mon ... bit6=Sun
  bool enabled;
  char label[48];    // UTF-8, до 19 символов в веб-панели
};

struct Button {
  uint8_t pin;
  bool lastRead;
  bool stable;
  uint32_t lastChange;
  uint32_t pressStart;
  bool longFired;
  bool shortEvent;
  bool longEvent;
};

Alarm alarms[MAX_ALARMS];
uint8_t alarmCount = 0;
AlarmState alarmState = ALARM_IDLE;
Screen screen = SCREEN_CLOCK;
int8_t activeAlarm = -1;
bool testAlarmActive = false;
uint32_t ringStartMs = 0;
uint32_t snoozeUntilMs = 0;
uint32_t sunriseStartMs = 0;
bool sunriseActive = false;
uint32_t lastMinuteKey = 0;
bool forceRedraw = true;
bool statusRedraw = false;
bool ntpRedraw = false;
uint32_t ntpRedrawAt = 0;
uint32_t infoUntil = 0;
int8_t listSelected = 0;
int8_t listTop = 0;
uint32_t listUntil = 0;

uint8_t dayBrightness = 60;
uint8_t currentBrightness = 0;
bool blPwm = false;
bool autoNight = true;
bool dimOverride = false;
String tz = WIFI_TZ;

bool timeSynced = false;
bool wifiConnected = false;
bool wifiConfigured = false;
bool mdnsStarted = false;
uint32_t lastNtpAttempt = 0;

Button btnA, btnB, btnBoot;

// ======================= ПРОТОТИПЫ =======================
bool readTime(struct tm &t);
bool dayEnabled(uint8_t days, int wday);
String daysTextEn(uint8_t d);
void copyAsciiLabel(const char *src, char *dst, size_t n);
int findNextAlarm(int &dayOffset, int &nextWday);
const char *dayLabelEn(int offset, int wday);
const char *dayLabelRu(int offset, int wday);
String nextAlarmTextEn();
String nextAlarmTextRu();

void loadSettings();
void saveSettings();
void loadAlarms();
void saveAlarms();
void seedDefaultAlarm();
uint8_t newAlarmId();
int findAlarmIndex(uint8_t id);

void startAlarm(int8_t idx, bool test);
void stopAlarm();
void snoozeAlarm();
void addSnooze();
void checkAlarms();
void updateAlarmState();

uint8_t targetBrightness();
void applyBrightness(uint8_t value, bool force = false);
void updateBrightness();

void buttonInit(Button &b, uint8_t pin);
void buttonUpdate(Button &b);
bool takeShort(Button &b);
bool takeLong(Button &b);
void handleButtons();
void onButtonA(bool longPress);
void onButtonB(bool longPress);
void onBootButton(bool longPress);

void drawTextCentered(const GFXfont *font, const char *text, int16_t cx,
                      int16_t baseline, uint16_t color, uint16_t bg);
void drawTextLeft(const GFXfont *font, const char *text, int16_t x,
                  int16_t baseline, uint16_t color, uint16_t bg);
void drawStatusBar();
void drawClockTime(bool valid, const struct tm &t);
void drawProgressBar(bool valid, const struct tm &t);
void drawNextAlarmCard();
void drawFooter();
void drawClockScreen(bool full);
void drawAlarmScreen(bool full);
void drawAlarmTime();
void updateAlarmScreen();
void drawInfoScreen();
void showInfoScreen();
void drawAlarmListScreen();
void showAlarmListScreen();
void listMove(int8_t delta);
void updateDisplay();

void startWifi();
void updateWifi();
void checkTime();

void setupWebServer();
void handleState();
void handleSaveAlarm();
void handleDeleteAlarm();
void handleAction();
void handleSettings();
// =======================================================

// ======================= УТИЛИТЫ =======================
bool readTime(struct tm &t) {
  time_t now = time(nullptr);
  if (now < 1700000000UL) return false;  // до 2023-11 => NTP ещё не было
  localtime_r(&now, &t);
  return true;
}

bool dayEnabled(uint8_t days, int wday) {
  int bit = (wday + 6) % 7;  // Mon=0 ... Sun=6
  return (days & (1 << bit)) != 0;
}

void copyAsciiLabel(const char *src, char *dst, size_t n) {
  if (n == 0) return;
  size_t j = 0;
  for (size_t i = 0; src[i] != 0 && j + 1 < n; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 32 && c < 127) dst[j++] = (char)c;
  }
  while (j > 0 && dst[j - 1] == ' ') j--;
  dst[j] = 0;
  if (j == 0) {
    strncpy(dst, "ALARM", n - 1);
    dst[n - 1] = 0;
  }
}

String daysTextEn(uint8_t d) {
  if (d == 0) return "Never";
  if (d == 127) return "Daily";
  if (d == 31) return "Mon-Fri";
  if (d == 96) return "Sat-Sun";
  int count = 0;
  for (int i = 0; i < 7; i++) if (d & (1 << i)) count++;
  if (count > 3) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d days", count);
    return String(buf);
  }
  static const char *N[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  String s;
  for (int i = 0; i < 7; i++) {
    if (d & (1 << i)) {
      if (s.length()) s += ",";
      s += N[i];
    }
  }
  return s;
}

const char *dayLabelEn(int offset, int wday) {
  if (offset == 0) return "TODAY";
  if (offset == 1) return "TOMORROW";
  static const char *N[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  return N[wday % 7];
}

const char *dayLabelRu(int offset, int wday) {
  if (offset == 0) return "Сегодня";
  if (offset == 1) return "Завтра";
  static const char *N[] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
  return N[wday % 7];
}

int findNextAlarm(int &dayOffset, int &nextWday) {
  dayOffset = 0;
  nextWday = 0;
  struct tm t;
  if (!readTime(t)) return -1;
  int nowMin = t.tm_hour * 60 + t.tm_min;
  int best = 1000000;
  int bestIdx = -1;
  int bestOff = 0;
  int bestWd = t.tm_wday;
  for (int i = 0; i < alarmCount; i++) {
    if (!alarms[i].enabled || alarms[i].days == 0) continue;
    for (int off = 0; off < 8; off++) {
      int wd = (t.tm_wday + off) % 7;
      if (!dayEnabled(alarms[i].days, wd)) continue;
      int target = off * 1440 + alarms[i].hour * 60 + alarms[i].minute;
      if (off == 0 && target <= nowMin) continue;
      if (target < best) {
        best = target;
        bestIdx = i;
        bestOff = off;
        bestWd = wd;
      }
      break;
    }
  }
  if (bestIdx >= 0) {
    dayOffset = bestOff;
    nextWday = bestWd;
  }
  return bestIdx;
}

String nextAlarmTextEn() {
  struct tm t;
  if (!readTime(t)) return "No time";
  int off, wd;
  int idx = findNextAlarm(off, wd);
  if (idx < 0) return "No alarms";
  char buf[40];
  snprintf(buf, sizeof(buf), "%s %02d:%02d", dayLabelEn(off, wd),
           alarms[idx].hour, alarms[idx].minute);
  return String(buf);
}

String nextAlarmTextRu() {
  struct tm t;
  if (!readTime(t)) return "нет времени";
  int off, wd;
  int idx = findNextAlarm(off, wd);
  if (idx < 0) return "нет будильников";
  char buf[56];
  snprintf(buf, sizeof(buf), "%s %02d:%02d", dayLabelRu(off, wd),
           alarms[idx].hour, alarms[idx].minute);
  return String(buf);
}

// ======================= КНОПКИ =======================
void buttonInit(Button &b, uint8_t pin) {
  b.pin = pin;
  pinMode(pin, INPUT_PULLUP);
  b.lastRead = digitalRead(pin);
  b.stable = b.lastRead;
  b.lastChange = millis();
  b.pressStart = 0;
  b.longFired = false;
  b.shortEvent = false;
  b.longEvent = false;
}

void buttonUpdate(Button &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRead) {
    b.lastRead = raw;
    b.lastChange = millis();
  }
  if (millis() - b.lastChange >= 25 && b.stable != raw) {
    b.stable = raw;
    if (b.stable == LOW) {
      b.pressStart = millis();
      b.longFired = false;
    } else {
      if (!b.longFired && (millis() - b.pressStart < LONG_PRESS_MS)) {
        b.shortEvent = true;
      }
    }
  }
  if (b.stable == LOW && !b.longFired &&
      (millis() - b.pressStart >= LONG_PRESS_MS)) {
    b.longFired = true;
    b.longEvent = true;
  }
}

bool takeShort(Button &b) {
  bool e = b.shortEvent;
  b.shortEvent = false;
  return e;
}

bool takeLong(Button &b) {
  bool e = b.longEvent;
  b.longEvent = false;
  return e;
}

void handleButtons() {
  buttonUpdate(btnA);
  buttonUpdate(btnB);
  buttonUpdate(btnBoot);

  bool aS = takeShort(btnA), aL = takeLong(btnA);
  bool bS = takeShort(btnB), bL = takeLong(btnB);
  bool oS = takeShort(btnBoot), oL = takeLong(btnBoot);

  if (screen == SCREEN_ALARM_LIST && alarmState == ALARM_IDLE) {
    if (aS) listMove(-1);
    if (aL) listMove(-1);
    if (bS) listMove(1);
    if (bL && alarmCount > 0) startAlarm(listSelected, true);
    if (oS || oL) {
      screen = SCREEN_CLOCK;
      forceRedraw = true;
    }
    return;
  }

  if (screen == SCREEN_INFO && (aS || aL || bS || bL || oS || oL)) {
    screen = SCREEN_CLOCK;
    forceRedraw = true;
    return;
  }

  if (aS) onButtonA(false);
  if (aL) onButtonA(true);
  if (bS) onButtonB(false);
  if (bL) onButtonB(true);
  if (oS) onBootButton(false);
  if (oL) onBootButton(true);
}

void onButtonA(bool longPress) {
  if (alarmState != ALARM_IDLE) {
    if (longPress) stopAlarm();
    else if (alarmState == ALARM_RINGING) snoozeAlarm();
    else addSnooze();
    return;
  }
  if (longPress) {
    dimOverride = !dimOverride;
    applyBrightness(targetBrightness(), true);
    forceRedraw = true;
  } else {
    showAlarmListScreen();
  }
}

void onButtonB(bool longPress) {
  if (longPress) {
    if (alarmState == ALARM_IDLE) startAlarm(-1, true);
    else stopAlarm();
    return;
  }
  if (alarmState != ALARM_IDLE) {
    stopAlarm();
  } else {
    if (wifiConfigured) {
      configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
      lastNtpAttempt = millis();
    }
    showInfoScreen();
  }
}

void onBootButton(bool longPress) {
  if (longPress) {
    if (alarmState != ALARM_IDLE) stopAlarm();
    else startAlarm(-1, true);
    return;
  }
  if (alarmState == ALARM_RINGING) snoozeAlarm();
  else if (alarmState == ALARM_SNOOZED) addSnooze();
  else showInfoScreen();
}

// ======================= ХРАНЕНИЕ =======================
void loadSettings() {
  tz = prefs.getString("tz", WIFI_TZ);
  if (tz.length() == 0 || tz.length() > 63) tz = WIFI_TZ;
  dayBrightness = prefs.getUChar("bright", 60);
  if (dayBrightness < 10) dayBrightness = 10;
  autoNight = prefs.getBool("autonight", true);
}

void saveSettings() {
  prefs.putString("tz", tz);
  prefs.putUChar("bright", dayBrightness);
  prefs.putBool("autonight", autoNight);
}

void loadAlarms() {
  alarmCount = 0;
  String json = prefs.getString("alarms", "[]");
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject o : arr) {
    if (alarmCount >= MAX_ALARMS) break;
    Alarm &a = alarms[alarmCount];
    a.id = o["id"] | (alarmCount + 1);
    a.hour = o["h"] | 7;
    a.minute = o["m"] | 30;
    a.days = o["days"] | 0;
    a.enabled = o["enabled"] | true;
    const char *l = o["label"] | "Alarm";
    strncpy(a.label, l, sizeof(a.label) - 1);
    a.label[sizeof(a.label) - 1] = 0;
    if (a.hour > 23) a.hour = 0;
    if (a.minute > 59) a.minute = 0;
    if (a.days > 127) a.days = 0;
    alarmCount++;
  }
}

void saveAlarms() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < alarmCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = alarms[i].id;
    o["h"] = alarms[i].hour;
    o["m"] = alarms[i].minute;
    o["days"] = alarms[i].days;
    o["enabled"] = alarms[i].enabled;
    o["label"] = alarms[i].label;
  }
  String out;
  serializeJson(doc, out);
  prefs.putString("alarms", out);
}

uint8_t newAlarmId() {
  for (int id = 1; id < 256; id++) {
    bool used = false;
    for (int i = 0; i < alarmCount; i++) {
      if (alarms[i].id == id) {
        used = true;
        break;
      }
    }
    if (!used) return (uint8_t)id;
  }
  return 1;
}

int findAlarmIndex(uint8_t id) {
  for (int i = 0; i < alarmCount; i++) {
    if (alarms[i].id == id) return i;
  }
  return -1;
}

void seedDefaultAlarm() {
  if (prefs.isKey("alarms")) return;
  alarmCount = 1;
  alarms[0].id = 1;
  alarms[0].hour = 7;
  alarms[0].minute = 30;
  alarms[0].days = 31;  // Mon-Fri
  alarms[0].enabled = true;
  strncpy(alarms[0].label, "Wake up", sizeof(alarms[0].label) - 1);
  alarms[0].label[sizeof(alarms[0].label) - 1] = 0;
  saveAlarms();
}

// ======================= БУДИЛЬНИКИ =======================
void startAlarm(int8_t idx, bool test) {
  if (alarmState != ALARM_IDLE) return;
  activeAlarm = idx;
  testAlarmActive = test;
  alarmState = ALARM_RINGING;
  ringStartMs = millis();
  sunriseStartMs = millis();
  sunriseActive = true;
  screen = SCREEN_ALARM;
  drawAlarmScreen(true);
  applyBrightness(15, true);
  Serial.printf("ALARM start: %s\n",
                test ? "TEST" : (idx >= 0 ? alarms[idx].label : "?"));
}

void stopAlarm() {
  if (alarmState == ALARM_IDLE) return;
  alarmState = ALARM_IDLE;
  activeAlarm = -1;
  testAlarmActive = false;
  sunriseActive = false;
  screen = SCREEN_CLOCK;
  forceRedraw = true;
  applyBrightness(targetBrightness(), true);
  Serial.println("ALARM stop");
}

void snoozeAlarm() {
  if (alarmState != ALARM_RINGING) return;
  if (testAlarmActive) {
    stopAlarm();
    return;
  }
  alarmState = ALARM_SNOOZED;
  snoozeUntilMs = millis() + (uint32_t)SNOOZE_MIN * 60000UL;
  sunriseActive = false;
  screen = SCREEN_CLOCK;
  forceRedraw = true;
  applyBrightness(15, true);
  Serial.println("ALARM snooze");
}

void addSnooze() {
  if (alarmState != ALARM_SNOOZED) return;
  uint32_t maxUntil = millis() + 60UL * 60000UL;
  uint32_t next = snoozeUntilMs + (uint32_t)SNOOZE_MIN * 60000UL;
  snoozeUntilMs = (next < maxUntil) ? next : maxUntil;
  drawNextAlarmCard();
}

void checkAlarms() {
  time_t now = time(nullptr);
  if (now < 1700000000UL) return;
  uint32_t key = (uint32_t)(now / 60);
  if (key == lastMinuteKey) return;
  lastMinuteKey = key;
  if (alarmState != ALARM_IDLE) return;

  struct tm t;
  localtime_r(&now, &t);
  for (int i = 0; i < alarmCount; i++) {
    if (!alarms[i].enabled) continue;
    if (!dayEnabled(alarms[i].days, t.tm_wday)) continue;
    if (alarms[i].hour == t.tm_hour && alarms[i].minute == t.tm_min) {
      startAlarm(i, false);
      break;
    }
  }
}

void updateAlarmState() {
  if (alarmState == ALARM_SNOOZED) {
    if ((int32_t)(millis() - snoozeUntilMs) >= 0) {
      alarmState = ALARM_RINGING;
      ringStartMs = millis();
      sunriseStartMs = millis();
      sunriseActive = true;
      screen = SCREEN_ALARM;
      drawAlarmScreen(true);
      applyBrightness(15, true);
      Serial.println("ALARM re-ring");
    }
  } else if (alarmState == ALARM_RINGING) {
    uint32_t maxRing = testAlarmActive ? TEST_ALARM_MS : ALARM_AUTO_STOP_MS;
    if (millis() - ringStartMs > maxRing) stopAlarm();
  }
}

// ======================= ЯРКОСТЬ =======================
uint8_t targetBrightness() {
  if (dimOverride) return 10;
  if (!autoNight) return dayBrightness;
  struct tm t;
  if (!readTime(t)) return dayBrightness;
  bool night = (t.tm_hour >= 23 || t.tm_hour < 7);
  if (!night) return dayBrightness;
  return (dayBrightness < 20) ? dayBrightness : 20;
}

void applyBrightness(uint8_t value, bool force) {
#if TFT_BL >= 0
  if (!blPwm) return;
  if (!force) {
    int diff = (int)value - (int)currentBrightness;
    if (diff < 0) diff = -diff;
    if (diff < 2) return;
  }
  currentBrightness = value;
  ledcWrite(TFT_BL, value);
#endif
}

void updateBrightness() {
  static uint32_t last = 0;
  if (millis() - last < 100) return;
  last = millis();

  if (alarmState == ALARM_RINGING) {
    // Будильник всегда достаточно яркий, даже если день выставлен тусклым.
    uint8_t alarmMax = (dayBrightness < 100) ? 100 : dayBrightness;
    uint32_t elapsed = millis() - sunriseStartMs;
    uint32_t riseMs = testAlarmActive ? 5000UL : 60000UL;
    uint8_t v;
    if (elapsed < riseMs) {
      float p = (float)elapsed / (float)riseMs;
      uint8_t low = (alarmMax > 15) ? 15 : alarmMax;
      v = low + (uint8_t)(p * (float)(alarmMax - low));
    } else {
      sunriseActive = false;
      uint8_t low = (alarmMax > 120) ? 80 : (alarmMax / 2);
      float s = (sinf((float)millis() / 500.0f) + 1.0f) * 0.5f;
      v = low + (uint8_t)(s * (float)(alarmMax - low));
    }
    applyBrightness(v);
  } else if (alarmState == ALARM_SNOOZED) {
    applyBrightness(15);
  } else {
    applyBrightness(targetBrightness());
  }
}

// ======================= ДИСПЛЕЙ =======================
void drawTextCentered(const GFXfont *font, const char *text, int16_t cx,
                      int16_t baseline, uint16_t color, uint16_t bg) {
  tft.setFont(font);
  tft.setTextSize(1);
  tft.setTextColor(color, bg);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(cx - (int16_t)w / 2 - x1, baseline);
  tft.print(text);
}

void drawTextLeft(const GFXfont *font, const char *text, int16_t x,
                  int16_t baseline, uint16_t color, uint16_t bg) {
  tft.setFont(font);
  tft.setTextSize(1);
  tft.setTextColor(color, bg);
  tft.setCursor(x, baseline);
  tft.print(text);
}

void drawStatusBar() {
  tft.fillRect(0, 0, TFT_W, 30, C_PANEL2);
  tft.drawFastHLine(0, 29, TFT_W, C_LINE);
  drawTextLeft(&FreeSans9pt7b, "SMART ALARM", 8, 21, C_ACCENT, C_PANEL2);

  tft.setFont(NULL);
  tft.setTextSize(1);
  tft.setTextColor(C_MUTED, C_PANEL2);

  uint16_t wifiCol = wifiConnected ? C_GREEN : (wifiConfigured ? C_ORANGE : C_RED);
  tft.fillCircle(146, 11, 3, wifiCol);
  tft.setCursor(153, 7);
  tft.print("Wi-Fi");

  uint16_t ntpCol = timeSynced ? C_GREEN : (wifiConnected ? C_ORANGE : C_RED);
  tft.fillCircle(146, 22, 3, ntpCol);
  tft.setCursor(153, 18);
  tft.print("NTP");

  char buf[12];
  snprintf(buf, sizeof(buf), "A:%u", (unsigned)alarmCount);
  tft.setCursor(208, 14);
  tft.print(buf);
}

void drawProgressBar(bool valid, const struct tm &t) {
  tft.fillRect(20, 174, 200, 4, C_PANEL);
  if (!valid) return;
  int w = (200 * t.tm_sec) / 60;
  if (w > 0) tft.fillRect(20, 174, w, 4, C_ACCENT);
}

void drawClockTime(bool valid, const struct tm &t) {
  tft.fillRect(0, 34, TFT_W, 138, C_BG);

  if (valid) {
    char tb[6];
    snprintf(tb, sizeof(tb), "%02d:%02d", t.tm_hour, t.tm_min);
    drawTextCentered(&FreeSansBold24pt7b, tb, 120, 128, C_TEXT, C_BG);

    char db[32];
    static const char *WD[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    snprintf(db, sizeof(db), "%02d.%02d.%04d  %s", t.tm_mday, t.tm_mon + 1,
             t.tm_year + 1900, WD[t.tm_wday % 7]);
    drawTextCentered(&FreeSans9pt7b, db, 120, 162, C_MUTED, C_BG);
  } else {
    drawTextCentered(&FreeSansBold24pt7b, "--:--", 120, 128, C_MUTED, C_BG);
    const char *msg = !wifiConfigured ? "EDIT INCLUDE/SECRETS.H"
                                     : (wifiConnected ? "SYNCING TIME..." : "CONNECTING WI-FI...");
    drawTextCentered(&FreeSans9pt7b, msg, 120, 162, C_ORANGE, C_BG);
  }
  drawProgressBar(valid, t);
}

void drawNextAlarmCard() {
  tft.fillRoundRect(8, 190, 224, 76, 10, C_PANEL);
  tft.drawRoundRect(8, 190, 224, 76, 10, C_LINE);
  tft.setFont(NULL);
  tft.setTextSize(1);

  if (alarmState == ALARM_SNOOZED) {
    int left = (snoozeUntilMs > millis()) ? (int)((snoozeUntilMs - millis()) / 1000) : 0;
    char sb[16];
    snprintf(sb, sizeof(sb), "%02d:%02d", left / 60, left % 60);
    tft.setTextColor(C_ORANGE, C_PANEL);
    tft.setCursor(18, 202);
    tft.print("SNOOZED");
    drawTextLeft(&FreeSansBold18pt7b, sb, 18, 254, C_ORANGE, C_PANEL);
    tft.setFont(NULL);
    tft.setTextSize(1);
    tft.setTextColor(C_MUTED, C_PANEL);
    tft.setCursor(120, 230);
    tft.print("A: +5 min");
    tft.setCursor(120, 244);
    tft.print("B: stop");
    return;
  }

  int off, wd;
  int idx = findNextAlarm(off, wd);
  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setCursor(18, 202);
  tft.print("NEXT ALARM");

  if (idx < 0) {
    struct tm tt;
    bool tvalid = readTime(tt);
    drawTextLeft(&FreeSansBold12pt7b, tvalid ? "No alarms" : "No time", 18, 240,
                 C_MUTED, C_PANEL);
    tft.setFont(NULL);
    tft.setTextSize(1);
    tft.setTextColor(C_MUTED, C_PANEL);
    tft.setCursor(18, 254);
    tft.print(tvalid ? "Set alarms in web panel" : "Waiting for NTP");
    return;
  }

  Alarm &a = alarms[idx];
  tft.setTextColor(C_ACCENT, C_PANEL);
  tft.setCursor(18, 216);
  tft.print(dayLabelEn(off, wd));

  char tb[8];
  snprintf(tb, sizeof(tb), "%02d:%02d", a.hour, a.minute);
  drawTextLeft(&FreeSansBold18pt7b, tb, 18, 254, C_TEXT, C_PANEL);

  tft.setFont(NULL);
  tft.setTextSize(1);
  tft.setTextColor(C_ACCENT, C_PANEL);
  tft.setCursor(120, 230);
  tft.print(daysTextEn(a.days));

  char lb[19];
  copyAsciiLabel(a.label, lb, sizeof(lb));
  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setCursor(120, 244);
  tft.print(lb);
}

void drawFooter() {
  tft.fillRect(0, 276, TFT_W, 44, C_BG);
  tft.drawFastHLine(0, 276, TFT_W, C_LINE);
  tft.setFont(NULL);
  tft.setTextSize(1);

  if (wifiConnected) {
    tft.setTextColor(C_GREEN, C_BG);
    tft.setCursor(8, 284);
    tft.printf("Wi-Fi %s", WiFi.localIP().toString().c_str());
  } else if (wifiConfigured) {
    tft.setTextColor(C_ORANGE, C_BG);
    tft.setCursor(8, 284);
    tft.print("Wi-Fi connecting...");
  } else {
    tft.setTextColor(C_RED, C_BG);
    tft.setCursor(8, 284);
    tft.print("Wi-Fi not configured");
  }

  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(8, 296);
  if (wifiConnected) {
    if (mdnsStarted) {
      tft.print("Web: http://alarm.local");
    } else {
      tft.printf("Web: http://%s", WiFi.localIP().toString().c_str());
    }
  } else {
    tft.print("Edit include/secrets.h");
  }

  tft.setCursor(8, 308);
  tft.print("A: alarms  B: info");
}

void drawClockScreen(bool full) {
  struct tm t;
  bool valid = readTime(t);
  if (full) {
    tft.fillScreen(C_BG);
    drawStatusBar();
  }
  drawClockTime(valid, t);
  drawNextAlarmCard();
  drawFooter();
}

void drawAlarmTime() {
  tft.fillRect(0, 100, TFT_W, 85, C_ALARM_BG);
  struct tm t;
  bool valid = readTime(t);
  if (valid) {
    char tb[8];
    snprintf(tb, sizeof(tb), "%02d:%02d", t.tm_hour, t.tm_min);
    drawTextCentered(&FreeSansBold24pt7b, tb, 120, 148, C_TEXT, C_ALARM_BG);
  } else {
    drawTextCentered(&FreeSansBold24pt7b, "--:--", 120, 148, C_MUTED, C_ALARM_BG);
  }
  const char *lab = "ALARM";
  char labBuf[21];
  if (testAlarmActive) {
    lab = "TEST ALARM";
  } else if (activeAlarm >= 0 && activeAlarm < alarmCount) {
    copyAsciiLabel(alarms[activeAlarm].label, labBuf, sizeof(labBuf));
    lab = labBuf;
  }
  drawTextCentered(&FreeSans9pt7b, lab, 120, 180, C_YELLOW, C_ALARM_BG);
}

void drawAlarmScreen(bool full) {
  if (!full) {
    updateAlarmScreen();
    return;
  }
  tft.fillScreen(C_ALARM_BG);

  tft.fillCircle(120, 235, 30, C_SUN);
  for (int a = 0; a < 360; a += 30) {
    float rad = (float)a * 0.0174532925f;
    int x1 = 120 + (int)(cosf(rad) * 38.0f);
    int y1 = 235 + (int)(sinf(rad) * 38.0f);
    int x2 = 120 + (int)(cosf(rad) * 48.0f);
    int y2 = 235 + (int)(sinf(rad) * 48.0f);
    tft.drawLine(x1, y1, x2, y2, C_SUN_DARK);
  }

  drawTextCentered(&FreeSans9pt7b, "ALARM", 120, 32, C_ORANGE, C_ALARM_BG);
  drawTextCentered(&FreeSansBold18pt7b, "WAKE UP", 120, 80, C_YELLOW, C_ALARM_BG);
  drawAlarmTime();

  tft.setFont(NULL);
  tft.setTextSize(1);
  tft.setTextColor(C_MUTED, C_ALARM_BG);
  tft.setCursor(14, 290);
  tft.print("A: SNOOZE");
  tft.setCursor(130, 290);
  tft.print("B: STOP");
  tft.setCursor(14, 304);
  tft.print("BOOT: SNOOZE / HOLD STOP");
}

void updateAlarmScreen() {
  static uint32_t lastSec = 0xFFFFFFFF;
  static uint32_t lastBlink = 0;
  struct tm t;
  bool valid = readTime(t);
  uint32_t sec = valid ? (uint32_t)(t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec)
                       : 0xFFFFFFFF;
  if (sec != lastSec) {
    lastSec = sec;
    drawAlarmTime();
  }
  if (millis() - lastBlink >= 600) {
    lastBlink = millis();
    bool on = ((millis() / 600) & 1) != 0;
    tft.fillRect(0, 48, TFT_W, 38, C_ALARM_BG);
    drawTextCentered(&FreeSansBold18pt7b, "WAKE UP", 120, 80,
                     on ? C_YELLOW : C_ORANGE, C_ALARM_BG);
  }
}

void drawInfoScreen() {
  tft.fillScreen(C_BG);
  drawStatusBar();
  drawTextCentered(&FreeSansBold18pt7b, "SYSTEM INFO", 120, 55, C_ACCENT, C_BG);

  tft.setFont(NULL);
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT, C_BG);

  char line[72];
  int y = 82;
  auto put = [&](const char *text) {
    tft.setCursor(14, y);
    tft.print(text);
    y += 18;
  };

  snprintf(line, sizeof(line), "IP: %s",
           wifiConnected ? WiFi.localIP().toString().c_str() : "not connected");
  put(line);
  snprintf(line, sizeof(line), "Web: %s",
           wifiConnected ? (mdnsStarted ? "http://alarm.local" : "see IP above") : "-");
  put(line);
  snprintf(line, sizeof(line), "Wi-Fi RSSI: %d dBm", wifiConnected ? WiFi.RSSI() : 0);
  put(line);
  snprintf(line, sizeof(line), "NTP: %s", timeSynced ? "OK" : "SYNCING");
  put(line);
  snprintf(line, sizeof(line), "TZ: %s", tz.c_str());
  put(line);
  snprintf(line, sizeof(line), "Brightness: %u", (unsigned)dayBrightness);
  put(line);
  snprintf(line, sizeof(line), "Auto night: %s", autoNight ? "ON" : "OFF");
  put(line);
  snprintf(line, sizeof(line), "Alarms: %u", (unsigned)alarmCount);
  put(line);
  snprintf(line, sizeof(line), "Next: %s", nextAlarmTextEn().c_str());
  put(line);
  snprintf(line, sizeof(line), "Free heap: %u KB", (unsigned)(ESP.getFreeHeap() / 1024));
  put(line);

  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(14, 300);
  tft.print("Any button to return");
}

void drawAlarmListScreen() {
  tft.fillScreen(C_BG);
  drawStatusBar();

  drawTextLeft(&FreeSansBold18pt7b, "ALARMS", 10, 56, C_TEXT, C_BG);

  char countBuf[12];
  if (alarmCount == 0) {
    snprintf(countBuf, sizeof(countBuf), "0/0");
  } else {
    snprintf(countBuf, sizeof(countBuf), "%u/%u",
             (unsigned)(listSelected + 1), (unsigned)alarmCount);
  }
  drawTextLeft(&FreeSans9pt7b, countBuf, 196, 56, C_ACCENT, C_BG);

  if (alarmCount == 0) {
    drawTextCentered(&FreeSansBold12pt7b, "No alarms yet", 120, 145, C_MUTED, C_BG);
    drawTextCentered(&FreeSans9pt7b, "Add them in the web panel", 120, 174,
                     C_MUTED, C_BG);
  } else {
    const int rows = 4;
    const int rowY = 66;
    const int rowH = 50;
    for (int r = 0; r < rows; r++) {
      int idx = listTop + r;
      if (idx >= alarmCount) break;
      int y = rowY + r * rowH;
      bool selected = (idx == listSelected);
      uint16_t bg = selected ? C_PANEL : C_BG;

      tft.fillRoundRect(4, y, 232, 44, 6, bg);
      if (selected) {
        tft.drawRoundRect(4, y, 232, 44, 6, C_ACCENT);
        tft.fillRect(4, y, 4, 44, C_ACCENT);
      } else {
        tft.drawFastHLine(12, y + 45, 216, C_LINE);
      }

      Alarm &a = alarms[idx];
      char tb[8];
      snprintf(tb, sizeof(tb), "%02d:%02d", a.hour, a.minute);
      drawTextLeft(&FreeSansBold18pt7b, tb, 12, y + 29,
                   a.enabled ? C_TEXT : C_MUTED, bg);

      char lb[14];
      copyAsciiLabel(a.label, lb, sizeof(lb));
      drawTextLeft(&FreeSans9pt7b, lb, 96, y + 18,
                   selected ? C_TEXT : C_MUTED, bg);

      String days = daysTextEn(a.days);
      drawTextLeft(&FreeSans9pt7b, days.c_str(), 96, y + 35,
                   selected ? C_ACCENT : C_MUTED, bg);

      tft.setFont(NULL);
      tft.setTextSize(1);
      tft.setTextColor(a.enabled ? C_GREEN : C_RED, bg);
      tft.setCursor(200, y + 13);
      tft.print(a.enabled ? "ON" : "OFF");
    }
  }

  tft.fillRect(0, 276, TFT_W, 44, C_BG);
  tft.drawFastHLine(0, 276, TFT_W, C_LINE);
  tft.setFont(NULL);
  tft.setTextSize(1);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(8, 286);
  tft.print("A: up  B: down");
  tft.setCursor(8, 298);
  tft.print("B hold: test  BOOT: back");
}

void showAlarmListScreen() {
  if (alarmState != ALARM_IDLE) return;
  screen = SCREEN_ALARM_LIST;
  listSelected = 0;
  listTop = 0;
  listUntil = millis() + LIST_SCREEN_MS;
  forceRedraw = false;
  drawAlarmListScreen();
}

void listMove(int8_t delta) {
  listUntil = millis() + LIST_SCREEN_MS;
  if (alarmCount == 0) {
    drawAlarmListScreen();
    return;
  }

  int next = (int)listSelected + delta;
  while (next < 0) next += alarmCount;
  while (next >= alarmCount) next -= alarmCount;
  listSelected = (int8_t)next;

  if (listSelected < listTop) listTop = listSelected;
  if (listSelected >= (int)listTop + 4) listTop = listSelected - 3;

  int maxTop = (alarmCount > 4) ? (int)alarmCount - 4 : 0;
  if (listTop > maxTop) listTop = (int8_t)maxTop;
  if (listTop < 0) listTop = 0;

  drawAlarmListScreen();
}

void showInfoScreen() {
  if (alarmState != ALARM_IDLE) return;
  screen = SCREEN_INFO;
  infoUntil = millis() + INFO_SCREEN_MS;
  drawInfoScreen();
}

void updateDisplay() {
  static uint32_t lastSec = 0xFFFFFFFF;
  static uint32_t lastMin = 0xFFFFFFFF;
  static uint32_t lastStatus = 0;
  static bool lastValid = false;

  if (screen == SCREEN_ALARM) {
    if (alarmState == ALARM_RINGING) updateAlarmScreen();
    return;
  }
  if (screen == SCREEN_ALARM_LIST) {
    if ((int32_t)(millis() - listUntil) >= 0) {
      screen = SCREEN_CLOCK;
      forceRedraw = true;
    }
    return;
  }
  if (screen == SCREEN_INFO) {
    if ((int32_t)(millis() - infoUntil) >= 0) {
      screen = SCREEN_CLOCK;
      forceRedraw = true;
    }
    return;
  }

  struct tm t;
  bool valid = readTime(t);
  uint32_t sec = valid ? (uint32_t)(t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec)
                       : 0xFFFFFFFF;
  uint32_t minute = valid ? (uint32_t)(t.tm_hour * 60 + t.tm_min) : 0xFFFFFFFF;

  if (forceRedraw) {
    drawClockScreen(true);
    forceRedraw = false;
    lastSec = sec;
    lastMin = minute;
    lastValid = valid;
    lastStatus = millis();
    return;
  }
  if (statusRedraw) {
    drawStatusBar();
    drawFooter();
    statusRedraw = false;
    lastStatus = millis();
  }
  if (ntpRedraw && (int32_t)(millis() - ntpRedrawAt) >= 0) {
    drawStatusBar();
    drawClockScreen(false);
    ntpRedraw = false;
    lastSec = sec;
    lastMin = minute;
    lastValid = valid;
    lastStatus = millis();
    return;
  }
  if (valid != lastValid) {
    drawClockScreen(false);
    lastValid = valid;
    lastMin = minute;
    lastSec = sec;
    return;
  }
  if (minute != lastMin) {
    drawClockTime(valid, t);
    drawNextAlarmCard();
    lastMin = minute;
    lastSec = sec;
  } else if (sec != lastSec) {
    drawProgressBar(valid, t);
    lastSec = sec;
    if (alarmState == ALARM_SNOOZED) drawNextAlarmCard();
  }
  if (millis() - lastStatus >= 5000) {
    drawStatusBar();
    drawFooter();
    lastStatus = millis();
  }
}

// ======================= WEB API =======================
void handleState() {
  JsonDocument doc;
  struct tm t;
  bool valid = readTime(t);

  char nowBuf[12];
  char dateBuf[32];
  if (valid) {
    snprintf(nowBuf, sizeof(nowBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    snprintf(dateBuf, sizeof(dateBuf), "%02d.%02d.%04d", t.tm_mday, t.tm_mon + 1,
             t.tm_year + 1900);
  } else {
    strcpy(nowBuf, "--:--:--");
    strcpy(dateBuf, "--.--.----");
  }

  int snoozeLeft = 0;
  if (alarmState == ALARM_SNOOZED) {
    int32_t d = (int32_t)(snoozeUntilMs - millis());
    snoozeLeft = d > 0 ? (int)(d / 1000) : 0;
  }

  String label = "";
  if (testAlarmActive) label = "TEST";
  else if (activeAlarm >= 0 && activeAlarm < alarmCount) label = alarms[activeAlarm].label;

  const char *st = alarmState == ALARM_RINGING ? "ringing"
                   : alarmState == ALARM_SNOOZED ? "snoozed" : "idle";

  doc["now"] = nowBuf;
  doc["date"] = dateBuf;
  doc["wday"] = valid ? t.tm_wday : -1;
  doc["synced"] = timeSynced;
  doc["wifi"] = wifiConnected;
  doc["ip"] = wifiConnected ? WiFi.localIP().toString() : String("");
  doc["state"] = st;
  doc["snoozeLeft"] = snoozeLeft;
  doc["test"] = testAlarmActive;
  doc["activeLabel"] = label;
  doc["next"] = nextAlarmTextRu();
  doc["brightness"] = dayBrightness;
  doc["tz"] = tz;
  doc["autoNight"] = autoNight;
  doc["heap"] = ESP.getFreeHeap() / 1024;
  doc["alarmCount"] = alarmCount;

  JsonArray arr = doc["alarms"].to<JsonArray>();
  for (int i = 0; i < alarmCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = alarms[i].id;
    o["h"] = alarms[i].hour;
    o["m"] = alarms[i].minute;
    o["days"] = alarms[i].days;
    o["enabled"] = alarms[i].enabled;
    o["label"] = alarms[i].label;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSaveAlarm() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }

  int id = doc["id"] | 0;
  int h = doc["h"] | -1;
  int m = doc["m"] | -1;
  int days = doc["days"] | 0;
  bool enabled = doc["enabled"] | true;
  String label = doc["label"].is<const char *>() ? doc["label"].as<String>() : String("Alarm");

  if (h < 0 || h > 23 || m < 0 || m > 59 || days < 0 || days > 127) {
    server.send(400, "application/json", "{\"error\":\"bad time\"}");
    return;
  }

  label.trim();
  if (label.length() == 0) label = "Alarm";
  if (label.length() > 40) label = label.substring(0, 40);

  int idx;
  if (id == 0) {
    if (alarmCount >= MAX_ALARMS) {
      server.send(400, "application/json", "{\"error\":\"alarm limit\"}");
      return;
    }
    idx = alarmCount++;
    alarms[idx].id = 0;
    alarms[idx].id = newAlarmId();
  } else {
    idx = findAlarmIndex((uint8_t)id);
    if (idx < 0) {
      server.send(404, "application/json", "{\"error\":\"not found\"}");
      return;
    }
  }

  alarms[idx].hour = (uint8_t)h;
  alarms[idx].minute = (uint8_t)m;
  alarms[idx].days = (uint8_t)days;
  alarms[idx].enabled = enabled;
  strncpy(alarms[idx].label, label.c_str(), sizeof(alarms[idx].label) - 1);
  alarms[idx].label[sizeof(alarms[idx].label) - 1] = 0;

  if (alarmState != ALARM_IDLE && activeAlarm == idx && !enabled) stopAlarm();

  saveAlarms();
  forceRedraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleDeleteAlarm() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  int id = doc["id"] | -1;
  int idx = findAlarmIndex((uint8_t)id);
  if (idx < 0) {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
    return;
  }

  if (alarmState != ALARM_IDLE) {
    if (activeAlarm == idx) {
      stopAlarm();
    } else if (activeAlarm > idx) {
      activeAlarm--;
    }
  }

  for (int i = idx; i < alarmCount - 1; i++) alarms[i] = alarms[i + 1];
  alarmCount--;
  saveAlarms();
  forceRedraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAction() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  String action = doc["action"].is<const char *>() ? doc["action"].as<String>() : String("");

  if (action == "test") {
    if (alarmState != ALARM_IDLE) stopAlarm();
    startAlarm(-1, true);
  } else if (action == "snooze") {
    snoozeAlarm();
  } else if (action == "stop") {
    stopAlarm();
  } else {
    server.send(400, "application/json", "{\"error\":\"unknown action\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSettings() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }

  String newTz = doc["tz"].is<const char *>() ? doc["tz"].as<String>() : tz;
  int bright = doc["brightness"] | (int)dayBrightness;
  bool an = doc["autoNight"] | autoNight;

  if (newTz.length() == 0 || newTz.length() > 63) newTz = tz;
  tz = newTz;
  dayBrightness = (uint8_t)constrain(bright, 10, 255);
  autoNight = an;
  dimOverride = false;

  saveSettings();
  configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
  lastNtpAttempt = millis();
  applyBrightness(targetBrightness(), true);
  forceRedraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", WEB_PAGE);
  });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/alarm", HTTP_POST, handleSaveAlarm);
  server.on("/api/delete", HTTP_POST, handleDeleteAlarm);
  server.on("/api/action", HTTP_POST, handleAction);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
  Serial.println("Web server: port 80");
}

// ======================= WI-FI =======================
void startWifi() {
  String ssid = WIFI_SSID;
  String pass = WIFI_PASSWORD;
  if (ssid.length() == 0 || ssid == "YOUR_WIFI") {
    wifiConfigured = false;
    Serial.println("Wi-Fi: not configured (edit include/secrets.h)");
    return;
  }
  wifiConfigured = true;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-alarm");
  // Пониженная мощность и modem sleep снижают пиковый ток Wi-Fi:
  // иначе дешёвый USB-порт/стабилизатор ESP32 уходит в brownout.
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_2dBm);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("Wi-Fi: connecting to %s...\n", ssid.c_str());
}

void updateWifi() {
  if (!wifiConfigured) return;
  static uint32_t lastReconnect = 0;
  bool conn = (WiFi.status() == WL_CONNECTED);

  if (conn != wifiConnected) {
    wifiConnected = conn;
    if (conn) {
      WiFi.setTxPower(WIFI_POWER_2dBm);
      Serial.printf("Wi-Fi OK: %s RSSI=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      Serial.printf("Wi-Fi TX power: %d\n", (int)WiFi.getTxPower());
      configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
      lastNtpAttempt = millis();
      if (!mdnsStarted) {
        if (MDNS.begin("alarm")) {
          MDNS.addService("http", "tcp", 80);
          mdnsStarted = true;
          Serial.println("mDNS: http://alarm.local");
        }
      }
    } else {
      Serial.println("Wi-Fi lost");
    }
    statusRedraw = true;
  }

  if (!conn && millis() - lastReconnect > 10000) {
    lastReconnect = millis();
    WiFi.reconnect();
  }
  if (conn && !timeSynced && millis() - lastNtpAttempt > 30000) {
    lastNtpAttempt = millis();
    configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
  }
}

void checkTime() {
  struct tm t;
  bool valid = readTime(t);
  if (valid != timeSynced) {
    timeSynced = valid;
    if (valid) {
      // После NTP не делаем полный fillScreen, пока Wi-Fi ещё активен.
      ntpRedraw = true;
      ntpRedrawAt = millis() + 2000;
    } else {
      forceRedraw = true;
    }
    Serial.printf("Time: %s\n", valid ? "synced" : "not synced");
  }
}

// ======================= SETUP / LOOP =======================
void setup() {
  Serial.begin(115200);
  delay(200);
  setCpuFrequencyMhz(80);
  Serial.println();
  Serial.println("=== ESP32 Smart Alarm ===");
  Serial.printf("TFT: CS=%d DC=%d RST=%d MOSI=%d SCLK=%d BL=%d\n",
                TFT_CS, TFT_DC, TFT_RST, TFT_MOSI, TFT_SCLK, TFT_BL);

#if TFT_BL >= 0
  blPwm = ledcAttach(TFT_BL, 5000, 8);
  if (blPwm) {
    ledcWrite(TFT_BL, 0);
  } else {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    Serial.println("Backlight: PWM failed, fallback to full ON");
  }
#endif

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(TFT_W, TFT_H);
  tft.setSPISpeed(20000000);
  tft.setRotation(2);
  tft.setTextWrap(false);
  tft.fillScreen(C_BG);

  prefs.begin("alarm", false);
  loadSettings();
  loadAlarms();
  seedDefaultAlarm();
  applyBrightness(dayBrightness, true);

  buttonInit(btnA, BTN_A);
  buttonInit(btnB, BTN_B);
  buttonInit(btnBoot, BTN_BOOT);

  drawTextCentered(&FreeSansBold18pt7b, "SMART", 120, 100, C_ACCENT, C_BG);
  drawTextCentered(&FreeSansBold24pt7b, "ALARM", 120, 150, C_TEXT, C_BG);
  drawTextCentered(&FreeSans9pt7b, "ESP32 + ST7789", 120, 195, C_MUTED, C_BG);
  drawTextCentered(&FreeSans9pt7b, "starting...", 120, 235, C_MUTED, C_BG);
  delay(700);

  // Сначала рисуем экран, пока Wi-Fi ещё выключен: так пик SPI не совпадает
  // с пиком радио. Статус Wi-Fi/NTP потом обновится без fillScreen.
  drawClockScreen(true);
  forceRedraw = false;

  startWifi();
  setupWebServer();
  configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
  lastNtpAttempt = millis();

  Serial.printf("Heap: %u KB, alarms: %u\n",
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)alarmCount);
}

void loop() {
  server.handleClient();
  updateWifi();
  checkTime();
  handleButtons();
  checkAlarms();
  updateAlarmState();
  updateBrightness();
  updateDisplay();
  delay(2);
}


