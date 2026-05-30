// Communication.cpp

#include "Communication.h"
#include "esp_wpa2.h"
#include <ArduinoJson.h>
#include "Config.h"
#include <SD.h>
#include "esp_eap_client.h"
#include "esp_wifi.h"
#include "time.h"
#include <LiquidCrystal_I2C.h>
#include <vector>
#include <Preferences.h>

// ════════════════════════════════════════════════
//  BACKEND SERVER — upload WAV via HTTPS
// ════════════════════════════════════════════════
static const char* BACKEND_SERVER = "ta2526018.onrender.com";

// ════════════════════════════════════════════════
//  MQTT CLIENT — WiFiClientSecure untuk TLS port 8883
// ════════════════════════════════════════════════
WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);

unsigned long lastWifiAttempt = 0;
bool wifiDibatalkan = false;

// ════════════════════════════════════════════════
//  VARIABEL STATE
// ════════════════════════════════════════════════
bool   sdSyncAktif         = false;
bool   sdAckDiterima       = false;
bool   sdAckBatalkan       = false;
String sdTargetKelas       = "";
String sdFileSedangDikirim = "";

bool   audioSyncAktif   = false;
String audioTargetKelas = "";

// ── NTP ──
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 25200;
const int   daylightOffset_sec = 0;

extern LiquidCrystal_I2C lcd;
extern volatile bool buttonFlag;
extern int active_threshold;

// ════════════════════════════════════════════════
//  VALIDASI NAMA FILE (ketat, karakter per karakter)
// ════════════════════════════════════════════════
static bool allDigits(const String& s) {
    if (s.length() == 0) return false;
    for (char c : s) if (!isDigit(c)) return false;
    return true;
}
static bool isFormatDSNTxt(const String& n) {
    if (!n.startsWith("DSN_") || !n.endsWith(".txt")) return false;
    String mid = n.substring(4, n.length() - 4);
    int us = mid.indexOf('_');
    if (us <= 0 || mid.indexOf('_', us + 1) != -1) return false;
    return allDigits(mid.substring(0, us)) && allDigits(mid.substring(us + 1));
}

static bool isFormatMHSTxt(const String& n) {
    if (!n.startsWith("MHS_") || !n.endsWith(".txt")) return false;
    String mid = n.substring(4, n.length() - 4);
    int us1 = mid.indexOf('_');
    if (us1 <= 0) return false;
    int us2 = mid.indexOf('_', us1 + 1);
    if (us2 <= us1 + 1 || mid.indexOf('_', us2 + 1) != -1) return false;
    return allDigits(mid.substring(0, us1)) && allDigits(mid.substring(us1 + 1, us2)) && allDigits(mid.substring(us2 + 1));
}

static bool isFormatDSNWav(const String& n) {
    if (!n.startsWith("DSN_") || !n.endsWith(".wav")) return false;
    String mid = n.substring(4, n.length() - 4);
    int us = mid.indexOf('_');
    if (us <= 0 || mid.indexOf('_', us + 1) != -1) return false;
    return allDigits(mid.substring(0, us)) && allDigits(mid.substring(us + 1));
}

static bool isFormatMHSWav(const String& n) {
    if (!n.startsWith("MHS_") || !n.endsWith(".wav")) return false;
    String mid = n.substring(4, n.length() - 4);
    int us1 = mid.indexOf('_');
    if (us1 <= 0) return false;
    int us2 = mid.indexOf('_', us1 + 1);
    if (us2 <= us1 + 1 || mid.indexOf('_', us2 + 1) != -1) return false;
    return allDigits(mid.substring(0, us1)) && allDigits(mid.substring(us1 + 1, us2)) && allDigits(mid.substring(us2 + 1));
}

