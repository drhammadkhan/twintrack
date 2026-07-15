#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GxEPD2_3C.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <time.h>

constexpr uint8_t kEpaperMosi = 23;
constexpr uint8_t kEpaperClock = 18;
constexpr uint8_t kEpaperChipSelect = 5;
constexpr uint8_t kEpaperDataCommand = 17;
constexpr uint8_t kEpaperReset = 4;
constexpr uint8_t kEpaperBusy = 16;
constexpr uint32_t kFetchIntervalMs = 60000;
constexpr uint32_t kDisplayRefreshIntervalMs = 300000;
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kWifiFallbackIntervalMs = 60000;
constexpr uint32_t kCredentialAttemptMs = 20000;
constexpr uint8_t kCredentialAttempts = 3;
constexpr size_t kMaxServices = 2;

const char *const kApiBase = "https://national-rail-api.davwheat.dev/departures";
const char *const kLondonTimeZone = "GMT0BST,M3.5.0/1,M10.5.0/2";
const char *const kPreferencesNamespace = "twintrack";
const char *const kSsidKey = "wifi_ssid";
const char *const kPasswordKey = "wifi_pass";
const char *const kStationKeys[] = {"station_0", "station_1"};
const char *const kDirectionKeys[] = {"direction_0", "direction_1"};
const char *const kStationIndexKey = "station_idx";
const char *const kDirectionIndexKey = "direction_idx";

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

GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT> display(
    GxEPD2_290_C90c(kEpaperChipSelect, kEpaperDataCommand, kEpaperReset,
                    kEpaperBusy));
DNSServer dnsServer;
WebServer webServer(80);

Departure departures[kMaxServices];
size_t departureCount = 0;
uint8_t stationIndex = 0;
uint8_t directionIndex = 0;
uint32_t lastFetchAt = 0;
uint32_t lastDisplayRefreshAt = 0;
uint32_t lastWifiRetryAt = 0;
uint32_t wifiDisconnectedAt = 0;
uint32_t lastClockCheckAt = 0;
uint32_t credentialAttemptStartedAt = 0;
uint32_t lastProvisioningWifiRetryAt = 0;
uint32_t restartAt = 0;
int32_t lastDisplayedMinute = -1;
uint8_t credentialAttempt = 0;
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
String lastRenderedSignature;
String wifiScanResponse;
bool wifiScanRunning = false;
ProvisioningState provisioningState = ProvisioningState::kIdle;

void configureWebHandlers();
void startLanWebUi();
void startWifiNetworkScan();
void updateWifiNetworkScan();
void drawDepartures();

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

int16_t textWidth(const String &text, uint8_t size) {
  int16_t x = 0;
  int16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  display.setFont(nullptr);
  display.setTextSize(size);
  display.getTextBounds(text, 0, 0, &x, &y, &width, &height);
  return static_cast<int16_t>(width);
}

String fitText(String text, int16_t maxWidth, uint8_t size) {
  if (textWidth(text, size) <= maxWidth) {
    return text;
  }

  while (text.length() > 1 && textWidth(text + "..", size) > maxWidth) {
    text.remove(text.length() - 1);
  }
  return text + "..";
}

void drawText(int16_t x, int16_t y, const String &text, uint8_t size,
              uint16_t colour = GxEPD_BLACK) {
  display.setFont(nullptr);
  display.setTextSize(size);
  display.setTextColor(colour);
  display.setCursor(x, y);
  display.print(text);
}

void drawRightText(int16_t right, int16_t y, const String &text, uint8_t size,
                   uint16_t colour = GxEPD_BLACK) {
  drawText(right - textWidth(text, size), y, text, size, colour);
}

