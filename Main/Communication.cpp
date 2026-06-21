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
#include "LCD_Helper.h"
#include "AudioSD_Module.h"

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

static TaskHandle_t mqttConnectTaskHandle = nullptr;
static volatile bool mqttConnecting = false;
static volatile bool mqttStopRequested = false;
static unsigned long lastMqttAttempt = 0;

static void mqttConnectTask(void* param) {
    String clientId = "ESP-" + String(DEVICE_ID) + "-" + String(random(0xffff), HEX);
    String willMsg  = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"status\":\"offline\"}";

    if (mqttStopRequested || WiFi.status() != WL_CONNECTED) {
        mqttConnecting = false;
        mqttConnectTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    bool ok = mqttClient.connect(
        clientId.c_str(),
        MQTT_USER,
        MQTT_PASS,
        MQTT_TOPIC_STATUS,
        0,
        true,
        willMsg.c_str()
    );

    if (ok && !mqttStopRequested && WiFi.status() == WL_CONNECTED) {

        mqttClient.subscribe(MQTT_TOPIC_PERINTAH);
        mqttClient.subscribe(MQTT_TOPIC_SYNC_STATUS);

        kirimStatus("online");
    } else if (ok) {
        mqttClient.disconnect();
    }

    mqttConnecting = false;
    mqttConnectTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool isMQTTConnecting() {
    return mqttConnecting;
}
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
extern int active_threshold;

static bool wifiConnecting = false;
static bool timeSyncing = false;
static unsigned long wifiConnectStarted = 0;
static unsigned long timeSyncStarted = 0;
static unsigned long lastWifiProgressLog = 0;

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
const unsigned long TIME_SYNC_TIMEOUT_MS = 9000;

bool isWiFiConnecting() {
    return wifiConnecting;
}

bool isTimeSyncing() {
    return timeSyncing;
}

static void stopTimeSyncState() {
    timeSyncing = false;
    timeSyncStarted = 0;
}

static void beginTimeSync() {
    if (timeSyncing) return;

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    timeSyncing = true;
    timeSyncStarted = millis();

    lcdPrint16(0, "PENGATURAN WAKTU");
    lcdPrint16(1, " MOHON TUNGGU" + animDots());
}

static bool finishTimeSyncIfReady() {
    if (!timeSyncing) return true;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        char ds[11];
        strftime(ds, sizeof(ds), "%d-%m-%Y", &timeinfo);

        if (ensureSdReady("ntp_date", true)) {
            File f = SD.open("/tanggal.txt", FILE_WRITE);
            if (f) {
                f.println(ds);
                f.close();
            }
        }

        lcdPrint16(0, " WAKTU UPDATED ");
        lcdPrint16(1, " MOHON TUNGGU" + animDots());

        stopTimeSyncState();
        return true;
    }

    if (millis() - timeSyncStarted >= TIME_SYNC_TIMEOUT_MS) {

        lcdPrint16(0, "  WAKTU GAGAL   ");
        lcdPrint16(1, " MOHON TUNGGU" + animDots());

        stopTimeSyncState();
        return true;
    }

    return false;
}

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
    if (String(saved_ssid).length() < 2) {

        wifiConnecting = false;
        stopTimeSyncState();
        stopMQTT();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);

        lcd.clear();
        lcdPrint16(0, " MODE: OFFLINE ");
        lcdPrint16(1, "WIFI BELUM DISET");

        lastWifiAttempt = millis();
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnecting = false;
        if (!timeSyncing) {
            beginTimeSync();
        }
        return;
    }

    if (wifiConnecting) return;

    lcd.clear();
    lcdPrint16(0, "  MODE: ONLINE  ");
    lcdPrint16(1, " KONEK WIFI");

    stopTimeSyncState();
    mqttStopRequested = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);

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

    wifiConnecting = true;
    wifiDibatalkan = false;
    wifiConnectStarted = millis();
    lastWifiProgressLog = 0;
    lastWifiAttempt = millis();
}

// ════════════════════════════════════════════════
//  stopWiFi
// ════════════════════════════════════════════════
void stopWiFi() {
    wifiConnecting = false;
    stopTimeSyncState();
    stopMQTT();

    if (WiFi.getMode() == WIFI_OFF || WiFi.getMode() == WIFI_MODE_NULL) return;
    esp_wifi_sta_wpa2_ent_disable();
    WiFi.disconnect(true);
    delay(10);
    WiFi.mode(WIFI_OFF);
}