// ════════════════════════════════════════════════
//  startWiFi
// ════════════════════════════════════════════════
void startWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("  [MODE: KOM]   ");
    lcd.setCursor(0, 1); lcd.print("Wi-Fi Connect  ");

    Serial.printf("\n[WiFi] Connect ke: %s (tipe: %s)\n", saved_ssid, wifi_type);
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    delay(500);

    if (String(wifi_type) == "eduroam") {
        String identity = String(eap_nim);
        if (identity.indexOf("@") == -1) identity += "@itb.ac.id";
        esp_eap_client_set_identity((uint8_t*)identity.c_str(), identity.length());
        esp_eap_client_set_username((uint8_t*)eap_nim, strlen(eap_nim));
        esp_eap_client_set_password((uint8_t*)eap_pass, strlen(eap_pass));
        esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
        esp_wifi_sta_enterprise_enable();
        WiFi.begin(saved_ssid);
    } else {
        WiFi.begin(saved_ssid, wifi_pass);
    }

    int tc = 0;
    while (WiFi.status() != WL_CONNECTED && !buttonFlag) {
        delay(500); Serial.print(".");
        String dots = ""; for (int d = 0; d < (tc % 4); d++) dots += ".";
        lcd.setCursor(13, 1); lcd.print(dots + "    ");
        if (++tc >= 60) {
            lcd.setCursor(0, 1); lcd.print(" Wi-Fi Timeout! ");
            delay(2000); break;
        }
    }
    if (buttonFlag) {
        WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
        lcd.setCursor(0, 1); lcd.print(" DIBATALKAN!    ");
        delay(1500); 
        lastWifiAttempt = millis(); 
        wifiDibatalkan  = true;    
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Terhubung! IP: %s\n", WiFi.localIP().toString().c_str());
        lcd.setCursor(0, 1); lcd.print("Wi-Fi Connected!");
        delay(1000);

        // NTP sync
        lcd.setCursor(0, 1); lcd.print(" Sync NTP       "); 
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        struct tm timeinfo;
        bool ntpOk = false;
        delay(500);

        unsigned long startNtpSync = millis();
        unsigned long ntpTimeout = 9000; // Total maksimal tunggu 9 detik (3 percobaan x 3 detik)

        // Loop sampai berhasil atau timeout 9 detik
        while (millis() - startNtpSync < ntpTimeout) {
            
            // Animasi Titik Time-based (berubah tiap 500ms)
            int numDots = ((millis() - startNtpSync) / 500) % 4;
            String text = " Sync NTP";
            for (int d = 0; d < numDots; d++) text += ".";
            while (text.length() < 16) text += " "; // Penuhi sisa baris dengan spasi
            
            lcd.setCursor(0, 1); 
            lcd.print(text);

            // Coba ambil waktu sekali (non-blocking, timeout 0)
            if (getLocalTime(&timeinfo, 0)) { 
                ntpOk = true; 
                break; // Keluar dari loop jika berhasil
            }

            // Jika user menekan tombol (batal)
            if (buttonFlag) {
                break;
            }

            delay(100); // Jeda pendek agar tidak membebani CPU
        }

        lcd.setCursor(0, 1);
        if (ntpOk) {
            char ds[11];
            strftime(ds, sizeof(ds), "%d-%m-%Y", &timeinfo);
            Serial.printf("[NTP] Tanggal: %s\n", ds);
            File f = SD.open("/tanggal.txt", FILE_WRITE);
            if (f) { f.println(ds); f.close(); }
            lcd.print(" Waktu Updated! ");
        } 
        else if (buttonFlag) {
            WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
            lcd.print(" DIBATALKAN!    ");
            delay(1500);
            wifiDibatalkan  = true;   // ← tambah
            lastWifiAttempt = millis(); // ← tambah
            return;
        }
        else {
            Serial.println("[NTP] Gagal sync.");
            lcd.print(" NTP Timeout!   ");
        }
        delay(1500);
    }
    lastWifiAttempt = millis();
}

// ════════════════════════════════════════════════
//  stopWiFi
// ════════════════════════════════════════════════
void stopWiFi() {
    if (WiFi.getMode() == WIFI_OFF || WiFi.getMode() == WIFI_MODE_NULL) return;
    Serial.println("[WiFi] Mematikan WiFi...");
    if (mqttClient.connected()) {
        kirimStatus("offline");
        delay(200);
        mqttClient.disconnect();
    }
    esp_wifi_sta_wpa2_ent_disable();
    WiFi.disconnect(true);
    delay(10);
    WiFi.mode(WIFI_OFF);
}

