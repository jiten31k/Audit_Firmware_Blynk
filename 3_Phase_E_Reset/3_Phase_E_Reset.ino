#define BLYNK_TEMPLATE_ID   "TMPL3udwGtdaA"
#define BLYNK_TEMPLATE_NAME "EnergInAI"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiManager.h>
#include <BlynkSimpleEsp32.h>
#include <ModbusMaster.h>
#include <ArduinoOTA.h>

char auth[] = "NXL4WubypxNAEGgP3c4-YUGUWvsnO2u-";
const char *WIFI_AP_SSID = "ENTS_001";
const char *WIFI_AP_PASSWORD = "energinai@123";

#define RX_PIN 16
#define TX_PIN 17
#define MODBUS_ID 1
#define SERIAL_BAUD 9600

ModbusMaster node;
BlynkTimer timer;

struct Phase {
  float V = NAN;
  float I = NAN;
  float P = NAN;
  float E = NAN;
  float PF = NAN;
  bool active = false;
};

Phase phases[3];
float frequencyHz = NAN;
float meterPF = NAN;
float totalEnergy = NAN;

uint16_t crc16_modbus(uint8_t *buf, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 0x0001)
        crc = (crc >> 1) ^ 0xA001;
      else
        crc >>= 1;
    }
  }
  return crc;
}

float readUInt16(uint16_t addr, float divisor = 1.0) {
  for (int r = 0; r < 3; r++) {
    uint8_t res = node.readHoldingRegisters(addr, 1);
    if (res == node.ku8MBSuccess) return node.getResponseBuffer(0) / divisor;
    delay(10);
  }
  return NAN;
}

float readUInt32(uint16_t addrHigh, float divisor = 1.0) {
  for (int r = 0; r < 3; r++) {
    uint8_t res = node.readHoldingRegisters(addrHigh, 2);
    if (res == node.ku8MBSuccess) {
      uint32_t hi = node.getResponseBuffer(0);
      uint32_t lo = node.getResponseBuffer(1);
      return ((hi << 16) | lo) / divisor;
    }
    delay(10);
  }
  return NAN;
}

bool isPhaseActive(float V, float I) {
  return !isnan(V) && !isnan(I) && V > 10.0 && I > 0.01;
}

void readAllPhases() {
  int activeCount = 0;
  float vSum = 0, iSum = 0, pSum = 0, eSum = 0, pfSum = 0;

  for (int ph = 0; ph < 3; ph++) {
    phases[ph].V  = readUInt16(0x0100 + ph, 100.0);
    phases[ph].I  = readUInt16(0x0103 + ph, 100.0);
    phases[ph].P  = readUInt16(0x0106 + ph, 1.0);
    phases[ph].PF = readUInt16(0x0116 + ph, 1000.0);
    phases[ph].E  = readUInt32(0x011A + (ph * 2), 100.0);
    phases[ph].active = isPhaseActive(phases[ph].V, phases[ph].I);

    if (phases[ph].active) {
      activeCount++;
      vSum += phases[ph].V;
      iSum += phases[ph].I;
      pSum += phases[ph].P;
      eSum += phases[ph].E;
      pfSum += phases[ph].PF;
    }
  }

  frequencyHz = readUInt16(0x0115, 100.0);
  if (isnan(frequencyHz)) frequencyHz = 0.0f;

  meterPF = readUInt16(0x0119, 1000.0);
  if (isnan(meterPF)) meterPF = (activeCount > 0) ? (pfSum / activeCount) : 0.0f;

  totalEnergy = readUInt32(0x0120, 100.0);
  if (isnan(totalEnergy)) totalEnergy = eSum;

  float Vavg = (activeCount > 0) ? vSum / activeCount : 0;

  Serial.printf("Active Phases: %d | Vavg: %.2f V | I: %.3f A | P: %.2f W | Etot: %.3f kWh | PF: %.3f | F: %.2f Hz\n",
                activeCount, Vavg, iSum, pSum, totalEnergy, meterPF, frequencyHz);

  for (int ph = 0; ph < 3; ph++) {
    Serial.printf("  Ph%c: V=%.2f V | I=%.3f A | P=%.2f W | E=%.3f kWh | PF=%.3f | Active=%s\n",
                  'A'+ph, phases[ph].V, phases[ph].I, phases[ph].P, phases[ph].E, phases[ph].PF,
                  phases[ph].active ? "YES" : "NO");
  }

  if (WiFi.status() == WL_CONNECTED && Blynk.connected()) {
    Blynk.virtualWrite(V0, Vavg);
    Blynk.virtualWrite(V1, iSum);
    Blynk.virtualWrite(V2, pSum);
    Blynk.virtualWrite(V3, totalEnergy);
    Blynk.virtualWrite(V4, meterPF);
    Blynk.virtualWrite(V5, frequencyHz);
  }
}

bool sendResetCommand_MK333G(uint8_t slaveId) {
  uint8_t packet[13];
  packet[0] = slaveId;
  packet[1] = 0x10;         
  packet[2] = 0x00; packet[3] = 0x0C; 
  packet[4] = 0x00; packet[5] = 0x02; 
  packet[6] = 0x04;       
  packet[7] = 0x00; packet[8] = 0x00;
  packet[9] = 0x00; packet[10] = 0x00;

  uint16_t crc = crc16_modbus(packet, 11);
  packet[11] = crc & 0xFF;
  packet[12] = (crc >> 8) & 0xFF;

  Serial2.write(packet, sizeof(packet));
  Serial.println("Sending Reset Command to MK333G...");

  uint8_t response[8];
  int idx = 0;
  unsigned long start = millis();
  while ((millis() - start) < 1000 && idx < 8) {
    if (Serial2.available()) response[idx++] = Serial2.read();
  }

  if (idx >= 8 && response[0] == slaveId && response[1] == 0x10 &&
      response[2] == 0x00 && response[3] == 0x0C) {
    Serial.println("Energy Reset Successful!");
    return true;
  } else {
    Serial.println("Reset Failed: No or invalid response.");
    return false;
  }
}

BLYNK_WRITE(V6) {
  int pinValue = param.asInt();
  if (pinValue == 1) {
    if (sendResetCommand_MK333G(MODBUS_ID)) {
      Blynk.virtualWrite(V6, 0);
      delay(200);
      readAllPhases(); 
    }
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("EnergInAI");
  WiFiManager wm;
  wm.setConnectTimeout(30);
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect(WIFI_AP_SSID, WIFI_AP_PASSWORD)) ESP.restart();
}

void setupOTA() {
  ArduinoOTA.setHostname("EnergInAI-OTA");
  ArduinoOTA.setPassword("energinai@123");
  ArduinoOTA.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  connectWiFi();
  Blynk.config(auth);
  if (WiFi.status() == WL_CONNECTED) Blynk.connect();

  Serial2.begin(SERIAL_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  node.begin(MODBUS_ID, Serial2);

  timer.setInterval(15000L, readAllPhases);
  setupOTA();

  Serial.println("Setup Complete — EnergInAI MK333G Ready!");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!Blynk.connected() && WiFi.status() == WL_CONNECTED) Blynk.connect(5000);

  Blynk.run();
  timer.run();
  ArduinoOTA.handle();
}
