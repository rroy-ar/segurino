/**
 * @file segurino_arduino.ino
 * @brief Arduino UNO alarm controller firmware for Segurino.
 *
 * Manages local hardware sensors (PIR motion sensor, magnetic reed switch),
 * user interface (4x4 matrix keypad, 16x2 I2C LCD, red/green status LEDs),
 * and bidirectional UART communication with the ESP32 Wi-Fi bridge.
 *
 * UART Protocol (9600 baud, newline-terminated):
 *   Outgoing (Arduino -> ESP32):
 *     - PIN:<digits>      Request PIN validation against Firebase
 *     - EVENT:<sensor>    Report confirmed intrusion ("PIR" or "REED")
 *     - STATE:ARMED       Notify cloud of local keypad arming
 *     - STATE:DISARMED    Notify cloud of local keypad disarming
 *     - SYNC              Request current cloud state after boot
 *
 *   Incoming (ESP32 -> Arduino):
 *     - PIN_OK            Submitted PIN is valid
 *     - PIN_FAIL          Submitted PIN is invalid or verification failed
 *     - CMD:ARM           Remote arm command from Firebase
 *     - CMD:DISARM        Remote disarm command from Firebase
 */

#include <Wire.h>
#include <Adafruit_LiquidCrystal.h>
#include <Keypad.h>

// --- Hardware Pin Configuration ---
const uint8_t PIR_PIN = 12;        // Active HIGH motion sensor
const uint8_t REED_PIN = 11;       // Active LOW magnetic door/window switch (INPUT_PULLUP)
const uint8_t GREEN_LED_PIN = 10;  // Lit when system is ARMED
const uint8_t RED_LED_PIN = 13;    // Lit when DISARMED, blinking during alarm states

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {9, 8, 7, 6};
byte colPins[COLS] = {5, 4, 3, 2};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
Adafruit_LiquidCrystal lcd(0);

// --- Timing & Limits ---
const unsigned long PRE_ALARM_DURATION_MS = 15000UL;   // 15s entry grace period
const unsigned long PRE_ALARM_BLINK_MS = 250UL;      // Pre-alarm LED/backlight blink rate
const unsigned long TRIGGERED_BLINK_MS = 150UL;      // Active alarm rapid blink rate
const unsigned long PIN_VERIFY_TIMEOUT_MS = 5000UL;  // Timeout waiting for ESP32 response
const unsigned long BANNER_DURATION_MS = 1200UL;     // Temporary LCD feedback duration
const uint8_t MAX_PIN_LENGTH = 8;
const uint8_t SERIAL_BUFFER_SIZE = 48;

// --- System States ---
enum SystemState {
  STATE_DISARMED = 0,
  STATE_ARMED = 1,
  STATE_PRE_ALARM = 2,
  STATE_ALARM_TRIGGERED = 3
};

SystemState currentState = STATE_DISARMED;

// --- Input & Serial Buffers ---
char enteredPin[MAX_PIN_LENGTH + 1] = "";
uint8_t enteredPinLen = 0;

char serialBuffer[SERIAL_BUFFER_SIZE];
uint8_t serialBufferLen = 0;

// --- Non-blocking Timers & Flags ---
unsigned long preAlarmStartMs = 0;
unsigned long lastBlinkMs = 0;
int lastDisplayedSecond = -1;
bool blinkPhaseOn = false;

bool awaitingPinResponse = false;
unsigned long pinSentMs = 0;

bool bannerActive = false;
unsigned long bannerUntilMs = 0;

char triggerSensor[12] = "";

// --- Forward Declarations ---
void transitionToState(SystemState newState);
void handleKeypadInput();
void handleArmedSensors();
void handlePreAlarmState();
void handleAlarmTriggeredState();
void pollSerialCommands();
void processSerialCommand(const char* cmd);
void checkPinVerificationTimeout();
void updateBannerExpiry();
void showTemporaryBanner(const char* message, unsigned long durationMs);
void renderPinPrompt();
void clearLcdRow(uint8_t row);
void updateBlink(unsigned long intervalMs);

void setup() {
  Serial.begin(9600);

  lcd.begin(16, 2);
  lcd.setBacklight(1);
  lcd.clear();

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(REED_PIN, INPUT_PULLUP);

  transitionToState(STATE_DISARMED);

  // Request current cloud state in case ESP32 is already connected
  Serial.println(F("SYNC"));
}

void loop() {
  pollSerialCommands();
  checkPinVerificationTimeout();
  updateBannerExpiry();
  handleKeypadInput();

  switch (currentState) {
    case STATE_DISARMED:
      break;
    case STATE_ARMED:
      handleArmedSensors();
      break;
    case STATE_PRE_ALARM:
      handlePreAlarmState();
      break;
    case STATE_ALARM_TRIGGERED:
      handleAlarmTriggeredState();
      break;
  }
}

void handleArmedSensors() {
  if (digitalRead(PIR_PIN) == HIGH) {
    strncpy(triggerSensor, "PIR", sizeof(triggerSensor) - 1);
    triggerSensor[sizeof(triggerSensor) - 1] = '\0';
    transitionToState(STATE_PRE_ALARM);
  } else if (digitalRead(REED_PIN) == LOW) {
    strncpy(triggerSensor, "REED", sizeof(triggerSensor) - 1);
    triggerSensor[sizeof(triggerSensor) - 1] = '\0';
    transitionToState(STATE_PRE_ALARM);
  }
}

