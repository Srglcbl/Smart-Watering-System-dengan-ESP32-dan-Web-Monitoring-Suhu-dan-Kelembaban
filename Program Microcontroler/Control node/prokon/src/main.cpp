/*
 * SISTEM PENYIRAMAN TANAMAN OTOMATIS - MODE LOKAL (HTTP Polling)
 * ESP32 + RTC DS3231 + Solenoid Valve + Web API Lokal
 * * * Fitur Utama:
 * - Penyiraman otomatis berdasarkan jadwal RTC/EEPROM
 * - Kontrol manual dari API Web Server Lokal (water-status)
 * - Sinkronisasi Jadwal dan Durasi dari API Web Server Lokal (schedules)
 */

// ==================== LIBRARY ====================
#include <WiFi.h>
#include <Wire.h>
#include <RTClib.h> 
#include <EEPROM.h>
#include <time.h> 
#include <HTTPClient.h> 
#include <ArduinoJson.h> 

// ==================== DEFINISI MINIMAL TIMER CLASS ====================
class MinimalTimer {
private:
    struct TimerJob {
        long interval;
        unsigned long prevMillis;
        void (*callback)(); 
        bool enabled;
        int id; 
    };
    TimerJob jobs[5]; 
    int jobCount = 0;

public:
    MinimalTimer() {}

    int setInterval(long interval, void (*callback)()) {
        if (jobCount >= 5) return -1; 
        jobs[jobCount].interval = interval;
        jobs[jobCount].prevMillis = millis();
        jobs[jobCount].callback = callback;
        jobs[jobCount].enabled = true;
        jobs[jobCount].id = jobCount;
        return jobCount++;
    }

    void run() {
        for (int i = 0; i < jobCount; i++) {
            if (jobs[i].enabled) {
                if (millis() - jobs[i].prevMillis >= (unsigned long)jobs[i].interval) {
                    jobs[i].prevMillis = millis(); 
                    jobs[i].callback();             
                }
            }
        }
    }
    
    void disable(int id) {
        if (id >= 0 && id < jobCount) {
            jobs[id].enabled = false;
        }
    }
};
// ====================================================================
// ==================== KONFIGURASI PIN & JARINGAN ====================
#define RELAY_PIN 5 
#define SDA_PIN 21  
#define SCL_PIN 22  
#define LED_PIN 2 

// --- KONFIGURASI WIFI & API ---
char ssid[] = "Galaxy A33 5G D004"; 
char pass[] = "gahya123";  

// --- KONFIGURASI API LOKAL (SERVER SIDE) ---
// ⚠️ GANTI IP INI DENGAN IP KOMPUTER/SERVER LARAVEL ANDA
const char* apiHost = "10.163.159.210"; // Ganti dengan IP server Laravel Anda
const int apiPort = 8000; 
const char* apiEndpoint = "/api/water-status"; 
// Di bagian konfigurasi API, ubah:
const char* apiScheduleEndpoint = "/api/schedules/esp32"; // 

const char* ntpServer = "id.pool.ntp.org"; 
const long gmtOffset_sec = 7 * 3600; 
const int daylightOffset_sec = 0;

// ==================== OBJEK ====================

RTC_DS3231 rtc;
MinimalTimer timer; 

// ==================== STRUKTUR DATA (EEPROM) ====================
struct Schedule {
    int hour;
    int minute;
    bool enabled;
};

struct Config {
    Schedule schedules[3];
    int duration; 
    int wateringCount;
    int magicNumber; 
} config;

#define MAGIC_NUMBER 54321
#define EEPROM_SIZE 512

// ==================== VARIABEL GLOBAL ====================
bool valveOpen = false;
bool isWatering = false;
bool manualMode = false; 
unsigned long valveOpenTime = 0;
bool lastScheduleCheck[3] = {false, false, false}; 
long lastRTCSync = 0; 
const long REMOTE_CHECK_INTERVAL = 5000L; 

// =========================================================
// ================ DEFINISI FUNGSI ==========================
// =========================================================

// --- FUNGSI HELPER & EEPROM ---
void blinkError() {
    for (int i = 0; i < 10; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(100);
        digitalWrite(LED_PIN, LOW);
        delay(100);
    }
}

