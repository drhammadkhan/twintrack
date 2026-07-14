#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <lvgl.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <time.h>

constexpr uint8_t kButtonStation = 9;
constexpr uint8_t kButtonDirection = 8;
constexpr uint8_t kButtonRefresh = 10;
constexpr uint8_t kStatusLed = 11;
constexpr uint32_t kRefreshIntervalMs = 30000;
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kWifiFallbackIntervalMs = 60000;
constexpr uint32_t kCredentialAttemptMs = 20000;
constexpr uint32_t kCredentialResetHoldMs = 3000;
constexpr uint32_t kFooterPageIntervalMs = 5000;
constexpr uint8_t kCredentialAttempts = 3;
constexpr size_t kMaxServices = 3;

const char *const kApiBase = "https://national-rail-api.davwheat.dev/departures";
const char *const kLondonTimeZone = "GMT0BST,M3.5.0/1,M10.5.0/2";
const char *const kPreferencesNamespace = "twintrack";
const char *const kSsidKey = "wifi_ssid";
const char *const kPasswordKey = "wifi_pass";
const char *const kStationKeys[] = {"station_0", "station_1"};
const char *const kDirectionKeys[] = {"direction_0", "direction_1"};

struct Station {
  String code;
  String label;
};

struct Direction {
  String code;
  String label;
};

Station kStations[] = {
    {"MAL", "MALDEN MANOR"},
    {"TOL", "TOLWORTH"},
};

Direction kDirections[] = {
    {"WAT", "WATERLOO"},
    {"CSS", "CHESSINGTON"},
};

struct Departure {
  String scheduled;
  String expected;
  String destination;
  String platform;
  bool cancelled = false;
};

enum class ProvisioningState {
  kIdle,
  kTesting,
  kFailed,
  kSaved,
};

struct DebouncedButton {
  uint8_t pin;
  bool rawPressed = false;
  bool stablePressed = false;
  uint32_t changedAt = 0;

  explicit DebouncedButton(uint8_t buttonPin) : pin(buttonPin) {}

  void begin() {
    pinMode(pin, INPUT_PULLUP);
  }

  bool pressed() {
    const bool current = digitalRead(pin) == LOW;
    const uint32_t now = millis();

    if (current != rawPressed) {
      rawPressed = current;
      changedAt = now;
    }

    if (rawPressed != stablePressed && now - changedAt >= 35) {
      stablePressed = rawPressed;
      return stablePressed;
    }

    return false;
  }
};

TFT_eSPI tft(128, 128);
constexpr uint16_t kScreenWidth = 128;
constexpr uint16_t kScreenHeight = 128;
constexpr uint16_t kHeaderHeight = 28;
constexpr uint16_t kFooterHeight = 16;
constexpr uint16_t kFooterY = kScreenHeight - kFooterHeight;
lv_disp_draw_buf_t lvDrawBuffer;
lv_color_t lvPixelBuffer[kScreenWidth * 12];
lv_disp_drv_t lvDisplayDriver;
lv_obj_t *headerOriginLabel = nullptr;
lv_obj_t *headerClockLabel = nullptr;
lv_obj_t *headerDestinationLabel = nullptr;
lv_obj_t *headerConnectionLabel = nullptr;
lv_obj_t *footerButtonLabels[3] = {nullptr, nullptr, nullptr};
lv_obj_t *footerWebLabel = nullptr;
DNSServer dnsServer;
WebServer webServer(80);
DebouncedButton stationButton{kButtonStation};
DebouncedButton directionButton{kButtonDirection};
DebouncedButton refreshButton{kButtonRefresh};

Departure departures[kMaxServices];
size_t departureCount = 0;
uint8_t stationIndex = 0;
uint8_t directionIndex = 0;
uint32_t lastRefreshAt = 0;
uint32_t lastWifiRetryAt = 0;
uint32_t wifiDisconnectedAt = 0;
uint32_t lastClockCheckAt = 0;
uint32_t credentialAttemptStartedAt = 0;
uint32_t lastProvisioningWifiRetryAt = 0;
uint32_t restartAt = 0;
int32_t lastDisplayedMinute = -1;
uint8_t credentialAttempt = 0;
uint8_t lastFooterPage = 255;
bool refreshRequested = true;
bool provisioningMode = false;
bool webHandlersConfigured = false;
bool lanWebUiRunning = false;
bool clockConfigured = false;
bool retrySavedWifiInProvisioning = false;
String statusMessage = "STARTING";
String savedWifiSsid;
String savedWifiPassword;
String pendingWifiSsid;
String pendingWifiPassword;
String setupApSsid;
String setupApPassword;
String deviceHostname;
ProvisioningState provisioningState = ProvisioningState::kIdle;

void configureWebHandlers();
void startLanWebUi();

lv_color_t colourFromRgb565(uint16_t colour) {
  const uint8_t red = ((colour >> 11) & 0x1F) * 255 / 31;
  const uint8_t green = ((colour >> 5) & 0x3F) * 255 / 63;
  const uint8_t blue = (colour & 0x1F) * 255 / 31;
  return lv_color_make(red, green, blue);
}

void flushLvglDisplay(lv_disp_drv_t *display, const lv_area_t *area,
                      lv_color_t *pixels) {
  const uint32_t width = area->x2 - area->x1 + 1;
  const uint32_t height = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, width, height);
  tft.pushColors(reinterpret_cast<uint16_t *>(&pixels->full), width * height,
                 true);
  tft.endWrite();
  lv_disp_flush_ready(display);
}

void renderNow() {
  lv_timer_handler();
  lv_refr_now(nullptr);
}

