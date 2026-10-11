#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <math.h>
#include <stdlib.h>

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Use the local IP of Home Assistant
// This HTTP example sends the HA token over your LOCAL NETWORK unencrypted.
// Keep it on a trusted LAN, do not expose this device publicly.
const char* HA_BASE_URL = "http://192.168.1.20:8123";
const char* HA_PM25_ENTITY = "sensor.replace_with_your_pm25_sensor";
// Create this under Home Assistant Profile > Security > Long-Lived Access Tokens.
// The ESPHome API encryption key is NOT a Home Assistant REST token.
const char* HA_TOKEN = "PASTE_HOME_ASSISTANT_LONG_LIVED_ACCESS_TOKEN_HERE";

// New credentials.
const char* WEB_USER = "fan";
const char* WEB_PASSWORD = "CHANGE_TO_A_PRIVATE_WEB_PASSWORD";
const char* OTA_PASSWORD = "CHANGE_TO_A_PRIVATE_OTA_PASSWORD";
const char* HOSTNAME = "pm25-fan-c3";
// ===============================

constexpr uint8_t PWM_PIN = 3;
constexpr uint8_t TACH_PIN = 4;
constexpr uint32_t PWM_FREQUENCY_HZ = 25000;
constexpr uint8_t PWM_RESOLUTION_BITS = 8;  // 0..255
constexpr uint32_t PM_POLL_MS = 200;
constexpr uint32_t PM_TIMEOUT_MS = 90000;   // stale PM data -> 100% in Auto
constexpr uint32_t RPM_MEASURE_MS = 5000;
constexpr uint8_t TACH_PULSES_PER_REV = 2;  // standard PC fan

WebServer server(80);
Preferences settings;

bool automaticMode = true;
uint8_t manualSpeed = 50;
uint8_t appliedSpeed = 100;
float pm25 = NAN;
bool pmAvailable = false;
uint32_t lastPMReadMs = 0;
uint32_t lastPMPollMs = 0;
uint32_t lastRPMMeasureMs = 0;
uint32_t lastWifiAttemptMs = 0;
bool wasWifiConnected = false;
bool pwmReady = false;
uint32_t fanRPM = 0;

volatile uint32_t tachPulses = 0;
volatile uint32_t lastTachPulseUs = 0;

void IRAM_ATTR tachInterrupt() {
  // Suppress electrical noise; 150us is much shorter than a normal PC fan pulse.
  const uint32_t nowUs = micros();
  if ((uint32_t)(nowUs - lastTachPulseUs) >= 150) {
    ++tachPulses;
    lastTachPulseUs = nowUs;
  }
}

uint8_t autoSpeedForPM(float pm) {
  if (pm < 5.0f) return 0;
  if (pm < 12.0f) return 0;
  if (pm < 35.0f) return 20;
  if (pm < 75.0f) return 85;
  return 100;
}

bool pmIsFresh() {
  return pmAvailable && WiFi.status() == WL_CONNECTED &&
         (uint32_t)(millis() - lastPMReadMs) < PM_TIMEOUT_MS;
}

void applyFanSpeed() {
  uint8_t target = manualSpeed;
  if (automaticMode) {
    target = pmIsFresh() ? autoSpeedForPM(pm25) : 100;
  }
  if (target == appliedSpeed && pwmReady) return;
  appliedSpeed = target;
  if (pwmReady) {
    // NPN common-emitter inverts PWM: fan input HIGH = fan on.
    const uint32_t npnDuty = (uint32_t)((100UL - appliedSpeed) * 255UL / 100UL);
    ledcWrite(PWM_PIN, npnDuty); // Arduino-ESP32 core 3.x: pin, duty
  }
}

void readFanRPM() {
  const uint32_t now = millis();
  const uint32_t elapsed = (uint32_t)(now - lastRPMMeasureMs);
  if (elapsed < RPM_MEASURE_MS) return;
  noInterrupts();
  const uint32_t pulses = tachPulses;
  tachPulses = 0;
  interrupts();
  fanRPM = (uint32_t)((uint64_t)pulses * 60000ULL /
                      ((uint64_t)elapsed * TACH_PULSES_PER_REV));
  lastRPMMeasureMs = now;
}

// Reads Home Assistant REST: GET /api/states/<entity_id>
void pollHomeAssistant() {
  if (WiFi.status() != WL_CONNECTED) {
    pmAvailable = false;
    applyFanSpeed();
    return;
  }

  WiFiClient client;
  HTTPClient http;
  const String url = String(HA_BASE_URL) + "/api/states/" + HA_PM25_ENTITY;
  if (!http.begin(client, url)) {
    pmAvailable = false;
    applyFanSpeed();
    Serial.println("Home Assistant: invalid or unreachable URL");
    return;
  }
  http.setTimeout(2500);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  http.addHeader("Accept", "application/json");

  const int status = http.GET();
  bool valid = false;
  float newPM = NAN;
  if (status == HTTP_CODE_OK) {
    // A typical HA state payload is small; limit retained input size.
    String body = http.getString();
    if (body.length() < 16384) {
      JsonDocument doc;                 // ArduinoJson 7.x
      if (!deserializeJson(doc, body)) {
        const char* value = doc["state"] | "";
        char* end = nullptr;
        const float parsed = strtof(value, &end);
        if (end != value && *end == '\0' && isfinite(parsed) &&
            parsed >= 0.0f && parsed <= 2000.0f) {
          valid = true;
          newPM = parsed;
        }
      }
    }
  }
  http.end();

  pmAvailable = valid;
  if (valid) {
    pm25 = newPM;
    lastPMReadMs = millis(); // polling same unchanged reading counts as fresh
    Serial.printf("PM2.5: %.1f ug/m3\n", pm25);
  } else {
    Serial.printf("Home Assistant PM2.5 unavailable (HTTP %d). Check URL, token, entity.\n", status);
  }
  applyFanSpeed();
}