void displayConfig() {
    Serial.println("📋 KONFIGURASI SISTEM:");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    for (int i = 0; i < 3; i++) {
        Serial.printf("    Jadwal %d: %02d:%02d (%s)\n", 
            i+1, 
            config.schedules[i].hour, 
            config.schedules[i].minute,
            config.schedules[i].enabled ? "AKTIF" : "NONAKTIF");
    }
    
    Serial.printf("    Durasi: %d detik\n", config.duration);
    Serial.printf("    Total Penyiraman: %d kali\n", config.wateringCount);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

void saveConfig() {
    EEPROM.put(0, config);
    EEPROM.commit();
}

void loadConfig() {
    EEPROM.get(0, config);
    
    if (config.magicNumber != MAGIC_NUMBER) {
        Serial.println("⚙️  Inisialisasi konfigurasi default...");
        
        config.schedules[0] = {6, 0, true};  
        config.schedules[1] = {18, 0, true}; 
        config.schedules[2] = {12, 0, false}; 
        config.duration = 30; 
        config.wateringCount = 0;
        config.magicNumber = MAGIC_NUMBER;
        
        saveConfig();
    } else {
        Serial.println("✅ Konfigurasi loaded dari EEPROM");
    }
}

// --- FUNGSI VALVE ---
void closeValve(); 

void openValve() {
    if (!valveOpen) {
        digitalWrite(RELAY_PIN, HIGH); 
        digitalWrite(LED_PIN, HIGH); 
        valveOpen = true;
        isWatering = true;
        valveOpenTime = millis();
        
        DateTime now = rtc.now();
        Serial.printf("[%02d:%02d:%02d] 💧 VALVE DIBUKA\n", 
                        now.hour(), now.minute(), now.second());
    }
}

void closeValve() {
    if (valveOpen) {
        digitalWrite(RELAY_PIN, LOW); 
        digitalWrite(LED_PIN, LOW); 
        
        unsigned long duration = (millis() - valveOpenTime) / 1000;
        
        valveOpen = false;
        isWatering = false;
        manualMode = false; 
        
        DateTime now = rtc.now();
        Serial.printf("[%02d:%02d:%02d] 🔒 VALVE DITUTUP - Durasi: %lu detik\n", 
                        now.hour(), now.minute(), now.second(), duration);
    }
}

// --- FUNGSI JADWAL & TIME SYNC ---
void coreRTCSyncLogic() {
    struct tm timeinfo;
    
    if (getLocalTime(&timeinfo, 5000)) { 
        time_t now = mktime(&timeinfo); 
        rtc.adjust(DateTime(now)); 
        
        DateTime newTime = rtc.now();
        Serial.printf("✅ Waktu RTC diupdate dari NTP: %04d-%02d-%02d %02d:%02d:%02d\n", 
                        newTime.year(), newTime.month(), newTime.day(),
                        newTime.hour(), newTime.minute(), newTime.second());
        
        lastRTCSync = millis();
    } else {
        Serial.println("⚠️ Gagal mendapatkan waktu dari server NTP. Periksa koneksi WiFi.");
    }
}

void syncRTCFromNTP() {
    if (lastRTCSync == 0 || millis() - lastRTCSync > 900000L) { 
        Serial.println("🔄 Memulai sinkronisasi RTC (Otomatis 15m interval)...");
        coreRTCSyncLogic();
    }
}

void checkSchedule() {
    if (manualMode || isWatering) return;
    
    DateTime now = rtc.now();
    
    for (int i = 0; i < 3; i++) {
        if (!config.schedules[i].enabled) continue;
        
        bool matchSchedule = (now.hour() == config.schedules[i].hour && 
                              now.minute() == config.schedules[i].minute && 
                              now.second() == 0);
        
        if (matchSchedule && !lastScheduleCheck[i]) {
            openValve();
            config.wateringCount++;
            saveConfig();
            
            Serial.printf("⏰ JADWAL #%d AKTIF (%02d:%02d)\n", 
                          i+1, config.schedules[i].hour, config.schedules[i].minute);
        }
        
        lastScheduleCheck[i] = matchSchedule;
    }
}

// Fungsi set waktu manual (serial monitor)
void setRTCFromSerial() {
    Serial.println("\nMasukkan Tanggal dan Waktu baru (DD/MM/YYYY HH:MM), lalu tekan Enter:");
    
    String input = Serial.readStringUntil('\n'); 
    input.trim();

    if (input.length() != 16 || input[2] != '/' || input[5] != '/' || input[10] != ' ') {
        Serial.println("❌ Format salah. Gunakan DD/MM/YYYY HH:MM (Contoh: 02/12/2025 06:55).");
        return;
    }

    int newDay = input.substring(0, 2).toInt();
    int newMonth = input.substring(3, 5).toInt();
    int newYear = input.substring(6, 10).toInt();
    int newHour = input.substring(11, 13).toInt();
    int newMinute = input.substring(14, 16).toInt();

    if (newYear >= 2000 && newMonth >= 1 && newMonth <= 12 && newDay >= 1 && newDay <= 31 &&
        newHour >= 0 && newHour <= 23 && newMinute >= 0 && newMinute <= 59) {
        
        DateTime newTime(newYear, newMonth, newDay, newHour, newMinute, 0);
        rtc.adjust(newTime);
        
        Serial.printf("✅ RTC berhasil disetel ke: %02d/%02d/%04d %02d:%02d\n", 
                         newDay, newMonth, newYear, newHour, newMinute);
    } else {
        Serial.println("❌ Tanggal atau Waktu tidak valid. Periksa rentang nilai.");
    }
}

// --- FUNGSI KOMUNIKASI API LOKAL ---

void connectWiFi() {
    Serial.print("📡 Menghubungkan ke WiFi ");
    Serial.println(ssid);
    WiFi.begin(ssid, pass);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ WiFi Terhubung!");
        Serial.print("Alamat IP ESP32: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n❌ Gagal terhubung ke WiFi.");
    }
}

