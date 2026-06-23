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
        Serial.println("[MQTT] Connect dibatalkan: stop requested / WiFi belum connected.");
        mqttConnecting = false;
        mqttConnectTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    Serial.print("Menghubungkan ke HiveMQ Cloud sebagai ");
    Serial.print(clientId);
    Serial.print(" ...");

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
        Serial.println(" ✅ Terhubung!");

        mqttClient.subscribe(MQTT_TOPIC_PERINTAH);
        mqttClient.subscribe(MQTT_TOPIC_SYNC_STATUS);

        Serial.print("[MQTT] Subscribe: ");
        Serial.println(MQTT_TOPIC_PERINTAH);
        Serial.print("[MQTT] Subscribe: ");
        Serial.println(MQTT_TOPIC_SYNC_STATUS);

        kirimStatus("online");
    } else if (ok) {
        Serial.println(" tersambung, tapi langsung disconnect karena stop requested / WiFi putus.");
        mqttClient.disconnect();
    } else {
        Serial.print(" ❌ Gagal rc=");
        Serial.println(mqttClient.state());
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

// ── Sinyal: pengaturan diperbarui dari web (durasi/peserta/banned) ──
volatile bool pengaturanDiperbarui = false;

// ── NTP ──
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 25200;
const int   daylightOffset_sec = 0;

extern LiquidCrystal_I2C lcd;
extern int active_threshold;

// ── Helper LCD ringkas untuk feedback proses sync ──
static void lcdSyncMsg(const String& l1, const String& l2) {
    lcdPrint16(0, l1);
    lcdPrint16(1, l2);
}

static String shortFileName(String name) {
    name.replace(".wav", "");
    name.replace(".txt", "");
    if (name.length() > 16) name = name.substring(0, 16);
    return name;
}

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
        Serial.println("[NTP] Waktu berhasil disinkronkan.");
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
        Serial.println("[NTP] Timeout sinkronisasi waktu.");

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
    Serial.println("\n[WiFi] startWiFi() dipanggil.");

    if (String(saved_ssid).length() < 2) {
        Serial.println("[WiFi] SSID kosong / belum diset.");

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
        Serial.print("[WiFi] Sudah connected. IP: ");
        Serial.println(WiFi.localIP());
        wifiConnecting = false;
        if (!timeSyncing) {
            beginTimeSync();
        }
        return;
    }

    if (wifiConnecting) {
        Serial.println("[WiFi] Masih proses connect, skip start baru.");
        return;
    }

    Serial.print("[WiFi] Mulai koneksi ke SSID: ");
    Serial.println(saved_ssid);

    lcd.clear();
    lcdPrint16(0, "  MODE: ONLINE  ");
    lcdPrint16(1, "  KONEK WIFI");

    stopTimeSyncState();
    mqttStopRequested = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);

    if (String(wifi_type) == "eduroam") {
        Serial.println("[WiFi] Mode eduroam.");
        String identity = String(eap_nim);
        if (identity.indexOf("@") == -1) identity += "@itb.ac.id";
        Serial.print("Identity : '");
        Serial.print(identity);
        Serial.println("'");

        esp_eap_client_set_identity((uint8_t*)identity.c_str(), identity.length());
        esp_eap_client_set_username((uint8_t*)eap_nim, strlen(eap_nim));
        esp_eap_client_set_password((uint8_t*)eap_pass, strlen(eap_pass));
        esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
        esp_wifi_sta_enterprise_enable();

        Serial.println("Mulai WiFi.begin()...");
        WiFi.begin(saved_ssid);
    } else {
        Serial.println("[WiFi] Mode WiFi biasa.");
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
    if (!ensureSdReady("sync_txt_read", true)) {
        Serial.println("SD tidak siap saat baca TXT: " + namaFile);
        return "";
    }

    File f = SD.open("/" + namaFile, FILE_READ);
    if (!f) {
        Serial.println("Gagal buka file TXT: " + namaFile);
        return "";
    }

    StaticJsonDocument<256> doc;
    doc["file"]         = namaFile;
    doc["target_kelas"] = targetKelas;

    if (isFormatDSNTxt(namaFile)) {
        String baris[2]; int idx = 0;
        while (f.available() && idx < 2) {
            baris[idx] = f.readStringUntil('\n');
            baris[idx].trim();
            idx++;
        }
        f.close();

        if (idx < 2) {
            Serial.println("DSN TXT tidak lengkap (butuh 2 baris): " + namaFile);
            return "";
        }

        doc["no_pertanyaan"] = baris[0].toInt();
        doc["tanggal"]       = baris[1];
        doc["uid"]           = "DOSEN";
    }
    else if (isFormatMHSTxt(namaFile)) {
        String baris[4]; int idx = 0;
        while (f.available() && idx < 4) {
            baris[idx] = f.readStringUntil('\n');
            baris[idx].trim();
            idx++;
        }
        f.close();

        if (idx < 4) {
            Serial.println("MHS TXT tidak lengkap (butuh 4 baris): " + namaFile);
            return "";
        }

        String ws = baris[3];
        ws.replace(" ms", "");
        ws.trim();

        doc["no_pertanyaan"] = baris[0].toInt();
        doc["no_jawaban"]    = baris[1].toInt();
        doc["uid"]           = baris[2];
        doc["waktu_diam_ms"] = ws.toInt();
    }
    else {
        f.close();
        Serial.println("Format nama TXT tidak dikenal: " + namaFile);
        return "";
    }

    char buf[256];
    serializeJson(doc, buf);
    Serial.println("📄 JSON TXT: " + String(buf));
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
            Serial.print("\nWiFi terhubung! IP: ");
            Serial.println(WiFi.localIP());
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
            lcdPrint16(1, "  KONEK WIFI" + animDots());
        }

        if (millis() - wifiConnectStarted >= WIFI_CONNECT_TIMEOUT_MS) {
            Serial.print("\nWiFi timeout! Status: ");
            Serial.println(WiFi.status());
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);

            wifiConnecting = false;
            lastWifiAttempt = millis();

            lcdPrint16(0, "   WIFI GAGAL   ");
            lcdPrint16(1, "  COBA LAGI 05s  ");
        }

        return;
    }

    if (millis() - lastWifiAttempt < WIFI_RETRY_INTERVAL_MS) return;

    startWiFi();
}

