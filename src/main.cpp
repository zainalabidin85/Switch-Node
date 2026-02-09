/**************************************************************
 * ESP32 Single Relay Controller
 *
 * Flow:
 *  - If STA connect succeeds -> serve:
 *      "/"         -> /www/index.html (relay control + gear icon)
 *      "/settings" -> /www/settings.html (MQTT config)
 *      APIs: /api/status, /api/relay, /api/mqtt
 *  - If STA connect fails (or no creds) -> AP captive portal:
 *      any URL -> /www/ap.html (Wi-Fi setup)
 *      API: /api/wifi  (save SSID/pass then reboot)
 *
 * Pins:
 *  - Relay GPIO: 16
 *  - Input GPIO: 25 (INPUT_PULLUP, dry contact to GND)
 *
 * mDNS:
 *  - Unique hostname: relaynode-XXXXXX.local  (XXXXXX = last 6 hex of MAC)
 *  - Sets WiFi hostname to match
 *  - Adds http service on tcp/80
 *  - Exposed in /api/status as "mdns"
 *
 * UI files expected in LittleFS:
 *  /www/ap.html
 *  /www/index.html
 *  /www/settings.html
 *  /www/app.js
 *  /www/style.css
 **************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <ESPmDNS.h>

/* ---------- GPIO ---------- */
#define RELAY_PIN 16
#define INPUT_PIN 25
#define RELAY_ACTIVE_LOW 0
/* -------------------------- */

static const char* FS_ROOT = "/www";
static const byte DNS_PORT = 53;

/* Input debounce (dry contact) */
static const uint32_t INPUT_DEBOUNCE_MS = 50;

AsyncWebServer server(80);
DNSServer dns;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

/* ---------- State ---------- */
bool relayState = false;

/* Dry-contact debounced input state (INPUT_PULLUP) */
static int  in_last_read   = HIGH;
static int  in_stable      = HIGH;
static uint32_t in_last_change_ms = 0;

/* IDs */
String deviceId;     // e.g. esp32-AB12CD
String shortId;      // e.g. AB12CD
String mdnsHost;     // e.g. relaynode-AB12CD
String mdnsFqdn;     // e.g. relaynode-AB12CD.local

/* ---------- WiFi config (stored by us) ---------- */
struct WifiCfg {
  String ssid;
  String pass;
} wifiCfg;

/* ---------- MQTT config ---------- */
struct MqttCfg {
  bool enabled = false;
  String host;
  uint16_t port = 1883;
  String user;
  String pass;        // stored; masked in GET
  String cmdTopic;    // REQUIRED when enabled
  String stateTopic;  // optional; if empty, use <cmdTopic>/state
} mqttCfg;

String topicCmd, topicState, topicDin;

/* ---------- Helpers ---------- */
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
  topicCmd = mqttCfg.cmdTopic;
  if (mqttCfg.stateTopic.length()) topicState = mqttCfg.stateTopic;
  else topicState = mqttCfg.cmdTopic + "/state";
  topicDin = mqttCfg.cmdTopic + "/din";
}

static void setRelay(bool on) {
  relayState = on;
  const int level = RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(RELAY_PIN, level);

  if (mqtt.connected()) {
    mqtt.publish(topicState.c_str(), relayState ? "ON" : "OFF", true);
  }
}

/* INPUT_PULLUP: HIGH=open, LOW=closed-to-GND */
static bool inputIsOpenRaw() {
  return digitalRead(INPUT_PIN) == HIGH;
}

/* Publish input: open->OFF, closed->ON (same as before) */
static void publishInputOpenBool(bool open) {
  if (!mqtt.connected()) return;
  mqtt.publish(topicDin.c_str(), open ? "OFF" : "ON", true);
}

/* Compute desired relay from stable input */
static bool desiredRelayFromInputStable() {
  // closed (LOW) => ON, open (HIGH) => OFF
  return (in_stable == LOW);
}

/* ---------- Preferences ---------- */
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

/* ---------- WiFi modes ---------- */
static bool connectSTA(uint32_t timeoutMs = 20000) {
  if (!wifiCfg.ssid.length()) return false;

  WiFi.mode(WIFI_STA);

  // set hostname before connect
  WiFi.setHostname(mdnsHost.c_str());

  WiFi.begin(wifiCfg.ssid.c_str(), wifiCfg.pass.c_str());

  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(250);
  }
  return false;
}

static void startAPPortal() {
  const String apSsid = "RelayNode-" + deviceId; // matches label user saw before
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), nullptr); // open AP
  delay(200);

  const IPAddress ip = WiFi.softAPIP();
  dns.start(DNS_PORT, "*", ip);

  Serial.println("AP Mode SSID: " + apSsid);
  Serial.println("AP IP: " + ip.toString());
}