// 📌 FUNGSI UTAMA: CEK STATUS VALVE DARI LARAVEL
void checkRemoteStatus() {
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi(); 
        if (WiFi.status() != WL_CONNECTED) return;
    }

    HTTPClient http;
    String serverPath = "http://" + String(apiHost) + ":" + String(apiPort) + String(apiEndpoint);
    
    http.begin(serverPath.c_str());
    int httpResponseCode = http.GET();
    
    if (httpResponseCode > 0) {
        String payload = http.getString();

        // Response dari Laravel SensorController:
        // {"id":1,"valve_status":"ON","duration":30,"created_at":"...","updated_at":"..."}
        StaticJsonDocument<300> doc; 
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            http.end();
            return;
        }

        // ⚠️ PENTING: Parse field "valve_status" (bukan "status")
        const char* status = doc["valve_status"] | ""; 

        if (strcmp(status, "ON") == 0) {
            if (!isWatering) {
                manualMode = true; 
                openValve();
                Serial.println("👤 Kontrol Remote: VALVE DIBUKA dari Laravel.");
            }
        } else if (strcmp(status, "OFF") == 0) {
            if (isWatering && manualMode) {
                closeValve();
                Serial.println("👤 Kontrol Remote: VALVE DITUTUP dari Laravel.");
            }
        } 

    } else {
        // Silent fail untuk menghindari spam
    }
    
    http.end();
}

// 📌 FUNGSI BARU: SINKRONISASI JADWAL DARI API LARAVEL
// Di main.cpp, ubah syncSchedulesFromAPI() untuk handle format Laravel yang benar
void syncSchedulesFromAPI() {
    Serial.printf("🧠 Free Heap: %d bytes\n", ESP.getFreeHeap());
    
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi(); 
        if (WiFi.status() != WL_CONNECTED) return;
    }

    HTTPClient http;
    String serverPath = "http://" + String(apiHost) + ":" + String(apiPort) + String(apiScheduleEndpoint);
    
    Serial.println("\n🔄 Meminta jadwal baru dari Laravel API...");
    http.begin(serverPath.c_str());
    
    int httpResponseCode = http.GET();
    
    if (httpResponseCode > 0) {
        String payload = http.getString();
        Serial.println("📥 Response dari Laravel:");
        Serial.println(payload);
        
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.printf("❌ Gagal parsing JSON: %s\n", error.f_str());
            Serial.printf("   Ukuran payload: %d bytes\n", payload.length());
            http.end();
            return;
        }

        bool configChanged = false;
        
        if (doc.is<JsonArray>()) {
            JsonArray schedules = doc.as<JsonArray>();
            Serial.printf("📊 Ditemukan %d jadwal dari server\n", schedules.size());
            
            int scheduleIndex = 0;
            for (JsonObject schedule : schedules) {
                if (scheduleIndex >= 3) break;
                
                const char* scheduleType = schedule["schedule_type"] | "";
                const char* scheduleTime = schedule["schedule_time"] | "";
                
                // 🔥 FIX: Parse is_active sebagai integer dulu, baru convert ke bool
                int isActiveInt = schedule["is_active"] | 0;
                bool isActive = (isActiveInt == 1 || isActiveInt == true);
                
                int duration = schedule["duration_minutes"] | 30;
                
                Serial.printf("   RAW: type=%s, time=%s, is_active_raw=%d, duration=%d\n",
                             scheduleType, scheduleTime, isActiveInt, duration);
                
                Serial.printf("   Jadwal %d: %s %s (%s) - %d menit\n",
                             scheduleIndex+1, scheduleType, scheduleTime, 
                             isActive ? "AKTIF" : "NONAKTIF", duration);
                
                if (strlen(scheduleTime) >= 5) {
                    int hour = (scheduleTime[0] - '0') * 10 + (scheduleTime[1] - '0');
                    int minute = (scheduleTime[3] - '0') * 10 + (scheduleTime[4] - '0');
                    
                    if (hour != config.schedules[scheduleIndex].hour || 
                        minute != config.schedules[scheduleIndex].minute || 
                        isActive != config.schedules[scheduleIndex].enabled) {
                        
                        config.schedules[scheduleIndex].hour = hour;
                        config.schedules[scheduleIndex].minute = minute;
                        config.schedules[scheduleIndex].enabled = isActive;
                        
                        configChanged = true;
                        Serial.printf("   ➡️ UPDATE Jadwal #%d: %02d:%02d (%s)\n", 
                                        scheduleIndex+1, hour, minute, 
                                        isActive ? "AKTIF" : "NONAKTIF");
                    }
                    
                    int newDuration = duration * 60;
                    if (config.duration != newDuration) {
                        config.duration = newDuration;
                        configChanged = true;
                        Serial.printf("   ➡️ UPDATE Durasi: %d detik\n", newDuration);
                    }
                }
                
                scheduleIndex++;
            }
            
            // Nonaktifkan jadwal yang tidak ada di server
            for (int i = scheduleIndex; i < 3; i++) {
                if (config.schedules[i].enabled) {
                    config.schedules[i].enabled = false;
                    configChanged = true;
                    Serial.printf("   ➡️ Jadwal #%d dinonaktifkan (tidak ada di server)\n", i+1);
                }
            }
        }
        
        if (configChanged) {
            saveConfig();
            displayConfig(); 
            Serial.println("✅ Konfigurasi Jadwal disinkronkan dari Laravel.");
        } else {
            Serial.println("ℹ️  Tidak ada perubahan jadwal.");
        }

    } else {
        Serial.printf("❌ HTTP Error %d saat sync jadwal\n", httpResponseCode);
    }
    
    http.end();
}

