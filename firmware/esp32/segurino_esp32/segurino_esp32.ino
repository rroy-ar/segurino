/**
 * @file segurino_esp32.ino
 * @brief ESP32 Wi-Fi & Firebase Realtime Database bridge for Segurino.
 *
 * Bridges UART commands between the Arduino UNO hardware controller and
 * Firebase Realtime Database:
 *   - Streams `/systemStatus/status` changes and forwards `CMD:ARM` / `CMD:DISARM`.
 *   - Validates keypad PINs against `/pins` (`PIN:<digits>` -> `PIN_OK` / `PIN_FAIL`).
 *   - Syncs local keypad arm/disarm transitions (`STATE:ARMED` / `STATE:DISARMED`).
 *   - Logs confirmed intrusion events to `/events` with UTC ISO-8601 timestamps and epoch.
 */

#include <WiFi.h>
#include <FirebaseESP32.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>

#include "config.h"

#if DEBUG_MODE
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif

// --- Firebase & NTP Globals ---
FirebaseData stream;
FirebaseData fbdo;
FirebaseConfig fbConfig;
FirebaseAuth fbAuth;

WiFiUDP ntpUDP;
// Always fetch UTC epoch (offset = 0) so daemon and web clients remain timezone-agnostic
NTPClient timeClient(ntpUDP, NTP_SERVER, 0, 60000);

// --- State & UART Globals ---
String lastKnownStatus = "";
volatile bool hasPendingCommand = false;
String pendingCommand = "";

String arduinoBuffer = "";

// --- Forward Declarations ---
void connectWiFi();
void initFirebase();
void initNtp();
void pollArduinoSerial();
void processArduinoMessage(const String& message);
void verifyPinInFirebase(const String& pin);
void logEventToFirebase(const String& eventType);
void updateSystemStatusInFirebase(const String& newStatus);
void sendToArduino(const String& message);
void streamCallback(StreamData data);
void streamTimeoutCallback(bool timeout);

void streamCallback(StreamData data) {
  if (data.dataType() != "string") {
    return;
  }

  String currentStatus = data.stringData();
  DEBUG_PRINTLN("[Firebase Stream] Status received: " + currentStatus);

  if (currentStatus != lastKnownStatus) {
    lastKnownStatus = currentStatus;
    if (currentStatus == "ARMED") {
      pendingCommand = "CMD:ARM";
      hasPendingCommand = true;
    } else if (currentStatus == "DISARMED") {
      pendingCommand = "CMD:DISARM";
      hasPendingCommand = true;
    }
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    DEBUG_PRINTLN("[Firebase Stream] Timeout occurred, reconnecting...");
  }
  if (!stream.httpConnected()) {
    DEBUG_PRINTF("[Firebase Stream] HTTP error: %s\n", stream.errorReason().c_str());
  }
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_RX_PIN >= 0 && ARDUINO_TX_PIN >= 0
  ARDUINO_SERIAL.begin(ARDUINO_BAUD_RATE, SERIAL_8N1, ARDUINO_RX_PIN, ARDUINO_TX_PIN);
#else
  ARDUINO_SERIAL.begin(ARDUINO_BAUD_RATE);
#endif
  ARDUINO_SERIAL.setTimeout(50);

  DEBUG_PRINTLN("\n--- Segurino ESP32 Bridge Starting ---");

  connectWiFi();
  initNtp();
  initFirebase();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN("[WiFi] Connection lost. Reconnecting...");
    connectWiFi();
  }

  timeClient.update();

  // Deliver queued stream commands safely from the main loop
  if (hasPendingCommand) {
    hasPendingCommand = false;
    sendToArduino(pendingCommand);
  }

  if (Firebase.ready()) {
    pollArduinoSerial();
  }

  delay(10);
}

void connectWiFi() {
  DEBUG_PRINT("[WiFi] Connecting to ");
  DEBUG_PRINT(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs > 20000UL) {
      DEBUG_PRINTLN("\n[WiFi] Connection timeout. Restarting ESP32...");
      delay(1000);
      ESP.restart();
    }
    DEBUG_PRINT(".");
    delay(500);
  }

  DEBUG_PRINTLN("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
}

void initNtp() {
  timeClient.begin();
  DEBUG_PRINT("[NTP] Syncing UTC time");
  unsigned long startMs = millis();
  while (!timeClient.update() && (millis() - startMs < 5000UL)) {
    timeClient.forceUpdate();
    DEBUG_PRINT(".");
    delay(250);
  }
  DEBUG_PRINTLN(" done.");
}