void stylePanel(lv_obj_t *object, lv_color_t colour, lv_coord_t radius = 0) {
  lv_obj_remove_style_all(object);
  lv_obj_set_style_bg_color(object, colour, 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(object, radius, 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *makePanel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                    lv_coord_t width, lv_coord_t height, lv_color_t colour,
                    lv_coord_t radius = 0) {
  lv_obj_t *panel = lv_obj_create(parent);
  stylePanel(panel, colour, radius);
  lv_obj_set_pos(panel, x, y);
  lv_obj_set_size(panel, width, height);
  return panel;
}

lv_obj_t *makeLabel(lv_obj_t *parent, const String &text,
                    const lv_font_t *font, lv_color_t colour, lv_coord_t x,
                    lv_coord_t y, lv_coord_t width, lv_coord_t height,
                    lv_text_align_t alignment = LV_TEXT_ALIGN_LEFT) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_size(label, width, height);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, colour, 0);
  lv_obj_set_style_text_align(label, alignment, 0);
  lv_obj_set_style_pad_all(label, 0, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_label_set_text(label, text.c_str());
  return label;
}

void resetScreen() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x030811), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  headerOriginLabel = nullptr;
  headerClockLabel = nullptr;
  headerDestinationLabel = nullptr;
  headerConnectionLabel = nullptr;
  footerButtonLabels[0] = nullptr;
  footerButtonLabels[1] = nullptr;
  footerButtonLabels[2] = nullptr;
  footerWebLabel = nullptr;
}

void createChrome() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_t *header = makePanel(screen, 0, 0, kScreenWidth, kHeaderHeight,
                               lv_color_hex(0x061827));
  headerOriginLabel = makeLabel(header, "", &lv_font_montserrat_12,
                                lv_color_hex(0xF3F8FC), 4, 0, 86, 14);
  headerClockLabel = makeLabel(header, "", &lv_font_montserrat_12,
                               lv_color_hex(0xF3F8FC), 91, 0, 33, 14,
                               LV_TEXT_ALIGN_RIGHT);
  headerDestinationLabel = makeLabel(
      header, "", &lv_font_montserrat_12, lv_color_hex(0x54D7FF), 4, 13, 88,
      14);
  headerConnectionLabel = makeLabel(
      header, "", &lv_font_montserrat_12, lv_color_hex(0x66F2A3), 95, 13,
      29, 14, LV_TEXT_ALIGN_RIGHT);
  makePanel(header, 0, 26, kScreenWidth, 2, lv_color_hex(0x087CA4));

  lv_obj_t *footer = makePanel(screen, 0, kFooterY, kScreenWidth,
                               kFooterHeight, lv_color_hex(0x061827));
  footerButtonLabels[0] = makeLabel(
      footer, "B0 STN", &lv_font_montserrat_12, lv_color_hex(0x8FA7BA), 0, 1,
      42, 14, LV_TEXT_ALIGN_CENTER);
  footerButtonLabels[1] = makeLabel(
      footer, "B1 DIR", &lv_font_montserrat_12, lv_color_hex(0x8FA7BA), 43,
      1, 42, 14, LV_TEXT_ALIGN_CENTER);
  footerButtonLabels[2] = makeLabel(
      footer, "B2 REF", &lv_font_montserrat_12, lv_color_hex(0x8FA7BA), 86,
      1, 42, 14, LV_TEXT_ALIGN_CENTER);
  footerWebLabel = makeLabel(footer, "", &lv_font_montserrat_12,
                             lv_color_hex(0x54D7FF), 2, 1, 124, 14,
                             LV_TEXT_ALIGN_CENTER);
}

void initialiseLvglDisplay() {
  tft.begin();
  tft.invertDisplay(false);
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);

  lv_init();
  lv_disp_draw_buf_init(&lvDrawBuffer, lvPixelBuffer, nullptr,
                        kScreenWidth * 12);
  lv_disp_drv_init(&lvDisplayDriver);
  lvDisplayDriver.hor_res = kScreenWidth;
  lvDisplayDriver.ver_res = kScreenHeight;
  lvDisplayDriver.flush_cb = flushLvglDisplay;
  lvDisplayDriver.draw_buf = &lvDrawBuffer;
  lv_disp_drv_register(&lvDisplayDriver);
  resetScreen();
  Serial.printf("DISPLAY_ENGINE LVGL %u.%u.%u\n", lv_version_major(),
                lv_version_minor(), lv_version_patch());
}

uint16_t statusColour(const Departure &departure) {
  if (departure.cancelled) {
    return TFT_RED;
  }
  if (departure.expected == "On time") {
    return TFT_GREEN;
  }
  if (departure.expected == "Delayed") {
    return TFT_ORANGE;
  }
  return TFT_YELLOW;
}

String shortStatus(const Departure &departure) {
  if (departure.cancelled) {
    return "CANCELLED";
  }
  if (departure.expected == "On time") {
    return "ON TIME";
  }
  if (departure.expected == "Delayed") {
    return "DELAYED";
  }
  if (departure.expected.length() == 5) {
    return "EXP " + departure.expected;
  }
  return departure.expected.substring(0, 10);
}

String routeDisplayName(const String &code, const String &label) {
  if (code == "MAL") {
    return "Malden Manor";
  }
  if (code == "TOL") {
    return "Tolworth";
  }
  if (code == "WAT") {
    return "Waterloo";
  }
  if (code == "CSS") {
    return "Chessington South";
  }
  return label.length() > 0 ? label : code;
}

bool readLocalTime(struct tm &timeInfo) {
  return getLocalTime(&timeInfo, 10) && timeInfo.tm_year >= 120;
}

