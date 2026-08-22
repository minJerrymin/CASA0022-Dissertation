#include <MKRWAN.h>
#include <ArduinoLowPower.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_BMP280.h>
#include "arduino_secrets.h"

LoRaModem modem;
Adafruit_BMP280 bmp280;
String appEui = SECRET_TTN_JOIN_EUI;
String appKey = SECRET_TTN_APP_KEY;

// ================= Timing =================
const unsigned long CYCLE_MS = 600000UL;       // 10 minutes duty cycle
const unsigned long PMS_WARMUP_MS = 30000UL;
const unsigned long PMS_READ_TIMEOUT_MS = 8000UL;

// ================= Pins =================

const int PMS_BOOST_SHDN_PIN = 5;

// Serial1 pins on the MKR WAN 1310. The final prototype intentionally wires
// MKR TX1 to PMS TX and MKR RX1 to PMS RX; no crossover is used.
#ifndef PIN_SERIAL1_RX
#define PIN_SERIAL1_RX 13
#endif

#ifndef PIN_SERIAL1_TX
#define PIN_SERIAL1_TX 14
#endif


const uint8_t HDC1080_ADDR = 0x40;
const uint8_t BMP280_ADDR_PRIMARY = 0x76;
const uint8_t BMP280_ADDR_SECONDARY = 0x77;


const uint8_t PMS_WAKE_CMD[]  = {0x42, 0x4D, 0xE4, 0x00, 0x01, 0x01, 0x74};
const uint8_t PMS_SLEEP_CMD[] = {0x42, 0x4D, 0xE4, 0x00, 0x00, 0x01, 0x73};

struct PMSData {
  uint16_t pm25;
  uint16_t pm10;
  bool valid;
};

void setup() {
  // Serial output is disabled during deployed low-power operation.
  delay(15000);
#if defined(USBCON)
  USBDevice.detach();
#endif

  // Keep LED off
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Important: disable Pololu immediately after boot
  pinMode(PMS_BOOST_SHDN_PIN, OUTPUT);
  digitalWrite(PMS_BOOST_SHDN_PIN, LOW);   // Pololu off, PMS physically off
  //digitalWrite(PMS_BOOST_SHDN_PIN, HIGH);
  //delay(3000);

  isolatePMSPins();
  sleepI2CBus();

  if (!modem.begin(EU868)) {
    while (1) {
      LowPower.deepSleep(60000);
    }
  }

  joinTTN();

  modem.setPort(2);
  modem.minPollInterval(60);

  modem.sleep(true);
}


void loop() {
  unsigned long cycleStart = millis();

  // Wake LoRa modem
  modem.sleep(false);
  delay(500);

  // Power on PMS 5V branch
  powerPMSOn();

  // PMS warm-up after physical power-on
  delay(PMS_WARMUP_MS);

  float temperature = NAN;
  float humidity = NAN;
  bool hdcOK = readHDC1080(temperature, humidity);

  float pressure_hPa = NAN;
  bool bmpOK = readBMP280Pressure(pressure_hPa);

  PMSData pms = readPMS5003_PM25_PM10(PMS_READ_TIMEOUT_MS);

  if (!hdcOK) {
    temperature = 0;
    humidity = 0;
  }

  if (!pms.valid) {
    pms.pm25 = 65535;
    pms.pm10 = 65535;
  }

  if (!bmpOK) {
    pressure_hPa = 6553.5;  // Encoded as 65535 after x10; invalid sentinel
  }

  sendPayload_PM25_PM10_BMP280(temperature, humidity, pms.pm25, pms.pm10, pressure_hPa);

  // Give LoRa modem time to settle after uplink
  delay(3000);

  // Power down everything possible
  powerPMSOff();
  sleepI2CBus();

  modem.sleep(true);

#if defined(USBCON)
  USBDevice.detach();
#endif

  unsigned long activeTime = millis() - cycleStart;
  unsigned long sleepTime = 10000UL;

  if (activeTime < CYCLE_MS) {
    sleepTime = CYCLE_MS - activeTime;
  }

  LowPower.deepSleep(sleepTime);
}

// ================= TTN =================

void joinTTN() {
  int connected = 0;

  while (!connected) {
    modem.sleep(false);
    delay(500);

    connected = modem.joinOTAA(appEui, appKey);

    if (!connected) {
      modem.sleep(true);

#if defined(USBCON)
      USBDevice.detach();
#endif

      LowPower.deepSleep(60000);
    }
  }
}

void sendPayload_PM25_PM10_BMP280(float temperature, float humidity, uint16_t pm25, uint16_t pm10, float pressure_hPa) {
  int16_t tempInt = (int16_t)round(temperature * 100.0);
  uint16_t humInt = (uint16_t)round(humidity * 100.0);
  uint16_t pressureInt = (uint16_t)round(pressure_hPa * 10.0);  // hPa x10

  uint8_t payload[10];

  payload[0] = highByte(tempInt);
  payload[1] = lowByte(tempInt);

  payload[2] = highByte(humInt);
  payload[3] = lowByte(humInt);

  payload[4] = highByte(pm25);
  payload[5] = lowByte(pm25);

  payload[6] = highByte(pm10);
  payload[7] = lowByte(pm10);

  payload[8] = highByte(pressureInt);
  payload[9] = lowByte(pressureInt);

  modem.beginPacket();
  modem.write(payload, sizeof(payload));
  modem.endPacket(false);
}

