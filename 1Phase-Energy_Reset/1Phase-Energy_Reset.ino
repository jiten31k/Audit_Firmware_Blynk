#define BLYNK_PRINT Serial
#define BLYNK_TEMPLATE_ID "TMPL387zUUjau"
#define BLYNK_TEMPLATE_NAME "EnergInAI"

#include <WiFi.h>
#include <WiFiManager.h>
#include <BlynkSimpleEsp32.h>
#include <ModbusMaster.h>
#include <Wire.h>
#include <ArduinoOTA.h>

char auth[] = "HyRd7H2nbq23p1MQPjqM172hUkCEtI5A";
const char *WIFI_AP_SSID = "ENSS_001";
const char *WIFI_AP_PASSWORD = "energinai@123";

#define RX_PIN 16
#define TX_PIN 17
#define MODBUS_ID 1
#define SERIAL_BAUD 4800

ModbusMaster node;
BlynkTimer timer;

const unsigned long interval_ms = 15000;

float voltage = NAN, current = NAN, power = NAN, energy = NAN, powerFactor = NAN, frequency = NAN;

float readFloat32(uint16_t addr, float divisor) {
  uint8_t result = node.readHoldingRegisters(addr, 2);
  if (result == node.ku8MBSuccess) {
    uint32_t hi = node.getResponseBuffer(0);
    uint32_t lo = node.getResponseBuffer(1);
    uint32_t raw = ((uint32_t)hi << 16) | lo;
    return raw / divisor;
  }
  return NAN;
}

void sendToBlynk(float v, float c, float p, float e, float f) {
  if (!Blynk.connected()) return;
  Blynk.virtualWrite(V0, v);
  Blynk.virtualWrite(V1, c);
  Blynk.virtualWrite(V2, p);
  Blynk.virtualWrite(V3, e);
  Blynk.virtualWrite(V4, f);
}

uint16_t crc16_modbus(const uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (int i = 8; i != 0; i--) {
      if ((crc & 0x0001) != 0) {
        crc >>= 1;
        crc ^= 0xA001;
      } else
        crc >>= 1;
    }
  }
  return crc;
}

bool sendResetCommand() {
  uint8_t packet[13] = {
    MODBUS_ID,
    0x10,
    0x00, 0x0C,
    0x00, 0x02,
    0x04,
    0x00, 0x00,
    0x00, 0x00,
    0x00, 0x00
  };

  uint16_t crc = crc16_modbus(packet, 11);
  packet[11] = crc & 0xFF;
  packet[12] = (crc >> 8) & 0xFF;

  Serial2.write(packet, sizeof(packet));
  Serial.println("Reset command sent, waiting for response...");

  uint8_t response[8];
  int idx = 0;
  unsigned long start_time = millis();
  while ((millis() - start_time) < 1000 && idx < 8) {
    if (Serial2.available()) {
      response[idx++] = Serial2.read();
    }
  }

  if (idx >= 8) {
    if (response[0] == packet[0] && response[1] == packet[1] &&
        response[2] == packet[2] && response[3] == packet[3] &&
        response[4] == packet[4] && response[5] == packet[5]) {
      uint16_t resp_crc = (response[7] << 8) | response[6];
      if (resp_crc == crc16_modbus(response, 6)) {
        Serial.println("Energy reset successful.");
        return true;
      } else {
        Serial.println("CRC error in response.");
      }
    } else {
      Serial.println("Invalid response from meter.");
    }
  } else {
    Serial.println("No or incomplete response from meter.");
  }
  return false;
}

void energyReset() {
  if (sendResetCommand()) {
    Serial.println("Energy meter reset success.");
  } else {
    Serial.println("Energy meter reset failed.");
  }
}

void readAll() {
  voltage     = readFloat32(0x0048, 10000.0);
  current     = readFloat32(0x0049, 10000.0);
  power       = readFloat32(0x004A, 10000.0);
  energy      = readFloat32(0x004B, 10000.0);
  powerFactor = readFloat32(0x004C, 1000.0);
  frequency   = readFloat32(0x004F, 100.0);

  if (current < 0.1) {
    voltage     = readFloat32(0x0050, 10000.0);
    current     = readFloat32(0x0051, 10000.0);
    power       = readFloat32(0x0052, 10000.0);
    energy      = readFloat32(0x0053, 10000.0);
    powerFactor = readFloat32(0x0054, 1000.0);
    Serial.println("Switched to Channel 2");
  }

  Serial.printf("V: %.2fV | I: %.3fA | P: %.2fW | kWh: %.3f | PF: %.3f | F: %.2fHz\n",
                voltage, current, power, energy, powerFactor, frequency);

  sendToBlynk(voltage, current, power, energy, frequency);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("EnergInAI");
  WiFiManager wm;
  wm.setConnectTimeout(30);
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect(WIFI_AP_SSID, WIFI_AP_PASSWORD)) ESP.restart();
  Serial.println("Connected to WiFi!");
}

void setupOTA() {
  ArduinoOTA.setHostname("EnergInAI-OTA");
  ArduinoOTA.setPassword("energinai@123");
  ArduinoOTA.onStart([]() { Serial.println("OTA Update Started"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA Update Complete"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\n", (progress * 100U) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error [%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
}

BLYNK_WRITE(V6) {
  int pinValue = param.asInt();
  if (pinValue == 1) {
    Serial.println("Blynk V6 ON - Triggering energy reset...");
    energyReset();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== EnergInAI Boot ===");

  connectWiFi();
  Blynk.config(auth);
  if (WiFi.status() == WL_CONNECTED) Blynk.connect();

  Serial2.begin(SERIAL_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(MODBUS_ID, Serial2);

  timer.setInterval(interval_ms, readAll);
  setupOTA();

  Serial.println("EnergInAI Started Successfully!");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!Blynk.connected() && WiFi.status() == WL_CONNECTED) Blynk.connect(5000);

  Blynk.run();
  timer.run();
  ArduinoOTA.handle();
}