String currentTimeText() {
  struct tm timeInfo;
  if (!readLocalTime(timeInfo)) {
    return "--:--";
  }

  char text[6];
  snprintf(text, sizeof(text), "%02d:%02d", timeInfo.tm_hour,
           timeInfo.tm_min);
  return text;
}

bool parseServiceTime(const String &value, int &minutesAfterMidnight) {
  if (value.length() != 5 || value.charAt(2) != ':' ||
      value.charAt(0) < '0' || value.charAt(0) > '9' ||
      value.charAt(1) < '0' || value.charAt(1) > '9' ||
      value.charAt(3) < '0' || value.charAt(3) > '9' ||
      value.charAt(4) < '0' || value.charAt(4) > '9') {
    return false;
  }

  const int hour = value.substring(0, 2).toInt();
  const int minute = value.substring(3, 5).toInt();
  if (hour > 23 || minute > 59) {
    return false;
  }
  minutesAfterMidnight = hour * 60 + minute;
  return true;
}

int minutesUntilDeparture(const Departure &departure) {
  if (departure.cancelled) {
    return -1;
  }

  struct tm timeInfo;
  if (!readLocalTime(timeInfo)) {
    return -1;
  }

  int departureMinutes = 0;
  const String effectiveTime =
      departure.expected.length() == 5 ? departure.expected
                                       : departure.scheduled;
  if (!parseServiceTime(effectiveTime, departureMinutes)) {
    return -1;
  }

  const int currentMinutes = timeInfo.tm_hour * 60 + timeInfo.tm_min;
  int difference = departureMinutes - currentMinutes;
  if (difference < -720) {
    difference += 1440;
  } else if (difference < 0) {
    difference = 0;
  }
  return difference;
}

String countdownText(int minutes) {
  if (minutes < 0) {
    return "--";
  }
  if (minutes == 0) {
    return "DUE";
  }
  if (minutes > 99) {
    return "99m+";
  }
  return String(minutes) + "m";
}

uint16_t countdownColour(int minutes) {
  if (minutes < 0) {
    return TFT_DARKGREY;
  }
  if (minutes <= 2) {
    return TFT_RED;
  }
  if (minutes <= 5) {
    return TFT_ORANGE;
  }
  return TFT_CYAN;
}

void configureClock() {
  if (clockConfigured) {
    return;
  }
  configTzTime(kLondonTimeZone, "pool.ntp.org", "time.cloudflare.com");
  clockConfigured = true;
  Serial.println("CLOCK_CONFIGURED timezone=Europe/London");
}

void drawHeader() {
  if (headerOriginLabel == nullptr) {
    createChrome();
  }

  const String originName =
      routeDisplayName(kStations[stationIndex].code,
                       kStations[stationIndex].label);
  const String destinationName =
      routeDisplayName(kDirections[directionIndex].code,
                       kDirections[directionIndex].label);

  lv_label_set_text(headerOriginLabel, originName.c_str());
  lv_label_set_text(headerClockLabel, currentTimeText().c_str());
  lv_label_set_text(headerDestinationLabel,
                    ("to " + destinationName).c_str());
  const bool connected = WiFi.status() == WL_CONNECTED;
  lv_label_set_text(headerConnectionLabel, connected ? "LIVE" : "OFF");
  lv_obj_set_style_text_color(headerConnectionLabel,
                              connected ? lv_color_hex(0x66F2A3)
                                        : lv_color_hex(0xFF6174),
                              0);
  renderNow();
}

