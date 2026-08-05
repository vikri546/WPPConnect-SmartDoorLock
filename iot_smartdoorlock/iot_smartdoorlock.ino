#include "driver/gpio.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include <ESP32Servo.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

const char *AP_SSID = "SmartLock_AP";
const char *AP_PASSWORD = "password123";
const char *AP_IP = "192.168.4.1";

const char *STA_SSID = "Test";
const char *STA_PASSWORD = "akecil123";
bool staConnected = false;

IPAddress STA_LOCAL_IP(192, 168, 1, 212);
IPAddress STA_GATEWAY(192, 168, 1, 1);
IPAddress STA_SUBNET(255, 255, 255, 0);
IPAddress STA_DNS(8, 8, 8, 8);
const char *MDNS_HOSTNAME = "smartlock";

WebServer server(80);
Servo lockServo;

const int PIN_SERVO = 18;
const int PIN_REED = 5;
const int REED_CLOSED_STATE = LOW;
const int PIN_BUZ = 4;
const int PIN_LED_GREEN = 13;
const int PIN_LED_RED = 27;

enum LockState { LOCKED = 0, BEEP, OPEN, WAIT_CLOSE, WAIT_LOCK };
LockState lockState = LOCKED;
bool triggerOpen = false;
unsigned long stateTimer = 0;
unsigned long OPEN_TIMEOUT = 5000;
bool enableBuzzer = true;
bool enableLED = true;
const bool USE_REED_SWITCH = true;

unsigned long LOCK_DELAY = 5000;

bool denyBuzzerActive = false;
unsigned long denyBuzzerStart = 0;
const unsigned long DENY_BUZZER_DURATION = 3000;

bool enableDoorJam = true;
unsigned long doorOpenSince = 0;
bool doorJamNotified = false;
bool doorJamBuzzerActive = false;
unsigned long doorJamBuzzerTimer = 0;
const unsigned long DOOR_JAM_TIMEOUT = 60000;

bool intrusionActive = false;

uint32_t grantedCount = 0;
uint32_t deniedCount = 0;

int reedDebounced = HIGH;
int reedRawLast = HIGH;
unsigned long reedDebounceTimer = 0;
const unsigned long REED_DEBOUNCE_MS = 100;

void updateReedDebounced() {
  int raw = digitalRead(PIN_REED);
  if (raw != reedRawLast) {
    reedRawLast = raw;
    reedDebounceTimer = millis();
    return;
  }
  if (millis() - reedDebounceTimer >= REED_DEBOUNCE_MS &&
      raw != reedDebounced) {
    reedDebounced = raw;
    if (reedDebounced == REED_CLOSED_STATE) {
      Serial.printf(
          "[SmartLock] [ReedSwitch] MAGNET DEKAT (RAW=%d - Pintu Tertutup)\n",
          raw);
    } else {
      Serial.printf(
          "[SmartLock] [ReedSwitch] MAGNET JAUH (RAW=%d - Pintu Terbuka)\n",
          raw);
    }
  }
}

void beep(int ms = 300) {
  digitalWrite(PIN_BUZ, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZ, LOW);
}

void doubleBeep() {
  beep(150);
  delay(100);
  beep(150);
}

void sendCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleLockLogic() {
  switch (lockState) {

  case LOCKED:
    if (triggerOpen) {
      triggerOpen = false;
      grantedCount++;
      Serial.printf("[SmartLock] >>> OPEN TRIGGERED (OPEN_TIMEOUT=%lu ms, "
                    "buzzer=%s)\n",
                    OPEN_TIMEOUT, enableBuzzer ? "ON" : "OFF");

      if (enableBuzzer) {
        lockState = BEEP;
        digitalWrite(PIN_BUZ, HIGH);
        stateTimer = millis();
        Serial.println("[SmartLock] State: LOCKED -> BEEP (buzzer ON for "
                       "300ms)");
      } else {
        lockServo.write(90);
        lockState = OPEN;
        stateTimer = millis();
        Serial.println("[SmartLock] State: LOCKED -> OPEN (servo 90°, no "
                       "buzzer)");
      }
    }
    break;

  case BEEP:
    if (millis() - stateTimer >= 300) {
      digitalWrite(PIN_BUZ, LOW);
      lockServo.write(90);
      lockState = OPEN;
      stateTimer = millis();

      Serial.println("[SmartLock] State: BEEP -> OPEN (servo 90°)");
    }
    break;

  case OPEN: {
    if (USE_REED_SWITCH) {
      int reedState = reedDebounced;

      if (reedState == REED_CLOSED_STATE) {
        lockState = WAIT_LOCK;
        stateTimer = millis();
        Serial.println("[SmartLock] State: OPEN -> WAIT_LOCK (Pintu tertutup, "
                       "mulai autolock)");
      } else {
        lockState = WAIT_CLOSE;
        stateTimer = millis();
        Serial.println("[SmartLock] State: OPEN -> WAIT_CLOSE (Pintu terbuka, "
                       "menunggu ditutup)");
      }
      break;
    }

    unsigned long elapsed = millis() - stateTimer;
    if (elapsed >= OPEN_TIMEOUT) {
      lockServo.write(0);
      if (enableBuzzer) {
        doubleBeep();
      }
      lockState = LOCKED;
      Serial.println("[SmartLock] State: OPEN -> LOCKED (no reed, timeout)");
    }
    break;
  }

  case WAIT_CLOSE: {
    if (USE_REED_SWITCH) {
      int reedState = reedDebounced;

      if (reedState == REED_CLOSED_STATE) {
        lockState = WAIT_LOCK;
        stateTimer = millis();
        Serial.println("[SmartLock] State: WAIT_CLOSE -> WAIT_LOCK (Pintu "
                       "ditutup (2 LED), memulai autolock)");
      }
    } else {
      lockServo.write(0);
      lockState = LOCKED;
    }
    break;
  }

  case WAIT_LOCK: {
    if (USE_REED_SWITCH) {
      int reedState = reedDebounced;

      if (reedState != REED_CLOSED_STATE) {
        lockState = WAIT_CLOSE;
        stateTimer = millis();
        Serial.println(
            "[SmartLock] State: WAIT_LOCK -> WAIT_CLOSE (Pintu terbuka kembali "
            "secara fisik sebelum terkunci, hitungan mundur dibatalkan)");
        break;
      }

      unsigned long elapsed = millis() - stateTimer;

      static int lastRemainingSeconds = -1;
      if (elapsed < 100) {
        lastRemainingSeconds = -1;
      }
      unsigned long remaining =
          (LOCK_DELAY > elapsed) ? (LOCK_DELAY - elapsed) : 0;
      int remainingSeconds = (remaining + 999) / 1000;
      if (remainingSeconds != lastRemainingSeconds) {
        Serial.printf("[SmartLock] [ReedSwitch] Pintu rapat. Mengunci dalam: "
                      "%d detik...\n",
                      remainingSeconds);
        lastRemainingSeconds = remainingSeconds;
      }

      if (elapsed >= LOCK_DELAY) {
        lockServo.write(0);
        if (enableBuzzer) {
          doubleBeep();
        }
        lockState = LOCKED;
        Serial.printf("[SmartLock] State: WAIT_LOCK -> LOCKED (delay %lu ms, "
                      "pintu otomatis dikunci)\n",
                      elapsed);
      }
    } else {
      lockServo.write(0);
      lockState = LOCKED;
    }
    break;
  }
  }
}