// ================= PMS5003 power control =================

void powerPMSOn() {
  // Enable Pololu 5V boost
  pinMode(PMS_BOOST_SHDN_PIN, OUTPUT);
  digitalWrite(PMS_BOOST_SHDN_PIN, HIGH);
  delay(1000);

  Serial1.begin(9600);
  delay(500);

  // Serial1.write(PMS_WAKE_CMD, sizeof(PMS_WAKE_CMD));
  // Serial1.flush();
  // delay(500);

  flushPMS();
}

void powerPMSOff() {
  // Optional graceful PMS sleep command before power cut
  Serial1.write(PMS_SLEEP_CMD, sizeof(PMS_SLEEP_CMD));
  Serial1.flush();
  delay(300);

  Serial1.end();

  // Avoid back-powering the unpowered PMS through UART pins
  isolatePMSPins();

  // Disable Pololu 5V boost: PMS physically off
  digitalWrite(PMS_BOOST_SHDN_PIN, LOW);
}

void isolatePMSPins() {
  // Do not use INPUT_PULLUP here, because PMS is unpowered during sleep.
  // Pullups may back-power the PMS through protection paths.
  pinMode(PIN_SERIAL1_RX, INPUT);
  pinMode(PIN_SERIAL1_TX, INPUT);
}

void flushPMS() {
  while (Serial1.available()) {
    Serial1.read();
  }
}

PMSData readPMS5003_PM25_PM10(unsigned long timeoutMs) {
  PMSData data;
  data.pm25 = 0;
  data.pm10 = 0;
  data.valid = false;

  uint8_t frame[32];
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (Serial1.available()) {
      uint8_t b = Serial1.read();

      if (b != 0x42) {
        continue;
      }

      unsigned long waitStart = millis();
      while (!Serial1.available() && millis() - waitStart < 100) {}

      if (!Serial1.available()) {
        continue;
      }

      if (Serial1.read() != 0x4D) {
        continue;
      }

      frame[0] = 0x42;
      frame[1] = 0x4D;

      int index = 2;
      unsigned long frameStart = millis();

      while (index < 32 && millis() - frameStart < 1000) {
        if (Serial1.available()) {
          frame[index++] = Serial1.read();
        }
      }

      if (index != 32) {
        continue;
      }

      uint16_t sum = 0;
      for (int i = 0; i < 30; i++) {
        sum += frame[i];
      }

      uint16_t checksum = ((uint16_t)frame[30] << 8) | frame[31];

      if (sum != checksum) {
        continue;
      }

      uint16_t frameLen = ((uint16_t)frame[2] << 8) | frame[3];

      if (frameLen != 28) {
        continue;
      }

      // Atmospheric PM2.5 and PM10
      data.pm25 = ((uint16_t)frame[12] << 8) | frame[13];
      data.pm10 = ((uint16_t)frame[14] << 8) | frame[15];
      data.valid = true;
      return data;
    }
  }

  return data;
}


// ================= BMP280 =================

bool beginBMP280() {
  if (bmp280.begin(BMP280_ADDR_PRIMARY)) {
    return true;
  }

  if (bmp280.begin(BMP280_ADDR_SECONDARY)) {
    return true;
  }

  return false;
}

bool readBMP280Pressure(float &pressure_hPa) {
  Wire.begin();
  delay(10);

  if (!beginBMP280()) {
    sleepI2CBus();
    return false;
  }

  bmp280.setSampling(
    Adafruit_BMP280::MODE_FORCED,
    Adafruit_BMP280::SAMPLING_X1,
    Adafruit_BMP280::SAMPLING_X1,
    Adafruit_BMP280::FILTER_OFF,
    Adafruit_BMP280::STANDBY_MS_1
  );

  if (!bmp280.takeForcedMeasurement()) {
    sleepI2CBus();
    return false;
  }

  pressure_hPa = bmp280.readPressure() / 100.0;

  sleepI2CBus();
  return true;
}

// ================= HDC1080 =================

void setupHDC1080() {
  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x02);
  Wire.write(0x10);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(20);
}

bool readHDC1080(float &temperature, float &humidity) {
  Wire.begin();
  delay(10);
  setupHDC1080();

  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x00);

  if (Wire.endTransmission() != 0) {
    sleepI2CBus();
    return false;
  }

  delay(20);

  Wire.requestFrom(HDC1080_ADDR, (uint8_t)4);

  if (Wire.available() < 4) {
    sleepI2CBus();
    return false;
  }

  uint16_t rawT = ((uint16_t)Wire.read() << 8) | Wire.read();
  uint16_t rawH = ((uint16_t)Wire.read() << 8) | Wire.read();

  temperature = ((float)rawT / 65536.0) * 165.0 - 40.0;
  humidity = ((float)rawH / 65536.0) * 100.0;

  if (humidity < 0) humidity = 0;
  if (humidity > 100) humidity = 100;

  sleepI2CBus();
  return true;
}

void sleepI2CBus() {
  Wire.end();

  // HDC1080 and BMP280 remain powered by MKR 3.3V.
  // Keep I2C lines in stable high state for low leakage.
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
}