void drawFooter() {
  if (footerWebLabel == nullptr) {
    createChrome();
  }
  lastFooterPage = (millis() / kFooterPageIntervalMs) % 2;

  if (lastFooterPage == 0) {
    for (lv_obj_t *label : footerButtonLabels) {
      lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(footerWebLabel, LV_OBJ_FLAG_HIDDEN);
  } else {
    for (lv_obj_t *label : footerButtonLabels) {
      lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    const String webAddress = deviceHostname.length() > 0
                                  ? deviceHostname + ".local"
                                  : "TwinTrack / LVGL";
    lv_label_set_text(footerWebLabel, webAddress.c_str());
    lv_obj_clear_flag(footerWebLabel, LV_OBJ_FLAG_HIDDEN);
  }
  renderNow();
}

void drawMessage(const String &message, uint16_t colour = TFT_WHITE) {
  resetScreen();
  createChrome();
  drawHeader();
  lv_obj_t *content = makePanel(lv_scr_act(), 0, kHeaderHeight, kScreenWidth,
                                kFooterY - kHeaderHeight,
                                lv_color_hex(0x07121E));
  makePanel(content, 45, 22, 38, 3, colourFromRgb565(colour), 2);
  makeLabel(content, message, &lv_font_montserrat_16,
            colourFromRgb565(colour), 4, 32, 120, 20,
            LV_TEXT_ALIGN_CENTER);
  makeLabel(content, "LVGL EDITION", &lv_font_montserrat_12,
            lv_color_hex(0x668096), 4, 55, 120, 16,
            LV_TEXT_ALIGN_CENTER);
  drawFooter();
  renderNow();
}

void drawSetupScreen() {
  resetScreen();
  makePanel(lv_scr_act(), 0, 0, kScreenWidth, 4, lv_color_hex(0x54D7FF));
  makeLabel(lv_scr_act(), "WIFI SETUP", &lv_font_montserrat_16,
            lv_color_hex(0xF3F8FC), 4, 5, 120, 20,
            LV_TEXT_ALIGN_CENTER);
  lv_obj_t *card = makePanel(lv_scr_act(), 6, 25, 116, 86,
                             lv_color_hex(0x0C2031), 8);
  makeLabel(card, "JOIN NETWORK", &lv_font_montserrat_12,
            lv_color_hex(0x668096), 8, 5, 100, 14);
  makeLabel(card, setupApSsid, &lv_font_montserrat_14,
            lv_color_hex(0x54D7FF), 8, 18, 100, 17);
  makeLabel(card, "PASSWORD", &lv_font_montserrat_12,
            lv_color_hex(0x668096), 8, 38, 100, 14);
  makeLabel(card, setupApPassword, &lv_font_montserrat_14,
            lv_color_hex(0x66F2A3), 8, 51, 100, 17);
  makeLabel(card, "OPEN  192.168.4.1", &lv_font_montserrat_12,
            lv_color_hex(0xF3F8FC), 8, 70, 100, 14);
  makeLabel(lv_scr_act(), "B2 at boot resets Wi-Fi", &lv_font_montserrat_12,
            lv_color_hex(0x52687A), 2, 114, 124, 14,
            LV_TEXT_ALIGN_CENTER);
  renderNow();
}

void drawFullScreenNotice(const String &title, const String &subtitle,
                          lv_color_t accent) {
  resetScreen();
  makePanel(lv_scr_act(), 0, 0, kScreenWidth, 5, accent);
  lv_obj_t *orb = makePanel(lv_scr_act(), 46, 23, 36, 36,
                            lv_color_hex(0x0C2031), 18);
  makePanel(orb, 13, 13, 10, 10, accent, 5);
  makeLabel(lv_scr_act(), title, &lv_font_montserrat_16, accent, 4, 66, 120,
            20, LV_TEXT_ALIGN_CENTER);
  makeLabel(lv_scr_act(), subtitle, &lv_font_montserrat_12,
            lv_color_hex(0xA8BBC9), 4, 90, 120, 18,
            LV_TEXT_ALIGN_CENTER);
  renderNow();
}

void drawCredentialTestScreen() {
  drawFullScreenNotice("TESTING WIFI", "Keep setup open",
                       lv_color_hex(0x54D7FF));
}

void drawCredentialSavedScreen() {
  drawFullScreenNotice("WIFI SAVED", "Restarting TwinTrack",
                       lv_color_hex(0x66F2A3));
}

String portalPage(const String &heading, const String &message, bool showForm,
                  bool autoRefresh) {
  String page;
  page.reserve(2200);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (autoRefresh) {
    page += F("<meta http-equiv='refresh' content='3;url=/status'>");
  }
  page += F("<title>TwinTrack setup</title><style>");
  page += F("body{font-family:system-ui,sans-serif;background:#07111f;color:#eef6ff;");
  page += F("margin:0;padding:24px}main{max-width:420px;margin:auto;background:#10243a;");
  page += F("padding:24px;border-radius:16px}h1{color:#58d8ff;margin-top:0}");
  page += F("label{display:block;margin-top:16px}input{box-sizing:border-box;width:100%;");
  page += F("padding:12px;margin-top:6px;border:1px solid #54708c;border-radius:8px;");
  page += F("font-size:16px}button{width:100%;margin-top:20px;padding:13px;");
  page += F("border:0;border-radius:8px;background:#21c77a;color:#04130b;font-weight:700}");
  page += F("small{color:#adc1d4}</style></head><body><main><h1>");
  page += heading;
  page += F("</h1><p>");
  page += message;
  page += F("</p>");
  if (showForm) {
    page += F("<form action='/save' method='post'>");
    page += F("<label>Home Wi-Fi name<input name='ssid' maxlength='32' required ");
    page += F("autocapitalize='none' autocomplete='off'></label>");
    page += F("<label>Wi-Fi password<input name='password' type='password' ");
    page += F("maxlength='63' autocomplete='new-password'></label>");
    page += F("<button type='submit'>Connect and save</button></form>");
    page += F("<p><small>The password is tested before it is stored on the device.</small></p>");
  }
  page += F("</main></body></html>");
  return page;
}

void sendPortalPage(const String &heading, const String &message, bool showForm,
                    bool autoRefresh = false) {
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "text/html",
                 portalPage(heading, message, showForm, autoRefresh));
}

void redirectToPortal() {
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.send(302, "text/plain", "");
}

bool loadCredentials() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) {
    return false;
  }
  savedWifiSsid = preferences.getString(kSsidKey, "");
  savedWifiPassword = preferences.getString(kPasswordKey, "");
  preferences.end();
  return savedWifiSsid.length() > 0;
}

bool saveCredentials() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    return false;
  }
  const size_t ssidBytes = preferences.putString(kSsidKey, pendingWifiSsid);
  const size_t passwordBytes =
      preferences.putString(kPasswordKey, pendingWifiPassword);
  preferences.end();

  if (ssidBytes == 0 ||
      (pendingWifiPassword.length() > 0 && passwordBytes == 0)) {
    return false;
  }

  savedWifiSsid = pendingWifiSsid;
  savedWifiPassword = pendingWifiPassword;
  return true;
}

void clearCredentials() {
  Preferences preferences;
  if (preferences.begin(kPreferencesNamespace, false)) {
    preferences.remove(kSsidKey);
    preferences.remove(kPasswordKey);
    preferences.end();
  }
  savedWifiSsid = "";
  savedWifiPassword = "";
  Serial.println("WIFI_SETTINGS_CLEARED");
}