//  kirimStatusSync
static void kirimStatusSync(const String& status, const String& pesan,
                            int total = 0, int berhasil = 0) {
    StaticJsonDocument<256> doc;
    doc["device_id"] = DEVICE_ID;
    doc["status"]    = status;
    doc["pesan"]     = pesan;
    if (total > 0)    doc["total"]    = total;
    if (berhasil > 0) doc["berhasil"] = berhasil;

    char buf[256];
    serializeJson(doc, buf);

    bool ok = mqttClient.publish(MQTT_TOPIC_SYNC_STATUS, buf);

    Serial.print("Status Sync ");
    Serial.print(ok ? "OK" : "GAGAL");
    Serial.print(" → ");
    Serial.println(buf);
}



//  kirimAudioHTTP — upload 1 file WAV via HTTPS
//  Pakai WiFiClientSecure terpisah dari MQTT client
static bool writeAll(WiFiClientSecure& client, const uint8_t* data, size_t len, unsigned long timeoutMs = 30000) {
    size_t sent = 0;
    unsigned long start = millis();

    while (sent < len) {
        if (!client.connected()) {
            Serial.println("HTTPS putus saat writeAll().");
            return false;
        }

        size_t n = client.write(data + sent, len - sent);

        if (n > 0) {
            sent += n;
            start = millis();
        } else {
            if (millis() - start > timeoutMs) {
                Serial.println("Timeout writeAll().");
                return false;
            }
            delay(1);
        }
    }

    return true;
}