// ==================== SETUP ====================
void setup() {
    Serial.begin(9600);
    Serial.println("\n╔══════════════════════════════════════════╗");
    Serial.println("║  Sistem Penyiraman - Mode Lokal API      ║");
    Serial.println("║          ESP32 + RTC + Laravel           ║");
    Serial.println("╚══════════════════════════════════════════╝\n");
    
    EEPROM.begin(EEPROM_SIZE);
    loadConfig();
    
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW); 
    digitalWrite(LED_PIN, LOW);
    
    Wire.begin(SDA_PIN, SCL_PIN);
    
    if (!rtc.begin()) {
        Serial.println("❌ ERROR: RTC DS3231 tidak ditemukan!");
        blinkError();
        while (1) delay(1000);
    }
    
    if (rtc.lostPower()) {
        Serial.println("⚠️  RTC kehilangan daya, waktu akan diatur dari NTP.");
    }
    
    DateTime now = rtc.now();
    Serial.printf("⏰ Waktu RTC Awal: %04d-%02d-%02d %02d:%02d:%02d\n\n", 
                    now.year(), now.month(), now.day(),
                    now.hour(), now.minute(), now.second());
    
    displayConfig();
    
    connectWiFi();
    
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    syncRTCFromNTP(); 

    // Setup Timers
    timer.setInterval(1000L, checkSchedule); // Cek jadwal setiap 1 detik
    timer.setInterval(REMOTE_CHECK_INTERVAL, checkRemoteStatus); // Cek status dari Laravel setiap 5 detik
    timer.setInterval(60000L, syncSchedulesFromAPI); // Sync Jadwal dari Laravel setiap 1 menit
    timer.setInterval(900000L, syncRTCFromNTP); // Sync RTC dari NTP setiap 15 menit
    
    Serial.println("✅ Sistem siap!");
    Serial.println("🔔 Untuk set waktu manual, ketik 'T' di Serial Monitor lalu Enter.");
    Serial.printf("🌐 Target API: http://%s:%d\n", apiHost, apiPort);
    Serial.println();
    
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(200);
        digitalWrite(LED_PIN, LOW);
        delay(200);
    }
}

// ==================== LOOP ====================
void loop() {
    timer.run();
    
    // 💡 PENANGANAN INPUT SERIAL UNTUK MENGATUR WAKTU
    if (Serial.available()) {
        char command = Serial.read();
        if (command == 'T' || command == 't') {
            setRTCFromSerial();
        } 
        
        while (Serial.available()) Serial.read();
    }
    
    // Auto-close valve setelah durasi jika BUKAN mode manual/remote
    // Jika sedang menyiram dan BUKAN karena perintah manual/remote, jalankan timer durasi
    if (isWatering && !manualMode) {
        if (millis() - valveOpenTime >= (unsigned long)config.duration * 1000UL) {
            closeValve();
            Serial.println("⏱️  Auto-close: Durasi penyiraman selesai (Jadwal RTC)");
        }
    }
    
    // LED heartbeat (berkedip pelan saat idle) - Non-aktifkan saat menyiram
    static unsigned long lastBlink = 0;
    if (!isWatering) {
        if (millis() - lastBlink >= 2000) { 
            digitalWrite(LED_PIN, !digitalRead(LED_PIN)); 
            lastBlink = millis();
        }
    }
}