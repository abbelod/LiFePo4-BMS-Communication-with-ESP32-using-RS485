#include <HardwareSerial.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ─── WiFi & MQTT config ───────────────────────────────────────
#define WIFI_SSID     "<YOUR WiFi SSID>"
#define WIFI_PASSWORD "<YOUR WiFi PASSWORD>"
#define MQTT_BROKER   "192.168.0.113"   // your MQTT broker IP
#define MQTT_PORT     1883		// your MQTT broker PORT
#define MQTT_USER     ""                // leave empty if no auth
#define MQTT_PASS     ""
#define MQTT_CLIENT   "esp32_bms"


// MQTT topics
#define TOPIC_SOC     "bms/soc"
#define TOPIC_VOLTAGE "bms/voltage"
#define TOPIC_CURRENT "bms/current"
#define TOPIC_TEMP    "bms/temperature"
#define TOPIC_STATUS  "bms/status"
#define TOPIC_CAP     "bms/capacity"

#define RS485_TX 17
#define RS485_RX 16
#define RS485_DE 4

HardwareSerial rs485(2);

const char* ssid = "StormFiber-4";
const char* password = "wharfedale786";
const char* mqtt_server = "192.168.0.118";
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ─── WiFi connect ─────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("Connecting to %s ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
}

// ─── MQTT connect ─────────────────────────────────────────────
void connectMQTT() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker...");
    bool connected = strlen(MQTT_USER) > 0
      ? mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)
      : mqtt.connect(MQTT_CLIENT);

    if (connected) {
      Serial.println("connected");
    } else {
      Serial.printf("failed rc=%d, retrying in 3s\n", mqtt.state());
      delay(3000);
    }
  }
}


void setup() {
  Serial.begin(115200);
  rs485.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);
  connectWiFi();
  connectMQTT();
}



