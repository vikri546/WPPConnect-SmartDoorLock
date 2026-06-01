
/*
 * ============================================================
 *  SmartLock — Face Recognition Access Control
 *  ESP32 Firmware  |  Version 3.0 — Hybrid Network
 * ============================================================
 *  Hardware:
 *    - ESP32 Dev Board
 *    - Servo Motor  → GPIO 18
 *    - Reed Switch  → GPIO 5   (door-closed sensor)
 *    - Buzzer       → GPIO 4
 *    - LED          → GPIO 2
 *
 *  Network: Dual AP+STA mode.
 *    - AP for local hardware control (always available)
 *    - STA connects to home/office WiFi for internet access
 * ============================================================
 */

#include <ESP32Servo.h>
#include <FS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

// ---- Wi-Fi AP Configuration ----
const char *AP_SSID = "SmartLock_AP";
const char *AP_PASSWORD = "password123";
const char *AP_IP = "192.168.4.1";

// ---- Wi-Fi STA Configuration (home/office router) ----
// Leave blank to run AP-only mode
const char *STA_SSID = "";
const char *STA_PASSWORD = "";
bool staConnected = false;

WebServer server(80);
Servo lockServo;

// ---- Pin Definitions ----
const int PIN_SERVO = 18;
const int PIN_REED = 5; // LOW = door closed, HIGH = door open
const int PIN_BUZ = 4;
const int PIN_LED = 2;

// ---- Lock State Machine ----
// 0 = Locked idle
// 1 = Buzzer beep  (300 ms)
// 2 = Door open    (waiting for timeout or close)
// 3 = Waiting for reed to signal close
enum LockState { LOCKED = 0, BEEP, OPEN, WAIT_CLOSE };
LockState lockState = LOCKED;
bool triggerOpen = false;
unsigned long stateTimer = 0;
unsigned long OPEN_TIMEOUT = 5000; // 5 seconds default; updated via /config

// ---- Access Stats ----
uint32_t grantedCount = 0;
uint32_t deniedCount = 0;

// ---- Utility: non-blocking tone ----
void beep(int ms = 300) {
  digitalWrite(PIN_BUZ, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZ, LOW);
}

// ============================================================
//  STATE MACHINE
// ============================================================
void handleLockLogic() {
  switch (lockState) {

  case LOCKED:
    if (triggerOpen) {
      triggerOpen = false;
      grantedCount++;
      lockState = BEEP;
      digitalWrite(PIN_BUZ, HIGH);
      digitalWrite(PIN_LED, HIGH);
      stateTimer = millis();
    }
    break;

  case BEEP:
    if (millis() - stateTimer >= 300) {
      digitalWrite(PIN_BUZ, LOW);
      lockServo.write(90); // Unlock position
      lockState = OPEN;
      stateTimer = millis();
    }
    break;

  case OPEN:
    // Auto-close after OPEN_TIMEOUT or when reed says door is closed
    if (millis() - stateTimer >= OPEN_TIMEOUT) {
      lockState = WAIT_CLOSE;
    }
    break;

  case WAIT_CLOSE:
    if (digitalRead(PIN_REED) == LOW) { // Door physically closed
      lockServo.write(0);               // Lock position
      digitalWrite(PIN_LED, LOW);
      lockState = LOCKED;
    }
    break;
  }
}

// ============================================================
//  HTTP HANDLERS
// ============================================================

// Serve the dashboard (loaded from LittleFS /index.html)
void handleRoot() {
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    server.send(500, "text/plain",
                "Dashboard file missing. Upload smart_lock_dashboard.html as "
                "/index.html via LittleFS.");
    return;
  }
  server.streamFile(f, "text/html");
  f.close();
}

// Trigger open from web UI
void handleOpen() {
  triggerOpen = true;
  server.send(200, "text/plain", "OK");
}

// Deny access log (called from web if unknown face confirmed)
void handleDeny() {
  deniedCount++;
  server.send(200, "text/plain", "OK");
}

// JSON status endpoint
void handleStatus() {
  bool doorClosed = digitalRead(PIN_REED) == LOW;
  String json = "{";
  json += "\"lock\":\"" + String(lockState == LOCKED ? "locked" : "unlocked") +
          "\",";
  json += "\"door\":\"" + String(doorClosed ? "closed" : "open") + "\",";
  json += "\"granted\":" + String(grantedCount) + ",";
  json += "\"denied\":" + String(deniedCount) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// Config endpoint — update settings from dashboard
// Example: GET /config?timeout=10000
void handleConfig() {
  if (server.hasArg("timeout")) {
    OPEN_TIMEOUT = server.arg("timeout").toInt();
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "OK");
}

// CORS preflight
void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// Heartbeat — lightweight connectivity check
void handleHeartbeat() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "pong");
}