// ════════════════════════════════════════════════
//  Baca file SD → JSON string
// ════════════════════════════════════════════════
static String bacaFileSdKeJson(const String& namaFile, const String& targetKelas) {
    File f = SD.open("/" + namaFile, FILE_READ);
    if (!f) return "";

    StaticJsonDocument<256> doc;
    doc["file"]         = namaFile;
    doc["target_kelas"] = targetKelas;

    if (isFormatDSNTxt(namaFile)) {
        String baris[2]; int idx = 0;
        while (f.available() && idx < 2) { baris[idx] = f.readStringUntil('\n'); baris[idx].trim(); idx++; }
        f.close();
        if (idx < 2) return "";
        doc["no_pertanyaan"] = baris[0].toInt();
        doc["tanggal"]       = baris[1];
        doc["uid"]           = "DOSEN";
    }
    else if (isFormatMHSTxt(namaFile)) {
        String baris[4]; int idx = 0;
        while (f.available() && idx < 4) { baris[idx] = f.readStringUntil('\n'); baris[idx].trim(); idx++; }
        f.close();
        if (idx < 4) return "";
        String ws = baris[3]; ws.replace(" ms", ""); ws.trim();
        doc["no_pertanyaan"] = baris[0].toInt();
        doc["no_jawaban"]    = baris[1].toInt();
        doc["uid"]           = baris[2];
        doc["waktu_diam_ms"] = ws.toInt();
    }
    else { f.close(); return ""; }

    char buf[256];
    serializeJson(doc, buf);
    return String(buf);
}


void handleWiFi() {
    if (WiFi.status() == WL_CONNECTED) { wifiDibatalkan = false; return; }
    if (wifiDibatalkan) return; // tunggu user ganti mode sendiri
    if (millis() - lastWifiAttempt < 5000) return;
    startWiFi();
}
// ════════════════════════════════════════════════
//  kirimStatusSync
// ════════════════════════════════════════════════
static void kirimStatusSync(const String& status, const String& pesan,
                            int total = 0, int berhasil = 0) {
    StaticJsonDocument<256> doc;
    doc["device_id"] = DEVICE_ID;
    doc["status"]    = status;
    doc["pesan"]     = pesan;
    if (total > 0)    doc["total"]    = total;
    if (berhasil > 0) doc["berhasil"] = berhasil;
    char buf[256]; serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_SYNC_STATUS, buf);
    Serial.println("📤 StatusSync → " + String(buf));
}