void handleRoot() {
  sendCORSHeaders();
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

void handleOpen() {
  sendCORSHeaders();
  triggerOpen = true;
  Serial.println("[SmartLock] /open endpoint called — triggerOpen=true");
  server.send(200, "text/plain", "OK");
}

void handleDeny() {
  sendCORSHeaders();
  deniedCount++;
  denyBuzzerActive = true;
  denyBuzzerStart = millis();
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  sendCORSHeaders();
  bool doorClosed =
      USE_REED_SWITCH ? (reedDebounced == REED_CLOSED_STATE) : true;
  bool jamActive = doorJamNotified;
  String json = "{";
  json += "\"lock\":\"" + String(lockState == LOCKED ? "locked" : "unlocked") +
          "\",";
  json += "\"door\":\"" + String(doorClosed ? "closed" : "open") + "\",";
  json += "\"doorjam\":" + String(jamActive ? "true" : "false") + ",";
  json += "\"intrusion\":" + String(intrusionActive ? "true" : "false") + ",";
  json += "\"granted\":" + String(grantedCount) + ",";
  json += "\"denied\":" + String(deniedCount) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";
  server.send(200, "application/json", json);
}

void handleConfig() {
  sendCORSHeaders();

  Serial.printf("[SmartLock] GET /config called with %d args\n", server.args());
  for (int i = 0; i < server.args(); i++) {
    Serial.printf("[SmartLock]   arg[%d] %s = %s\n", i,
                  server.argName(i).c_str(), server.arg(i).c_str());
  }

  int meaningfulArgs = 0;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) != "_t")
      meaningfulArgs++;
  }

  if (meaningfulArgs == 0) {
    String json = "{";
    json += "\"timeout\":" + String(OPEN_TIMEOUT) + ",";
    json += "\"buzzer\":" + String(enableBuzzer ? "true" : "false") + ",";
    json += "\"lockdelay\":" + String(LOCK_DELAY) + ",";
    json += "\"doorjam\":" + String(enableDoorJam ? "true" : "false") + ",";
    json += "\"led\":" + String(enableLED ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
    return;
  }

  if (server.hasArg("timeout")) {
    long t = server.arg("timeout").toInt();

    if (t >= 1000) {
      OPEN_TIMEOUT = t;
      Serial.printf("[SmartLock] Config timeout updated: %lu ms\n",
                    OPEN_TIMEOUT);
      File f = LittleFS.open("/timeout.txt", "w");
      if (f) {
        f.print(OPEN_TIMEOUT);
        f.close();
        Serial.println("[SmartLock] Timeout saved to LittleFS.");
      }
    } else {
      Serial.printf("[SmartLock] Warning: Timeout %ld ms diabaikan karena "
                    "kurang dari 1000ms\n",
                    t);
    }
  }

  if (server.hasArg("buzzer")) {
    enableBuzzer =
        (server.arg("buzzer") == "1" || server.arg("buzzer") == "true");
    Serial.printf("[SmartLock] Config buzzer updated: %s\n",
                  enableBuzzer ? "enabled" : "disabled");
    File f = LittleFS.open("/buzzer.txt", "w");
    if (f) {
      f.print(enableBuzzer ? "1" : "0");
      f.close();
    }
  }

  if (server.hasArg("lockdelay")) {
    long ld = server.arg("lockdelay").toInt();
    if (ld >= 0) {
      LOCK_DELAY = ld;
      Serial.printf("[SmartLock] Config lockdelay updated: %lu ms\n",
                    LOCK_DELAY);
      File f = LittleFS.open("/lockdelay.txt", "w");
      if (f) {
        f.print(LOCK_DELAY);
        f.close();
      }
    }
  }

  if (server.hasArg("doorjam")) {
    enableDoorJam =
        (server.arg("doorjam") == "1" || server.arg("doorjam") == "true");
    Serial.printf("[SmartLock] Config doorjam updated: %s\n",
                  enableDoorJam ? "enabled" : "disabled");
    File f = LittleFS.open("/doorjam.txt", "w");
    if (f) {
      f.print(enableDoorJam ? "1" : "0");
      f.close();
    }
  }

  if (server.hasArg("led")) {
    enableLED = (server.arg("led") == "1" || server.arg("led") == "true");
    Serial.printf("[SmartLock] Config led updated: %s\n",
                  enableLED ? "enabled" : "disabled");
    File f = LittleFS.open("/led.txt", "w");
    if (f) {
      f.print(enableLED ? "1" : "0");
      f.close();
    }
  }

  String json = "{";
  json += "\"status\":\"ok\",";
  json += "\"timeout\":" + String(OPEN_TIMEOUT) + ",";
  json += "\"buzzer\":" + String(enableBuzzer ? "true" : "false") + ",";
  json += "\"lockdelay\":" + String(LOCK_DELAY) + ",";
  json += "\"doorjam\":" + String(enableDoorJam ? "true" : "false") + ",";
  json += "\"led\":" + String(enableLED ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleOptions() {
  sendCORSHeaders();
  server.send(204);
}

void handleHeartbeat() {
  sendCORSHeaders();
  server.send(200, "text/plain", "pong");
}

void handleNetwork() {
  sendCORSHeaders();
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
  json += "\"hostname\":\"" + String(MDNS_HOSTNAME) + ".local\",";
  json += "\"firmware\":\"3.0\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleConfigPost() {
  sendCORSHeaders();
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    Serial.printf("[SmartLock] POST /config body: %s\n", body.c_str());
    int idx = body.indexOf("\"timeout\"");
    if (idx >= 0) {
      int colon = body.indexOf(':', idx);
      int end = body.indexOf(',', colon);
      if (end < 0)
        end = body.indexOf('}', colon);
      if (colon > 0 && end > 0) {
        long t = body.substring(colon + 1, end).toInt();
        if (t >= 1000) {
          OPEN_TIMEOUT = t;
          Serial.printf("[SmartLock] Config timeout updated (POST): %lu ms\n",
                        OPEN_TIMEOUT);
          File f = LittleFS.open("/timeout.txt", "w");
          if (f) {
            f.print(OPEN_TIMEOUT);
            f.close();
            Serial.println("[SmartLock] Timeout saved to LittleFS (POST).");
          }
        } else {
          Serial.printf("[SmartLock] Warning: POST timeout %ld ms diabaikan "
                        "karena kurang dari 1000ms\n",
                        t);
        }
      }
    }
    int bidx = body.indexOf("\"buzzer\"");
    if (bidx >= 0) {
      int colon = body.indexOf(':', bidx);
      int end = body.indexOf(',', colon);
      if (end < 0)
        end = body.indexOf('}', colon);
      if (colon > 0 && end > 0) {
        String val = body.substring(colon + 1, end);
        val.trim();
        enableBuzzer = (val == "true" || val == "1");
        File f = LittleFS.open("/buzzer.txt", "w");
        if (f) {
          f.print(enableBuzzer ? "1" : "0");
          f.close();
        }
      }
    }
    int lidx = body.indexOf("\"lockdelay\"");
    if (lidx >= 0) {
      int colon = body.indexOf(':', lidx);
      int end = body.indexOf(',', colon);
      if (end < 0)
        end = body.indexOf('}', colon);
      if (colon > 0 && end > 0) {
        long ld = body.substring(colon + 1, end).toInt();
        if (ld >= 0) {
          LOCK_DELAY = ld;
          File f = LittleFS.open("/lockdelay.txt", "w");
          if (f) {
            f.print(LOCK_DELAY);
            f.close();
          }
        }
      }
    }
    int djidx = body.indexOf("\"doorjam\"");
    if (djidx >= 0) {
      int colon = body.indexOf(':', djidx);
      int end = body.indexOf(',', colon);
      if (end < 0)
        end = body.indexOf('}', colon);
      if (colon > 0 && end > 0) {
        String val = body.substring(colon + 1, end);
        val.trim();
        enableDoorJam = (val == "true" || val == "1");
        File f = LittleFS.open("/doorjam.txt", "w");
        if (f) {
          f.print(enableDoorJam ? "1" : "0");
          f.close();
        }
      }
    }
    int ledidx = body.indexOf("\"led\"");
    if (ledidx >= 0) {
      int colon = body.indexOf(':', ledidx);
      int end = body.indexOf(',', colon);
      if (end < 0)
        end = body.indexOf('}', colon);
      if (colon > 0 && end > 0) {
        String val = body.substring(colon + 1, end);
        val.trim();
        enableLED = (val == "true" || val == "1");
        File f = LittleFS.open("/led.txt", "w");
        if (f) {
          f.print(enableLED ? "1" : "0");
          f.close();
        }
      }
    }
  }
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  Serial.println("\n[SmartLock] Booting...");

  pinMode(PIN_REED, INPUT_PULLUP);
  pinMode(PIN_BUZ, OUTPUT);
  gpio_reset_pin((gpio_num_t)PIN_LED_GREEN);
  gpio_reset_pin((gpio_num_t)PIN_LED_RED);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  digitalWrite(PIN_BUZ, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED, LOW);

  lockServo.attach(PIN_SERVO);
  lockServo.write(0);

  if (!LittleFS.begin(true)) {
    Serial.println(
        "[SmartLock] LittleFS mount failed — dashboard will not load.");
  } else {
    Serial.println("[SmartLock] LittleFS ready.");
    if (LittleFS.exists("/timeout.txt")) {
      File f = LittleFS.open("/timeout.txt", "r");
      if (f) {
        String val = f.readString();
        long t = val.toInt();
        f.close();
        if (t >= 1000) {
          OPEN_TIMEOUT = t;
          Serial.printf(
              "[SmartLock] Loaded saved timeout from LittleFS: %lu ms\n",
              OPEN_TIMEOUT);
        } else {
          OPEN_TIMEOUT = 5000;
          Serial.printf("[SmartLock] Saved timeout was invalid (%ld ms). "
                        "Resetting to default: 5000 ms\n",
                        t);
          File wf = LittleFS.open("/timeout.txt", "w");
          if (wf) {
            wf.print(OPEN_TIMEOUT);
            wf.close();
          }
        }
      }
    }
    if (LittleFS.exists("/buzzer.txt")) {
      File f = LittleFS.open("/buzzer.txt", "r");
      if (f) {
        enableBuzzer = f.readString() == "1";
        Serial.printf("[SmartLock] Loaded saved buzzer from LittleFS: %s\n",
                      enableBuzzer ? "enabled" : "disabled");
        f.close();
      }
    }

    if (LittleFS.exists("/lockdelay.txt")) {
      File f = LittleFS.open("/lockdelay.txt", "r");
      if (f) {
        long ld = f.readString().toInt();
        if (ld >= 0)
          LOCK_DELAY = ld;
        f.close();
        Serial.printf("[SmartLock] Loaded lockdelay: %lu ms\n", LOCK_DELAY);
      }
    }
    if (LittleFS.exists("/doorjam.txt")) {
      File f = LittleFS.open("/doorjam.txt", "r");
      if (f) {
        enableDoorJam = f.readString() == "1";
        f.close();
        Serial.printf("[SmartLock] Loaded doorjam: %s\n",
                      enableDoorJam ? "enabled" : "disabled");
      }
    }
    if (LittleFS.exists("/led.txt")) {
      File f = LittleFS.open("/led.txt", "r");
      if (f) {
        enableLED = f.readString() == "1";
        Serial.printf("[SmartLock] Loaded saved led from LittleFS: %s\n",
                      enableLED ? "enabled" : "disabled");
        f.close();
      }
    }
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[SmartLock] AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (strlen(STA_SSID) > 0) {
    Serial.printf("[SmartLock] Connecting to STA: %s ...\n", STA_SSID);
    WiFi.begin(STA_SSID, STA_PASSWORD);
    unsigned long staStart = millis();
    int dotCount = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - staStart < 15000) {
      delay(500);
      Serial.print(".");
      dotCount++;
      if (dotCount % 10 == 0) {
        Serial.printf(" (%ds)\n", dotCount / 2);
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      IPAddress gw = WiFi.gatewayIP();
      if (gw[0] != 0) {
        IPAddress autoStaticIP(gw[0], gw[1], gw[2], 212);
        WiFi.config(autoStaticIP, gw, IPAddress(255, 255, 255, 0),
                    IPAddress(8, 8, 8, 8));
      }
      Serial.printf("\n[SmartLock] STA Connected! Fixed Subnet IP: %s\n",
                    WiFi.localIP().toString().c_str());
      Serial.printf("[SmartLock] SSID: %s | RSSI: %d dBm\n",
                    WiFi.SSID().c_str(), WiFi.RSSI());

      if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[SmartLock] mDNS started: http://%s.local\n",
                      MDNS_HOSTNAME);
      } else {
        Serial.println("[SmartLock] WARNING: mDNS failed to start");
      }
    } else {
      Serial.printf("\n[SmartLock] STA connection FAILED (WiFi status: %d)\n",
                    WiFi.status());
      Serial.println("[SmartLock] Running in AP-only mode.");
      Serial.println(
          "[SmartLock] Tip: Check SSID/Password, or move closer to router.");
    }
  }

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
  server.onNotFound([]() {
    sendCORSHeaders();
    server.send(404, "text/plain", "Not found");
  });

  server.enableCORS(true);

  server.begin();

  Serial.println(
      "[SmartLock] HTTP server started (AP+STA hybrid, CORS enabled).");
  int rawReed = digitalRead(PIN_REED);
  reedDebounced = rawReed;
  reedRawLast = rawReed;
  Serial.printf("[SmartLock] Status Awal Reed Switch: RAW=%d -> %s\n", rawReed,
                rawReed == REED_CLOSED_STATE ? "Magnet Dekat (TUTUP - 2 LED)"
                                             : "Magnet Jauh (BUKA - 1 LED)");
  updateLEDs();
  beep(100);
  delay(80);
  beep(100);
}

