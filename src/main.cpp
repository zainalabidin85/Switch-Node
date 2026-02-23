/**************************************************************
 * ESP32 Single Relay Controller (STA UI protected with HTTP Basic Auth)
 *
 * ✅ Basic Auth (always ON in STA mode)
 *    - Protects: "/", "/settings", "/api/*", and static files under /www
 *    - AP captive portal remains OPEN for provisioning
 *
 * Basic Auth credentials:
 *   USER: admin
 *   PASS: switchnode
 *
 * Pins:
 *  - Relay GPIO: 16 (ACTIVE HIGH by default)
 *  - Input GPIO: 25 (INPUT_PULLUP, dry contact to GND)
 *
 * DEBUG:
 *  - Verbose WiFi connect status prints + event-based disconnect reasons
 *  - Prints stored SSID + password length at boot
 *  - AP captive portal probe endpoints handled to reduce VFS "open()" spam
 **************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <dirent.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <ESPmDNS.h>
#include "esp_wifi.h"

// -------------------- GPIO --------------------
#define RELAY_PIN 16
#define INPUT_PIN 25
#define RELAY_ACTIVE_LOW 0   // 0 = ACTIVE HIGH, 1 = ACTIVE LOW

// -------------------- FS/DNS ------------------
static const char* FS_ROOT = "/www";
static const byte DNS_PORT = 53;

// -------------------- Debounce ----------------
static const uint32_t INPUT_DEBOUNCE_MS = 50;

// -------------------- Web/MQTT ----------------
AsyncWebServer server(80);
DNSServer dns;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

// -------------------- BASIC AUTH (STA) --------
static const bool  BASIC_AUTH_ON = true;
static const char* BASIC_USER   = "admin";
static const char* BASIC_PASS   = "switchnode";

// -------------------- State -------------------
bool relayState = false;

// Debounced input state (INPUT_PULLUP)
static int in_last_read = HIGH;
static int in_stable = HIGH;
static uint32_t in_last_change_ms = 0;

// IDs
String deviceId;
String shortId;
String mdnsHost;
String mdnsFqdn;

// WiFi config
struct WifiCfg {
  String ssid;
  String pass;
} wifiCfg;

// MQTT config
struct MqttCfg {
  bool enabled = false;
  String host;
  uint16_t port = 1883;
  String user;
  String pass;
  String cmdTopic;
  String stateTopic;
} mqttCfg;

String topicCmd, topicState, topicDin;

// -------------------- Helpers -----------------
static String macToDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "esp32-%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

static String macSuffix6() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

static void applyTopics() {
  topicCmd   = mqttCfg.cmdTopic;
  topicState = mqttCfg.stateTopic.length() ? mqttCfg.stateTopic : (mqttCfg.cmdTopic + "/state");
  topicDin   = mqttCfg.cmdTopic + "/din";
}

// -------------------- Debug WiFi --------------------
static const char* wlStatusStr(wl_status_t st) {
  switch (st) {
    case WL_NO_SHIELD:       return "WL_NO_SHIELD";
    case WL_IDLE_STATUS:     return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:   return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:       return "WL_CONNECTED";
    case WL_CONNECT_FAILED:  return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:    return "WL_DISCONNECTED";
    default: return "WL_UNKNOWN";
  }
}

static const char* wifiDiscReasonStr(int reason) {
  // Common reasons (not exhaustive)
  switch (reason) {
    case 1:  return "UNSPECIFIED";
    case 2:  return "AUTH_EXPIRE";
    case 3:  return "AUTH_LEAVE";
    case 4:  return "ASSOC_EXPIRE";
    case 5:  return "ASSOC_TOOMANY";
    case 6:  return "NOT_AUTHED";
    case 7:  return "NOT_ASSOCED";
    case 8:  return "ASSOC_LEAVE";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17: return "IE_IN_4WAY_DIFFERS";
    case 18: return "GROUP_CIPHER_INVALID";
    case 19: return "PAIRWISE_CIPHER_INVALID";
    case 20: return "AKMP_INVALID";
    case 21: return "UNSUPP_RSN_IE_VERSION";
    case 22: return "INVALID_RSN_IE_CAP";
    case 23: return "802_1X_AUTH_FAILED";
    case 24: return "CIPHER_SUITE_REJECTED";
    case 201:return "NO_AP_FOUND";
    case 202:return "AUTH_FAIL";
    case 203:return "ASSOC_FAIL";
    case 204:return "HANDSHAKE_TIMEOUT";
    default: return "UNKNOWN";
  }
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFiEvent] STA_CONNECTED");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFiEvent] GOT_IP: %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFiEvent] STA_DISCONNECTED reason=%d (%s)\n",
                    (int)info.wifi_sta_disconnected.reason,
                    wifiDiscReasonStr((int)info.wifi_sta_disconnected.reason));
      break;
    default:
      break;
  }
}

// -------------------- Basic Auth helpers (STA only) --------------------
static inline bool authOK(AsyncWebServerRequest *r) {
  if (!BASIC_AUTH_ON) return true;
  return r->authenticate(BASIC_USER, BASIC_PASS);
}

static inline bool requireAuthOr401(AsyncWebServerRequest *r) {
  if (authOK(r)) return true;
  r->requestAuthentication();
  return false;
}

// -------------------- Relay --------------------
static void setRelay(bool on) {
  relayState = on;
  const int level = RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(RELAY_PIN, level);

  Serial.printf("[RELAY] setRelay(%s) -> GPIO=%d\n", on ? "ON" : "OFF", level);

  if (mqtt.connected() && topicState.length()) {
    mqtt.publish(topicState.c_str(), relayState ? "ON" : "OFF", true);
    Serial.printf("[MQTT] publish state %s => %s\n", topicState.c_str(), relayState ? "ON" : "OFF");
  }
}

// Input publishing (keep your ON/OFF semantics, but log it)
static void publishInputOpenBool(bool open) {
  if (!mqtt.connected() || !topicDin.length()) return;
  const char* payload = open ? "OFF" : "ON";
  mqtt.publish(topicDin.c_str(), payload, true);
  Serial.printf("[MQTT] publish din %s => %s (open=%d)\n", topicDin.c_str(), payload, open ? 1 : 0);
}

// -------------------- Preferences --------------------
static void loadWifiCfg() {
  prefs.begin("wifi", true);
  wifiCfg.ssid = prefs.getString("ssid", "");
  wifiCfg.pass = prefs.getString("pass", "");
  prefs.end();
}

static void saveWifiCfg() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", wifiCfg.ssid);
  prefs.putString("pass", wifiCfg.pass);
  prefs.end();
}

static void loadMqttCfg() {
  prefs.begin("mqtt", true);
  mqttCfg.enabled    = prefs.getBool("en", false);
  mqttCfg.host       = prefs.getString("host", "");
  mqttCfg.port       = prefs.getUShort("port", 1883);
  mqttCfg.user       = prefs.getString("user", "");
  mqttCfg.pass       = prefs.getString("pass", "");
  mqttCfg.cmdTopic   = prefs.getString("cmd", "");
  mqttCfg.stateTopic = prefs.getString("st", "");
  prefs.end();
  applyTopics();
}

static void saveMqttCfg() {
  prefs.begin("mqtt", false);
  prefs.putBool("en", mqttCfg.enabled);
  prefs.putString("host", mqttCfg.host);
  prefs.putUShort("port", mqttCfg.port);
  prefs.putString("user", mqttCfg.user);
  prefs.putString("pass", mqttCfg.pass);
  prefs.putString("cmd",  mqttCfg.cmdTopic);
  prefs.putString("st",   mqttCfg.stateTopic);
  prefs.end();
}

// -------------------- WiFi --------------------
static bool connectSTA(uint32_t timeoutMs = 20000) {
  if (!wifiCfg.ssid.length()) {
    Serial.println("[WiFi] No SSID saved.");
    return false;
  }

  Serial.println("[WiFi] connectSTA()");
  Serial.println("[WiFi] Saved SSID = [" + wifiCfg.ssid + "]");
  Serial.println("[WiFi] Saved PASS length = " + String(wifiCfg.pass.length()));

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(mdnsHost.c_str());
  WiFi.setAutoReconnect(true);

  // Reset previous state
  WiFi.disconnect(true, true);
  delay(200);

  Serial.println("[WiFi] Connecting...");
  WiFi.begin(wifiCfg.ssid.c_str(), wifiCfg.pass.c_str());

  wl_status_t last = WL_IDLE_STATUS;
  uint32_t t0 = millis();

  while (millis() - t0 < timeoutMs) {
    wl_status_t st = WiFi.status();
    if (st != last) {
      last = st;
      Serial.printf("[WiFi] status=%d (%s)\n", (int)st, wlStatusStr(st));
    }
    if (st == WL_CONNECTED) {
      Serial.printf("[WiFi] Connected! IP=%s RSSI=%d\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      return true;
    }
    delay(250);
  }

  Serial.printf("[WiFi] Timeout. Final status=%d (%s)\n",
                (int)WiFi.status(), wlStatusStr(WiFi.status()));
  Serial.printf("[WiFi] RSSI (if any)=%d\n", WiFi.RSSI());
  return false;
}

static void startAPPortal() {
  // Ensure STA is stopped
  WiFi.disconnect(true, true);
  delay(200);

  const String apSsid = "SwitchNode-" + deviceId;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), nullptr);
  delay(200);

  const IPAddress ip = WiFi.softAPIP();
  dns.start(DNS_PORT, "*", ip);

  Serial.println("[AP] Mode SSID: " + apSsid);
  Serial.println("[AP] IP: " + ip.toString());
}

// -------------------- mDNS --------------------
static void startMDNS() {
  if (MDNS.begin(mdnsHost.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://" + mdnsFqdn + "/");
  } else {
    Serial.println("[mDNS] start failed");
  }
}

// -------------------- MQTT --------------------
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();

  Serial.printf("[MQTT] RX topic=%s payload=%s\n", topic, msg.c_str());

  if (String(topic) == topicCmd) {
    if (msg.equalsIgnoreCase("ON") || msg == "1" || msg.equalsIgnoreCase("true")) setRelay(true);
    if (msg.equalsIgnoreCase("OFF") || msg == "0" || msg.equalsIgnoreCase("false")) setRelay(false);
  }
}

static bool mqttReady() {
  if (!mqttCfg.enabled) return false;
  if (!mqttCfg.host.length()) return false;
  if (!mqttCfg.cmdTopic.length()) return false;
  return true;
}

static void mqttEnsureConnected() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Hard OFF when disabled
  if (!mqttCfg.enabled) {
    if (mqtt.connected()) {
      Serial.println("[MQTT] Disabled -> disconnect");
      mqtt.disconnect();
    }
    return;
  }

  if (!mqttReady()) return;
  if (mqtt.connected()) return;

  mqtt.setServer(mqttCfg.host.c_str(), mqttCfg.port);
  mqtt.setCallback(mqttCallback);

  const String clientId = mdnsHost + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.printf("[MQTT] Connecting to %s:%u user=%s\n",
                mqttCfg.host.c_str(),
                mqttCfg.port,
                mqttCfg.user.length() ? mqttCfg.user.c_str() : "(none)");

  bool ok;
  if (mqttCfg.user.length()) ok = mqtt.connect(clientId.c_str(), mqttCfg.user.c_str(), mqttCfg.pass.c_str());
  else                       ok = mqtt.connect(clientId.c_str());

  if (ok) {
    Serial.println("[MQTT] Connected.");
    mqtt.subscribe(topicCmd.c_str());
    Serial.printf("[MQTT] Subscribed: %s\n", topicCmd.c_str());

    mqtt.publish(topicState.c_str(), relayState ? "ON" : "OFF", true);
    Serial.printf("[MQTT] Published retained state: %s=%s\n", topicState.c_str(), relayState ? "ON" : "OFF");

    publishInputOpenBool(in_stable == HIGH);
  } else {
    Serial.printf("[MQTT] Connect failed, rc=%d\n", mqtt.state());
  }
}

// -------------------- Web routes --------------------
static void setupRoutes_AP() {
  // Captive portal probe endpoints (avoid LittleFS "open()" errors)
  server.on("/connecttest.txt", HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/ncc.txt",         HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/generate_204",    HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/hotspot-detect.html", HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/fwlink",          HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/canonical.html",  HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/success.txt",     HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/library/test/success.html", HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/redirect",        HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/ncsi.txt",        HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  server.on("/chromehotstart.crx", HTTP_ANY, [](AsyncWebServerRequest *r){ r->redirect("/"); });
  
  // Handle any .txt or .crx files
  server.onNotFound([](AsyncWebServerRequest *r){
    r->redirect("/");
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(LittleFS, "/www/ap.html", "text/html");
  });

  // WiFi scan endpoint
  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *r){
    Serial.println("[AP] Scanning WiFi networks...");
    int n = WiFi.scanNetworks();
    String json = "{\"networks\":[";
    
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
      json += "\"encryption\":\"" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "SECURE") + "\"}";
    }
    
    json += "]}";
    r->send(200, "application/json", json);
    WiFi.scanDelete();
  });

  // Add this after the /api/scan endpoint, before /api/wifi
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r){
    // In AP mode, just return the device ID and mDNS info
    String json = "{\"ok\":true,\"mdns\":\"" + mdnsFqdn + "\"}";
    r->send(200, "application/json", json);
  });

  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *r){
    Serial.println("[AP] /api/wifi POST received");
    
    // Log all parameters received
    int params = r->params();
    Serial.printf("[AP] Number of params: %d\n", params);
    for (int i = 0; i < params; i++) {
        AsyncWebParameter* p = r->getParam(i);
        if (p->isPost()) {
            Serial.printf("[AP] POST param: %s = %s\n", 
                p->name().c_str(), 
                p->value().c_str());
        }
    }
    
    auto v = [&](const char* k)->String{
        if (r->hasParam(k, true)) {
            String val = r->getParam(k, true)->value();
            Serial.printf("[AP] Found param %s = %s\n", k, val.c_str());
            return val;
        }
        Serial.printf("[AP] Param %s not found!\n", k);
        return "";
    };

    const String ssid = v("ssid");
    const String pass = v("pass");

    Serial.println("[AP] /api/wifi POST ssid=[" + ssid + "] passLen=" + String(pass.length()));

    if (!ssid.length()) {
        Serial.println("[AP] ERROR: ssid required but not provided");
        r->send(400, "application/json", "{\"ok\":false,\"err\":\"ssid_required\"}");
        return;
    }

    wifiCfg.ssid = ssid;
    wifiCfg.pass = pass;
    saveWifiCfg();
    
    Serial.println("[AP] WiFi credentials saved, sending response and rebooting...");
    r->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    
    // Flush the response before rebooting
    delay(500);
    Serial.println("[AP] Rebooting now...");
    ESP.restart();
  });

  server.onNotFound([](AsyncWebServerRequest *r){
    r->send(LittleFS, "/www/ap.html", "text/html");
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(LittleFS, "/www/ap.html", "text/html");
  });

  server.serveStatic("/", LittleFS, FS_ROOT); // AP is open
  server.begin();

  Serial.println("[AP] Web server started (open).");
}

static void setupRoutes_STA() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!requireAuthOr401(r)) return;
    r->send(LittleFS, "/www/index.html", "text/html");
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!requireAuthOr401(r)) return;
    r->send(LittleFS, "/www/settings.html", "text/html");
  });

  // Static under auth
  {
    auto &h = server.serveStatic("/", LittleFS, FS_ROOT);
    h.setFilter([](AsyncWebServerRequest *r){
      return authOK(r);
    });
  }

  // Status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!requireAuthOr401(r)) return;

    StaticJsonDocument<640> d;
    d["ok"] = true;
    d["ip"] = WiFi.localIP().toString();
    d["mdns"] = mdnsFqdn;
    d["rssi"] = WiFi.RSSI();
    d["relay"] = relayState;
    d["input_pressed"] = (in_stable == LOW);
    d["mqtt_enabled"] = mqttCfg.enabled;
    d["mqtt_connected"] = mqtt.connected();
    d["cmd_topic"] = mqttCfg.cmdTopic;
    d["state_topic"] = topicState;
    d["din_topic"] = topicDin;

    String out;
    serializeJson(d, out);
    r->send(200, "application/json", out);
  });

  // Relay set
  server.on("/api/relay", HTTP_POST, [](AsyncWebServerRequest *r){
    if (!requireAuthOr401(r)) return;

    if (!r->hasParam("state", true)) {
      r->send(400, "application/json", "{\"ok\":false,\"err\":\"missing_state\"}");
      return;
    }
    const String s = r->getParam("state", true)->value();
    const bool on = (s == "1" || s.equalsIgnoreCase("on") || s.equalsIgnoreCase("true"));
    setRelay(on);
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // MQTT GET (masked)
  server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!requireAuthOr401(r)) return;

    StaticJsonDocument<520> d;
    d["ok"] = true;
    d["enabled"] = mqttCfg.enabled;
    d["host"] = mqttCfg.host;
    d["port"] = mqttCfg.port;
    d["user"] = mqttCfg.user;
    d["pass_set"] = mqttCfg.pass.length() > 0;
    d["cmdTopic"] = mqttCfg.cmdTopic;
    d["stateTopic"] = mqttCfg.stateTopic;

    String out;
    serializeJson(d, out);
    r->send(200, "application/json", out);
  });

  // MQTT POST
  server.on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest *r){
    if (!requireAuthOr401(r)) return;

    auto v = [&](const char* k)->String{
      if (r->hasParam(k, true)) return r->getParam(k, true)->value();
      return "";
    };

    const String enS = v("enabled");
    mqttCfg.enabled = (enS == "1" || enS.equalsIgnoreCase("true") || enS.equalsIgnoreCase("on"));

    mqttCfg.host = v("host");

    long p = v("port").toInt();
    if (p <= 0 || p > 65535) p = 1883;
    mqttCfg.port = (uint16_t)p;

    mqttCfg.user = v("user");
    const String pass = v("pass");
    if (pass.length()) mqttCfg.pass = pass;

    mqttCfg.cmdTopic = v("cmdTopic");
    mqttCfg.stateTopic = v("stateTopic");

    saveMqttCfg();
    applyTopics();

    if (!mqttCfg.enabled && mqtt.connected()) mqtt.disconnect();
    if (mqtt.connected()) mqtt.disconnect(); // force reconnect with new params

    r->send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
  Serial.println("[STA] Web server started (Basic Auth ON).");
}

void listFiles(const char* dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\r\n", dirname);
  
  // Ensure the path starts with /
  String path = dirname;
  if (!path.startsWith("/")) {
    path = "/" + path;
  }
  
  File root = LittleFS.open(path);
  if (!root) {
    Serial.printf("- failed to open directory: %s\r\n", path.c_str());
    return;
  }
  if (!root.isDirectory()) {
    Serial.println(" - not a directory");
    return;
  }
  
  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels) {
        listFiles(file.name(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

// -------------------- setup/loop --------------------
enum Mode { MODE_AP, MODE_STA };
Mode modeNow = MODE_AP;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== SwitchNode boot ===");

  WiFi.onEvent(onWiFiEvent);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(INPUT_PIN, INPUT_PULLUP);

  setRelay(false);

  // Safer: do NOT format on fail in production.
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed (formatted if needed).");
  } else {
    Serial.println("[FS] LittleFS mounted.");
    listFiles("/", 2);
    listFiles("/www", 1);  // Explicitly list www directory
  // Add debug file system check
  }

  deviceId = macToDeviceId();
  shortId  = macSuffix6();
  mdnsHost = "switchnode-" + shortId;
  mdnsFqdn = mdnsHost + ".local";

  loadWifiCfg();
  loadMqttCfg();

  in_last_read = digitalRead(INPUT_PIN);
  in_stable = in_last_read;
  in_last_change_ms = millis();

  Serial.println("[ID] Device ID: " + deviceId);
  Serial.println("[ID] mDNS host:  " + mdnsHost);
  Serial.println(String("[AUTH] ") + (BASIC_AUTH_ON ? "ENABLED" : "disabled") + " user=" + BASIC_USER);

  if (connectSTA(20000)) {
    modeNow = MODE_STA;
    Serial.println("[WiFi] STA connected, IP: " + WiFi.localIP().toString());
    startMDNS();
    setupRoutes_STA();
  } else {
    modeNow = MODE_AP;
    startAPPortal();
    setupRoutes_AP();
  }
}

void loop() {
  if (modeNow == MODE_AP) {
    dns.processNextRequest();
    delay(10);
    return;
  }

  mqttEnsureConnected();
  mqtt.loop();

  // Dry contact debounce (INPUT_PULLUP)
  int level = digitalRead(INPUT_PIN);
  uint32_t now = millis();

  if (level != in_last_read) {
    in_last_read = level;
    in_last_change_ms = now;
  }

  if ((now - in_last_change_ms) > INPUT_DEBOUNCE_MS && in_stable != in_last_read) {
    in_stable = in_last_read;

    const bool isOpen = (in_stable == HIGH);
    Serial.printf("[DIN] stable change -> %s\n", isOpen ? "OPEN(HIGH)" : "CLOSED(LOW)");

    publishInputOpenBool(isOpen);

    // IMPORTANT: toggle only on press/close (LOW) to avoid double-toggle on release
    if (!isOpen) {
      setRelay(!relayState);
    }
  }

  delay(10);
}