// ════════════════════════════════════════════════
//  kirimAudioHTTP — upload 1 file WAV via HTTPS
//  Pakai WiFiClientSecure terpisah dari MQTT client
// ════════════════════════════════════════════════
static int kirimAudioHTTP(const String& namaFile, const String& targetKelas) {
    File f = SD.open("/" + namaFile, FILE_READ);
    if (!f) { Serial.println("❌ Gagal buka: " + namaFile); return -1; }
    size_t ukuranFile = f.size();
    Serial.printf("📂 Upload WAV: %s (%u bytes)\n", namaFile.c_str(), ukuranFile);

    String boundary   = "ESP32Boundary7a3f";
    String partHeader =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"" + namaFile + "\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    String partMid =
        "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"nama_file\"\r\n\r\n" + namaFile +
        "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"target_kelas\"\r\n\r\n" + targetKelas +
        "\r\n--" + boundary + "--\r\n";
    size_t contentLength = partHeader.length() + ukuranFile + partMid.length();

    // Gunakan client HTTPS terpisah (bukan espClient yang dipakai MQTT)
    WiFiClientSecure httpClient;
    httpClient.setInsecure();

    Serial.printf("🔌 Konek ke %s:443...\n", BACKEND_SERVER);
    if (!httpClient.connect(BACKEND_SERVER, 443)) {
        Serial.println("❌ Gagal konek ke backend HTTPS!");
        f.close(); return -1;
    }

    httpClient.printf(
        "POST /api/upload-audio-sd HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        BACKEND_SERVER, boundary.c_str(), (unsigned int)contentLength
    );
    httpClient.print(partHeader);

    const size_t CHUNK = 512;
    uint8_t chunkBuf[CHUNK];
    size_t terkirim = 0;
    unsigned long lastPrint = millis();
    while (f.available()) {
        size_t baca = f.read(chunkBuf, CHUNK);
        terkirim += httpClient.write(chunkBuf, baca);
        if (millis() - lastPrint > 2000) {
            Serial.printf("   ⏳ %u/%u bytes (%.0f%%)\n",
                terkirim, (unsigned int)ukuranFile,
                (float)terkirim / ukuranFile * 100.0f);
            lastPrint = millis();
        }
    }
    f.close();
    httpClient.print(partMid);
    Serial.printf("✅ Stream selesai: %u bytes\n", terkirim);

    // Baca response code
    int httpCode = -1;
    unsigned long timeout = millis();
    while (millis() - timeout < 15000) {
        if (httpClient.available()) {
            String line = httpClient.readStringUntil('\n');
            line.trim();
            if (line.startsWith("HTTP/")) {
                httpCode = line.substring(9, 12).toInt();
                Serial.printf("📡 HTTP %d ← %s\n", httpCode, namaFile.c_str());
                break;
            }
        }
        delay(10);
    }
    if (httpCode == -1) Serial.println("⏱️ Timeout baca response!");

    String body = ""; unsigned long bt = millis();
    while (httpClient.available() && millis() - bt < 3000) body += (char)httpClient.read();
    int js = body.indexOf('{');
    if (js >= 0) Serial.println("   Response: " + body.substring(js));

    httpClient.stop();
    return httpCode;
}