void drawCentredText(int16_t centre, int16_t y, const String &text,
                     uint8_t size, uint16_t colour = GxEPD_BLACK) {
  drawText(centre - textWidth(text, size) / 2, y, text, size, colour);
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

String boardDestinationName(const Departure &departure) {
  String name = departure.destination;
  if (name.indexOf("Waterloo") >= 0 ||
      kDirections[directionIndex].code == "WAT") {
    return "WATERLOO";
  }
  if (name.indexOf("Chessington South") >= 0 ||
      kDirections[directionIndex].code == "CSS") {
    return "CHESSINGTON SOUTH";
  }
  if (name.length() == 0) {
    name = routeDisplayName(kDirections[directionIndex].code,
                            kDirections[directionIndex].label);
  }
  name.toUpperCase();
  return name;
}

String displayContentSignature() {
  String signature;
  signature.reserve(220);
  signature += kStations[stationIndex].code;
  signature += '|';
  signature += kDirections[directionIndex].code;
  signature += '|';
  signature += WiFi.status() == WL_CONNECTED ? "online" : "offline";
  signature += '|';
  signature += String(departureCount);

  if (departureCount == 0) {
    signature += '|';
    signature += statusMessage;
  }

  for (size_t i = 0; i < departureCount; ++i) {
    const Departure &departure = departures[i];
    signature += '|';
    signature += departure.scheduled;
    signature += '|';
    signature += departure.expected;
    signature += '|';
    signature += departure.destination;
    signature += '|';
    signature += departure.platform;
    signature += '|';
    signature += departure.cancelled ? '1' : '0';
  }
  return signature;
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

String currentDateText() {
  struct tm timeInfo;
  if (!readLocalTime(timeInfo)) {
    return "--- -- ---";
  }

  char text[12];
  strftime(text, sizeof(text), "%a %d %b", &timeInfo);
  String value = text;
  value.toUpperCase();
  return value;
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

void configureClock() {
  if (clockConfigured) {
    return;
  }
  configTzTime(kLondonTimeZone, "pool.ntp.org", "time.cloudflare.com");
  clockConfigured = true;
  Serial.println("CLOCK_CONFIGURED timezone=Europe/London");
}

void drawHeaderContent() {
  const String originName =
      routeDisplayName(kStations[stationIndex].code,
                       kStations[stationIndex].label);
  const String destinationName =
      routeDisplayName(kDirections[directionIndex].code,
                       kDirections[directionIndex].label);
  String routeLine = "TO " + destinationName;
  routeLine.toUpperCase();

  display.fillRect(0, 0, 296, 29, GxEPD_BLACK);
  drawText(7, 3, fitText(originName, 208, 2), 2, GxEPD_WHITE);
  drawRightText(289, 4, currentTimeText(), 2, GxEPD_WHITE);
  drawText(7, 20, fitText(routeLine, 165, 1), 1, GxEPD_WHITE);
  drawRightText(251, 20, currentDateText(), 1, GxEPD_WHITE);

  const bool connected = WiFi.status() == WL_CONNECTED;
  display.fillRoundRect(257, 18, 32, 10, 2,
                        connected ? GxEPD_RED : GxEPD_WHITE);
  drawCentredText(273, 19, connected ? "LIVE" : "OFF", 1,
                   connected ? GxEPD_WHITE : GxEPD_BLACK);
  display.fillRect(0, 29, 296, 3, GxEPD_RED);
}

void drawFooterContent() {
  display.fillRect(0, 116, 296, 12, GxEPD_RED);
  drawText(8, 118, "NEXT TRAINS", 1, GxEPD_WHITE);
  const String address = deviceHostname.length() > 0
                             ? deviceHostname + ".local"
                             : "TWINTRACK E-PAPER";
  drawRightText(288, 118, address, 1, GxEPD_WHITE);
}

void drawMessage(const String &message, uint16_t colour = GxEPD_BLACK) {
  (void)colour;
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeaderContent();
    drawCentredText(148, 68, fitText(message, 280, 2), 2);
    drawFooterContent();
  } while (display.nextPage());
}

void drawSetupScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, 296, 28, GxEPD_BLACK);
    drawText(8, 6, "TWINTRACK", 2, GxEPD_WHITE);
    drawRightText(288, 10, "WI-FI SETUP", 1, GxEPD_WHITE);
    drawText(10, 37, "JOIN", 1);
    drawText(72, 33, fitText(setupApSsid, 210, 2), 2);
    drawText(10, 60, "PASSWORD", 1);
    drawText(92, 56, setupApPassword, 2);
    display.drawFastHLine(10, 80, 276, GxEPD_BLACK);
    drawText(10, 89, "OPEN", 1);
    drawText(72, 85, "192.168.4.1", 2);
    drawText(10, 112, "Use the captive portal to connect this display", 1);
  } while (display.nextPage());
}

void drawCredentialTestScreen() {
  drawMessage("TESTING WI-FI");
}

void drawCredentialSavedScreen() {
  drawMessage("WI-FI SAVED");
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
  stationIndex = min<uint8_t>(preferences.getUChar(kStationIndexKey, 0), 1);
  directionIndex =
      min<uint8_t>(preferences.getUChar(kDirectionIndexKey, 0), 1);
  preferences.end();
}

