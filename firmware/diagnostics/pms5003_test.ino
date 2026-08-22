struct PMSData {
  uint16_t pm1_cf1;
  uint16_t pm25_cf1;
  uint16_t pm10_cf1;

  uint16_t pm1_env;
  uint16_t pm25_env;
  uint16_t pm10_env;

  uint16_t particles_03um;
  uint16_t particles_05um;
  uint16_t particles_10um;
  uint16_t particles_25um;
  uint16_t particles_50um;
  uint16_t particles_100um;

  bool valid;
};

uint16_t readU16(uint8_t highByte, uint8_t lowByte) {
  return ((uint16_t)highByte << 8) | lowByte;
}

void printRawFrame(uint8_t *frame) {
  Serial.println("Raw frame:");
  for (int i = 0; i < 32; i++) {
    if (frame[i] < 16) Serial.print("0");
    Serial.print(frame[i], HEX);
    Serial.print(" ");

    if ((i + 1) % 16 == 0) {
      Serial.println();
    }
  }
}

bool readPMS5003(PMSData &data, unsigned long timeoutMs = 8000, bool showRawFrame = false) {
  uint8_t frame[32];
  unsigned long start = millis();

  data.valid = false;

  while (millis() - start < timeoutMs) {
    if (!Serial1.available()) {
      continue;
    }

    // Look for frame header: 0x42 0x4D
    int firstByte = Serial1.read();
    if (firstByte != 0x42) {
      continue;
    }

    while (!Serial1.available()) {
      if (millis() - start >= timeoutMs) return false;
    }

    int secondByte = Serial1.read();
    if (secondByte != 0x4D) {
      continue;
    }

    frame[0] = 0x42;
    frame[1] = 0x4D;

    for (int i = 2; i < 32; i++) {
      while (!Serial1.available()) {
        if (millis() - start >= timeoutMs) return false;
      }

      frame[i] = Serial1.read();
    }

    if (showRawFrame) {
      printRawFrame(frame);
    }

    uint16_t frameLength = readU16(frame[2], frame[3]);

    uint16_t checksum = 0;
    for (int i = 0; i < 30; i++) {
      checksum += frame[i];
    }

    uint16_t receivedChecksum = readU16(frame[30], frame[31]);

    Serial.print("Frame length: ");
    Serial.println(frameLength);

    Serial.print("Calculated checksum: ");
    Serial.println(checksum);

    Serial.print("Received checksum: ");
    Serial.println(receivedChecksum);

    if (frameLength != 28) {
      Serial.println("ERROR: Unexpected frame length.");
      return false;
    }

    if (checksum != receivedChecksum) {
      Serial.println("ERROR: Checksum failed.");
      return false;
    }

    // PMS5003 frame structure:
    // 0-1   Header: 0x42 0x4D
    // 2-3   Frame length: 28
    // 4-5   PM1.0 CF=1
    // 6-7   PM2.5 CF=1
    // 8-9   PM10  CF=1
    // 10-11 PM1.0 atmospheric environment
    // 12-13 PM2.5 atmospheric environment
    // 14-15 PM10  atmospheric environment
    // 16-17 particles >0.3um / 0.1L air
    // 18-19 particles >0.5um / 0.1L air
    // 20-21 particles >1.0um / 0.1L air
    // 22-23 particles >2.5um / 0.1L air
    // 24-25 particles >5.0um / 0.1L air
    // 26-27 particles >10um  / 0.1L air
    // 28-29 reserved / version / error code depending on model
    // 30-31 checksum

    data.pm1_cf1  = readU16(frame[4], frame[5]);
    data.pm25_cf1 = readU16(frame[6], frame[7]);
    data.pm10_cf1 = readU16(frame[8], frame[9]);

    data.pm1_env  = readU16(frame[10], frame[11]);
    data.pm25_env = readU16(frame[12], frame[13]);
    data.pm10_env = readU16(frame[14], frame[15]);

    data.particles_03um  = readU16(frame[16], frame[17]);
    data.particles_05um  = readU16(frame[18], frame[19]);
    data.particles_10um  = readU16(frame[20], frame[21]);
    data.particles_25um  = readU16(frame[22], frame[23]);
    data.particles_50um  = readU16(frame[24], frame[25]);
    data.particles_100um = readU16(frame[26], frame[27]);

    data.valid = true;
    return true;
  }

  return false;
}

void setup() {
  Serial.begin(115200);

  while (!Serial) {
    delay(10);
  }

  Serial.println();
  Serial.println("====================================");
  Serial.println("PMS5003 Local Test Only");
  Serial.println("No HDC1080 / No TTN / No LoRaWAN");
  Serial.println("====================================");

  Serial1.begin(9600);
  Serial.println("Serial1 started at 9600 baud.");

  Serial.println("Warming up PMS5003 for 30 seconds...");
  delay(30000);

  Serial.println("Start reading PMS5003.");
}

void loop() {
  PMSData pms;

  Serial.println();
  Serial.println("------------- PMS5003 Reading -------------");

  bool pmsOK = readPMS5003(pms, 8000, true);

  if (pmsOK && pms.valid) {
    Serial.println("PMS5003 frame valid.");

    Serial.println();
    Serial.println("Mass concentration, CF=1:");
    Serial.print("PM1.0  CF=1: ");
    Serial.print(pms.pm1_cf1);
    Serial.println(" ug/m3");

    Serial.print("PM2.5  CF=1: ");
    Serial.print(pms.pm25_cf1);
    Serial.println(" ug/m3");

    Serial.print("PM10   CF=1: ");
    Serial.print(pms.pm10_cf1);
    Serial.println(" ug/m3");

    Serial.println();
    Serial.println("Mass concentration, atmospheric environment:");
    Serial.print("PM1.0  atmospheric: ");
    Serial.print(pms.pm1_env);
    Serial.println(" ug/m3");

    Serial.print("PM2.5  atmospheric: ");
    Serial.print(pms.pm25_env);
    Serial.println(" ug/m3");

    Serial.print("PM10   atmospheric: ");
    Serial.print(pms.pm10_env);
    Serial.println(" ug/m3");

    Serial.println();
    Serial.println("Particle count per 0.1L air:");
    Serial.print(">0.3 um:  ");
    Serial.println(pms.particles_03um);

    Serial.print(">0.5 um:  ");
    Serial.println(pms.particles_05um);

    Serial.print(">1.0 um:  ");
    Serial.println(pms.particles_10um);

    Serial.print(">2.5 um:  ");
    Serial.println(pms.particles_25um);

    Serial.print(">5.0 um:  ");
    Serial.println(pms.particles_50um);

    Serial.print(">10 um:   ");
    Serial.println(pms.particles_100um);
  } else {
    Serial.println("PMS5003 read failed.");
    Serial.println("Check 5V, GND, TX/RX, fan, and Serial1 wiring.");
  }

  Serial.println("--------------------------------------------");

  delay(5000);
}