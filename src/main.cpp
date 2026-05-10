#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <EEPROM.h>
#include <vector>
#include <algorithm>
#include "config.h"

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
JsonDocument doc;
WiFiServer server(WEB_SERVER_PORT);

struct Arrival {
  String lineName;
  String towards;         // often empty for rail — use destinationName instead
  String destinationName; // e.g. "Bank DLR Station", "Stratford DLR Station"
  String modeName;        // "bus", "dlr", "overground", "tube", etc.
  int timeToStation;      // seconds
};

String stopIds[MAX_STOPS];
String stopLabels[MAX_STOPS];
int currentStopIndex = 0;
unsigned long lastUpdateTime = 0;
unsigned long lastCycleTime  = 0;
unsigned long lastScrollTime = 0;
int scrollIdx = 0;
std::vector<Arrival> currentArrivals;

// Forward declarations
void connectToWiFi();
bool fetchArrivals(const String& stopId, std::vector<Arrival>& arrivals);
void displayArrivals(const std::vector<Arrival>& arrivals, int stopIdx);
void printArrivalRow(const Arrival& a);
void displayArrivals(const std::vector<Arrival>& arrivals, int stopIdx);
String htmlEscape(const String& s);
String urlDecode(const String& input);
void parsePostData(const String& data);
void loadFromEEPROM();
void saveToEEPROM();
void handleWebServer();
String generateHtmlPage(bool saved);

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  loadFromEEPROM();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  connectToWiFi();
  server.begin();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi OK");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP().toString().substring(0, 16));
  delay(2000);

  // Force an immediate fetch on first loop iteration
  lastUpdateTime = millis() - UPDATE_INTERVAL;
}

void loop() {
  handleWebServer();

  // Reconnect if WiFi dropped
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
    return;
  }

  unsigned long now = millis();

  // Rotate to next configured stop
  if (now - lastCycleTime >= CYCLE_INTERVAL) {
    lastCycleTime = now;
    int tries = 0;
    do {
      currentStopIndex = (currentStopIndex + 1) % MAX_STOPS;
      tries++;
    } while (stopIds[currentStopIndex].length() == 0 && tries < MAX_STOPS);
    lastUpdateTime = 0; // Force immediate refresh after rotating
  }

  // Fetch and display
  if (now - lastUpdateTime >= UPDATE_INTERVAL) {
    lastUpdateTime = now;
    String stopId = stopIds[currentStopIndex];
    if (stopId.length() > 0) {
      std::vector<Arrival> arrivals;
      if (fetchArrivals(stopId, arrivals)) {
        currentArrivals = arrivals;
        scrollIdx = 0;
        lastScrollTime = now;
        displayArrivals(currentArrivals, currentStopIndex);
      } else {
        currentArrivals.clear();
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("API Error");
        lcd.setCursor(0, 1);
        lcd.print(stopId.substring(0, 16));
      }
    } else {
      currentArrivals.clear();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("No stop set");
      lcd.setCursor(0, 1);
      lcd.print(WiFi.localIP().toString().substring(0, 16));
    }
  }

  // Scroll row 1 between first two arrivals
  if (currentArrivals.size() > 1 && now - lastScrollTime >= SCROLL_INTERVAL) {
    lastScrollTime = now;
    scrollIdx = 1 - scrollIdx; // toggle 0 / 1
    lcd.setCursor(0, 1);
    printArrivalRow(currentArrivals[scrollIdx]);
  }
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) {
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed");
  }
}

// ---------------------------------------------------------------------------
// TfL API
// ---------------------------------------------------------------------------

bool fetchArrivals(const String& stopId, std::vector<Arrival>& arrivals) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.setTimeout(10000);

  String url = String(TFL_API_BASE) + stopId + "/arrivals";
  if (!https.begin(client, url)) {
    Serial.println("Failed to connect to TfL API");
    return false;
  }

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d for stop %s\n", code, stopId.c_str());
    https.end();
    return false;
  }

  // Filter to only keep the two fields we use — avoids loading the full
  // 20-50 KB TfL response into a String, which exhausts Pico W heap.
  JsonDocument filter;
  filter[0]["timeToStation"]   = true;
  filter[0]["lineName"]        = true;
  filter[0]["towards"]         = true;
  filter[0]["destinationName"] = true;
  filter[0]["modeName"]        = true;

  DeserializationError err = deserializeJson(doc, https.getStream(),
                               DeserializationOption::Filter(filter));
  https.end();
  if (err) {
    Serial.printf("JSON error: %s\n", err.f_str());
    return false;
  }

  arrivals.clear();
  for (JsonObject item : doc.as<JsonArray>()) {
    if (!item["timeToStation"].isNull()) {
      Arrival a;
      a.timeToStation   = item["timeToStation"].as<int>();
      a.lineName        = item["lineName"]        | "";
      a.towards         = item["towards"]         | "";
      a.destinationName = item["destinationName"] | "";
      a.modeName        = item["modeName"]        | "";
      arrivals.push_back(a);
    }
  }

  std::sort(arrivals.begin(), arrivals.end(),
    [](const Arrival& a, const Arrival& b) { return a.timeToStation < b.timeToStation; });

  Serial.printf("Stop %s: %d arrivals\n", stopId.c_str(), (int)arrivals.size());
  return true;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

