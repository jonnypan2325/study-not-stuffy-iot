#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <SensirionI2CScd4x.h>
#include "iot_pins.h"
#include "iot_display.h"
#include "secrets.h"

SensirionI2CScd4x scd4x;
uint32_t seq = 0;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t MEASUREMENT_INTERVAL_MS = 5000;
uint32_t lastMeasurementCheckMs = 0;

constexpr uint16_t FIRST_RED_PPM = 1201;
constexpr size_t SAMPLE_COUNT = 13;  // One minute at the normal sampling interval.
struct Co2Sample {
  uint32_t timeMs;
  uint16_t ppm;
};
Co2Sample samples[SAMPLE_COUNT];
size_t nextSample = 0;
size_t samplesUsed = 0;

void recordCo2(uint16_t ppm) {
  samples[nextSample] = {millis(), ppm};
  nextSample = (nextSample + 1) % SAMPLE_COUNT;
  if (samplesUsed < SAMPLE_COUNT) ++samplesUsed;
}

void showCo2Forecast() {
  static bool displayReady = false;
  if (!displayReady) {
    displayReady = iotInitDisplay();
    if (!displayReady) return;
  }

  if (samplesUsed == 0) {
    iotDisplayLines("Waiting for CO2 data");
    return;
  }

  const Co2Sample& latest = samples[(nextSample + SAMPLE_COUNT - 1) % SAMPLE_COUNT];
  const String current = String("CO2: ") + latest.ppm + " ppm";
  if (millis() - latest.timeMs > 15000) {
    iotDisplayLines(current, "Reading is stale");
    return;
  }

  const bool alreadyRed = latest.ppm >= FIRST_RED_PPM;
  // Once the buffer is full, nextSample points to the oldest reading.
  const Co2Sample& oldest = samples[nextSample];
  const uint32_t elapsedMs = latest.timeMs - oldest.timeMs;
  if (samplesUsed < SAMPLE_COUNT || elapsedMs == 0 || elapsedMs > 90000) {
    iotDisplayLines(current, "Collecting trend...", alreadyRed ? "Already RED" : "");
    return;
  }

  const float rate = (int(latest.ppm) - int(oldest.ppm)) * 60000.0f / elapsedMs;
  const String trend = String("Rate: ") + (rate > 0 ? "+" : "") + String(rate, 1) + " ppm/min";
  String estimate;
  if (alreadyRed) {
    estimate = "Already RED";
  } else if (rate <= 0) {
    estimate = "CO2 flat or falling";
  } else {
    const float minutes = (FIRST_RED_PPM - latest.ppm) / rate;
    estimate = minutes < 1 ? String("RED in <1 min")
                           : String("RED in ~") + String(minutes, 0) + " min";
  }
  iotDisplayLines(current, trend, estimate);
}

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
      showCo2Forecast();
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

  recordCo2(co2);
  sendToFirebase(co2, tempC, humPct);
}