bool normaliseCrsCode(String &code) {
  code.trim();
  code.toUpperCase();
  if (code.length() != 3) {
    return false;
  }
  for (size_t i = 0; i < code.length(); ++i) {
    const char value = code.charAt(i);
    if (!((value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9'))) {
      return false;
    }
  }
  return true;
}

void loadTrainSettings() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) {
    return;
  }

  for (uint8_t i = 0; i < 2; ++i) {
    String stationCode =
        preferences.getString(kStationKeys[i], kStations[i].code);
    String directionCode =
        preferences.getString(kDirectionKeys[i], kDirections[i].code);
    if (normaliseCrsCode(stationCode)) {
      kStations[i].code = stationCode;
      kStations[i].label = stationCode;
    }
    if (normaliseCrsCode(directionCode)) {
      kDirections[i].code = directionCode;
      kDirections[i].label = directionCode;
    }
  }
  preferences.end();
}

bool saveTrainSettings(const String stationCodes[2],
                       const String directionCodes[2]) {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    return false;
  }

  bool saved = true;
  for (uint8_t i = 0; i < 2; ++i) {
    saved = preferences.putString(kStationKeys[i], stationCodes[i]) > 0 &&
            saved;
    saved =
        preferences.putString(kDirectionKeys[i], directionCodes[i]) > 0 &&
        saved;
  }
  preferences.end();

  if (!saved) {
    return false;
  }

  for (uint8_t i = 0; i < 2; ++i) {
    kStations[i].code = stationCodes[i];
    kStations[i].label = stationCodes[i];
    kDirections[i].code = directionCodes[i];
    kDirections[i].label = directionCodes[i];
  }
  return true;
}

String settingsPage(const String &message) {
  String page;
  page.reserve(3200);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>TwinTrack</title><style>");
  page += F("body{font-family:system-ui,sans-serif;background:#07111f;color:#eef6ff;");
  page += F("margin:0;padding:24px}main{max-width:560px;margin:auto}section{background:#10243a;");
  page += F("padding:22px;border-radius:16px;margin-bottom:16px}h1{color:#58d8ff}");
  page += F("h2{font-size:18px;margin-top:0}label{display:block;margin-top:14px}");
  page += F("input{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;");
  page += F("border:1px solid #54708c;border-radius:8px;font-size:18px;text-transform:uppercase}");
  page += F("button{width:100%;margin-top:20px;padding:13px;border:0;border-radius:8px;");
  page += F("background:#21c77a;color:#04130b;font-weight:700}.notice{color:#75efa8}");
  page += F("small{color:#adc1d4}code{color:#ffd55e}</style></head><body><main>");
  page += F("<h1>TwinTrack</h1><p>Live departure board settings</p>");
  if (message.length() > 0) {
    page += F("<section class='notice'>");
    page += message;
    page += F("</section>");
  }
  page += F("<form action='/settings' method='post'><section><h2>Origin stations</h2>");
  page += F("<label>Station button position 1<input name='station0' maxlength='3' value='");
  page += kStations[0].code;
  page += F("' required></label><label>Station button position 2<input name='station1' maxlength='3' value='");
  page += kStations[1].code;
  page += F("' required></label></section><section><h2>Destination filters</h2>");
  page += F("<label>Direction button position 1<input name='direction0' maxlength='3' value='");
  page += kDirections[0].code;
  page += F("' required></label><label>Direction button position 2<input name='direction1' maxlength='3' value='");
  page += kDirections[1].code;
  page += F("' required></label><p><small>Enter three-character National Rail CRS codes, such as <code>MAL</code>, <code>TOL</code>, <code>WAT</code>, or <code>CSS</code>.</small></p>");
  page += F("<button type='submit'>Save and refresh display</button></section></form>");
  page += F("<section><small>Device: http://");
  page += deviceHostname;
  page += F(".local/ &middot; IP: ");
  page += WiFi.localIP().toString();
  page += F("</small></section></main></body></html>");
  return page;
}

void sendSettingsPage(const String &message = "") {
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "text/html", settingsPage(message));
}

bool credentialResetRequested() {
  if (digitalRead(kButtonRefresh) != LOW) {
    return false;
  }

  drawFullScreenNotice("HOLD B2", "Reset saved Wi-Fi",
                       lv_color_hex(0xFFB85A));

  const uint32_t startedAt = millis();
  while (digitalRead(kButtonRefresh) == LOW &&
         millis() - startedAt < kCredentialResetHoldMs) {
    lv_timer_handler();
    delay(20);
  }

  const bool requested = millis() - startedAt >= kCredentialResetHoldMs;
  while (digitalRead(kButtonRefresh) == LOW) {
    delay(20);
  }
  return requested;
}

void generateSetupNetworkDetails() {
  char ssid[20];
  const uint16_t deviceSuffix =
      static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFF);
  snprintf(ssid, sizeof(ssid), "TwinTrack-%04X", deviceSuffix);
  setupApSsid = ssid;
  deviceHostname = "twintrack";

  static const char kPasswordAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  setupApPassword = "";
  for (uint8_t i = 0; i < 8; ++i) {
    const uint32_t value = esp_random();
    setupApPassword +=
        kPasswordAlphabet[value % (sizeof(kPasswordAlphabet) - 1)];
  }
}

