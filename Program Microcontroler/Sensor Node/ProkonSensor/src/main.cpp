#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// =================================================================
// 0. FUNCTION PROTOTYPES
// =================================================================
void setupWiFi();
void sendSensorData();

// =================================================================
// 1. KONFIGURASI JARINGAN & SERVER
// =================================================================
const char* ssid = "Galaxy A33 5G D004"; 
const char* password = "gahya123";      
const char* apiHost = "10.163.159.210";   // Pastikan ini IP Laptop kamu
const int apiPort = 8000;                // Laravel default port adalah 8000

// Endpoint Laravel API
const char* apiReceiveSensorEndpoint = "/api/receive-sensor";

// =================================================================
// 2. KONFIGURASI PIN HARDWARE
// =================================================================
const int DHT_PIN = 5;   
const int SOIL_PIN = 33; 

#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// =================================================================
// 3. KONFIGURASI INTERVAL & KALIBRASI
// =================================================================
const long SENSOR_REPORT_INTERVAL = 30000; // Testing: kirim setiap 30 detik
unsigned long lastSensorReport = 0;

// KALIBRASI SOIL MOISTURE
// Sensor Kapasitif biasanya: Kering (nilai besar), Basah (nilai kecil)
const int SOIL_DRY = 3500; 
const int SOIL_WET = 1200; 

// =================================================================
// 4. SETUP & LOOP
// =================================================================

void setup() {
    Serial.begin(115200); // Baudrate standar ESP32
    
    dht.begin();
    pinMode(SOIL_PIN, INPUT); 

    setupWiFi(); 
    
    // Kirim data pertama kali saat startup
    if (WiFi.status() == WL_CONNECTED) {
        sendSensorData();
    }
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        setupWiFi(); 
        return;
    }

    if (millis() - lastSensorReport >= SENSOR_REPORT_INTERVAL) {
        sendSensorData(); 
        lastSensorReport = millis();
    }
}

// =================================================================
// 5. FUNGSI WiFi 
// =================================================================

void setupWiFi() {
    Serial.printf("\nConnecting to %s ", ssid);
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { 
        delay(500); 
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi Connected!");
        Serial.print("IP ESP32: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n❌ WiFi Failed. Restarting...");
        delay(3000);
        ESP.restart(); 
    }
}

// =================================================================
// 6. FUNGSI KIRIM DATA SENSOR
// =================================================================

void sendSensorData() {
    if (WiFi.status() != WL_CONNECTED) return; 

    // Baca Sensor
    float t = dht.readTemperature(); 
    float h = dht.readHumidity();    
    int soil_analog = analogRead(SOIL_PIN); 
    
    // Konversi ke persen
    float soil_percent = map(soil_analog, SOIL_DRY, SOIL_WET, 0, 100);
    soil_percent = constrain(soil_percent, 0, 100); 
    
    // VALIDASI PEMBACAAN DHT (DIUBAH DI SINI)
    // nan = Not a Number (sensor tidak terdeteksi)
    // t < -10 atau t > 60 = Filter untuk pembacaan sampah/error
    if (isnan(h) || isnan(t) || t < -10 || t > 60) {
        Serial.printf("❌ DHT Error! Bacaan: T=%.1f H=%.1f (Data tidak dikirim)\n", t, h);
        return; 
    }

    HTTPClient http;
    String url = "http://" + String(apiHost) + ":" + String(apiPort) + String(apiReceiveSensorEndpoint);
    
    JsonDocument doc; 
    doc["temp"] = t;
    doc["humid"] = h;
    doc["soil"] = (int)soil_percent; 

    String payload;
    serializeJson(doc, payload);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    Serial.print("⬆️ Sending to Laravel: ");
    Serial.println(payload);

    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST(payload);
    
    if (httpResponseCode > 0) {
        Serial.printf("✅ Response: %d\n", httpResponseCode);
        if(httpResponseCode == 200) {
             Serial.println("   Data saved successfully!");
        }
    } else {
        Serial.printf("❌ Error: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    
    http.end();
}