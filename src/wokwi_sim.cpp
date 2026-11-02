#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>

// Pin Definitions for Simulation
#define SS_PIN    5
#define RST_PIN   22
#define LED_PIN   13

MFRC522 rfid(SS_PIN, RST_PIN);

// Network Config for Wokwi Gateway
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// Host Gateway IP (10.0.2.2 reaches the Host PC in Wokwi)
const char* broker_url = "http://10.0.2.2:5000/api/kids/tap";
const char* status_url = "http://10.0.2.2:5000/api/player/status";

void handleCardTap(String uidStr);
void checkBrokerStatus();

void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Serial.println();
    Serial.println("===============================================");
    Serial.println("  Yoto ESP32 Controller - Virtual Hardware Bench");
    Serial.println("  Wokwi Simulation + PlatformIO");
    Serial.println("===============================================");

    // Initialize SPI and MFRC522
    SPI.begin(18, 19, 23, SS_PIN);
    rfid.PCD_Init();
    Serial.println("[NFC] MFRC522 Reader Initialized (SPI: SCK=18, MISO=19, MOSI=23, SS=5)");

    // Connect to WiFi
    Serial.print("[WiFi] Connecting to ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        Serial.print(".");
        timeout++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        Serial.print("[WiFi] Connected! IP Address: ");
        Serial.println(WiFi.localIP());
        digitalWrite(LED_PIN, HIGH);
        checkBrokerStatus();
    } else {
        Serial.println();
        Serial.println("[WiFi] Connection timed out (Wokwi Gateway required for LAN routing)");
    }
}

unsigned long lastStatusCheck = 0;

void loop() {
    // 1. Check for NFC Card Tap
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        String uidStr = "";
        for (byte i = 0; i < rfid.uid.size; i++) {
            if (rfid.uid.uidByte[i] < 0x10) uidStr += "0";
            uidStr += String(rfid.uid.uidByte[i], HEX);
        }
        uidStr.toUpperCase();

        Serial.println();
        Serial.println("-----------------------------------------------");
        Serial.print("[NFC] Card Detected! UID: ");
        Serial.println(uidStr);

        handleCardTap(uidStr);

        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        delay(1000); // Debounce card reads
    }

    // 2. Periodic Status Ping to Broker every 10 seconds
    if (millis() - lastStatusCheck > 10000) {
        lastStatusCheck = millis();
        if (WiFi.status() == WL_CONNECTED) {
            checkBrokerStatus();
        }
    }

    delay(50);
}

void handleCardTap(String uidStr) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTP] WiFi not connected; cannot send tap event.");
        return;
    }

    HTTPClient http;
    http.begin(broker_url);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["nfc_uid"] = uidStr;
    doc["device_id"] = "wokwi-sim-01";

    String requestBody;
    serializeJson(doc, requestBody);

    Serial.print("[HTTP] POST ");
    Serial.print(broker_url);
    Serial.print(" -> ");
    Serial.println(requestBody);

    int httpResponseCode = http.POST(requestBody);

    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.print("[HTTP] Response (");
        Serial.print(httpResponseCode);
        Serial.print("): ");
        Serial.println(response);
    } else {
        Serial.print("[HTTP] Error on sending POST: ");
        Serial.println(httpResponseCode);
    }

    http.end();
}

void checkBrokerStatus() {
    HTTPClient http;
    http.begin(status_url);

    int httpResponseCode = http.GET();
    if (httpResponseCode > 0) {
        String payload = http.getString();
        Serial.print("[Broker Status] ");
        Serial.println(payload);
    }
    http.end();
}