void drawDepartures() {
  resetScreen();
  createChrome();
  drawHeader();
  drawFooter();

  if (departureCount == 0) {
    makePanel(lv_scr_act(), 47, 48, 34, 3, lv_color_hex(0xFFCF5A), 2);
    makeLabel(lv_scr_act(), statusMessage, &lv_font_montserrat_16,
              lv_color_hex(0xFFCF5A), 4, 59, 120, 22,
              LV_TEXT_ALIGN_CENTER);
    makeLabel(lv_scr_act(), "Waiting for departures",
              &lv_font_montserrat_12, lv_color_hex(0x668096), 4, 82, 120,
              16, LV_TEXT_ALIGN_CENTER);
    renderNow();
    return;
  }

  for (size_t i = 0; i < departureCount; ++i) {
    const int y = kHeaderHeight + static_cast<int>(i) * 28;
    const Departure &departure = departures[i];
    const int minutes = minutesUntilDeparture(departure);
    const uint16_t urgencyColour = countdownColour(minutes);
    const String urgencyText =
        departure.cancelled ? "CXL" : countdownText(minutes);
    const lv_color_t urgency = colourFromRgb565(
        departure.cancelled ? TFT_RED : urgencyColour);
    lv_obj_t *row = makePanel(lv_scr_act(), 0, y, kScreenWidth, 28,
                              i % 2 == 0 ? lv_color_hex(0x071521)
                                         : lv_color_hex(0x0A1B29));
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x163244), 0);
    makePanel(row, 0, 0, 3, 28, urgency);
    makeLabel(row, departure.scheduled, &lv_font_montserrat_16,
              lv_color_hex(0xF3F8FC), 7, 0, 58, 18);
    makeLabel(row, urgencyText, &lv_font_montserrat_16, urgency, 67, 0, 57,
              18, LV_TEXT_ALIGN_RIGHT);

    const uint16_t serviceColour = statusColour(departure);
    const lv_color_t service = colourFromRgb565(serviceColour);
    makePanel(row, 7, 20, 5, 5, service, 3);
    makeLabel(row, shortStatus(departure), &lv_font_montserrat_12, service, 16,
              15, 82, 14);

    const String platform = departure.platform.length() > 0
                                ? "P" + departure.platform
                                : "P-";
    lv_obj_t *platformPill = makePanel(row, 101, 15, 25, 12,
                                       lv_color_hex(0x073C50), 6);
    makeLabel(platformPill, platform, &lv_font_montserrat_12,
              lv_color_hex(0xDFF8FF), 1, 0, 23, 12,
              LV_TEXT_ALIGN_CENTER);
  }

  renderNow();
}

bool connectStoredWifi() {
  if (savedWifiSsid.length() == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
  statusMessage = "WIFI...";
  drawMessage(statusMessage, TFT_CYAN);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
    digitalWrite(kStatusLed, !digitalRead(kStatusLed));
    delay(250);
  }

  digitalWrite(kStatusLed, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
  if (WiFi.status() == WL_CONNECTED) {
    statusMessage = "CONNECTED";
    Serial.println("WIFI_CONNECTED");
    refreshRequested = true;
    wifiDisconnectedAt = 0;
    return true;
  } else {
    statusMessage = "WIFI SETUP";
    Serial.println("WIFI_FAILED");
    return false;
  }
}

void beginCredentialAttempt() {
  retrySavedWifiInProvisioning = false;
  provisioningState = ProvisioningState::kTesting;
  credentialAttempt = 1;
  credentialAttemptStartedAt = millis();
  drawCredentialTestScreen();
  Serial.println("PROVISIONING_TEST_BEGIN attempt=1");

  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(pendingWifiSsid.c_str(), pendingWifiPassword.c_str());
}

void resumeSavedWifiRetries() {
  retrySavedWifiInProvisioning = savedWifiSsid.length() > 0;
  if (!retrySavedWifiInProvisioning) {
    return;
  }

  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
  lastProvisioningWifiRetryAt = millis();
  Serial.println("PROVISIONING_SAVED_WIFI_RETRY");
}

void resumeDeparturesFromProvisioning() {
  dnsServer.stop();
  webServer.stop();
  WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);

  provisioningMode = false;
  provisioningState = ProvisioningState::kIdle;
  retrySavedWifiInProvisioning = false;
  setupApPassword = "";
  statusMessage = "CONNECTED";
  refreshRequested = true;
  lastRefreshAt = 0;
  wifiDisconnectedAt = 0;
  digitalWrite(kStatusLed, HIGH);
  drawMessage("RECONNECTED", TFT_GREEN);
  startLanWebUi();
  Serial.println("PROVISIONING_AUTO_RECOVERED");
}