// ════════════════════════════════════════════════
//  MQTT CALLBACK
// ════════════════════════════════════════════════
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    Serial.println("\n📩 [" + String(topic) + "] " + msg);

    if (String(topic) != MQTT_TOPIC_PERINTAH) return;

    // UBAH KAPASITAS JSON DI SINI (2048 bytes cukup untuk ~80+ UID)
    DynamicJsonDocument doc(2048); 
    DeserializationError error = deserializeJson(doc, msg);
    
    if (error) { 
        Serial.print("⚠️ JSON tidak valid: "); 
        Serial.println(error.c_str()); 
        return; 
    }
    
    String perintah = doc["perintah"] | "";

    if (perintah == "request_sync_audio") {
        // Sync file TXT
        String kelas = doc["target_kelas"] | "";
        Serial.println("📥 request_sync_audio → " + kelas);
        if (!sdSyncAktif) { sdSyncAktif = true; sdTargetKelas = kelas; }
        else Serial.println("⚠️ TXT sync sudah berjalan.");
    }
    else if (perintah == "request_sync_wav") {
        // Sync file WAV
        String kelas = doc["target_kelas"] | "";
        Serial.println("📥 request_sync_wav → " + kelas);
        if (!audioSyncAktif) { audioSyncAktif = true; audioTargetKelas = kelas; }
        else Serial.println("⚠️ Audio sync sudah berjalan.");
    }
    else if (perintah == "ack_file") {
        String namaFile = doc["file"] | "";
        Serial.println("✅ ACK: " + namaFile);
        if (namaFile == sdFileSedangDikirim || namaFile == "") sdAckDiterima = true;
    }
    else if (perintah == "batalkan_sync") {
        Serial.println("🛑 Batalkan sync.");
        sdAckBatalkan = true;
    }
    else if (perintah == "set_timer") {
        int durasi_detik = doc["durasi_detik"] | 0; // Ambil nilai, default 0 jika gagal
        
        if (durasi_detik > 0) {
            maxRecordMs = durasi_detik * 1000UL; // Konversi ke milidetik
            
            // Simpan ke NVS (Flash Memory) agar permanen
            Preferences prefs;
            prefs.begin("catch_note", false);
            prefs.putULong("max_record_ms", maxRecordMs);
            prefs.end();
            
            // --- PRINT VARIABEL TERUPDATE DI SINI ---
            Serial.println("====================================");
            Serial.printf("⏱️ [UPDATE DARI WEB] Batas waktu rekaman berhasil diubah!\n");
            Serial.printf("⏱️ Variabel maxRecordMs sekarang: %lu ms\n", maxRecordMs);
            Serial.println("====================================");
            
        } else {
            Serial.println("⚠️ Parameter 'durasi_detik' kosong atau tidak valid!");
        }
    }
   // --- FITUR: SYNC DAFTAR MAHASISWA BANNED (Dari command web "sync_uid_kelas") ---
    else if (perintah == "sync_uid_aktif") { 
        Serial.println("📥 Menerima sinkronisasi daftar mahasiswa BANNED...");
        
        // 1. Hapus file lama agar benar-benar ter-overwrite
        if (SD.exists("/banned.txt")) {
            SD.remove("/banned.txt");
        }

        // 2. Buat file baru
        File file = SD.open("/banned.txt", FILE_WRITE);
        if (!file) {
            Serial.println("❌ Gagal membuka /banned.txt untuk menulis!");
            return;
        }

        // 3. Ambil array 'uids' dari JSON
        JsonArray uids = doc["uids"].as<JsonArray>();
        int count = 0;
        
        // 4. Looping isi array dan tulis ke SD Card baris demi baris
        for (JsonVariant v : uids) {
            String uid = v.as<String>();
            file.println(uid); 
            count++;
        }
        
        file.close();
        
        Serial.println("====================================");
        Serial.printf("[UPDATE BANNED] %d UID Mahasiswa berhasil disimpan ke banned.txt!\n", count);
        Serial.println("====================================");
    }

    // --- FITUR BARU: SYNC DAFTAR PESERTA AKTIF (Sesuai gambar baris ke-2) ---
    else if (perintah == "sync_uid_kelas") {
        Serial.println("📥 Menerima sinkronisasi peserta aktif sesi...");
        
        if (SD.exists("/eligible.txt")) {
            SD.remove("/eligible.txt");
        }

        File file = SD.open("/eligible.txt", FILE_WRITE);
        if (!file) {
            Serial.println("Gagal membuka /eligible.txt!");
            return;
        }

        JsonArray uids = doc["uids"].as<JsonArray>();
        int count = 0;
        for (JsonVariant v : uids) {
            file.println(v.as<String>());
            count++;
        }
        file.close();
        
        Serial.println("====================================");
        Serial.printf("[UPDATE eligible] %d UID berhasil disimpan ke eligible.txt!\n", count);
        Serial.println("====================================");
    }
}

// ════════════════════════════════════════════════
//  handleMQTT — jaga koneksi ke HiveMQ Cloud TLS
// ════════════════════════════════════════════════
void handleMQTT() {
    static bool callbackSet = false;
    if (!callbackSet) {
        // Skip verifikasi sertifikat HiveMQ 
        espClient.setInsecure();
        mqttClient.setCallback(mqttCallback);
        mqttClient.setBufferSize(512);
        callbackSet = true;
    }

    if (WiFi.status() != WL_CONNECTED) return;

    if (!mqttClient.connected()) {
        static unsigned long lastAttempt = 0;
        if (millis() - lastAttempt < 5000) return;
        lastAttempt = millis();

        String clientId = "ESP-" + String(random(0xffff), HEX);
        String willMsg  = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"status\":\"offline\"}";

        Serial.printf("[MQTT] Konek ke %s:%d sebagai %s...\n",
                      MQTT_SERVER, MQTT_PORT, MQTT_USER);

        // Connect dengan username & password HiveMQ
        if (mqttClient.connect(clientId.c_str(),
                               MQTT_USER, MQTT_PASS,
                               MQTT_TOPIC_STATUS, 0, true, willMsg.c_str())) {
            Serial.println("[MQTT] ✅ Terhubung ke HiveMQ Cloud!");
            mqttClient.subscribe(MQTT_TOPIC_PERINTAH);
            mqttClient.subscribe(MQTT_TOPIC_SYNC_STATUS);
            kirimStatus("online");
        } else {
            Serial.printf("[MQTT] ❌ Gagal rc=%d (retry 5 detik)\n", mqttClient.state());
        }
    } else {
        mqttClient.loop();
    }
}