void initFirebase() {
  fbConfig.api_key = FIREBASE_API_KEY;
  fbConfig.database_url = "https://" + String(FIREBASE_PROJECT_ID) + "-default-rtdb.firebaseio.com/";
  fbAuth.user.email = FIREBASE_USER_EMAIL;
  fbAuth.user.password = FIREBASE_USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  Firebase.begin(&fbConfig, &fbAuth);

  DEBUG_PRINTLN("[Firebase] Authenticating...");
  unsigned long startMs = millis();
  while (!Firebase.ready() && (millis() - startMs < 15000UL)) {
    delay(100);
  }

  if (!Firebase.ready()) {
    DEBUG_PRINTLN("[Firebase] Authentication failed. Restarting ESP32...");
    delay(2000);
    ESP.restart();
  }

  DEBUG_PRINTLN("[Firebase] Authenticated successfully.");

  if (!Firebase.beginStream(stream, "/systemStatus/status")) {
    DEBUG_PRINTLN("[Firebase] Failed to start stream: " + stream.errorReason());
  } else {
    Firebase.setStreamCallback(stream, streamCallback, streamTimeoutCallback);
    DEBUG_PRINTLN("[Firebase] Stream active on /systemStatus/status");
  }
}

void pollArduinoSerial() {
  while (ARDUINO_SERIAL.available() > 0) {
    char c = (char)ARDUINO_SERIAL.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      arduinoBuffer.trim();
      if (arduinoBuffer.length() > 0) {
        processArduinoMessage(arduinoBuffer);
      }
      arduinoBuffer = "";
    } else if (arduinoBuffer.length() < 64) {
      arduinoBuffer += c;
    }
  }
}

void processArduinoMessage(const String& message) {
  DEBUG_PRINTLN("[UART <- Arduino] " + message);

  if (message.startsWith("PIN:")) {
    String pin = message.substring(4);
    verifyPinInFirebase(pin);
  } else if (message.startsWith("EVENT:")) {
    String eventType = message.substring(6);
    logEventToFirebase(eventType);
  } else if (message.startsWith("STATE:")) {
    String newStatus = message.substring(6);
    if (newStatus == "ARMED" || newStatus == "DISARMED") {
      updateSystemStatusInFirebase(newStatus);
    }
  } else if (message == "SYNC") {
    if (lastKnownStatus == "ARMED") {
      sendToArduino("CMD:ARM");
    } else if (lastKnownStatus == "DISARMED") {
      sendToArduino("CMD:DISARM");
    }
  }
}

void sendToArduino(const String& message) {
  DEBUG_PRINTLN("[UART -> Arduino] " + message);
  ARDUINO_SERIAL.println(message);
}

void verifyPinInFirebase(const String& pin) {
  DEBUG_PRINTLN("[Firebase] Verifying keypad PIN...");

  if (!Firebase.getJSON(fbdo, "/pins")) {
    DEBUG_PRINTLN("[Firebase] Error reading /pins: " + fbdo.errorReason());
    sendToArduino("PIN_FAIL");
    return;
  }

  bool isValid = false;
  FirebaseJson& json = fbdo.jsonObject();
  size_t len = json.iteratorBegin();
  String key, ignoredValue;
  int type;

  for (size_t i = 0; i < len; i++) {
    json.iteratorGet(i, type, key, ignoredValue);
    FirebaseJsonData result;
    if (json.get(result, key + "/value") && result.success) {
      if (result.stringValue == pin) {
        isValid = true;
        break;
      }
    }
  }
  json.iteratorEnd();

  sendToArduino(isValid ? "PIN_OK" : "PIN_FAIL");
}

void updateSystemStatusInFirebase(const String& newStatus) {
  // Update local cache before writing so the stream callback ignores the echo
  lastKnownStatus = newStatus;
  if (Firebase.setString(fbdo, "/systemStatus/status", newStatus)) {
    DEBUG_PRINTLN("[Firebase] Synced /systemStatus/status -> " + newStatus);
  } else {
    DEBUG_PRINTLN("[Firebase] Failed to update status: " + fbdo.errorReason());
  }
}

void logEventToFirebase(const String& eventType) {
  timeClient.update();
  unsigned long epochTime = timeClient.getEpochTime();
  time_t rawTime = (time_t)epochTime;
  struct tm* ptm = gmtime(&rawTime);

  char isoTimestamp[25];
  strftime(isoTimestamp, sizeof(isoTimestamp), "%Y-%m-%dT%H:%M:%SZ", ptm);

  DEBUG_PRINTLN("[Firebase] Logging intrusion event: " + eventType + " at " + String(isoTimestamp));

  FirebaseJson json;
  json.set("type", eventType);
  json.set("timestamp", String(isoTimestamp));
  json.set("epoch", (int)epochTime);

  if (Firebase.pushJSON(fbdo, "/events", json)) {
    DEBUG_PRINTLN("[Firebase] Event logged successfully.");
  } else {
    DEBUG_PRINTLN("[Firebase] Failed to log event: " + fbdo.errorReason());
  }
}
