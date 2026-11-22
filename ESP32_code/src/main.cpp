#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <SensirionI2CScd4x.h>
#include "iot_pins.h"
#include "secrets.h"

SensirionI2CScd4x scd4x;
uint32_t seq = 0;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t MEASUREMENT_INTERVAL_MS = 5000;
uint32_t lastMeasurementCheckMs = 0;

void checkButton2() {
  static int lastReading = HIGH;
  static int stableReading = HIGH;
  static uint32_t lastChangeMs = 0;

  const int reading = digitalRead(IoTPins::BUTTON_2);
  const uint32_t now = millis();
  if (reading != lastReading) {
    lastReading = reading;
    lastChangeMs = now;
  }

  if (reading != stableReading && now - lastChangeMs >= BUTTON_DEBOUNCE_MS) {
    stableReading = reading;
    if (stableReading == LOW) {
      Serial.println("Button 2 pressed");
    }
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  
  Serial.printf("\nConnecting to SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA); // Explicitly set station mode
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    // If it takes more than 15 seconds, print status and retry
    if (attempts > 30) {
      Serial.printf("\nConnection status code: %d (Refer to wl_status_t)", WiFi.status());
      Serial.println("\nRetrying Wi-Fi connection...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      attempts = 0;
    }
  }
  
  Serial.print("\nConnected! IP Address: ");
  Serial.println(WiFi.localIP());
}

void sendToFirebase(uint16_t co2, float tempC, float humPct) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  WiFiClientSecure client;
  client.setInsecure(); // Skip root cert validation for fast Lab 5 checkoff

  HTTPClient https;

  // Determine threshold status
  const char* status = "GREEN";
  if (co2 > 1200) status = "RED";
  else if (co2 >= 800) status = "YELLOW";

  // Build JSON payload matching your Progress Report schema
  String payload = "{";
  payload += "\"device_id\":\"group12-esp32-01\",";
  payload += "\"seq\":" + String(++seq) + ",";
  payload += "\"co2_ppm\":" + String(co2) + ",";
  payload += "\"temp_c\":" + String(tempC, 1) + ",";
  payload += "\"humidity_pct\":" + String(humPct, 1) + ",";
  payload += "\"status\":\"" + String(status) + "\",";
  payload += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"server_ts\":{\".sv\":\"timestamp\"}"; // Firebase auto-inserts UTC ms timestamp
  payload += "}";

  // 1. PUT to /latest.json for instant live dashboard display
  String latestUrl = String(FIREBASE_HOST) + "/devices/group12/latest.json";
  if (https.begin(client, latestUrl)) {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.PUT(payload);
    Serial.printf("[PUT latest] HTTP %d\n", httpCode);
    https.end();
  }

  // 2. POST to /history.json for historical time-series chart data
  String historyUrl = String(FIREBASE_HOST) + "/devices/group12/history.json";
  if (https.begin(client, historyUrl)) {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.POST(payload);
    Serial.printf("[POST history] HTTP %d | CO2: %u ppm | Temp: %.1f C\n", httpCode, co2, tempC);
    https.end();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(IoTPins::BUTTON_2, INPUT_PULLUP);
  delay(500);

  // Default ESP32 I2C pins: SDA = 21, SCL = 22
  Wire.begin(21, 22);
  scd4x.begin(Wire);

  // Stop any ongoing measurement before starting periodic mode
  scd4x.stopPeriodicMeasurement();
  delay(500);
  
  int16_t err = scd4x.startPeriodicMeasurement();
  if (err != 0) {
    Serial.printf("SCD40 start error: %d\n", err);
  } else {
    Serial.println("SCD40 started successfully. Waiting 5s for first measurement...");
  }

  connectWiFi();
  lastMeasurementCheckMs = millis();
}

void loop() {
  checkButton2();

  const uint32_t now = millis();
  if (now - lastMeasurementCheckMs < MEASUREMENT_INTERVAL_MS) return;
  lastMeasurementCheckMs = now;

  bool dataReady = false;
  uint16_t error = scd4x.getDataReadyFlag(dataReady);
  if (!dataReady) {
    Serial.println("Data not ready yet...");
    return;
  }

  uint16_t co2 = 0;
  float tempC = 0.0f;
  float humPct = 0.0f;

  int16_t err = scd4x.readMeasurement(co2, tempC, humPct);
  if (err != 0 || co2 == 0) {
    Serial.printf("Read error or invalid CO2 (err=%d, co2=%u)\n", err, co2);
    return;
  }

  sendToFirebase(co2, tempC, humPct);
}