//  kirimAudioHTTP — upload 1 file WAV via HTTPS
//  Pakai WiFiClientSecure terpisah dari MQTT client
static int kirimAudioHTTP(const String& namaFile, const String& targetKelas, int index, int total) {
    if (!ensureSdReady("sync_audio_read", true)) {
        Serial.println("SD tidak siap saat upload audio.");
        lcdSyncMsg("SD CARD ERROR", "BACA GAGAL");
        return -1;
    }

    File f = SD.open("/" + namaFile, FILE_READ);
    if (!f) {
        Serial.println("Gagal buka audio: /" + namaFile);
        lcdSyncMsg("GAGAL BUKA", shortFileName(namaFile));
        return -1;
    }

    size_t ukuranFile = f.size();
    Serial.printf("Buka: %s (%u bytes)\n", namaFile.c_str(), (unsigned int)ukuranFile);

    if (ukuranFile <= 44) {
        Serial.println("Ukuran WAV terlalu kecil / kemungkinan rusak.");
        lcdSyncMsg("FILE RUSAK", shortFileName(namaFile));
        f.close();
        return -1;
    }

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
    Serial.printf("Content-Length: %u bytes\n", (unsigned int)contentLength);

    // Gunakan client HTTPS terpisah (bukan espClient yang dipakai MQTT)
    WiFiClientSecure httpClient;
    httpClient.setInsecure();
    httpClient.setTimeout(15000);

    lcdSyncMsg("AUDIO " + String(index) + "/" + String(total), "KONEK SERVER");
    Serial.printf("Menghubungkan ke %s:443 ...\n", BACKEND_SERVER);
    if (!httpClient.connect(BACKEND_SERVER, 443)) {
        Serial.println("Gagal konek ke " + String(BACKEND_SERVER) + ":443");
        lcdSyncMsg("SERVER GAGAL", "CEK INTERNET");
        f.close();
        return -1;
    }

    Serial.println("Terhubung ke Render, mulai stream...");

    httpClient.printf(
        "POST /api/upload-audio-sd HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        BACKEND_SERVER, boundary.c_str(), (unsigned int)contentLength
    );

    if (!httpClient.print(partHeader)) {
        Serial.println("Gagal kirim multipart header.");
        lcdSyncMsg("UPLOAD GAGAL", shortFileName(namaFile));
        f.close();
        httpClient.stop();
        return -1;
    }

    const size_t CHUNK = 512;
    uint8_t chunkBuf[CHUNK];
    size_t terkirim = 0;
    unsigned long lastPrint = millis();
    unsigned long lastLcdUpdate = 0;
    unsigned long lastMqttKeep = millis();

    while (f.available()) {
        size_t baca = f.read(chunkBuf, CHUNK);
        if (baca == 0) break;

        if (!writeAll(httpClient, chunkBuf, baca)) {
            Serial.printf("Stream audio gagal pada %u/%u bytes.\n",
                          (unsigned int)terkirim, (unsigned int)ukuranFile);
            lcdSyncMsg("UPLOAD GAGAL", shortFileName(namaFile));
            f.close();
            httpClient.stop();
            return -1;
        }

        terkirim += baca;

        // Update progress LCD maksimal 1x/detik agar tak ganggu timing upload
        if (millis() - lastLcdUpdate >= 1000) {
            lastLcdUpdate = millis();
            int percent = (ukuranFile > 0)
                ? (int)(((uint64_t)terkirim * 100ULL) / ukuranFile) : 0;
            lcdSyncMsg(
                "UP " + String(index) + "/" + String(total) + " " + String(percent) + "%",
                String((unsigned long)(terkirim / 1024)) + "K/" + String((unsigned long)(ukuranFile / 1024)) + "K"
            );
        }

        if (millis() - lastPrint > 2000) {
            Serial.printf("   Progress: %u/%u bytes (%.0f%%)\n",
                (unsigned int)terkirim, (unsigned int)ukuranFile,
                ukuranFile > 0 ? ((float)terkirim / ukuranFile * 100.0f) : 0.0f);
            lastPrint = millis();
        }

        // Jaga koneksi MQTT tetap hidup selama upload panjang (keepAlive 15s).
        // mqttClient memakai espClient yang TERPISAH dari httpClient di sini,
        // jadi memanggil loop() tidak mengganggu byte stream upload. Tanpa ini
        // sesi MQTT putus saat upload >~15s dan status "selesai" gagal terkirim.
        if (millis() - lastMqttKeep >= 3000) {
            lastMqttKeep = millis();
            if (mqttClient.connected()) mqttClient.loop();
        }
    }

    f.close();
    Serial.printf("File selesai distream: %u bytes\n", (unsigned int)terkirim);

    if (terkirim != ukuranFile) {
        Serial.printf("Byte terkirim tidak sama! terkirim=%u, ukuran=%u\n",
                      (unsigned int)terkirim, (unsigned int)ukuranFile);
        lcdSyncMsg("UPLOAD GAGAL", shortFileName(namaFile));
        httpClient.stop();
        return -1;
    }

    if (!httpClient.print(partMid)) {
        Serial.println("Gagal kirim multipart penutup/metadata.");
        lcdSyncMsg("UPLOAD GAGAL", shortFileName(namaFile));
        httpClient.stop();
        return -1;
    }

    lcdSyncMsg("UPLOAD " + String(index) + "/" + String(total), "TUNGGU SERVER");

    // Baca response code
    int httpCode = -1;
    String responseLine = "";
    unsigned long timeout = millis();

    while (millis() - timeout < 15000) {
        if (httpClient.available()) {
            responseLine = httpClient.readStringUntil('\n');
            responseLine.trim();
            if (responseLine.startsWith("HTTP/")) {
                httpCode = responseLine.substring(9, 12).toInt();
                Serial.printf("HTTP %d untuk: %s\n", httpCode, namaFile.c_str());
                break;
            }
        }
        // Tetap servis MQTT selama menunggu respons server (bisa sampai 15s).
        if (millis() - lastMqttKeep >= 3000) {
            lastMqttKeep = millis();
            if (mqttClient.connected()) mqttClient.loop();
        }
        delay(10);
    }

    if (httpCode == -1) {
        Serial.println("Timeout baca response dari server.");
    }

    String body = "";
    unsigned long bodyTimeout = millis();
    while (millis() - bodyTimeout < 3000) {
        while (httpClient.available()) {
            body += (char)httpClient.read();
            bodyTimeout = millis();
        }
        if (!httpClient.connected() && !httpClient.available()) break;
        delay(10);
    }

    if (body.length() > 0) {
        int jsonStart = body.indexOf('{');
        if (jsonStart >= 0) {
            Serial.println("   Response: " + body.substring(jsonStart));
        } else {
            Serial.println("   Response body: " + body);
        }
    }

    if (httpCode == 200 || httpCode == 201) {
        lcdSyncMsg("SERVER OK", "FILE TERKIRIM");
    } else if (httpCode > 0) {
        lcdSyncMsg("HTTP ERROR " + String(httpCode), "CEK SERVER");
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
        Serial.println("SD tidak siap saat tulis cache UID.");
        return false;
    }

    if (uids.isNull()) {
        Serial.println("Array UID kosong/null.");
        return false;
    }

    if (SD.exists(tempPath)) SD.remove(tempPath);

    File file = SD.open(tempPath, FILE_WRITE);
    if (!file) {
        Serial.println("Gagal buka file temp UID: " + String(tempPath));
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

    Serial.printf("✅ Cache UID tersimpan: %s (%d UID)\n", finalPath, count);

    pengaturanDiperbarui = true; // feedback LCD: cache peserta/banned berhasil ditulis
    return true;
}

//  MQTT CALLBACK
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.print("\n📩 [PESAN MASUK] Topik: ");
    Serial.println(topic);

    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

    Serial.println("Isi: " + msg);

    if (String(topic) != MQTT_TOPIC_PERINTAH) {
        Serial.println("⏭️ Topik diabaikan.");
        return;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, msg);

    if (error) {
        Serial.println("⚠️ JSON tidak valid / buffer kurang: " + String(error.c_str()));
        kirimStatusSync("error", "JSON MQTT gagal diparse. Payload terlalu besar/rusak.", 0, 0);
        return;
    }

    String perintah = doc["perintah"] | "";

    if (perintah == "request_sync_audio") {
        // Sync file TXT
        String kelas = doc["target_kelas"] | "";
        Serial.println("📥 request_sync_audio → Sync TXT, Kelas: " + kelas);

        if (!sdSyncAktif) {
            sdSyncAktif = true;
            sdTargetKelas = kelas;
        } else {
            Serial.println("⚠️ TXT sync sudah berjalan.");
        }
    }
    else if (perintah == "request_sync_wav") {
        // Sync file WAV
        String kelas = doc["target_kelas"] | "";
        Serial.println("📥 request_sync_wav → Sync WAV, Kelas: " + kelas);

        if (!audioSyncAktif) {
            audioSyncAktif = true;
            audioTargetKelas = kelas;
        } else {
            Serial.println("⚠️ Audio sync sudah berjalan.");
        }
    }
    else if (perintah == "ack_file") {
        String namaFile = doc["file"] | "";
        Serial.println("✅ ACK diterima untuk: " + namaFile);

        if (namaFile == sdFileSedangDikirim || namaFile == "") {
            sdAckDiterima = true;
        }
    }
    else if (perintah == "batalkan_sync") {
        Serial.println("🛑 Server meminta pembatalan sync.");
        sdAckBatalkan = true;
    }
    else if (perintah == "set_timer") {
        float durasi_menit = doc["durasi_detik"] | 0.0f;   // baca sebagai float, bukan int

        Serial.printf("⏱️ set_timer diterima: %.2f menit\n", durasi_menit);

        if (durasi_menit > 0.0f) {
            maxRecordMs = (unsigned long)(durasi_menit * 60.0f * 1000.0f);

            Preferences prefs;
            prefs.begin("catch_note", false);
            prefs.putULong("max_record_ms", maxRecordMs);
            prefs.end();

            Serial.printf("✅ Durasi rekaman disimpan: %lu ms\n", maxRecordMs);

            pengaturanDiperbarui = true;
        }
    }
    else if (perintah == "set_threshold") {
        int nilai = doc["nilai"] | -1;

        Serial.printf("🎚️ set_threshold diterima: %d\n", nilai);

        if (nilai > 0) {
            active_threshold = nilai;

            Preferences prefs;
            prefs.begin("catch_note", false);
            prefs.putInt("threshold", active_threshold);
            prefs.end();

            Serial.printf("✅ Threshold disimpan: %d\n", active_threshold);

            pengaturanDiperbarui = true;
        }
    }
    // SYNC DAFTAR MAHASISWA BANNED
    else if (perintah == "sync_uid_aktif") {
        JsonArray uids = doc["uids"].as<JsonArray>();
        int count = 0;

        Serial.println("📥 sync_uid_aktif → tulis banned.txt");

        if (!tulisUidCacheAtomik("/banned.txt", "/banned.tmp", "/banned.bak", uids, true, count)) {
            kirimStatusSync("error", "Gagal menyimpan daftar banned.", 0, 0);
            return;
        }

        kirimStatusSync("selesai", "Daftar banned tersimpan.", count, count);
        return;
    }
    // SYNC DAFTAR PESERTA AKTIF
    else if (perintah == "sync_uid_kelas") {
        JsonArray uids = doc["uids"].as<JsonArray>();
        int count = 0;

        Serial.println("📥 sync_uid_kelas → tulis eligible.txt");

        if (!tulisUidCacheAtomik("/eligible.txt", "/eligible.tmp", "/eligible.bak", uids, false, count)) {
            kirimStatusSync("error", "Gagal menyimpan daftar peserta.", 0, 0);
            return;
        }

        kirimStatusSync("selesai", "Daftar peserta tersimpan.", count, count);
        return;
    }
    else {
        Serial.println("⚠️ Perintah tidak dikenal: " + perintah);
    }
}