// ════════════════════════════════════════════════
//  Baca file SD → JSON string
// ════════════════════════════════════════════════
static String bacaFileSdKeJson(const String& namaFile, const String& targetKelas) {
    if (!ensureSdReady("sync_txt_read", true)) return "";

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
    if (String(saved_ssid).length() < 2) {
        wifiConnecting = false;
        stopTimeSyncState();
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiDibatalkan = false;

        if (wifiConnecting) {
            wifiConnecting = false;
            lcdPrint16(0, "WIFI TERHUBUNG ");
            lcdPrint16(1, " MOHON TUNGGU" + animDots());
            beginTimeSync();
        }

        if (timeSyncing) {
            finishTimeSyncIfReady();
        }

        return;
    }

    stopTimeSyncState();

    if (wifiConnecting) {
        if (millis() - lastWifiProgressLog >= 500) {
            lastWifiProgressLog = millis();
            lcdPrint16(0, "  MODE: ONLINE  ");
            lcdPrint16(1, " KONEK WIFI" + animDots());
        }

        if (millis() - wifiConnectStarted >= WIFI_CONNECT_TIMEOUT_MS) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);

            wifiConnecting = false;
            lastWifiAttempt = millis();

            lcdPrint16(0, "  WIFI GAGAL   ");
            lcdPrint16(1, "COBA LAGI 05s  ");
        }

        return;
    }

    if (millis() - lastWifiAttempt < WIFI_RETRY_INTERVAL_MS) return;

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
}