void kirimStatus(String status_device) {
    if (!mqttClient.connected()) return;

    StaticJsonDocument<200> doc;
    doc["device_id"] = DEVICE_ID;
    doc["status"]    = status_device;
    doc["battery"]   = round(batteryPercent);
    doc["threshold"] = active_threshold; // <-- TAMBAHKAN INI

    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_STATUS, buf, true);
    Serial.println("📤 Status: " + String(buf));
}


void sendRegistration(String uid) {
    if (!mqttClient.connected()) return;

    StaticJsonDocument<200> doc;
    doc["device_id"] = DEVICE_ID;
    doc["action"]    = "tap_rfid";
    doc["uid"]       = uid;
    doc["battery"]   = round(batteryPercent);
    doc["threshold"] = active_threshold; 

    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_REG, buf);
    Serial.println("[MQTT] Registrasi: " + String(buf));
}

// ════════════════════════════════════════════════
//  prosesSinkronisasiAudio — semua WAV via HTTPS
// ════════════════════════════════════════════════
void prosesSinkronisasiAudio(const String& targetKelas) {
    Serial.println("\n🎵 Sinkronisasi Audio WAV → kelas: " + targetKelas);

    File root = SD.open("/");
    std::vector<String> daftarAudio;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        String nama = String(entry.name()); bool isDir = entry.isDirectory(); entry.close();
        if (isDir) continue;
        if (isFormatDSNWav(nama) || isFormatMHSWav(nama)) {
            daftarAudio.push_back(nama);
            Serial.println("🎵 Ditemukan: " + nama);
        } else { Serial.println("⏭️  Skip: " + nama); }
    }
    root.close();

    int total = daftarAudio.size(), berhasil = 0;
    if (total == 0) {
        kirimStatusSync("selesai", "Tidak ada file audio untuk dikirim.", 0, 0);
        audioSyncAktif = false; return;
    }
    Serial.printf("📦 Total WAV: %d file\n", total);

    for (int i = 0; i < total; i++) {
        if (sdAckBatalkan) {
            kirimStatusSync("dibatalkan", "Sinkronisasi audio dihentikan.", total, berhasil);
            audioSyncAktif = false; sdAckBatalkan = false; return;
        }
        String nama = daftarAudio[i];
        Serial.printf("\n🎵 [%d/%d] Upload: %s\n", i + 1, total, nama.c_str());
        int httpCode = kirimAudioHTTP(nama, targetKelas);
        if (httpCode == 200 || httpCode == 201) {
            SD.remove("/" + nama); berhasil++;
            Serial.printf("🗑️  Dihapus. Progress: %d/%d\n", berhasil, total);
        } else {
            kirimStatusSync("error",
                "Gagal upload: " + nama + ". Ulangi atau batalkan?", total, berhasil);
            sdAckDiterima = false; sdAckBatalkan = false;
            unsigned long tw = millis();
            while (!sdAckDiterima && !sdAckBatalkan && millis() - tw < 30000)
                { mqttClient.loop(); delay(100); }
            if (sdAckBatalkan || !sdAckDiterima) {
                kirimStatusSync("dibatalkan", "Sinkronisasi audio dihentikan.", total, berhasil);
                audioSyncAktif = false; return;
            }
        }
    }
    Serial.printf("\n🎉 Audio sync selesai! %d/%d\n", berhasil, total);
    kirimStatusSync("selesai", "Semua audio berhasil dikirim.", total, berhasil);
    audioSyncAktif = false;
}