void handleMQTT() {
    static bool callbackSet = false;

    if (!callbackSet) {
        espClient.setInsecure();

        mqttClient.setCallback(mqttCallback);
        mqttClient.setBufferSize(4096);
        Serial.println("[MQTT] Callback diset, buffer=4096.");

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

    Serial.println("[MQTT] Membuat task koneksi MQTT...");

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
        Serial.println("❌ Gagal membuat task MQTTConnect.");
        mqttConnecting = false;
        mqttConnectTaskHandle = nullptr;
    }
}

void stopMQTT() {
  Serial.println("[MQTT] stopMQTT() dipanggil.");
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
    doc["threshold"] = active_threshold; 

    char buf[256];
    serializeJson(doc, buf);
    bool ok = mqttClient.publish(MQTT_TOPIC_STATUS, buf, true);
    Serial.print("📤 Status ");
    Serial.print(ok ? "OK" : "GAGAL");
    Serial.print(" → ");
    Serial.println(buf);
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
    bool ok = mqttClient.publish(MQTT_TOPIC_REG, buf);
    Serial.print("📤 RFID ");
    Serial.print(ok ? "OK" : "GAGAL");
    Serial.print(" → ");
    Serial.println(buf);
}

//  prosesSinkronisasiAudio — semua WAV via HTTPS
void prosesSinkronisasiAudio(const String& targetKelas) {
    Serial.println("\n🎵 Memulai sinkronisasi AUDIO untuk kelas: " + targetKelas);
    lcdSyncMsg("SYNC AUDIO", "CEK SD CARD...");

    if (!ensureSdReady("sync_audio_start", true)) {
        Serial.println("❌ SD Card tidak siap saat mulai sync audio.");
        lcdSyncMsg("SD CARD ERROR", "SYNC GAGAL");
        kirimStatusSync("error", "SD Card tidak siap.", 0, 0);
        audioSyncAktif = false;
        return;
    }

    lcdSyncMsg("SYNC AUDIO", "CARI FILE...");
    File root = SD.open("/");
    if (!root) {
        Serial.println("❌ Gagal buka root SD saat sync audio.");
        lcdSyncMsg("SD CARD ERROR", "SYNC GAGAL");
        markSdLost("sync_audio_root");
        kirimStatusSync("error", "SD Card gagal dibaca.", 0, 0);
        audioSyncAktif = false;
        return;
    }

    std::vector<String> daftarAudio;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;

        String nama = String(entry.name());
        bool isDir = entry.isDirectory();
        entry.close();

        if (isDir) continue;

        if (isFormatDSNWav(nama) || isFormatMHSWav(nama)) {
            daftarAudio.push_back(nama);
            Serial.println("🎵 Audio ditemukan: " + nama);
        } else {
            Serial.println("⏭️  Dilewati: " + nama);
        }
    }
    root.close();

    int total = daftarAudio.size(), berhasil = 0;

    if (total == 0) {
        Serial.println("ℹ️ Tidak ada file audio di SD Card.");
        lcdSyncMsg("AUDIO KOSONG", "TIDAK ADA FILE");
        kirimStatusSync("selesai", "Tidak ada file audio untuk dikirim.", 0, 0);
        audioSyncAktif = false;
        return;
    }

    Serial.printf("📦 Total audio: %d file\n", total);
    lcdSyncMsg("AUDIO DITEMUKAN", "TOTAL: " + String(total) + " FILE");
    // Beri tahu web jumlah file audio agar progress bar mulai bergerak.
    kirimStatusSync("progress", "Menyiapkan audio...", total, 0);
    delay(600);

    for (int i = 0; i < total; i++) {
        if (sdAckBatalkan) {
            Serial.println("🛑 Sinkronisasi audio dibatalkan sebelum upload berikutnya.");
            lcdSyncMsg("SYNC BATAL", String(berhasil) + "/" + String(total) + " TERKIRIM");
            kirimStatusSync("dibatalkan", "Sinkronisasi audio dihentikan.", total, berhasil);
            audioSyncAktif = false;
            sdAckBatalkan = false;
            return;
        }

        String nama = daftarAudio[i];
        Serial.printf("\n🎵 [%d/%d] Mengirim audio: %s\n", i + 1, total, nama.c_str());
        lcdSyncMsg("AUDIO " + String(i + 1) + "/" + String(total), shortFileName(nama));

        int httpCode = kirimAudioHTTP(nama, targetKelas, i + 1, total);

        if (httpCode == 200 || httpCode == 201) {
            Serial.println("✅ Diterima server! Menghapus: " + nama);

            if (!ensureSdReady("sync_audio_remove", true) || !SD.remove("/" + nama)) {
                Serial.println("❌ SD hilang / gagal hapus audio: " + nama);
                lcdSyncMsg("GAGAL HAPUS", "CEK SD CARD");
                kirimStatusSync("error", "SD hilang/gagal hapus: " + nama, total, berhasil);
                audioSyncAktif = false;
                return;
            }

            berhasil++;
            Serial.printf("🗑️  Progress: %d/%d\n", berhasil, total);
            // Laporkan progress ke web (handler memetakan "progress" -> loading).
            kirimStatusSync("progress",
                "Audio terkirim " + String(berhasil) + "/" + String(total),
                total, berhasil);
        } else {
            Serial.printf("❌ Gagal upload: %s (HTTP %d)\n", nama.c_str(), httpCode);

            kirimStatusSync("error",
                "Gagal upload: " + nama + ". Ulangi atau batalkan?", total, berhasil);

            sdAckDiterima = false;
            sdAckBatalkan = false;

            unsigned long tw = millis();
            while (!sdAckDiterima && !sdAckBatalkan && millis() - tw < 30000) {
                mqttClient.loop();
                delay(100);
            }

            if (sdAckBatalkan || !sdAckDiterima) {
                Serial.println("🛑 Sinkronisasi audio dihentikan setelah gagal upload.");
                lcdSyncMsg("SYNC BATAL", String(berhasil) + "/" + String(total) + " TERKIRIM");
                kirimStatusSync("dibatalkan", "Sinkronisasi audio dihentikan.", total, berhasil);
                audioSyncAktif = false;
                return;
            }

            Serial.println("↩️ Server/user memilih lanjut, skip file ini.");
        }
    }

    Serial.printf("\n🎉 Audio selesai! %d/%d berhasil.\n", berhasil, total);
    lcdSyncMsg("SYNC SELESAI", String(berhasil) + "/" + String(total) + " TERKIRIM");
    kirimStatusSync("selesai", "Semua audio berhasil dikirim.", total, berhasil);
    audioSyncAktif = false;
}