bool saveTrainSettings(const String stationCodes[2],
                       const String directionCodes[2], uint8_t selectedStation,
                       uint8_t selectedDirection) {
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
  saved = preferences.putUChar(kStationIndexKey, selectedStation) > 0 && saved;
  saved =
      preferences.putUChar(kDirectionIndexKey, selectedDirection) > 0 && saved;
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
  stationIndex = selectedStation;
  directionIndex = selectedDirection;
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
  page += F("input,select{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;");
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
  page += F("<label>Origin 1<input name='station0' maxlength='3' value='");
  page += kStations[0].code;
  page += F("' required></label><label>Origin 2<input name='station1' maxlength='3' value='");
  page += kStations[1].code;
  page += F("' required></label><label>Displayed origin<select name='station_index'>");
  page += stationIndex == 0 ? F("<option value='0' selected>Origin 1</option><option value='1'>Origin 2</option>")
                            : F("<option value='0'>Origin 1</option><option value='1' selected>Origin 2</option>");
  page += F("</select></label></section><section><h2>Destination filters</h2>");
  page += F("<label>Destination 1<input name='direction0' maxlength='3' value='");
  page += kDirections[0].code;
  page += F("' required></label><label>Destination 2<input name='direction1' maxlength='3' value='");
  page += kDirections[1].code;
  page += F("' required></label><label>Displayed destination<select name='direction_index'>");
  page += directionIndex == 0 ? F("<option value='0' selected>Destination 1</option><option value='1'>Destination 2</option>")
                              : F("<option value='0'>Destination 1</option><option value='1' selected>Destination 2</option>");
  page += F("</select></label><p><small>Enter three-character National Rail CRS codes, such as <code>MAL</code>, <code>TOL</code>, <code>WAT</code>, or <code>CSS</code>.</small></p>");
  page += F("<button type='submit'>Save and refresh display</button></section></form>");
  page += F("<form action='/reset-wifi' method='post'><section><h2>Wi-Fi</h2><p><small>Erase the saved network and reopen first-boot setup.</small></p><button type='submit'>Reset Wi-Fi settings</button></section></form>");
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

void generateSetupNetworkDetails() {
  char ssid[20];
  const uint16_t deviceSuffix =
      static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFF);
  snprintf(ssid, sizeof(ssid), "TwinTrack-%04X", deviceSuffix);
  setupApSsid = ssid;
  deviceHostname = "twintrack-paper";

  static const char kPasswordAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  setupApPassword = "";
  for (uint8_t i = 0; i < 8; ++i) {
    const uint32_t value = esp_random();
    setupApPassword +=
        kPasswordAlphabet[value % (sizeof(kPasswordAlphabet) - 1)];
  }
}

void drawDepartures() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeaderContent();

    if (departureCount == 0) {
      drawCentredText(148, 68, fitText(statusMessage, 280, 2), 2);
    } else {
      for (size_t i = 0; i < departureCount && i < kMaxServices; ++i) {
        const int16_t y = 33 + static_cast<int16_t>(i) * 41;
        const Departure &departure = departures[i];
        const int minutes = minutesUntilDeparture(departure);
        const String urgencyText =
            departure.cancelled ? "CXL" : countdownText(minutes);
        const String platform = departure.platform.length() > 0
                                    ? "PLATFORM " + departure.platform
                                    : "PLATFORM -";
        const String destination = boardDestinationName(departure);
        const uint8_t destinationSize = textWidth(destination, 2) <= 110 ? 2 : 1;
        const int16_t destinationY = y + (destinationSize == 2 ? 5 : 10);
        const bool urgent = departure.cancelled || (minutes >= 0 && minutes <= 5);
        const bool alert = departure.cancelled || departure.expected == "Delayed" ||
                           departure.expected.length() == 5;

        display.fillRect(0, y, 4, 39, GxEPD_RED);
        display.drawFastHLine(7, y + 40, 282, GxEPD_BLACK);
        display.drawFastVLine(104, y + 4, 31, GxEPD_BLACK);
        drawText(11, y + 5, departure.scheduled, 3);
        drawText(12, y + 30, fitText(shortStatus(departure), 86, 1), 1,
                 alert ? GxEPD_RED : GxEPD_BLACK);
        drawText(113, destinationY,
                 fitText(destination, 110, destinationSize), destinationSize);
        drawText(113, y + 26, fitText(platform, 110, 1), 1);

        if (urgent) {
          display.fillRoundRect(229, y + 4, 59, 31, 3, GxEPD_RED);
          drawCentredText(258, y + 11, urgencyText, 2, GxEPD_WHITE);
        } else {
          display.drawRoundRect(229, y + 4, 59, 31, 3, GxEPD_RED);
          drawCentredText(258, y + 11, urgencyText, 2, GxEPD_RED);
        }
      }
    }

    drawFooterContent();
  } while (display.nextPage());
  lastRenderedSignature = displayContentSignature();
  lastDisplayRefreshAt = millis();
}