// Strips common TfL station suffixes so "Bank DLR Station" → "Bank"
String cleanDestination(const String& raw) {
  String s = raw;
  static const char* suffixes[] = {
    " DLR Station", " Underground Station", " Overground Station",
    " Rail Station", " Station", " DLR", nullptr
  };
  for (int i = 0; suffixes[i]; i++) {
    if (s.endsWith(suffixes[i])) {
      s = s.substring(0, s.length() - strlen(suffixes[i]));
      break;
    }
  }
  return s;
}

// Writes one 16-char arrival line at the current cursor position.
// Format: "<name padded to 12><time right-aligned in 4>"
// Buses: "D3 Aldgate E    3m"  Rail/DLR: "Bank            3m"
void printArrivalRow(const Arrival& a) {
  bool isBus = (a.modeName == "bus" || a.modeName == "");

  String name;
  if (isBus) {
    name = a.lineName;
    if (a.towards.length() > 0) name += " " + a.towards;
  } else {
    // Prefer destinationName (cleaned), then towards, then lineName
    if (a.destinationName.length() > 0)
      name = cleanDestination(a.destinationName);
    else if (a.towards.length() > 0)
      name = a.towards;
    else
      name = a.lineName;
  }
  name = name.substring(0, 12);
  while ((int)name.length() < 12) name += ' ';

  // Time: right-aligned in 4 chars (" due", "  3m", " 12m")
  String t;
  if (a.timeToStation < 30) {
    t = " due";
  } else {
    int m = a.timeToStation / 60;
    t = String(m) + "m";
    while ((int)t.length() < 4) t = " " + t;
    t = t.substring(0, 4);
  }

  lcd.print(name + t);
}

void displayArrivals(const std::vector<Arrival>& arrivals, int stopIdx) {
  lcd.clear();

  // Row 0: label (or stop ID if no label set)
  String label = stopLabels[stopIdx].length() > 0 ? stopLabels[stopIdx] : stopIds[stopIdx];
  lcd.setCursor(0, 0);
  lcd.print(label.substring(0, 16));

  // Row 1: first arrival (scrolling handled in loop())
  lcd.setCursor(0, 1);
  if (arrivals.empty()) {
    lcd.print("No arrivals     ");
    return;
  }
  printArrivalRow(arrivals[0]);
}

// ---------------------------------------------------------------------------
// EEPROM
// ---------------------------------------------------------------------------

void loadFromEEPROM() {
  for (int i = 0; i < MAX_STOPS; i++) {
    stopIds[i] = "";
    for (int j = 0; j < STOP_ID_LENGTH; j++) {
      char c = EEPROM.read(i * STOP_ID_LENGTH + j);
      if (c == '\0' || (byte)c == 0xFF) break;
      stopIds[i] += c;
    }
    stopLabels[i] = "";
    int labelBase = EEPROM_LABELS_OFFSET + i * STOP_LABEL_LENGTH;
    for (int j = 0; j < STOP_LABEL_LENGTH; j++) {
      char c = EEPROM.read(labelBase + j);
      if (c == '\0' || (byte)c == 0xFF) break;
      stopLabels[i] += c;
    }
  }
  // No default stop — configure via the web UI
}

void saveToEEPROM() {
  for (int i = 0; i < MAX_STOPS; i++) {
    for (int j = 0; j < STOP_ID_LENGTH; j++)
      EEPROM.write(i * STOP_ID_LENGTH + j,
        j < (int)stopIds[i].length() ? stopIds[i][j] : '\0');

    int labelBase = EEPROM_LABELS_OFFSET + i * STOP_LABEL_LENGTH;
    for (int j = 0; j < STOP_LABEL_LENGTH; j++)
      EEPROM.write(labelBase + j,
        j < (int)stopLabels[i].length() ? stopLabels[i][j] : '\0');
  }
  EEPROM.commit();
}

// ---------------------------------------------------------------------------
// Web server
// ---------------------------------------------------------------------------

String urlDecode(const String& input) {
  String out;
  for (int i = 0; i < (int)input.length(); i++) {
    if (input[i] == '+') {
      out += ' ';
    } else if (input[i] == '%' && i + 2 < (int)input.length()) {
      char hex[3] = { input[i + 1], input[i + 2], '\0' };
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += input[i];
    }
  }
  return out;
}

String htmlEscape(const String& s) {
  String out;
  for (char c : s) {
    if      (c == '<')  out += "&lt;";
    else if (c == '>')  out += "&gt;";
    else if (c == '"')  out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else if (c == '&')  out += "&amp;";
    else                out += c;
  }
  return out;
}

