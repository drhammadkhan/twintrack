// TwinTrack CYD Matrix edition: a UK station CIS-style departure board for
// the "Cheap Yellow Display" (ESP32-2432S028R). Amber dot-matrix styling on
// black, ordinal service rows, a scrolling "Calling at:" line for the next
// service, and a large HH:MM:SS clock. Same train feed, provisioning,
// fallback, web UI, touch zones, and NVS settings as the CYD edition.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <XPT2046_Touchscreen.h>
#include <esp_system.h>
#include <time.h>

constexpr uint8_t kBootButton = 0;
constexpr uint8_t kLedRed = 4;
constexpr uint8_t kLedGreen = 16;
constexpr uint8_t kLedBlue = 17;
constexpr uint8_t kTouchClk = 25;
constexpr uint8_t kTouchCs = 33;
constexpr uint8_t kTouchIrq = 36;
constexpr uint8_t kTouchMiso = 39;
constexpr uint8_t kTouchMosi = 32;
constexpr uint32_t kRefreshIntervalMs = 30000;
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kWifiFallbackIntervalMs = 60000;
constexpr uint32_t kCredentialAttemptMs = 20000;
constexpr uint32_t kCredentialResetHoldMs = 3000;
constexpr uint32_t kFooterPageIntervalMs = 5000;
constexpr uint32_t kScrollStepMs = 33;
constexpr uint8_t kCredentialAttempts = 3;
constexpr size_t kMaxServices = 4;

// Screen geometry (landscape, rotation 1).
constexpr int16_t kScreenWidth = 320;
constexpr int16_t kScreenHeight = 240;
constexpr int16_t kRow1Y = 4;
constexpr int16_t kScrollY = 28;
constexpr int16_t kScrollHeight = 20;
constexpr int16_t kRow2Y = 56;
constexpr int16_t kRowPitch = 24;
constexpr int16_t kClockCentreY = 180;
constexpr int16_t kHintY = 230;

// Typical raw XPT2046 range on this panel; coarse zone taps do not need
// per-device calibration.
constexpr int16_t kTouchRawXMin = 200;
constexpr int16_t kTouchRawXMax = 3700;

const char *const kApiBase = "https://national-rail-api.davwheat.dev/departures";
const char *const kLondonTimeZone = "GMT0BST,M3.5.0/1,M10.5.0/2";
const char *const kPreferencesNamespace = "twintrack";
const char *const kSsidKey = "wifi_ssid";
const char *const kPasswordKey = "wifi_pass";
const char *const kStationKeys[] = {"station_0", "station_1"};
const char *const kDirectionKeys[] = {"direction_0", "direction_1"};
const char *const kOrdinals[] = {"1st", "2nd", "3rd", "4th"};

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
  String callingAt;
  bool cancelled = false;
};

enum class ProvisioningState {
  kIdle,
  kTesting,
  kFailed,
  kSaved,
};

enum class TouchZone {
  kNone,
  kStation,
  kDirection,
  kRefresh,
};

TFT_eSPI tft(kScreenHeight, kScreenWidth);
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(kTouchCs, kTouchIrq);
DNSServer dnsServer;
WebServer webServer(80);

Departure departures[kMaxServices];
size_t departureCount = 0;
uint8_t stationIndex = 0;
uint8_t directionIndex = 0;
uint32_t lastRefreshAt = 0;
uint32_t lastWifiRetryAt = 0;
uint32_t wifiDisconnectedAt = 0;
uint32_t lastClockCheckAt = 0;
uint32_t lastScrollAt = 0;
uint32_t credentialAttemptStartedAt = 0;
uint32_t lastProvisioningWifiRetryAt = 0;
uint32_t restartAt = 0;
int32_t scrollOffset = 0;
int32_t scrollTextWidth = 0;
uint8_t credentialAttempt = 0;
uint8_t lastFooterPage = 255;
bool refreshRequested = true;
bool provisioningMode = false;
bool webHandlersConfigured = false;
bool lanWebUiRunning = false;
bool clockConfigured = false;
bool retrySavedWifiInProvisioning = false;
bool statusLedOn = false;
bool touchWasPressed = false;
bool boardDrawn = false;
String statusMessage = "STARTING";
String scrollText;
String lastClockText;
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

// Classic CIS amber, plus a dimmer variant for secondary text.
uint16_t amber() { return tft.color565(255, 179, 0); }
uint16_t amberDim() { return tft.color565(140, 96, 0); }