void configureWebHandlers() {
  if (webHandlersConfigured) {
    return;
  }
  webHandlersConfigured = true;

  webServer.on("/", HTTP_GET, []() {
    if (provisioningMode) {
      sendPortalPage("TwinTrack Wi-Fi",
                     "Enter the network this departure board should use.",
                     true);
    } else {
      sendSettingsPage();
    }
  });

  webServer.on("/save", HTTP_POST, []() {
    if (!provisioningMode) {
      webServer.send(404, "text/plain", "Not found");
      return;
    }
    if (provisioningState == ProvisioningState::kTesting) {
      sendPortalPage("Testing Wi-Fi",
                     "The device is already checking a network.", false, true);
      return;
    }

    const String candidateSsid = webServer.arg("ssid");
    const String candidatePassword = webServer.arg("password");
    if (candidateSsid.length() == 0 || candidateSsid.length() > 32 ||
        candidatePassword.length() > 63) {
      sendPortalPage("Check the details",
                     "The Wi-Fi name or password has an invalid length.", true);
      return;
    }

    pendingWifiSsid = candidateSsid;
    pendingWifiPassword = candidatePassword;
    beginCredentialAttempt();
    sendPortalPage("Testing Wi-Fi",
                   "TwinTrack is connecting. This can take up to one minute.",
                   false, true);
  });

  webServer.on("/status", HTTP_GET, []() {
    if (!provisioningMode) {
      webServer.sendHeader("Location", "/", true);
      webServer.send(302, "text/plain", "");
      return;
    }
    if (provisioningState == ProvisioningState::kTesting) {
      sendPortalPage("Testing Wi-Fi", "Still checking the connection.", false,
                     true);
    } else if (provisioningState == ProvisioningState::kSaved) {
      sendPortalPage("Connected", "Wi-Fi saved. TwinTrack is restarting.",
                     false);
    } else if (provisioningState == ProvisioningState::kFailed) {
      sendPortalPage("Could not connect",
                     "Check the network name and password, then try again.",
                     true);
    } else {
      redirectToPortal();
    }
  });

  webServer.on("/settings", HTTP_GET, []() {
    if (provisioningMode) {
      redirectToPortal();
    } else {
      sendSettingsPage();
    }
  });

  webServer.on("/settings", HTTP_POST, []() {
    if (provisioningMode) {
      redirectToPortal();
      return;
    }

    String stationCodes[] = {webServer.arg("station0"),
                             webServer.arg("station1")};
    String directionCodes[] = {webServer.arg("direction0"),
                               webServer.arg("direction1")};
    for (uint8_t i = 0; i < 2; ++i) {
      if (!normaliseCrsCode(stationCodes[i]) ||
          !normaliseCrsCode(directionCodes[i])) {
        sendSettingsPage(
            "Each station and destination must be a three-character CRS code.");
        return;
      }
    }

    if (!saveTrainSettings(stationCodes, directionCodes)) {
      sendSettingsPage("The settings could not be stored. Please try again.");
      return;
    }

    stationIndex = 0;
    directionIndex = 0;
    departureCount = 0;
    refreshRequested = true;
    lastRefreshAt = 0;
    Serial.println("TRAIN_SETTINGS_SAVED");
    sendSettingsPage("Settings saved. The display is refreshing now.");
  });

  webServer.onNotFound([]() {
    if (provisioningMode) {
      redirectToPortal();
    } else {
      webServer.send(404, "text/plain", "Not found");
    }
  });
}

void startLanWebUi() {
  if (lanWebUiRunning || WiFi.status() != WL_CONNECTED) {
    return;
  }

  configureWebHandlers();
  webServer.begin();
  lanWebUiRunning = true;
  configureClock();

  if (MDNS.begin(deviceHostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }
  Serial.printf("WEB_UI_STARTED host=%s.local ip=%s\n",
                deviceHostname.c_str(), WiFi.localIP().toString().c_str());
}

void startProvisioning() {
  if (provisioningMode) {
    return;
  }

  provisioningMode = true;
  provisioningState = ProvisioningState::kIdle;
  pendingWifiSsid = "";
  pendingWifiPassword = "";
  departureCount = 0;

  if (lanWebUiRunning) {
    webServer.stop();
    MDNS.end();
    lanWebUiRunning = false;
  }

  WiFi.disconnect(true, true);
  delay(150);
  WiFi.mode(WIFI_AP_STA);
  generateSetupNetworkDetails();

  if (!WiFi.softAP(setupApSsid.c_str(), setupApPassword.c_str())) {
    Serial.println("PROVISIONING_AP_FAILED");
    drawMessage("AP ERROR", TFT_RED);
    return;
  }

  configureWebHandlers();
  dnsServer.start(53, "*", WiFi.softAPIP());
  webServer.begin();
  drawSetupScreen();
  Serial.printf("PROVISIONING_AP_STARTED ssid=%s\n", setupApSsid.c_str());
  resumeSavedWifiRetries();
}

void updateProvisioning() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  if (provisioningState == ProvisioningState::kTesting) {
    if (WiFi.status() == WL_CONNECTED) {
      if (saveCredentials()) {
        provisioningState = ProvisioningState::kSaved;
        pendingWifiPassword = "";
        drawCredentialSavedScreen();
        restartAt = millis() + 4000;
        Serial.println("PROVISIONING_SAVED");
      } else {
        provisioningState = ProvisioningState::kFailed;
        pendingWifiPassword = "";
        drawSetupScreen();
        Serial.println("PROVISIONING_SAVE_FAILED");
        resumeSavedWifiRetries();
      }
    } else if (millis() - credentialAttemptStartedAt >=
               kCredentialAttemptMs) {
      if (credentialAttempt < kCredentialAttempts) {
        ++credentialAttempt;
        credentialAttemptStartedAt = millis();
        WiFi.disconnect(false, false);
        delay(100);
        WiFi.begin(pendingWifiSsid.c_str(), pendingWifiPassword.c_str());
        Serial.printf("PROVISIONING_TEST_RETRY attempt=%u\n",
                      credentialAttempt);
      } else {
        provisioningState = ProvisioningState::kFailed;
        pendingWifiPassword = "";
        WiFi.disconnect(false, false);
        drawSetupScreen();
        Serial.println("PROVISIONING_TEST_FAILED");
        resumeSavedWifiRetries();
      }
    }
  }

  if (retrySavedWifiInProvisioning &&
      provisioningState != ProvisioningState::kTesting) {
    if (WiFi.status() == WL_CONNECTED) {
      resumeDeparturesFromProvisioning();
      return;
    }
    if (millis() - lastProvisioningWifiRetryAt >= kWifiRetryIntervalMs) {
      WiFi.disconnect(false, false);
      delay(100);
      WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
      lastProvisioningWifiRetryAt = millis();
      Serial.println("PROVISIONING_SAVED_WIFI_RETRY");
    }
  }

  if (restartAt > 0 && static_cast<int32_t>(millis() - restartAt) >= 0) {
    ESP.restart();
  }
}