void parsePostData(const String& data) {
  for (int i = 0; i < MAX_STOPS; i++) {
    auto readField = [&](const String& key, int maxLen) -> String {
      String k = key + String(i) + "=";
      int idx = data.indexOf(k);
      if (idx == -1) return "";
      int vs = idx + k.length();
      int ve = data.indexOf('&', vs);
      if (ve == -1) ve = data.length();
      String v = urlDecode(data.substring(vs, ve));
      v.trim();
      if (v.length() > (unsigned)maxLen) v = v.substring(0, maxLen);
      return v;
    };
    stopIds[i]    = readField("stop",  STOP_ID_LENGTH);
    stopLabels[i] = readField("label", STOP_LABEL_LENGTH);
  }
}

void handleWebServer() {
  WiFiClient client = server.accept();
  if (!client) return;

  String requestLine;
  int contentLength = 0;
  bool isPost = false;
  bool savedParam = false;

  // Read headers — yield() lets the CYW43 driver process packets between reads
  String line;
  unsigned long deadline = millis() + 3000;
  while (client.connected() && millis() < deadline) {
    if (!client.available()) { yield(); continue; }
    char c = client.read();
    if (c == '\n') {
      line.trim();
      if (line.length() == 0) break; // blank line = end of headers
      if (requestLine.length() == 0) {
        requestLine = line;
        isPost      = requestLine.startsWith("POST");
        savedParam  = requestLine.indexOf("saved=1") != -1;
      } else if (line.startsWith("Content-Length:")) {
        contentLength = line.substring(15).toInt();
      }
      line = "";
    } else if (c != '\r') {
      line += c;
    }
  }

  // Read POST body
  String body;
  if (isPost && contentLength > 0) {
    unsigned long bodyDeadline = millis() + 2000;
    while ((int)body.length() < contentLength && millis() < bodyDeadline) {
      if (client.available()) body += (char)client.read();
      else yield();
    }
  }

  if (isPost && requestLine.indexOf("/config") != -1) {
    parsePostData(body);
    saveToEEPROM();
    client.println("HTTP/1.1 303 See Other");
    client.println("Location: /?saved=1");
    client.println("Connection: close");
    client.println();
  } else {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=UTF-8");
    client.println("Connection: close");
    client.println();
    client.println(generateHtmlPage(savedParam));
  }

  client.stop();
}

String generateHtmlPage(bool saved) {
  String h;
  h.reserve(2400);

  h += "<!DOCTYPE html><html><head>"
       "<meta charset='UTF-8'>"
       "<meta name='viewport' content='width=device-width,initial-scale=1'>"
       "<title>PicoBus Config</title>"
       "<style>"
       "body{font-family:sans-serif;max-width:540px;margin:20px auto;padding:0 16px;color:#222}"
       "h1{color:#c00;margin-bottom:4px}"
       ".status{background:#f5f5f5;border-radius:6px;padding:8px 12px;margin-bottom:16px;font-size:.9em}"
       ".saved{background:#d4edda;color:#155724;border-radius:6px;padding:8px 12px;margin-bottom:16px}"
       "table{width:100%;border-collapse:collapse;margin-top:4px}"
       "th{text-align:left;padding:6px 8px;background:#eee;font-size:.82em;white-space:nowrap}"
       "td{padding:4px 6px;vertical-align:middle}"
       "td:first-child{width:18px;color:#888;font-size:.85em;text-align:center}"
       "input[type=text]{width:100%;box-sizing:border-box;padding:5px 6px;"
         "border:1px solid #ccc;border-radius:3px;font-size:.95em}"
       ".hint{color:#888;font-size:.8em;margin-top:10px}"
       "button{margin-top:14px;padding:9px 28px;background:#c00;color:#fff;"
         "border:none;border-radius:4px;font-size:1em;cursor:pointer}"
       "button:hover{background:#900}"
       "</style></head><body>";

  h += "<h1>PicoBus Config</h1>";
  h += "<div class='status'>IP: ";
  h += WiFi.localIP().toString();
  h += " &bull; Showing stop ";
  h += String(currentStopIndex + 1);
  h += " of ";
  h += String(MAX_STOPS);
  h += "</div>";

  if (saved) h += "<div class='saved'>&#10003; Configuration saved!</div>";

  h += "<form action='/config' method='POST'>"
       "<table>"
       "<tr><th>#</th><th>Label (LCD top row, 12 chars)</th><th>TfL Stop ID</th></tr>";

  for (int i = 0; i < MAX_STOPS; i++) {
    h += "<tr><td>";
    h += String(i + 1);
    h += "</td><td><input type='text' name='label";
    h += String(i);
    h += "' value='";
    h += htmlEscape(stopLabels[i]);
    h += "' maxlength='12' placeholder='e.g. Aldgate E'></td>"
         "<td><input type='text' name='stop";
    h += String(i);
    h += "' value='";
    h += htmlEscape(stopIds[i]);
    h += "' maxlength='20' placeholder='e.g. 490014211E'></td></tr>";
  }

  h += "</table>"
       "<p class='hint'>Leave Stop ID blank to skip that slot. "
       "Works for bus stops and tube stations. "
       "Find stop IDs at <a href='https://tfl.gov.uk/plan-a-journey' target='_blank'>tfl.gov.uk</a> "
       "or via the TfL API StopPoint search.</p>"
       "<button type='submit'>Save &amp; Apply</button>"
       "</form></body></html>";

  return h;
}