void handlePreAlarmState() {
  unsigned long now = millis();
  unsigned long elapsed = now - preAlarmStartMs;

  if (elapsed >= PRE_ALARM_DURATION_MS) {
    Serial.print(F("EVENT:"));
    Serial.println(triggerSensor);
    transitionToState(STATE_ALARM_TRIGGERED);
    return;
  }

  updateBlink(PRE_ALARM_BLINK_MS);

  int remainingSeconds = (int)((PRE_ALARM_DURATION_MS - elapsed) / 1000UL);
  if (remainingSeconds != lastDisplayedSecond) {
    lastDisplayedSecond = remainingSeconds;
    char line[17];
    snprintf(line, sizeof(line), "INTRUSION: %2ds  ", remainingSeconds);
    lcd.setCursor(0, 0);
    lcd.print(line);
  }
}

void handleAlarmTriggeredState() {
  updateBlink(TRIGGERED_BLINK_MS);
}

void updateBlink(unsigned long intervalMs) {
  unsigned long now = millis();
  if (now - lastBlinkMs >= intervalMs) {
    lastBlinkMs = now;
    blinkPhaseOn = !blinkPhaseOn;
    digitalWrite(RED_LED_PIN, blinkPhaseOn ? HIGH : LOW);
    lcd.setBacklight(blinkPhaseOn ? 1 : 0);
  }
}

void handleKeypadInput() {
  if (awaitingPinResponse) {
    return;
  }

  char key = keypad.getKey();
  if (!key) {
    return;
  }

  if (key >= '0' && key <= '9') {
    if (enteredPinLen < MAX_PIN_LENGTH) {
      enteredPin[enteredPinLen++] = key;
      enteredPin[enteredPinLen] = '\0';
      bannerActive = false;
      renderPinPrompt();
    }
  } else if (key == '#' && enteredPinLen > 0) {
    Serial.print(F("PIN:"));
    Serial.println(enteredPin);

    enteredPinLen = 0;
    enteredPin[0] = '\0';
    awaitingPinResponse = true;
    pinSentMs = millis();

    clearLcdRow(1);
    lcd.setCursor(0, 1);
    lcd.print(F("Verifying PIN..."));
  } else if (key == '*') {
    enteredPinLen = 0;
    enteredPin[0] = '\0';
    showTemporaryBanner("PIN cleared", 800UL);
  }
}

void checkPinVerificationTimeout() {
  if (awaitingPinResponse && (millis() - pinSentMs >= PIN_VERIFY_TIMEOUT_MS)) {
    awaitingPinResponse = false;
    showTemporaryBanner("Verify timeout!", BANNER_DURATION_MS);
  }
}

void pollSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      serialBuffer[serialBufferLen] = '\0';
      if (serialBufferLen > 0) {
        processSerialCommand(serialBuffer);
      }
      serialBufferLen = 0;
    } else if (serialBufferLen < SERIAL_BUFFER_SIZE - 1) {
      serialBuffer[serialBufferLen++] = c;
    }
  }
}

void processSerialCommand(const char* cmd) {
  if (strcmp(cmd, "PIN_OK") == 0) {
    awaitingPinResponse = false;
    if (currentState == STATE_DISARMED) {
      transitionToState(STATE_ARMED);
      Serial.println(F("STATE:ARMED"));
    } else {
      transitionToState(STATE_DISARMED);
      Serial.println(F("STATE:DISARMED"));
    }
  } else if (strcmp(cmd, "PIN_FAIL") == 0) {
    awaitingPinResponse = false;
    showTemporaryBanner("Invalid PIN!", BANNER_DURATION_MS);
  } else if (strcmp(cmd, "CMD:ARM") == 0) {
    awaitingPinResponse = false;
    if (currentState != STATE_ARMED) {
      transitionToState(STATE_ARMED);
    }
  } else if (strcmp(cmd, "CMD:DISARM") == 0) {
    awaitingPinResponse = false;
    if (currentState != STATE_DISARMED) {
      transitionToState(STATE_DISARMED);
    }
  }
}

void transitionToState(SystemState newState) {
  currentState = newState;
  enteredPinLen = 0;
  enteredPin[0] = '\0';
  bannerActive = false;
  blinkPhaseOn = false;
  lastBlinkMs = millis();

  lcd.clear();
  lcd.setBacklight(1);

  if (currentState == STATE_DISARMED) {
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, LOW);
    lcd.setCursor(0, 0);
    lcd.print(F("System Disarmed"));
    renderPinPrompt();
  } else if (currentState == STATE_ARMED) {
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, HIGH);
    lcd.setCursor(0, 0);
    lcd.print(F("System Armed"));
    clearLcdRow(1);
  } else if (currentState == STATE_PRE_ALARM) {
    digitalWrite(GREEN_LED_PIN, LOW);
    preAlarmStartMs = millis();
    lastDisplayedSecond = -1;
    renderPinPrompt();
  } else if (currentState == STATE_ALARM_TRIGGERED) {
    digitalWrite(GREEN_LED_PIN, LOW);
    char header[17];
    snprintf(header, sizeof(header), "ALARM: %-9s", triggerSensor);
    lcd.setCursor(0, 0);
    lcd.print(header);
    renderPinPrompt();
  }
}

void showTemporaryBanner(const char* message, unsigned long durationMs) {
  clearLcdRow(1);
  lcd.setCursor(0, 1);
  lcd.print(message);
  bannerActive = true;
  bannerUntilMs = millis() + durationMs;
}

void updateBannerExpiry() {
  if (bannerActive && (long)(millis() - bannerUntilMs) >= 0) {
    bannerActive = false;
    if (currentState == STATE_ARMED) {
      clearLcdRow(1);
    } else {
      renderPinPrompt();
    }
  }
}

void renderPinPrompt() {
  clearLcdRow(1);
  lcd.setCursor(0, 1);
  lcd.print(F("PIN: "));
  for (uint8_t i = 0; i < enteredPinLen; i++) {
    lcd.print('*');
  }
}

void clearLcdRow(uint8_t row) {
  lcd.setCursor(0, row);
  lcd.print(F("                "));
}