// ════════════════════════════════════════════════
//  kirimAudioHTTP — upload 1 file WAV via HTTPS
//  Pakai WiFiClientSecure terpisah dari MQTT client
// ════════════════════════════════════════════════
static int kirimAudioHTTP(const String& namaFile, const String& targetKelas) {
    if (!ensureSdReady("sync_audio_read", true)) return -1;

    File f = SD.open("/" + namaFile, FILE_READ);
    if (!f) {  return -1; }
    size_t ukuranFile = f.size();

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

    if (!httpClient.connect(BACKEND_SERVER, 443)) {
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

    const size_t CHUNK = 4096;
    uint8_t chunkBuf[CHUNK];
    size_t terkirim = 0;
    unsigned long lastPrint = millis();
    while (f.available()) {
        size_t baca = f.read(chunkBuf, CHUNK);
        terkirim += httpClient.write(chunkBuf, baca);
        if (millis() - lastPrint > 2000) {
            lastPrint = millis();
        }
    }
    f.close();
    httpClient.print(partMid);

    // Baca response code
    int httpCode = -1;
    unsigned long timeout = millis();
    while (millis() - timeout < 15000) {
        if (httpClient.available()) {
            String line = httpClient.readStringUntil('\n');
            line.trim();
            if (line.startsWith("HTTP/")) {
                httpCode = line.substring(9, 12).toInt();
                break;
            }
        }
        delay(10);
    }

    httpClient.stop();
    return httpCode;
}

static bool tulisUidCacheAtomik(const char* finalPath,
                                const char* tempPath,
                                const char* backupPath,
                                JsonArray uids,
                                bool allowEmpty,
                                int &count) {
    count = 0;

    if (!ensureSdReady("uid_cache_write", true)) {
        return false;
    }

    if (uids.isNull()) {
        return false;
    }

    if (SD.exists(tempPath)) SD.remove(tempPath);

    File file = SD.open(tempPath, FILE_WRITE);
    if (!file) {
        return false;
    }

    for (JsonVariant v : uids) {
        String uid = v.as<String>();
        uid.trim();
        if (uid.length() == 0) continue;

        file.println(uid);
        count++;
    }

    file.close();

    if (!ensureSdReady("uid_cache_temp_done", true)) {
        return false;
    }

    if (count == 0 && !allowEmpty) {
        SD.remove(tempPath);
        return false;
    }

    if (SD.exists(backupPath)) SD.remove(backupPath);

    bool hadFinal = SD.exists(finalPath);
    if (hadFinal && !SD.rename(finalPath, backupPath)) {
        SD.remove(tempPath);
        return false;
    }

    if (!SD.rename(tempPath, finalPath)) {
        if (hadFinal) {
            SD.rename(backupPath, finalPath);
        }
        SD.remove(tempPath);
        return false;
    }

    if (hadFinal) SD.remove(backupPath);
    return true;
}

// ════════════════════════════════════════════════
//  MQTT CALLBACK
// ════════════════════════════════════════════════
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

    if (String(topic) != MQTT_TOPIC_PERINTAH) return;

    // UBAH KAPASITAS JSON DI SINI (2048 bytes cukup untuk ~80+ UID)
    DynamicJsonDocument doc(2048); 
    DeserializationError error = deserializeJson(doc, msg);
    
    if (error) { 
        return; 
    }
    
    String perintah = doc["perintah"] | "";

    if (perintah == "request_sync_audio") {
        // Sync file TXT
        String kelas = doc["target_kelas"] | "";
        if (!sdSyncAktif) { sdSyncAktif = true; sdTargetKelas = kelas; }
    }
    else if (perintah == "request_sync_wav") {
        // Sync file WAV
        String kelas = doc["target_kelas"] | "";
        if (!audioSyncAktif) { audioSyncAktif = true; audioTargetKelas = kelas; }
    }
    else if (perintah == "ack_file") {
        String namaFile = doc["file"] | "";
        if (namaFile == sdFileSedangDikirim || namaFile == "") sdAckDiterima = true;
    }
    else if (perintah == "batalkan_sync") {
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
        }
    }
   // --- FITUR: SYNC DAFTAR MAHASISWA BANNED (Dari command web "sync_uid_kelas") ---
    else if (perintah == "sync_uid_aktif") { 
        {
            JsonArray uids = doc["uids"].as<JsonArray>();
            int count = 0;
            if (!tulisUidCacheAtomik("/banned.txt", "/banned.tmp", "/banned.bak", uids, true, count)) {
                return;
            }

            return;
        }
#if 0
        
        // 1. Hapus file lama agar benar-benar ter-overwrite
        if (SD.exists("/banned.txt")) {
            SD.remove("/banned.txt");
        }

        // 2. Buat file baru
        File file = SD.open("/banned.txt", FILE_WRITE);
        if (!file) {
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
        
    }

#endif
    }
    // --- FITUR BARU: SYNC DAFTAR PESERTA AKTIF (Sesuai gambar baris ke-2) ---
    else if (perintah == "sync_uid_kelas") {
        {
            JsonArray uids = doc["uids"].as<JsonArray>();
            int count = 0;
            if (!tulisUidCacheAtomik("/eligible.txt", "/eligible.tmp", "/eligible.bak", uids, false, count)) {
                return;
            }

            return;
        }
#if 0
        
        if (SD.exists("/eligible.txt")) {
            SD.remove("/eligible.txt");
        }

        File file = SD.open("/eligible.txt", FILE_WRITE);
        if (!file) {
            return;
        }

        JsonArray uids = doc["uids"].as<JsonArray>();
        int count = 0;
        for (JsonVariant v : uids) {
            file.println(v.as<String>());
            count++;
        }
        file.close();
        
    }
}

// ════════════════════════════════════════════════
//  handleMQTT — jaga koneksi ke HiveMQ Cloud TLS
// ════════════════════════════════════════════════
#endif
    }
}