uint16_t crc16(uint8_t* buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

void sendModbus(uint8_t* frame, int len) {
  // Print what we're sending
  Serial.print(">>> TX: ");
  for (int i = 0; i < len; i++)
    Serial.printf("%02X ", frame[i]);
  Serial.println();

  digitalWrite(RS485_DE, HIGH);
  delayMicroseconds(100);
  rs485.write(frame, len);
  rs485.flush();
  delayMicroseconds(100);
  digitalWrite(RS485_DE, LOW);
}

void decodeBMS(uint8_t* regs, int len) {

  uint16_t soc = (regs[106] << 8) | regs[107];  // correct
  uint16_t temp = (regs[80] << 8) | regs[81];   // correct
  uint16_t vol = (regs[104] << 8) | regs[105];  // correct
  uint16_t rawChargeCurr  = (regs[100] << 8) | regs[101];  // reg 0x0031
  uint16_t rawDischgCurr  = (regs[102] << 8) | regs[103];  // reg 0x0032

  float chargeCurr = rawChargeCurr/10.0;
  float dischargeCurr = rawDischgCurr/10.0;
  float temperature = temp/100.0;
  float voltage = vol/10.0;


Serial.print("soc: ");
Serial.println(soc);

Serial.print("temp: ");
Serial.println(temperature);

Serial.print("ChargeCurr: ");
Serial.println(chargeCurr);

Serial.print("DischareCurr: ");
Serial.println(dischargeCurr);

Serial.print("vol: ");
Serial.println(vol);
Serial.println(voltage);

  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  // ── Publish each value ──
  char payload[16];

snprintf(payload, sizeof(payload), "%.1f", voltage);
mqtt.publish(TOPIC_VOLTAGE, payload);

snprintf(payload, sizeof(payload), "%d", soc);
mqtt.publish(TOPIC_SOC, payload);

snprintf(payload, sizeof(payload), "%.1f", chargeCurr);
mqtt.publish("bms/charge_current", payload);

snprintf(payload, sizeof(payload), "%.1f", dischargeCurr);
mqtt.publish("bms/discharge_current", payload);

snprintf(payload, sizeof(payload), "%.1f", temperature);
mqtt.publish(TOPIC_TEMP, payload);

  Serial.println("[MQTT] Published all values");
}

void printHexDump(uint8_t* buf, int len) {
  Serial.printf("<<< RX: %d bytes\n", len);

  // Hex + ASCII side by side, 16 bytes per row
  for (int i = 0; i < len; i += 16) {
    Serial.printf("  %04X:  ", i);  // offset

    // Hex columns
    for (int j = 0; j < 16; j++) {
      if (i + j < len)
        Serial.printf("%02X ", buf[i + j]);
      else
        Serial.print("   ");        // padding for last row
      if (j == 7) Serial.print(" ");// extra space at midpoint
    }

    Serial.print("  ");

    // ASCII column
    for (int j = 0; j < 16 && i + j < len; j++) {
      char c = buf[i + j];
      Serial.print(isPrintable(c) ? c : '.');
    }
    Serial.println();
  }

  // CRC check (last 2 bytes are CRC in Modbus RTU)
  if (len >= 4) {
    uint16_t received = buf[len - 1] << 8 | buf[len - 2];
    uint16_t computed = crc16(buf, len - 2);
    Serial.printf("  CRC: received=0x%04X computed=0x%04X [%s]\n",
                  received, computed,
                  received == computed ? "OK" : "FAIL");
  }
  Serial.println();
}

void queryRegister(uint8_t devAddr, uint16_t regAddr, uint16_t count) {
  uint8_t req[8];
  req[0] = devAddr;
  req[1] = 0x03;           // Read Holding Registers
  req[2] = regAddr >> 8;
  req[3] = regAddr & 0xFF;
  req[4] = count >> 8;
  req[5] = count & 0xFF;
  uint16_t crc = crc16(req, 6);
  req[6] = crc & 0xFF;
  req[7] = crc >> 8;

  sendModbus(req, 8);

  // Collect response with timeout
  uint8_t resp[256];
  int n = 0;
  uint32_t deadline = millis() + 500;   // 500ms timeout

  while (millis() < deadline) {
    if (rs485.available()) {
      resp[n++] = rs485.read();
      deadline = millis() + 50;          // extend timeout on each new byte
      if (n >= 256) break;
    }
  }

  if (n == 0) {
    Serial.println("<<< RX: no response (timeout)\n");
    return;
  }

  // CRC check
  if (n>= 4) {
    uint16_t received = resp[n - 1] << 8 | resp[n - 2];
    uint16_t computed = crc16(resp, n - 2);
    // If CRC check pass, decode and publish over mqtt
    if(received == computed) {
      decodeBMS(resp, n);
    }
  }

  printHexDump(resp, n);
  // decodeBMS(resp, n);
}

void sweepAll() {
  uint16_t addr = 0x0000;
  uint16_t consecutiveFails = 0;

  Serial.println("Starting register sweep...\n");

  while (consecutiveFails < 5) {  // stop after 5 misses in a row
    Serial.printf("--- reg 0x%04X ---\n", addr);
    
    uint8_t req[8];
    req[0] = 0x01;
    req[1] = 0x03;
    req[2] = addr >> 8;
    req[3] = addr & 0xFF;
    req[4] = 0x00;
    req[5] = 0x01;          // read 1 register at a time
    uint16_t crc = crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = crc >> 8;

    sendModbus(req, 8);

    uint8_t resp[256];
    int n = 0;
    uint32_t deadline = millis() + 500;
    while (millis() < deadline) {
      if (rs485.available()) {
        resp[n++] = rs485.read();
        deadline = millis() + 1000;
        if (n >= 256) break;
      }
    }

    if (n == 0) {
      Serial.println("<<< no response\n");
      consecutiveFails++;
    } else if (n >= 3 && resp[1] & 0x80) {
      Serial.printf("<<< exception code: 0x%02X\n\n", resp[2]);
      consecutiveFails++;
    } else {
      printHexDump(resp, n);
      consecutiveFails = 0;  // reset on success
    }

    addr++;
    delay(200);
  }

  Serial.printf("Sweep done. Last valid register around 0x%04X\n", addr - consecutiveFails - 1);
}

void loop() {
  Serial.println("========================================");
  queryRegister(0x01, 0x0000, 60);
  // sweepAll();
  delay(1000);
}