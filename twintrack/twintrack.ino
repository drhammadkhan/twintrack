#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
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
String wifiScanResponse;
bool wifiScanRunning = false;
ProvisioningState provisioningState = ProvisioningState::kIdle;

void configureWebHandlers();
void startLanWebUi();
void startWifiNetworkScan();
void updateWifiNetworkScan();

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

String fitText(String text, int16_t maxWidth, uint8_t font) {
  if (tft.textWidth(text, font) <= maxWidth) {
    return text;
  }

  while (text.length() > 1 &&
         tft.textWidth(text + "..", font) > maxWidth) {
    text.remove(text.length() - 1);
  }
  return text + "..";
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
  const uint16_t headerBackground = tft.color565(4, 15, 31);
  const uint16_t connectionColour =
      WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED;
  const String originName =
      routeDisplayName(kStations[stationIndex].code,
                       kStations[stationIndex].label);
  const String destinationName =
      routeDisplayName(kDirections[directionIndex].code,
                       kDirections[directionIndex].label);

  tft.fillRect(0, 0, 128, 20, headerBackground);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, headerBackground);
  tft.drawString(fitText(originName, 89, 1), 3, 1, 1);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, headerBackground);
  tft.drawString(currentTimeText(), 126, 1, 1);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, headerBackground);
  tft.drawString(fitText("to " + destinationName, 96, 1), 3, 10, 1);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(connectionColour, headerBackground);
  tft.drawString(WiFi.status() == WL_CONNECTED ? "LIVE" : "OFF", 126, 10,
                 1);
  tft.fillRect(0, 18, 128, 2, tft.color565(0, 92, 122));
}

void drawFooter() {
  const uint16_t footerBackground = tft.color565(4, 15, 31);
  tft.fillRect(0, 108, 128, 20, footerBackground);
  tft.setTextDatum(TC_DATUM);
  lastFooterPage = (millis() / kFooterPageIntervalMs) % 2;

  if (lastFooterPage == 0) {
    tft.setTextColor(TFT_LIGHTGREY, footerBackground);
    tft.drawString("B0", 21, 108, 1);
    tft.drawString("B1", 64, 108, 1);
    tft.drawString("B2", 107, 108, 1);
    tft.setTextColor(TFT_CYAN, footerBackground);
    tft.drawString("STN", 21, 118, 1);
    tft.drawString("DIR", 64, 118, 1);
    tft.drawString("REF", 107, 118, 1);
  } else if (deviceHostname.length() > 0) {
    tft.setTextColor(TFT_CYAN, footerBackground);
    tft.drawString("WEB UI", 64, 108, 1);
    tft.setTextColor(TFT_LIGHTGREY, footerBackground);
    tft.drawString(deviceHostname + ".local", 64, 118, 1);
  }
}

void drawMessage(const String &message, uint16_t colour = TFT_WHITE) {
  drawHeader();
  tft.fillRect(0, 20, 128, 88, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colour, TFT_BLACK);
  tft.drawString(fitText(message, 124, 2), 64, 61, 2);
  drawFooter();
}

void drawSetupScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("WIFI SETUP", 64, 2, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("JOIN", 64, 23, 1);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(setupApSsid, 64, 34, 1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("PASSWORD", 64, 50, 1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(setupApPassword, 64, 61, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("OPEN", 64, 82, 1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("192.168.4.1", 64, 94, 1);

  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("B2 HOLD AT BOOT: RESET", 64, 116, 1);
}

void drawCredentialTestScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("TESTING WIFI", 64, 50, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("KEEP SETUP OPEN", 64, 77, 1);
}

void drawCredentialSavedScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("WIFI SAVED", 64, 50, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("RESTARTING", 64, 77, 1);
}

String portalPage(const String &heading, const String &message, bool showForm,
                  bool autoRefresh) {
  String page;
  page.reserve(5200);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (autoRefresh) {
    page += F("<meta http-equiv='refresh' content='3;url=/status'>");
  }
  page += F("<title>TwinTrack setup</title><style>");
  page += F("body{font-family:system-ui,sans-serif;background:#07111f;color:#eef6ff;");
  page += F("margin:0;padding:24px}main{max-width:420px;margin:auto;background:#10243a;");
  page += F("padding:24px;border-radius:16px}h1{color:#58d8ff;margin-top:0}");
  page += F("label{display:block;margin-top:16px}input,select{box-sizing:border-box;width:100%;");
  page += F("padding:12px;margin-top:6px;border:1px solid #54708c;border-radius:8px;");
  page += F("font-size:16px}button{width:100%;margin-top:20px;padding:13px;");
  page += F("border:0;border-radius:8px;background:#21c77a;color:#04130b;font-weight:700}");
  page += F(".secondary{background:#314b64;color:#eef6ff;margin-top:10px}");
  page += F("small{display:block;color:#adc1d4;margin-top:10px}</style></head><body><main><h1>");
  page += heading;
  page += F("</h1><p>");
  page += message;
  page += F("</p>");
  if (showForm) {
    page += F("<form action='/save' method='post'>");
    page += F("<label>Nearby networks<select id='networks' disabled>");
    page += F("<option>Searching...</option></select></label>");
    page += F("<button id='rescan' class='secondary' type='button'>Scan again</button>");
    page += F("<small id='scan-status'>Searching for nearby networks...</small>");
    page += F("<label>Network name<input id='ssid' name='ssid' maxlength='32' required ");
    page += F("autocapitalize='none' autocomplete='off' placeholder='Or enter a hidden network'></label>");
    page += F("<label>Wi-Fi password<input name='password' type='password' ");
    page += F("maxlength='63' autocomplete='new-password'></label>");
    page += F("<button type='submit'>Connect and save</button></form>");
    page += F("<p><small>The password is tested before it is stored on the device.</small></p>");
    page += F("<script>(()=>{const list=document.getElementById('networks'),");
    page += F("ssid=document.getElementById('ssid'),status=document.getElementById('scan-status');");
    page += F("const quality=r=>r>=-55?'excellent':r>=-67?'good':r>=-75?'fair':'weak';");
    page += F("async function scan(refresh=false){status.textContent=refresh?'Scanning again...':'Searching for nearby networks...';");
    page += F("try{const response=await fetch('/networks'+(refresh?'?refresh=1':''),{cache:'no-store'});");
    page += F("const data=await response.json();if(data.scanning){setTimeout(()=>scan(false),1200);return;}");
    page += F("list.innerHTML='';const networks=data.networks||[];if(!networks.length){");
    page += F("const option=document.createElement('option');option.textContent='No networks found';");
    page += F("option.value='';list.appendChild(option);list.disabled=true;");
    page += F("status.textContent=data.error||'No visible networks found. Enter a network name below.';return;}");
    page += F("for(const network of networks){const option=document.createElement('option');");
    page += F("option.value=network.ssid;option.textContent=network.ssid+' · '+quality(network.rssi)");
    page += F("+' · '+(network.secure?'secured':'open');list.appendChild(option);}");
    page += F("list.disabled=false;if(!ssid.value){ssid.value=networks[0].ssid;}");
    page += F("status.textContent=networks.length+' network'+(networks.length===1?'':'s')+' found.';");
    page += F("}catch(error){status.textContent='Scan unavailable. Enter the network name below.';}}");
    page += F("list.addEventListener('change',()=>{if(list.value)ssid.value=list.value;});");
    page += F("document.getElementById('rescan').addEventListener('click',()=>scan(true));");
    page += F("scan(false);})();</script>");
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

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t character = static_cast<uint8_t>(value.charAt(i));
    if (character == '"' || character == '\\') {
      escaped += '\\';
      escaped += static_cast<char>(character);
    } else if (character < 0x20) {
      char unicodeEscape[7];
      snprintf(unicodeEscape, sizeof(unicodeEscape), "\\u%04X", character);
      escaped += unicodeEscape;
    } else {
      escaped += static_cast<char>(character);
    }
  }
  return escaped;
}

void cacheWifiNetworkResults(int16_t count) {
  wifiScanResponse = F("{\"scanning\":false,\"networks\":[");
  wifiScanResponse.reserve(2400);
  size_t added = 0;
  for (int16_t i = 0; i < count && added < 20; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) {
      continue;
    }

    bool duplicate = false;
    for (int16_t previous = 0; previous < i; ++previous) {
      if (WiFi.SSID(previous) == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    if (added > 0) {
      wifiScanResponse += ',';
    }
    wifiScanResponse += F("{\"ssid\":\"");
    wifiScanResponse += jsonEscape(ssid);
    wifiScanResponse += F("\",\"rssi\":");
    wifiScanResponse += WiFi.RSSI(i);
    wifiScanResponse += F(",\"secure\":");
    wifiScanResponse +=
        WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? F("false") : F("true");
    wifiScanResponse += '}';
    ++added;
  }
  wifiScanResponse += F("]}");
  Serial.printf("WIFI_SCAN_COMPLETE visible=%u\n",
                static_cast<unsigned int>(added));
}

void startWifiNetworkScan() {
  if (!provisioningMode ||
      provisioningState == ProvisioningState::kTesting) {
    return;
  }

  if (retrySavedWifiInProvisioning) {
    WiFi.disconnect(false, false);
    lastProvisioningWifiRetryAt = millis();
    delay(50);
  }
  WiFi.scanDelete();
  wifiScanResponse = "";
  const int16_t scanState = WiFi.scanNetworks(true, false);
  if (scanState == WIFI_SCAN_RUNNING) {
    wifiScanRunning = true;
    Serial.println("WIFI_SCAN_STARTED");
  } else if (scanState >= 0) {
    wifiScanRunning = false;
    cacheWifiNetworkResults(scanState);
    WiFi.scanDelete();
  } else {
    wifiScanRunning = false;
    wifiScanResponse =
        F("{\"scanning\":false,\"networks\":[],\"error\":\"Scan failed\"}");
    Serial.println("WIFI_SCAN_FAILED start");
  }
}

void updateWifiNetworkScan() {
  if (!wifiScanRunning) {
    return;
  }

  const int16_t scanState = WiFi.scanComplete();
  if (scanState == WIFI_SCAN_RUNNING) {
    return;
  }
  wifiScanRunning = false;
  if (scanState >= 0) {
    cacheWifiNetworkResults(scanState);
    WiFi.scanDelete();
  } else {
    wifiScanResponse =
        F("{\"scanning\":false,\"networks\":[],\"error\":\"Scan failed\"}");
    Serial.println("WIFI_SCAN_FAILED complete");
  }
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

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("HOLD B2", 64, 48, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("RESET WIFI", 64, 73, 1);

  const uint32_t startedAt = millis();
  while (digitalRead(kButtonRefresh) == LOW &&
         millis() - startedAt < kCredentialResetHoldMs) {
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
  drawHeader();
  tft.fillRect(0, 20, 128, 88, TFT_BLACK);

  if (departureCount == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(statusMessage, 64, 61, 2);
    drawFooter();
    return;
  }

  for (size_t i = 0; i < departureCount; ++i) {
    const int y = 20 + static_cast<int>(i) * 29;
    const Departure &departure = departures[i];
    const uint16_t rowBackground = i % 2 == 0
                                       ? tft.color565(5, 17, 28)
                                       : tft.color565(7, 23, 36);
    const uint16_t platformBackground = tft.color565(0, 58, 78);
    const int minutes = minutesUntilDeparture(departure);
    const uint16_t urgencyColour = countdownColour(minutes);
    const String urgencyText =
        departure.cancelled ? "CXL" : countdownText(minutes);

    tft.fillRect(0, y, 128, 28, rowBackground);
    tft.fillRect(0, y, 3, 28,
                 departure.cancelled ? TFT_RED : urgencyColour);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, rowBackground);
    tft.drawString(departure.scheduled, 7, y + 1, 2);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(departure.cancelled ? TFT_RED : urgencyColour,
                     rowBackground);
    tft.drawString(urgencyText, 124, y + 1, 2);

    const uint16_t serviceColour = statusColour(departure);
    tft.fillCircle(9, y + 21, 2, serviceColour);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(serviceColour, rowBackground);
    tft.drawString(fitText(shortStatus(departure), 82, 1), 15, y + 17, 1);

    const String platform = departure.platform.length() > 0
                                ? "P" + departure.platform
                                : "P-";
    tft.fillRoundRect(103, y + 15, 23, 11, 2, platformBackground);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, platformBackground);
    tft.drawString(fitText(platform, 18, 1), 114, y + 20, 1);
  }

  drawFooter();
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

  wifiScanRunning = false;
  WiFi.scanDelete();
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(pendingWifiSsid.c_str(), pendingWifiPassword.c_str());
}

void resumeSavedWifiRetries() {
  retrySavedWifiInProvisioning = savedWifiSsid.length() > 0;
  if (!retrySavedWifiInProvisioning) {
    return;
  }

  lastProvisioningWifiRetryAt = millis();
  if (wifiScanRunning) {
    Serial.println("PROVISIONING_SAVED_WIFI_RETRY deferred_for_scan");
    return;
  }

  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
  Serial.println("PROVISIONING_SAVED_WIFI_RETRY");
}

void resumeDeparturesFromProvisioning() {
  dnsServer.stop();
  webServer.stop();
  wifiScanRunning = false;
  wifiScanResponse = "";
  WiFi.scanDelete();
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

  webServer.on("/networks", HTTP_GET, []() {
    if (!provisioningMode) {
      webServer.send(404, "application/json", "{\"error\":\"Not found\"}");
      return;
    }
    if (webServer.arg("refresh") == "1") {
      startWifiNetworkScan();
    }
    updateWifiNetworkScan();
    if (wifiScanResponse.length() == 0 && !wifiScanRunning) {
      startWifiNetworkScan();
    }
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json",
                   wifiScanRunning
                       ? "{\"scanning\":true,\"networks\":[]}"
                       : wifiScanResponse);
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
  startWifiNetworkScan();
  drawSetupScreen();
  Serial.printf("PROVISIONING_AP_STARTED ssid=%s\n", setupApSsid.c_str());
  resumeSavedWifiRetries();
}

void updateProvisioning() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  updateWifiNetworkScan();

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
    if (!wifiScanRunning &&
        millis() - lastProvisioningWifiRetryAt >= kWifiRetryIntervalMs) {
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

  tft.begin();
  tft.invertDisplay(false);
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
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