void handleDenyBuzzer() {
  if (!denyBuzzerActive)
    return;
  unsigned long elapsed = millis() - denyBuzzerStart;
  if (elapsed > DENY_BUZZER_DURATION) {
    denyBuzzerActive = false;
    digitalWrite(PIN_BUZ, LOW);
    return;
  }
  bool on = ((elapsed / 200) % 2) == 0;
  digitalWrite(PIN_BUZ, enableBuzzer && on ? HIGH : LOW);
}

void handleDoorJamDetection() {
  if (!enableDoorJam || !USE_REED_SWITCH)
    return;
  int reedState = reedDebounced;
  if (reedState != REED_CLOSED_STATE) {
    if (doorOpenSince == 0)
      doorOpenSince = millis();
    unsigned long openDuration = millis() - doorOpenSince;
    if (openDuration >= DOOR_JAM_TIMEOUT) {
      if (!doorJamNotified) {
        doorJamNotified = true;
        doorJamBuzzerActive = true;
        Serial.println("[SmartLock] DOOR JAM DETECTED — open > 1 minute");
      }
      if (enableBuzzer && doorJamBuzzerActive) {
        unsigned long cycle = (millis() / 200) % 5;
        digitalWrite(PIN_BUZ, cycle == 0 ? HIGH : LOW);
      }
    }
  } else {
    doorOpenSince = 0;
    if (doorJamNotified) {
      doorJamNotified = false;
      Serial.println("[SmartLock] Door jam resolved — closed again");
    }
    if (doorJamBuzzerActive) {
      doorJamBuzzerActive = false;
      digitalWrite(PIN_BUZ, LOW);
    }
  }
}