bool requireLogin() {
  if (server.authenticate(WEB_USER, WEB_PASSWORD)) return true;
  server.requestAuthentication(DIGEST_AUTH, "PM2.5 Fan Controller");
  return false;
}

void sendState() {
  JsonDocument out;
  out["auto"] = automaticMode;
  out["manual"] = manualSpeed;
  out["applied"] = appliedSpeed;
  out["rpm"] = fanRPM;
  out["pm25"] = pmIsFresh() ? pm25 : NAN; // ArduinoJson serializes NaN as null
  out["ha_ok"] = pmIsFresh();
  out["wifi_ok"] = WiFi.status() == WL_CONNECTED;
  String payload;
  serializeJson(out, payload);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", payload);
}

void setMode() {
  if (!requireLogin()) return;
  if (!server.hasArg("auto")) {
    server.send(400, "text/plain", "Missing auto parameter");
    return;
  }
  const String value = server.arg("auto");
  if (value != "0" && value != "1") {
    server.send(400, "text/plain", "auto must be 0 or 1");
    return;
  }
  automaticMode = (value == "1");
  settings.putBool("auto", automaticMode);
  applyFanSpeed();
  sendState();
}

void setManualSpeed() {
  if (!requireLogin()) return;
  if (!server.hasArg("speed")) {
    server.send(400, "text/plain", "Missing speed parameter");
    return;
  }
  const String value = server.arg("speed");
  char* end = nullptr;
  const long requested = strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0' || requested < 0 || requested > 100) {
    server.send(400, "text/plain", "speed must be an integer 0-100");
    return;
  }
  manualSpeed = (uint8_t)requested;
  settings.putUChar("manual", manualSpeed);
  applyFanSpeed();
  sendState();
}