void handleMQTT() {
    static bool callbackSet = false;

    if (!callbackSet) {
        espClient.setInsecure();

        mqttClient.setCallback(mqttCallback);
        mqttClient.setBufferSize(4096);

        // Tetap dipakai, tapi bukan solusi utama.
        mqttClient.setSocketTimeout(1);
        mqttClient.setKeepAlive(15);

        callbackSet = true;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    // Kalau task connect sedang jalan, jangan akses mqttClient dari loop utama
    if (mqttConnecting) {
        return;
    }

    if (mqttClient.connected()) {
        mqttClient.loop();
        return;
    }

    const unsigned long MQTT_RETRY_INTERVAL = 5000;

    if (millis() - lastMqttAttempt < MQTT_RETRY_INTERVAL) {
        return;
    }

    lastMqttAttempt = millis();

    mqttStopRequested = false;
    mqttConnecting = true;

    BaseType_t result = xTaskCreatePinnedToCore(
        mqttConnectTask,
        "MQTTConnect",
        8192,
        nullptr,
        1,
        &mqttConnectTaskHandle,
        0
    );

    if (result != pdPASS) {
        mqttConnecting = false;
        mqttConnectTaskHandle = nullptr;
    }
}

void stopMQTT() {
  mqttStopRequested = true;

  if (mqttConnecting) {
    unsigned long startWait = millis();
    while (mqttConnecting && millis() - startWait < 2500) {
      espClient.stop();
      delay(20);
    }

    if (mqttConnecting) {
      espClient.stop();
      return;
    }
  }

  if (mqttClient.connected()) {
    kirimStatus("offline");
    delay(100);
    mqttClient.disconnect();
  }

  espClient.stop();
  mqttConnectTaskHandle = nullptr;

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
}

// ════════════════════════════════════════════════
//  prosesSinkronisasiAudio — semua WAV via HTTPS
// ════════════════════════════════════════════════
void prosesSinkronisasiAudio(const String& targetKelas) {

    if (!ensureSdReady("sync_audio_start", true)) {
        kirimStatusSync("error", "SD Card tidak siap.", 0, 0);
        audioSyncAktif = false;
        return;
    }

    File root = SD.open("/");
    if (!root) {
        markSdLost("sync_audio_root");
        kirimStatusSync("error", "SD Card gagal dibaca.", 0, 0);
        audioSyncAktif = false;
        return;
    }
    std::vector<String> daftarAudio;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        String nama = String(entry.name()); bool isDir = entry.isDirectory(); entry.close();
        if (isDir) continue;
        if (isFormatDSNWav(nama) || isFormatMHSWav(nama)) {
            daftarAudio.push_back(nama);
        }
    }
    root.close();

    int total = daftarAudio.size(), berhasil = 0;
    if (total == 0) {
        kirimStatusSync("selesai", "Tidak ada file audio untuk dikirim.", 0, 0);
        audioSyncAktif = false; return;
    }

    for (int i = 0; i < total; i++) {
        if (sdAckBatalkan) {
            kirimStatusSync("dibatalkan", "Sinkronisasi audio dihentikan.", total, berhasil);
            audioSyncAktif = false; sdAckBatalkan = false; return;
        }
        String nama = daftarAudio[i];
        int httpCode = kirimAudioHTTP(nama, targetKelas);
        if (httpCode == 200 || httpCode == 201) {
            if (!ensureSdReady("sync_audio_remove", true) || !SD.remove("/" + nama)) {
                kirimStatusSync("error", "SD hilang/gagal hapus: " + nama, total, berhasil);
                audioSyncAktif = false;
                return;
            }
            berhasil++;
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
    kirimStatusSync("selesai", "Semua audio berhasil dikirim.", total, berhasil);
    audioSyncAktif = false;
}

// ════════════════════════════════════════════════
//  prosesSinkronisasiSD — semua TXT via MQTT
// ════════════════════════════════════════════════
void prosesSinkronisasiSD(const String& targetKelas) {

    if (!ensureSdReady("sync_txt_start", true)) {
        kirimStatusSync("error", "SD Card tidak siap.", 0, 0);
        sdSyncAktif = false;
        return;
    }

    File root = SD.open("/");
    if (!root) {
        markSdLost("sync_txt_root");
        kirimStatusSync("error", "SD Card gagal dibaca.", 0, 0);
        sdSyncAktif = false;
        return;
    }
    std::vector<String> daftarFile;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        String nama = String(entry.name()); bool isDir = entry.isDirectory(); entry.close();
        if (isDir) continue;
        if (isFormatDSNTxt(nama) || isFormatMHSTxt(nama)) {
            daftarFile.push_back(nama);
        }
    }
    root.close();

    int total = daftarFile.size(), berhasil = 0;
    if (total == 0) {
        kirimStatusSync("selesai", "Tidak ada file untuk dikirim. SD Card kosong.", 0, 0);
        sdSyncAktif = false; return;
    }

    for (int i = 0; i < total; i++) {
        String namaFile = daftarFile[i];

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
        mqttClient.setBufferSize(4096);
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

        if (!ensureSdReady("sync_txt_remove", true) || !SD.remove("/" + namaFile)) {
            kirimStatusSync("error", "SD hilang/gagal hapus: " + namaFile, total, berhasil);
            sdSyncAktif = false;
            return;
        }
        berhasil++;
    }

    kirimStatusSync("selesai", "Semua file berhasil dikirim.", total, berhasil);
    sdSyncAktif = false;
}