bool fetchDepartures() {
  if (WiFi.status() != WL_CONNECTED) {
    statusMessage = "NO WIFI";
    Serial.println("FETCH_SKIPPED no_wifi");
    return false;
  }

  drawMessage("UPDATING", TFT_CYAN);

  const Station &station = kStations[stationIndex];
  const Direction &direction = kDirections[directionIndex];
  const String url = String(kApiBase) + "/" + station.code + "/to/" +
                     direction.code + "/4";

  Serial.printf("FETCH_BEGIN station=%s direction=%s\n", station.code.c_str(),
                direction.code.c_str());

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setUserAgent("TwinTrack/1.1");

  if (!http.begin(client, url)) {
    statusMessage = "API SETUP ERR";
    Serial.println("FETCH_FAILED setup");
    return false;
  }

  const int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    statusMessage = "HTTP " + String(responseCode);
    Serial.printf("FETCH_FAILED http=%d\n", responseCode);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  DynamicJsonDocument document(16384);
  const DeserializationError error = deserializeJson(document, payload);

  if (error) {
    statusMessage = "JSON ERROR";
    Serial.printf("FETCH_FAILED json=%s\n", error.c_str());
    return false;
  }

  departureCount = 0;
  const String stationName = document["locationName"] | "";
  if (stationName.length() > 0) {
    kStations[stationIndex].label = stationName;
  }
  const JsonArray services = document["trainServices"].as<JsonArray>();
  for (JsonObject service : services) {
    if (departureCount >= kMaxServices) {
      break;
    }

    Departure &departure = departures[departureCount++];
    departure.scheduled = service["std"] | "--:--";
    departure.expected = service["etd"] | "Unknown";
    departure.platform = service["platform"] | "";
    departure.cancelled = service["isCancelled"] | false;
    departure.destination =
        service["destination"][0]["locationName"] | "Unknown";
    if (departureCount == 1 && departure.destination != "Unknown") {
      kDirections[directionIndex].label = departure.destination;
    }
  }

  statusMessage = departureCount > 0 ? "LIVE" : "NO SERVICES";
  Serial.printf("FETCH_OK count=%u\n",
                static_cast<unsigned int>(departureCount));
  if (departureCount > 0) {
    const String originName =
        routeDisplayName(kStations[stationIndex].code,
                         kStations[stationIndex].label);
    const String destinationName =
        routeDisplayName(kDirections[directionIndex].code,
                         kDirections[directionIndex].label);
    const String localClock = currentTimeText();
    Serial.printf("DISPLAY_ROUTE origin=%s destination=%s\n",
                  originName.c_str(), destinationName.c_str());
    Serial.printf("DISPLAY_DATA local=%s first_minutes=%d\n",
                  localClock.c_str(), minutesUntilDeparture(departures[0]));
  }
  return true;
}

void setup() {
  Serial.begin(115200);

  pinMode(kStatusLed, OUTPUT);
  digitalWrite(kStatusLed, LOW);
  stationButton.begin();
  directionButton.begin();
  refreshButton.begin();

  initialiseLvglDisplay();
  drawMessage("TWINTRACK", TFT_CYAN);
  generateSetupNetworkDetails();
  loadTrainSettings();

  if (credentialResetRequested()) {
    clearCredentials();
  }

  const bool hasCredentials = loadCredentials();
  if (!hasCredentials || !connectStoredWifi()) {
    startProvisioning();
  } else {
    startLanWebUi();
  }
}

void loop() {
  lv_timer_handler();

  if (provisioningMode) {
    updateProvisioning();
    delay(5);
    return;
  }

  if (lanWebUiRunning) {
    webServer.handleClient();
  }

  if (millis() - lastClockCheckAt >= 500) {
    lastClockCheckAt = millis();
    struct tm timeInfo;
    if (readLocalTime(timeInfo)) {
      const int32_t minuteStamp = static_cast<int32_t>(time(nullptr) / 60);
      if (minuteStamp != lastDisplayedMinute) {
        if (lastDisplayedMinute < 0) {
          Serial.printf("CLOCK_SYNCED local=%02d:%02d\n", timeInfo.tm_hour,
                        timeInfo.tm_min);
        }
        lastDisplayedMinute = minuteStamp;
        if (departureCount > 0) {
          drawDepartures();
        } else {
          drawHeader();
        }
      }
    }
  }

  const uint8_t footerPage = (millis() / kFooterPageIntervalMs) % 2;
  if (footerPage != lastFooterPage) {
    drawFooter();
  }

  if (stationButton.pressed()) {
    stationIndex = (stationIndex + 1) % 2;
    refreshRequested = true;
    drawMessage(kStations[stationIndex].label, TFT_CYAN);
  }

  if (directionButton.pressed()) {
    directionIndex = (directionIndex + 1) % 2;
    refreshRequested = true;
    drawMessage(kDirections[directionIndex].label, TFT_CYAN);
  }

  if (refreshButton.pressed()) {
    refreshRequested = true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDisconnectedAt == 0) {
      wifiDisconnectedAt = millis();
    }
    if (millis() - lastWifiRetryAt >= kWifiRetryIntervalMs) {
      lastWifiRetryAt = millis();
      WiFi.reconnect();
      drawHeader();
    }
    if (millis() - wifiDisconnectedAt >= kWifiFallbackIntervalMs) {
      Serial.println("WIFI_FALLBACK_TO_SETUP");
      startProvisioning();
      return;
    }
  } else {
    wifiDisconnectedAt = 0;
  }

  if (refreshRequested || millis() - lastRefreshAt >= kRefreshIntervalMs) {
    refreshRequested = false;
    lastRefreshAt = millis();
    fetchDepartures();
    drawDepartures();
  }

  delay(5);
}