/* ---------- mDNS ---------- */
static void startMDNS() {
  if (MDNS.begin(mdnsHost.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://" + mdnsFqdn + "/");
  } else {
    Serial.println("mDNS start failed");
  }
}

/* ---------- MQTT ---------- */
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();

  // NOTE: Physical dry-contact is MASTER. Remote commands may be overridden by input.
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
  if (!mqttReady()) return;
  if (mqtt.connected()) return;

  mqtt.setServer(mqttCfg.host.c_str(), mqttCfg.port);
  mqtt.setCallback(mqttCallback);

  const String clientId = mdnsHost + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok;
  if (mqttCfg.user.length()) ok = mqtt.connect(clientId.c_str(), mqttCfg.user.c_str(), mqttCfg.pass.c_str());
  else                       ok = mqtt.connect(clientId.c_str());

  if (ok) {
    mqtt.subscribe(topicCmd.c_str());
    mqtt.publish(topicState.c_str(), relayState ? "ON" : "OFF", true);
    publishInputOpenBool(in_stable == HIGH);
  }
}

/* ---------- Web routes ---------- */
static void setupRoutes_AP() {
  server.onNotFound([](AsyncWebServerRequest *r){
    r->send(LittleFS, "/www/ap.html", "text/html");
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(LittleFS, "/www/ap.html", "text/html");
  });

  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *r){
    auto v = [&](const char* k)->String{
      if (r->hasParam(k, true)) return r->getParam(k, true)->value();
      return "";
    };

    const String ssid = v("ssid");
    const String pass = v("pass");

    if (!ssid.length()) {
      r->send(400, "application/json", "{\"ok\":false,\"err\":\"ssid_required\"}");
      return;
    }

    wifiCfg.ssid = ssid;
    wifiCfg.pass = pass;
    saveWifiCfg();

    r->send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    delay(350);
    ESP.restart();
  });

  server.serveStatic("/", LittleFS, FS_ROOT);
  server.begin();
}

static void setupRoutes_STA() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(LittleFS, "/www/index.html", "text/html");
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(LittleFS, "/www/settings.html", "text/html");
  });

  server.serveStatic("/", LittleFS, FS_ROOT);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r){
    StaticJsonDocument<380> d;
    d["ok"] = true;
    d["ip"] = WiFi.localIP().toString();
    d["mdns"] = mdnsFqdn;
    d["relay"] = relayState;
    d["input_pressed"] = (in_stable == LOW);     // closed contact = "pressed"
    d["mqtt_enabled"] = mqttCfg.enabled;
    d["mqtt_connected"] = mqtt.connected();
    d["cmd_topic"] = mqttCfg.cmdTopic;

    String out;
    serializeJson(d, out);
    r->send(200, "application/json", out);
  });

  server.on("/api/relay", HTTP_POST, [](AsyncWebServerRequest *r){
    if (!r->hasParam("state", true)) {
      r->send(400, "application/json", "{\"ok\":false,\"err\":\"missing_state\"}");
      return;
    }
    const String s = r->getParam("state", true)->value();
    const bool on = (s == "1" || s.equalsIgnoreCase("on") || s.equalsIgnoreCase("true"));

    // NOTE: Physical dry-contact is MASTER; loop() will enforce it.
    setRelay(on);

    r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *r){
    StaticJsonDocument<420> d;
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

  server.on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest *r){
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

    if (mqtt.connected()) mqtt.disconnect();

    r->send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
}

/* ---------- setup/loop ---------- */
enum Mode { MODE_AP, MODE_STA };
Mode modeNow = MODE_AP;

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(INPUT_PIN, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
  }

  deviceId = macToDeviceId();
  shortId  = macSuffix6();
  mdnsHost = "relaynode-" + shortId;
  mdnsFqdn = mdnsHost + ".local";

  loadWifiCfg();
  loadMqttCfg();

  // Initialize debounced input state
  in_last_read = digitalRead(INPUT_PIN);
  in_stable = in_last_read;
  in_last_change_ms = millis();

  // On boot: relay follows dry-contact state
  setRelay(desiredRelayFromInputStable());

  Serial.println("\nDevice ID: " + deviceId);
  Serial.println("mDNS host:  " + mdnsHost);

  if (connectSTA(20000)) {
    modeNow = MODE_STA;
    Serial.println("STA connected, IP: " + WiFi.localIP().toString());

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

  // STA mode
  mqttEnsureConnected();
  mqtt.loop();

  // ---- Dry contact debounce + enforce relay follows input ----
  int level = digitalRead(INPUT_PIN); // HIGH=open, LOW=closed
  uint32_t now = millis();

  if (level != in_last_read) {
    in_last_read = level;
    in_last_change_ms = now;
  }

  // accept stable change after debounce time
  if ((now - in_last_change_ms) > INPUT_DEBOUNCE_MS && in_stable != in_last_read) {
    in_stable = in_last_read;

    // publish input state on change
    publishInputOpenBool(in_stable == HIGH);
  }

  // continuously enforce: physical switch is master
  bool desired = desiredRelayFromInputStable();
  if (relayState != desired) {
    setRelay(desired);
  }

  delay(10);
}