// Network info — returns AP + STA details
void handleNetwork() {
  String json = "{";
  json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"ap_ssid\":\"" + String(AP_SSID) + "\",";
  json += "\"ap_clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  json += "\"sta_connected\":" + String(staConnected ? "true" : "false") + ",";
  json += "\"sta_ip\":\"" +
          (staConnected ? WiFi.localIP().toString() : String("N/A")) + "\",";
  json +=
      "\"sta_ssid\":\"" + String(staConnected ? WiFi.SSID() : "N/A") + "\",";
  json += "\"sta_rssi\":" + String(staConnected ? WiFi.RSSI() : 0) + ",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\",";
  json += "\"firmware\":\"3.0\"";
  json += "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// POST /config — accepts JSON body
void handleConfigPost() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    // Parse timeout
    int idx = body.indexOf("\"timeout\"");
    if (idx >= 0) {
      int colon = body.indexOf(':', idx);
      int end = body.indexOf(',', colon);
      if (end < 0)
        end = body.indexOf('}', colon);
      if (colon > 0 && end > 0) {
        OPEN_TIMEOUT = body.substring(colon + 1, end).toInt();
      }
    }
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n[SmartLock] Booting...");

  // GPIO
  pinMode(PIN_REED, INPUT_PULLUP);
  pinMode(PIN_BUZ, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_BUZ, LOW);
  digitalWrite(PIN_LED, LOW);

  // Servo
  lockServo.attach(PIN_SERVO);
  lockServo.write(0); // Start locked

  // LittleFS (for dashboard file)
  if (!LittleFS.begin(true)) {
    Serial.println(
        "[SmartLock] LittleFS mount failed — dashboard will not load.");
  } else {
    Serial.println("[SmartLock] LittleFS ready.");
  }

  // Wi-Fi: Dual AP + STA mode
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[SmartLock] AP IP: ");
  Serial.println(WiFi.softAPIP());

  // Attempt STA connection if credentials provided
  if (strlen(STA_SSID) > 0) {
    Serial.printf("[SmartLock] Connecting to STA: %s\n", STA_SSID);
    WiFi.begin(STA_SSID, STA_PASSWORD);
    unsigned long staStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - staStart < 8000) {
      delay(250);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      Serial.printf("\n[SmartLock] STA IP: %s\n",
                    WiFi.localIP().toString().c_str());
    } else {
      Serial.println("\n[SmartLock] STA connection failed — AP-only mode.");
    }
  }

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/open", HTTP_GET, handleOpen);
  server.on("/deny", HTTP_GET, handleDeny);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/heartbeat", HTTP_GET, handleHeartbeat);
  server.on("/network", HTTP_GET, handleNetwork);
  server.on("/open", HTTP_OPTIONS, handleOptions);
  server.on("/deny", HTTP_OPTIONS, handleOptions);
  server.on("/status", HTTP_OPTIONS, handleOptions);
  server.on("/config", HTTP_OPTIONS, handleOptions);
  server.on("/heartbeat", HTTP_OPTIONS, handleOptions);
  server.on("/network", HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  Serial.println("[SmartLock] HTTP server started (AP+STA hybrid).");
  // Boot beep
  beep(100);
  delay(80);
  beep(100);
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  server.handleClient();
  handleLockLogic();
}

/*
 * ============================================================
 *  DEPLOYMENT NOTES
 * ============================================================
 *
 *  1. Upload the dashboard file to LittleFS:
 *     - Install "ESP32 Sketch Data Upload" tool in Arduino IDE
 *     - Place smart_lock_dashboard.html inside a "data/" folder
 *       next to this .ino file, renamed to "index.html"
 *     - Run: Tools → ESP32 Sketch Data Upload
 *
 *  2. Flash this firmware normally via Arduino IDE.
 *
 *  3. Connect to the "SmartLock_AP" Wi-Fi (password: password123)
 *     and open http://192.168.4.1 in a browser.
 *
 *  4. On first boot the dashboard auto-loads face-api.js models
 *     from CDN — needs internet. After that, everything runs
 *     locally on the browser (no cloud).
 *
 *  5. API Endpoints:
 *     GET  /open           → trigger unlock
 *     GET  /deny           → log a denied attempt
 *     GET  /status         → JSON: lock/door/counts/uptime
 *     GET  /config?timeout=N → set auto-lock timeout (ms)
 *     POST /config         → JSON body: {"timeout": 10000}
 *     GET  /heartbeat      → returns "pong" (connectivity check)
 *     GET  /network        → JSON: AP/STA IPs, RSSI, clients
 * ============================================================
 */