bool connectStoredWifi() {
  if (savedWifiSsid.length() == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
  statusMessage = "WIFI...";

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
    delay(250);
  }

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
  lastFetchAt = 0;
  wifiDisconnectedAt = 0;
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
    const uint8_t selectedStation = webServer.arg("station_index") == "1" ? 1 : 0;
    const uint8_t selectedDirection =
        webServer.arg("direction_index") == "1" ? 1 : 0;
    for (uint8_t i = 0; i < 2; ++i) {
      if (!normaliseCrsCode(stationCodes[i]) ||
          !normaliseCrsCode(directionCodes[i])) {
        sendSettingsPage(
            "Each station and destination must be a three-character CRS code.");
        return;
      }
    }

    if (!saveTrainSettings(stationCodes, directionCodes, selectedStation,
                           selectedDirection)) {
      sendSettingsPage("The settings could not be stored. Please try again.");
      return;
    }

    departureCount = 0;
    refreshRequested = true;
    lastFetchAt = 0;
    Serial.println("TRAIN_SETTINGS_SAVED");
    sendSettingsPage("Settings saved. The display is refreshing now.");
  });

  webServer.on("/reset-wifi", HTTP_POST, []() {
    if (provisioningMode) {
      redirectToPortal();
      return;
    }
    clearCredentials();
    webServer.send(200, "text/html",
                   portalPage("Wi-Fi reset",
                              "Saved Wi-Fi has been erased. TwinTrack is restarting in setup mode.",
                              false, false));
    restartAt = millis() + 1500;
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
    drawMessage("AP ERROR");
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
  delay(100);
  SPI.begin(kEpaperClock, -1, kEpaperMosi, kEpaperChipSelect);
  display.init(115200, true, 50, false);
  display.setRotation(1);
  display.setTextWrap(false);
  Serial.printf("DISPLAY_ENGINE GxEPD2 SSD1680 3C %ux%u\n", display.width(),
                display.height());
  generateSetupNetworkDetails();
  loadTrainSettings();

  const bool hasCredentials = loadCredentials();
  if (!hasCredentials || !connectStoredWifi()) {
    startProvisioning();
  } else {
    startLanWebUi();
  }
}

void loop() {
  if (restartAt > 0 && static_cast<int32_t>(millis() - restartAt) >= 0) {
    ESP.restart();
  }

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
          refreshRequested = true;
        }
        lastDisplayedMinute = minuteStamp;
      }
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDisconnectedAt == 0) {
      wifiDisconnectedAt = millis();
      statusMessage = "WI-FI OFFLINE";
      Serial.println("DISPLAY_REFRESH_SKIPPED transient_wifi_loss");
    }
    if (millis() - lastWifiRetryAt >= kWifiRetryIntervalMs) {
      lastWifiRetryAt = millis();
      WiFi.reconnect();
    }
    if (millis() - wifiDisconnectedAt >= kWifiFallbackIntervalMs) {
      Serial.println("WIFI_FALLBACK_TO_SETUP");
      startProvisioning();
      return;
    }
  } else {
    wifiDisconnectedAt = 0;
  }

  if (lastDisplayedMinute >= 0 &&
      (refreshRequested || millis() - lastFetchAt >= kFetchIntervalMs)) {
    const bool forceDisplayRefresh = refreshRequested;
    refreshRequested = false;
    lastFetchAt = millis();
    fetchDepartures();

    const String signature = displayContentSignature();
    const bool contentChanged = signature != lastRenderedSignature;
    const bool displayExpired =
        lastDisplayRefreshAt == 0 ||
        millis() - lastDisplayRefreshAt >= kDisplayRefreshIntervalMs;
    if (forceDisplayRefresh || contentChanged || displayExpired) {
      Serial.printf("DISPLAY_REFRESH reason=%s%s%s\n",
                    forceDisplayRefresh ? "forced " : "",
                    contentChanged ? "content " : "",
                    displayExpired ? "age" : "");
      drawDepartures();
    } else {
      Serial.println("DISPLAY_REFRESH_SKIPPED unchanged");
    }
  }

  delay(5);
}
