#ifndef CONFIG_EXAMPLE_H
#define CONFIG_EXAMPLE_H

/**
 * Copy this file to `config.h` in the same folder and fill in your credentials.
 * `config.h` is ignored by Git so secrets are never committed.
 */

// Wi-Fi credentials
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Firebase project & device account credentials
#define FIREBASE_API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define FIREBASE_PROJECT_ID "your-firebase-project-id"
#define FIREBASE_USER_EMAIL "device-account@example.com"
#define FIREBASE_USER_PASSWORD "YOUR_DEVICE_ACCOUNT_PASSWORD"

// Serial / UART configuration
// USB Serial is used for debug logs (115200 baud).
// Serial2 (GPIO16 RX2, GPIO17 TX2) is used for Arduino UNO UART (9600 baud).
// Remember to use a voltage divider (5V -> 3.3V) on Arduino TX -> ESP32 RX2!
#define DEBUG_MODE true
#define ARDUINO_SERIAL Serial2
#define ARDUINO_BAUD_RATE 9600
#define ARDUINO_RX_PIN 16
#define ARDUINO_TX_PIN 17

// NTP server for UTC event timestamps
#define NTP_SERVER "pool.ntp.org"

#endif