//  prosesSinkronisasiSD — semua TXT via MQTT
void prosesSinkronisasiSD(const String& targetKelas) {
    Serial.println("\n🔄 Memulai sinkronisasi TXT untuk kelas: " + targetKelas);
    lcdSyncMsg("SYNC DATA TXT", "CEK SD CARD...");

    if (!ensureSdReady("sync_txt_start", true)) {
        Serial.println("❌ SD Card tidak siap saat mulai sync TXT.");
        lcdSyncMsg("SD CARD ERROR", "SYNC GAGAL");
        kirimStatusSync("error", "SD Card tidak siap.", 0, 0);
        sdSyncAktif = false;
        return;
    }

    lcdSyncMsg("SYNC DATA TXT", "CARI FILE...");
    File root = SD.open("/");
    if (!root) {
        Serial.println("❌ Gagal buka root SD saat sync TXT.");
        lcdSyncMsg("SD CARD ERROR", "SYNC GAGAL");
        markSdLost("sync_txt_root");
        kirimStatusSync("error", "SD Card gagal dibaca.", 0, 0);
        sdSyncAktif = false;
        return;
    }

    std::vector<String> daftarFile;
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;

        String nama = String(entry.name());
        bool isDir = entry.isDirectory();
        entry.close();

        if (isDir) continue;

        if (isFormatDSNTxt(nama) || isFormatMHSTxt(nama)) {
            daftarFile.push_back(nama);
            Serial.println("📄 Ditemukan: " + nama);
        } else {
            Serial.println("⏭️  Dilewati (bukan format MHS/DSN txt): " + nama);
        }
    }
    root.close();

    int total = daftarFile.size(), berhasil = 0;

    if (total == 0) {
        Serial.println("ℹ️ Tidak ada file TXT di SD Card.");
        lcdSyncMsg("DATA KOSONG", "TIDAK ADA FILE");
        kirimStatusSync("selesai", "Tidak ada file untuk dikirim. SD Card kosong.", 0, 0);
        sdSyncAktif = false;
        return;
    }

    Serial.printf("📦 Total TXT: %d file\n", total);
    lcdSyncMsg("DATA DITEMUKAN", "TOTAL: " + String(total) + " FILE");
    delay(600);

    for (int i = 0; i < total; i++) {
        String namaFile = daftarFile[i];
        Serial.printf("\n📤 [%d/%d] Mengirim TXT: %s\n", i + 1, total, namaFile.c_str());
        lcdSyncMsg("DATA " + String(i + 1) + "/" + String(total), shortFileName(namaFile));

        String jsonData = bacaFileSdKeJson(namaFile, targetKelas);
        if (jsonData == "") {
            Serial.println("⚠️ File dilewati karena gagal dibaca/format tidak valid: " + namaFile);
            lcdSyncMsg("GAGAL BACA", shortFileName(namaFile));

            sdFileSedangDikirim = namaFile;
            kirimStatusSync("error", "Gagal baca: " + namaFile + ". Lanjut atau batalkan?", total, berhasil);

            sdAckDiterima = false;
            sdAckBatalkan = false;

            unsigned long tw = millis();
            while (!sdAckDiterima && !sdAckBatalkan && millis() - tw < 30000) {
                mqttClient.loop();
                delay(100);
            }

            if (sdAckBatalkan || !sdAckDiterima) {
                Serial.println("🛑 Sinkronisasi TXT dihentikan saat gagal baca file.");
                lcdSyncMsg("SYNC BATAL", String(berhasil) + "/" + String(total) + " TERKIRIM");
                kirimStatusSync("dibatalkan", "Dihentikan.", total, berhasil);
                sdSyncAktif = false;
                return;
            }

            continue;
        }

        sdFileSedangDikirim = namaFile;
        sdAckDiterima = false;
        sdAckBatalkan = false;

        mqttClient.setBufferSize(4096);
        bool terkirim = mqttClient.publish(MQTT_TOPIC_AUDIO_DATA, jsonData.c_str(), true);

        Serial.print("📤 Publish TXT ");
        Serial.print(terkirim ? "OK" : "GAGAL");
        Serial.print(" → ");
        Serial.println(namaFile);

        if (!terkirim) {
            lcdSyncMsg("MQTT GAGAL", shortFileName(namaFile));
            kirimStatusSync("error", "Gagal kirim: " + namaFile, total, berhasil);

            unsigned long tw = millis();
            while (!sdAckDiterima && !sdAckBatalkan && millis() - tw < 30000) {
                mqttClient.loop();
                delay(100);
            }

            if (sdAckBatalkan || !sdAckDiterima) {
                Serial.println("🛑 Sinkronisasi TXT dihentikan setelah publish gagal.");
                lcdSyncMsg("SYNC BATAL", String(berhasil) + "/" + String(total) + " TERKIRIM");
                kirimStatusSync("dibatalkan", "Dihentikan.", total, berhasil);
                sdSyncAktif = false;
                return;
            }

            continue;
        }

        lcdSyncMsg("DATA " + String(i + 1) + "/" + String(total), "TUNGGU ACK...");
        Serial.println("⏳ Menunggu konfirmasi server...");

        unsigned long tw = millis();
        while (!sdAckDiterima && !sdAckBatalkan && millis() - tw < 15000) {
            mqttClient.loop();
            delay(100);
        }

        if (sdAckBatalkan) {
            Serial.println("🛑 Sinkronisasi TXT dibatalkan server.");
            lcdSyncMsg("SYNC BATAL", String(berhasil) + "/" + String(total) + " TERKIRIM");
            kirimStatusSync("dibatalkan", "Dibatalkan server.", total, berhasil);
            sdSyncAktif = false;
            return;
        }

        if (!sdAckDiterima) {
            Serial.println("⏱️ Timeout! Server tidak merespon: " + namaFile);
            lcdSyncMsg("ACK TIMEOUT", "ULANG/BATAL?");
            kirimStatusSync("error", "Timeout: " + namaFile + ". Ulangi atau batalkan?", total, berhasil);

            unsigned long tw2 = millis();
            while (!sdAckDiterima && !sdAckBatalkan && millis() - tw2 < 30000) {
                mqttClient.loop();
                delay(100);
            }

            if (sdAckBatalkan || !sdAckDiterima) {
                Serial.println("🛑 Sinkronisasi TXT dihentikan karena timeout.");
                lcdSyncMsg("SYNC BATAL", String(berhasil) + "/" + String(total) + " TERKIRIM");
                kirimStatusSync("dibatalkan", "Dihentikan (timeout).", total, berhasil);
                sdSyncAktif = false;
                return;
            }
        }

        lcdSyncMsg("DATA " + String(i + 1) + "/" + String(total) + " OK", "LANJUT...");
        Serial.println("✅ Diterima server! Menghapus: " + namaFile);

        if (!ensureSdReady("sync_txt_remove", true) || !SD.remove("/" + namaFile)) {
            Serial.println("❌ SD hilang / gagal hapus TXT: " + namaFile);
            lcdSyncMsg("GAGAL HAPUS", "CEK SD CARD");
            kirimStatusSync("error", "SD hilang/gagal hapus: " + namaFile, total, berhasil);
            sdSyncAktif = false;
            return;
        }

        berhasil++;
        Serial.printf("🗑️ Progress: %d/%d\n", berhasil, total);
    }

    Serial.printf("\n🎉 TXT selesai! %d/%d berhasil.\n", berhasil, total);
    lcdSyncMsg("SYNC SELESAI", String(berhasil) + "/" + String(total) + " TERKIRIM");
    kirimStatusSync("selesai", "Semua file berhasil dikirim.", total, berhasil);
    sdSyncAktif = false;
}