void handleIntrusionDetection() {
  if (!USE_REED_SWITCH)
    return;

  if (lockState == LOCKED && reedDebounced != REED_CLOSED_STATE) {
    if (!intrusionActive) {
      intrusionActive = true;
      Serial.println(
          "[SmartLock] !!! WARNING: INTRUSION DETECTED — DOOR FORCED OPEN !!!");
    }

    if (enableBuzzer) {
      unsigned long cycle = (millis() / 150) % 2;
      digitalWrite(PIN_BUZ, cycle == 0 ? HIGH : LOW);
    }
  } else {
    if (intrusionActive) {
      intrusionActive = false;
      digitalWrite(PIN_BUZ, LOW);
      Serial.println(
          "[SmartLock] Intrusion resolved — door closed or lock opened");
    }
  }
}

void updateLEDs() {
  static int lastGreenState = -1;
  static int lastRedState = -1;

  int targetGreen = LOW;
  int targetRed = LOW;

  if (enableLED) {
    if (denyBuzzerActive) {
      unsigned long elapsed = millis() - denyBuzzerStart;
      bool flashOn = ((elapsed / 200) % 2) == 0;
      targetRed = flashOn ? HIGH : LOW;
      targetGreen = LOW;
    } else {
      if (lockState == LOCKED) {
        targetRed = HIGH;
        targetGreen = LOW;
      } else {
        targetGreen = HIGH;
        targetRed = LOW;
      }
    }
  }

  if (targetGreen != lastGreenState) {
    digitalWrite(PIN_LED_GREEN, targetGreen);
    lastGreenState = targetGreen;
  }
  if (targetRed != lastRedState) {
    digitalWrite(PIN_LED_RED, targetRed);
    lastRedState = targetRed;
  }
}

void logReedRaw() {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 500) {
    lastPrint = millis();
    int raw = digitalRead(PIN_REED);
    Serial.printf("[Debug] Reed Pin %d Raw: %d (LEDs: %s), Debounced: %d\n",
                  PIN_REED, raw,
                  raw == LOW ? "2 LEDs (Magnet Near)" : "1 LED (Magnet Far)",
                  reedDebounced);
  }
}

void loop() {
  updateReedDebounced();
  logReedRaw();
  server.handleClient();
  handleLockLogic();
  handleDenyBuzzer();
  handleDoorJamDetection();
  handleIntrusionDetection();
  updateLEDs();
}