void setStatusLed(bool on) {
  // The CYD RGB LED is active low; the green channel doubles as the old
  // status LED, the other channels stay off.
  statusLedOn = on;
  digitalWrite(kLedGreen, on ? LOW : HIGH);
}

// All board text uses the blocky GLCD font scaled up, which reads like an
// LED dot matrix. Size 2 = 12 x 16 px per character, size 4 = 24 x 32 px.
void selectMatrixFont(uint8_t size) {
  tft.setTextFont(1);
  tft.setTextSize(size);
}

int16_t matrixTextWidth(const String &text, uint8_t size) {
  selectMatrixFont(size);
  return tft.textWidth(text);
}

void drawMatrixText(const String &text, int32_t x, int32_t y, uint8_t datum,
                    uint8_t size, uint16_t colour) {
  selectMatrixFont(size);
  tft.setTextDatum(datum);
  tft.setTextColor(colour, TFT_BLACK);
  tft.drawString(text, x, y);
}

String fitMatrixText(String text, int16_t maxWidth, uint8_t size) {
  if (matrixTextWidth(text, size) <= maxWidth) {
    return text;
  }
  while (text.length() > 1 &&
         matrixTextWidth(text + "..", size) > maxWidth) {
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

String currentClockText() {
  struct tm timeInfo;
  if (!readLocalTime(timeInfo)) {
    return "--:--:--";
  }

  char text[9];
  snprintf(text, sizeof(text), "%02d:%02d:%02d", timeInfo.tm_hour,
           timeInfo.tm_min, timeInfo.tm_sec);
  return text;
}

String matrixStatus(const Departure &departure) {
  if (departure.cancelled) {
    return "Cancelled";
  }
  if (departure.expected == "On time") {
    return "On time";
  }
  if (departure.expected == "Delayed") {
    return "Delayed";
  }
  if (departure.expected.length() == 5) {
    return "Exp " + departure.expected;
  }
  return departure.expected.substring(0, 9);
}

void configureClock() {
  if (clockConfigured) {
    return;
  }
  configTzTime(kLondonTimeZone, "pool.ntp.org", "time.cloudflare.com");
  clockConfigured = true;
  Serial.println("CLOCK_CONFIGURED timezone=Europe/London");
}

void setScrollText(const String &text) {
  if (text == scrollText) {
    return;
  }
  scrollText = text;
  scrollTextWidth = matrixTextWidth(scrollText, 2);
  scrollOffset = 0;
}

void updateScroller() {
  if (scrollText.length() == 0 || millis() - lastScrollAt < kScrollStepMs) {
    return;
  }
  lastScrollAt = millis();

  tft.setViewport(0, kScrollY, kScreenWidth, kScrollHeight);
  tft.fillRect(0, 0, kScreenWidth, kScrollHeight, TFT_BLACK);

  if (scrollTextWidth <= kScreenWidth) {
    drawMatrixText(scrollText, 4, 2, TL_DATUM, 2, amber());
  } else {
    scrollOffset += 2;
    if (scrollOffset > scrollTextWidth + kScreenWidth) {
      scrollOffset = 0;
    }
    drawMatrixText(scrollText, kScreenWidth - scrollOffset, 2, TL_DATUM, 2,
                   amber());
  }
  tft.resetViewport();
}

void drawClock(bool force = false) {
  const String clockText = currentClockText();
  if (!force && clockText == lastClockText) {
    return;
  }
  lastClockText = clockText;
  drawMatrixText(clockText, 160, kClockCentreY, MC_DATUM, 4, amber());
}

void drawHint() {
  lastFooterPage = (millis() / kFooterPageIntervalMs) % 2;
  tft.fillRect(0, kHintY - 2, kScreenWidth, kScreenHeight - kHintY + 2,
               TFT_BLACK);
  const String hint =
      lastFooterPage == 0
          ? "TAP:  STATION  |  DIRECTION  |  REFRESH"
          : "http://" + deviceHostname + ".local/";
  drawMatrixText(hint, 160, kHintY, TC_DATUM, 1, amberDim());
}

void drawServiceRow(size_t index, int16_t y) {
  const Departure &departure = departures[index];
  const String status = matrixStatus(departure);
  const int16_t statusWidth = matrixTextWidth(status, 2);
  const int16_t destinationX = 116;
  const int16_t destinationMax = 316 - statusWidth - 10 - destinationX;

  drawMatrixText(kOrdinals[index], 4, y, TL_DATUM, 2, amber());
  drawMatrixText(departure.scheduled, 46, y, TL_DATUM, 2, amber());
  drawMatrixText(fitMatrixText(departure.destination, destinationMax, 2),
                 destinationX, y, TL_DATUM, 2, amber());
  drawMatrixText(status, 316, y, TR_DATUM, 2, amber());
}

void drawDepartures() {
  tft.fillScreen(TFT_BLACK);
  boardDrawn = true;
  lastClockText = "";

  if (departureCount == 0) {
    drawMatrixText(statusMessage, 160, 64, MC_DATUM, 2, amber());
    setScrollText("");
  } else {
    drawServiceRow(0, kRow1Y);
    for (size_t i = 1; i < departureCount; ++i) {
      drawServiceRow(i, kRow2Y + static_cast<int16_t>(i - 1) * kRowPitch);
    }
    if (departures[0].callingAt.length() > 0) {
      setScrollText(departures[0].callingAt);
    } else {
      setScrollText("Calling at: " + departures[0].destination + " only.");
    }
  }

  drawClock(true);
  drawHint();
}

void drawMessage(const String &message, uint16_t colour = 0) {
  if (colour == 0) {
    colour = amber();
  }
  tft.fillScreen(TFT_BLACK);
  boardDrawn = false;
  setScrollText("");
  drawMatrixText(fitMatrixText(message, 312, 2), 160, 110, MC_DATUM, 2,
                 colour);
  drawHint();
}

void drawSetupScreen() {
  tft.fillScreen(TFT_BLACK);
  boardDrawn = false;
  setScrollText("");
  drawMatrixText("WIFI SETUP", 160, 8, TC_DATUM, 2, amber());

  drawMatrixText("JOIN", 160, 44, TC_DATUM, 1, amberDim());
  drawMatrixText(setupApSsid, 160, 58, TC_DATUM, 2, amber());

  drawMatrixText("PASSWORD", 160, 92, TC_DATUM, 1, amberDim());
  drawMatrixText(setupApPassword, 160, 106, TC_DATUM, 2, amber());

  drawMatrixText("OPEN", 160, 140, TC_DATUM, 1, amberDim());
  drawMatrixText("192.168.4.1", 160, 154, TC_DATUM, 2, amber());

  drawMatrixText("HOLD BOOT AT POWER-ON: RESET WI-FI", 160, 210, TC_DATUM, 1,
                 amberDim());
}

void drawCredentialTestScreen() {
  tft.fillScreen(TFT_BLACK);
  boardDrawn = false;
  drawMatrixText("TESTING WIFI", 160, 100, MC_DATUM, 2, amber());
  drawMatrixText("KEEP SETUP OPEN", 160, 130, MC_DATUM, 1, amberDim());
}

void drawCredentialSavedScreen() {
  tft.fillScreen(TFT_BLACK);
  boardDrawn = false;
  drawMatrixText("WIFI SAVED", 160, 100, MC_DATUM, 2, amber());
  drawMatrixText("RESTARTING", 160, 130, MC_DATUM, 1, amberDim());
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
  page += F("<label>Station slot 1<input name='station0' maxlength='3' value='");
  page += kStations[0].code;
  page += F("' required></label><label>Station slot 2<input name='station1' maxlength='3' value='");
  page += kStations[1].code;
  page += F("' required></label></section><section><h2>Destination filters</h2>");
  page += F("<label>Direction slot 1<input name='direction0' maxlength='3' value='");
  page += kDirections[0].code;
  page += F("' required></label><label>Direction slot 2<input name='direction1' maxlength='3' value='");
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
  if (digitalRead(kBootButton) != LOW) {
    return false;
  }

  tft.fillScreen(TFT_BLACK);
  drawMatrixText("HOLD BOOT", 160, 96, MC_DATUM, 2, amber());
  drawMatrixText("RESET WIFI", 160, 126, MC_DATUM, 1, amberDim());

  const uint32_t startedAt = millis();
  while (digitalRead(kBootButton) == LOW &&
         millis() - startedAt < kCredentialResetHoldMs) {
    delay(20);
  }

  const bool requested = millis() - startedAt >= kCredentialResetHoldMs;
  while (digitalRead(kBootButton) == LOW) {
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

TouchZone pollTouchZone() {
  const bool pressed = touch.tirqTouched() && touch.touched();
  if (!pressed) {
    touchWasPressed = false;
    return TouchZone::kNone;
  }
  if (touchWasPressed) {
    return TouchZone::kNone;
  }
  touchWasPressed = true;

  const TS_Point point = touch.getPoint();
  const int32_t x = constrain(
      map(point.x, kTouchRawXMin, kTouchRawXMax, 0, kScreenWidth), 0,
      kScreenWidth - 1);
  if (x < kScreenWidth / 3) {
    return TouchZone::kStation;
  }
  if (x < 2 * kScreenWidth / 3) {
    return TouchZone::kDirection;
  }
  return TouchZone::kRefresh;
}

bool connectStoredWifi() {
  if (savedWifiSsid.length() == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedWifiSsid.c_str(), savedWifiPassword.c_str());
  statusMessage = "WIFI...";
  drawMessage(statusMessage);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
    setStatusLed(!statusLedOn);
    delay(250);
  }

  setStatusLed(WiFi.status() == WL_CONNECTED);
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
  setStatusLed(true);
  drawMessage("RECONNECTED");
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

String buildCallingAt(JsonObject service) {
  JsonArray points =
      service["subsequentCallingPoints"][0]["callingPoint"].as<JsonArray>();
  if (points.isNull() || points.size() == 0) {
    return "";
  }

  String text = "Calling at: ";
  const size_t count = points.size();
  for (size_t i = 0; i < count; ++i) {
    const String name = points[i]["locationName"] | "";
    if (name.length() == 0) {
      continue;
    }
    if (i > 0) {
      text += (i == count - 1) ? " and " : ", ";
    }
    text += name;
  }
  text += ".";

  const String platform = service["platform"] | "";
  if (platform.length() > 0) {
    text += "  --  Platform " + platform + ".";
  }
  return text;
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
                     direction.code + "/4?expand=true";

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

  // The expanded payload includes full calling-point lists, so filter the
  // parse down to just the fields the board renders.
  StaticJsonDocument<512> filter;
  filter["locationName"] = true;
  JsonObject serviceFilter = filter["trainServices"].createNestedObject();
  serviceFilter["std"] = true;
  serviceFilter["etd"] = true;
  serviceFilter["platform"] = true;
  serviceFilter["isCancelled"] = true;
  serviceFilter["destination"][0]["locationName"] = true;
  serviceFilter["subsequentCallingPoints"][0]["callingPoint"][0]
               ["locationName"] = true;

  DynamicJsonDocument document(24576);
  const DeserializationError error =
      deserializeJson(document, payload, DeserializationOption::Filter(filter));

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
    departure.callingAt =
        departureCount == 1 ? buildCallingAt(service) : "";
    if (departureCount == 1 && departure.destination != "Unknown") {
      kDirections[directionIndex].label = departure.destination;
    }
  }

  statusMessage = departureCount > 0 ? "LIVE" : "No services";
  Serial.printf("FETCH_OK count=%u\n",
                static_cast<unsigned int>(departureCount));
  return true;
}

void setup() {
  Serial.begin(115200);

  pinMode(kLedRed, OUTPUT);
  pinMode(kLedGreen, OUTPUT);
  pinMode(kLedBlue, OUTPUT);
  digitalWrite(kLedRed, HIGH);
  digitalWrite(kLedGreen, HIGH);
  digitalWrite(kLedBlue, HIGH);
  pinMode(kBootButton, INPUT_PULLUP);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  touchSpi.begin(kTouchClk, kTouchMiso, kTouchMosi, kTouchCs);
  touch.begin(touchSpi);
  touch.setRotation(1);

  drawMessage("TWINTRACK");
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

  if (boardDrawn) {
    updateScroller();
    if (millis() - lastClockCheckAt >= 200) {
      lastClockCheckAt = millis();
      drawClock();
    }
  }

  const uint8_t footerPage = (millis() / kFooterPageIntervalMs) % 2;
  if (footerPage != lastFooterPage) {
    drawHint();
  }

  const TouchZone zone = pollTouchZone();
  if (zone == TouchZone::kStation) {
    stationIndex = (stationIndex + 1) % 2;
    refreshRequested = true;
    drawMessage(kStations[stationIndex].label);
  } else if (zone == TouchZone::kDirection) {
    directionIndex = (directionIndex + 1) % 2;
    refreshRequested = true;
    drawMessage(kDirections[directionIndex].label);
  } else if (zone == TouchZone::kRefresh) {
    refreshRequested = true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDisconnectedAt == 0) {
      wifiDisconnectedAt = millis();
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

  if (refreshRequested || millis() - lastRefreshAt >= kRefreshIntervalMs) {
    refreshRequested = false;
    lastRefreshAt = millis();
    fetchDepartures();
    drawDepartures();
  }

  delay(5);
}