// Phone interface: simple labels only, no Auto/Manual description text.
const char PAGE_HTML[] PROGMEM = R"HTMLPAGE(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#101c2b">
<title>PM2.5 Fan Controller</title>
<style>
:root{color-scheme:dark;font-family:system-ui,-apple-system,Segoe UI,sans-serif}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;background:#101c2b;color:#f4f7fb;padding:24px 14px}
main{max-width:420px;margin:auto}
header{display:flex;align-items:center;gap:12px;margin:4px 0 20px}
.logo{font-size:30px;width:50px;height:50px;display:grid;place-items:center;border-radius:14px;background:#24354b}
h1{font-size:21px;margin:0} .small{font-size:12px;color:#98a9bc;margin:4px 0 0}
.card{background:#1c2a3b;border:1px solid #314257;padding:20px;border-radius:18px;margin:12px 0}
.overline{font-size:11px;letter-spacing:.1em;color:#a8bdd2;font-weight:700}
.reading{font-size:39px;font-weight:750;letter-spacing:-1px;margin-top:7px}
.unit{font-size:14px;color:#b8c8d8;font-weight:400;letter-spacing:0}
.line{display:flex;justify-content:space-between;align-items:center;gap:10px}
.label{font-size:15px;font-weight:650}
.toggle{min-width:67px;padding:10px 14px;border:1px solid #687e96;background:#3d4d60;border-radius:11px;color:white;font-size:14px;font-weight:700;cursor:pointer}
.toggle.on{background:#2585ee;border-color:#2585ee}
#speed{width:100%;margin:18px 0 5px;accent-color:#4aabff;cursor:pointer}
#speed:disabled{opacity:.38;cursor:default}
.ends{display:flex;justify-content:space-between;font-size:12px;color:#9fb0c2}
.stats{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.stat{background:#1c2a3b;border:1px solid #314257;border-radius:16px;padding:16px}
.value{font-size:27px;font-weight:750;margin-top:6px}
.message{font-size:12px;color:#c2d1e0;text-align:center;min-height:16px;margin:16px 0}
</style>
</head>
<body>
<main>
<header><div class="logo">&#x1F32C;</div><div><h1>PM2.5 Fan Controller</h1><div class="small">ESP32-C3 &middot; NPN PWM Fan</div></div></header>
<section class="card"><div class="overline">CURRENT AIR QUALITY</div><div class="reading"><span id="pm">--</span> <span class="unit">&micro;g/m&sup3; PM2.5</span></div></section>
<section class="card"><div class="line"><span class="label">Automatic PM2.5 Mode</span><button id="mode" class="toggle">--</button></div></section>
<section class="card"><div class="line"><span class="label">Manual Fan Speed</span><strong id="speedNum">50%</strong></div><input type="range" id="speed" min="0" max="100" step="1" value="50"><div class="ends"><span>0%</span><span>100%</span></div></section>
<div class="stats"><section class="stat"><div class="overline">FAN RPM</div><div class="value" id="rpm">--</div></section><section class="stat"><div class="overline">FAN PWM</div><div class="value" id="pwm">--%</div></section></div>
<div id="message" class="message"></div>
</main>
<script>
const mode=document.getElementById('mode');
const speed=document.getElementById('speed');
const speedNum=document.getElementById('speedNum');
const message=document.getElementById('message');
let auto=true, dragging=false;
async function request(path,params){
 const res=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(params),cache:'no-store'});
 if(!res.ok)throw new Error('Request failed: '+res.status);
 return res.json();
}
function paint(s){
 auto=s.auto;
 mode.textContent=auto?'ON':'OFF';
 mode.classList.toggle('on',auto);
 speed.disabled=auto;
 if(!dragging){speed.value=s.manual;speedNum.textContent=s.manual+'%';}
 document.getElementById('pm').textContent=(s.ha_ok&&typeof s.pm25==='number')?s.pm25.toFixed(1):'--';
 document.getElementById('rpm').textContent=s.rpm;
 document.getElementById('pwm').textContent=s.applied+'%';
 message.textContent=s.ha_ok?'':'Home Assistant PM2.5 unavailable';
}
async function refresh(){
 try{const r=await fetch('/api/state',{cache:'no-store'});if(!r.ok)throw Error('Offline');paint(await r.json());}
 catch(e){message.textContent='Connection to fan controller lost';}
}
mode.addEventListener('click',async()=>{
 mode.disabled=true;
 try{paint(await request('/api/mode',{auto:auto?'0':'1'}));}
 catch(e){message.textContent='Could not change mode';}
 finally{mode.disabled=false;}
});
speed.addEventListener('pointerdown',()=>dragging=true);
speed.addEventListener('input',()=>{dragging=true;speedNum.textContent=speed.value+'%';});
speed.addEventListener('change',async()=>{
 const value=speed.value;
 try{paint(await request('/api/manual',{speed:value}));}
 catch(e){message.textContent='Could not set fan speed';}
 finally{dragging=false;}
});
refresh();setInterval(refresh,3000);
</script>
</body></html>
)HTMLPAGE";

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    if (!requireLogin()) return;
    server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
  });
  server.on("/api/state", HTTP_GET, []() {
    if (!requireLogin()) return;
    sendState();
  });
  server.on("/api/mode", HTTP_POST, setMode);
  server.on("/api/manual", HTTP_POST, setManualSpeed);
  server.onNotFound([]() {
    if (!requireLogin()) return;
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // NPN OFF = fan PWM line HIGH = usually full fan speed at startup.
  pinMode(PWM_PIN, OUTPUT);
  digitalWrite(PWM_PIN, LOW);
  pwmReady = ledcAttach(PWM_PIN, PWM_FREQUENCY_HZ, PWM_RESOLUTION_BITS);
  if (pwmReady) ledcWrite(PWM_PIN, 0);
  else Serial.println("ERROR: Cannot configure 25 kHz PWM on GPIO3");

  pinMode(TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachInterrupt, FALLING);
  lastRPMMeasureMs = millis();

  settings.begin("fan", false);
  automaticMode = settings.getBool("auto", true);
  manualSpeed = settings.getUChar("manual", 50);
  if (manualSpeed > 100) manualSpeed = 50;
  // Force the first duty update even when target is the default 100%.
  appliedSpeed = 255;
  applyFanSpeed();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  uint32_t beginMs = millis();
  while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - beginMs) < 12000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  setupWebServer();
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(HOSTNAME)) Serial.printf("mDNS: http://%s.local/\n", HOSTNAME);
    Serial.printf("Web UI: http://%s/\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Wi-Fi not connected; trying again in background.");
  }

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin(); // Arduino IDE network OTA (not ESPHome OTA)

  lastPMPollMs = millis() - PM_POLL_MS; // Poll immediately when Wi-Fi is ready
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  readFanRPM();

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected && wasWifiConnected) {
    pmAvailable = false;
    applyFanSpeed();
  }
  if (connected && !wasWifiConnected) {
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin(HOSTNAME);
    lastPMPollMs = millis() - PM_POLL_MS;
  }
  wasWifiConnected = connected;

  if (!connected && (uint32_t)(millis() - lastWifiAttemptMs) >= 15000) {
    lastWifiAttemptMs = millis();
    WiFi.reconnect();
  }
  if (connected && (uint32_t)(millis() - lastPMPollMs) >= PM_POLL_MS) {
    lastPMPollMs = millis();
    pollHomeAssistant();
  }
  // Re-evaluate Auto if the last measurement becomes stale.
  applyFanSpeed();
  delay(2);
}