// ════════════════════════════════════════════════
//  prosesSinkronisasiSD — semua TXT via MQTT
// ════════════════════════════════════════════════
void prosesSinkronisasiSD(const String& targetKelas) {
    Serial.println("\n🔄 Sinkronisasi TXT → kelas: " + targetKelas);

    File root = SD.open("/");
    std::vector<String> daftarFile;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        String nama = String(entry.name()); bool isDir = entry.isDirectory(); entry.close();
        if (isDir) continue;
        if (isFormatDSNTxt(nama) || isFormatMHSTxt(nama)) {
            daftarFile.push_back(nama);
            Serial.println("📄 Ditemukan: " + nama);
        } else { Serial.println("⏭️  Skip: " + nama); }
    }
    root.close();

    int total = daftarFile.size(), berhasil = 0;
    if (total == 0) {
        kirimStatusSync("selesai", "Tidak ada file untuk dikirim. SD Card kosong.", 0, 0);
        sdSyncAktif = false; return;
    }
    Serial.printf("📦 Total TXT: %d file\n", total);

    for (int i = 0; i < total; i++) {
        String namaFile = daftarFile[i];
        Serial.printf("\n📤 [%d/%d] %s\n", i + 1, total, namaFile.c_str());

        String jsonData = bacaFileSdKeJson(namaFile, targetKelas);
        if (jsonData == "") {
            sdFileSedangDikirim = namaFile;
            kirimStatusSync("error", "Gagal baca: " + namaFile + ". Lanjut atau batalkan?", total, berhasil);
            sdAckDiterima = false; sdAckBatalkan = false;
            unsigned long tw = millis();
            while (!sdAckDiterima && !sdAckBatalkan && millis() - tw < 30000)
                { mqttClient.loop(); delay(100); }
            if (sdAckBatalkan || !sdAckDiterima)
                { kirimStatusSync("dibatalkan", "Dihentikan.", total, berhasil); sdSyncAktif = false; return; }
            continue;
        }

        sdFileSedangDikirim = namaFile;
        sdAckDiterima = false; sdAckBatalkan = false;
        mqttClient.setBufferSize(512);
        bool terkirim = mqttClient.publish(MQTT_TOPIC_AUDIO_DATA, jsonData.c_str(), true);

        if (!terkirim) {
            kirimStatusSync("error", "Gagal kirim: " + namaFile, total, berhasil);
            unsigned long tw = millis();
            while (!sdAckDiterima && !sdAckBatalkan && millis() - tw < 30000)
                { mqttClient.loop(); delay(100); }
            if (sdAckBatalkan || !sdAckDiterima)
                { kirimStatusSync("dibatalkan", "Dihentikan.", total, berhasil); sdSyncAktif = false; return; }
            continue;
        }

        Serial.println("⏳ Menunggu ACK server (maks 15 detik)...");
        unsigned long tw = millis();
        while (!sdAckDiterima && !sdAckBatalkan && millis() - tw < 15000)
            { mqttClient.loop(); delay(100); }

        if (sdAckBatalkan)
            { kirimStatusSync("dibatalkan", "Dibatalkan server.", total, berhasil); sdSyncAktif = false; return; }

        if (!sdAckDiterima) {
            kirimStatusSync("error", "Timeout: " + namaFile + ". Ulangi atau batalkan?", total, berhasil);
            unsigned long tw2 = millis();
            while (!sdAckDiterima && !sdAckBatalkan && millis() - tw2 < 30000)
                { mqttClient.loop(); delay(100); }
            if (sdAckBatalkan || !sdAckDiterima)
                { kirimStatusSync("dibatalkan", "Dihentikan (timeout).", total, berhasil); sdSyncAktif = false; return; }
        }

        SD.remove("/" + namaFile); berhasil++;
        Serial.printf("✅ Dihapus. Progress: %d/%d\n", berhasil, total);
    }

    Serial.printf("\n🎉 TXT sync selesai! %d/%d\n", berhasil, total);
    kirimStatusSync("selesai", "Semua file berhasil dikirim.", total, berhasil);
    sdSyncAktif = false;
}
