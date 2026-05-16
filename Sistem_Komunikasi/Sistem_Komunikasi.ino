#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include "esp_eap_client.h"
#include "esp_wifi.h"
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <DNSServer.h>

DNSServer dnsServer;
const byte DNS_PORT = 53;

// ================= KONFIGURASI HIVEMQ CLOUD =================
const char* mqtt_server = "c4bbf4787735464dadc96ca13e4a9c6b.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "catchnote";
const char* mqtt_pass   = "Ta2526018";

// ================= KONFIGURASI BACKEND (Render — HTTPS) =================
const char* backend_server = "ta2526018.onrender.com";

// ================= KONFIGURASI WIFI =================
char eap_nim[40]    = "";
char eap_pass[40]   = "";
char saved_ssid[40] = "";
char wifi_type[10]  = "biasa";
char wifi_pass[64]  = "";

// ================= KONFIGURASI THRESHOLD =================
// Nilai default: Tinggi=90, Sedang=70, Rendah=50
int threshold_value = 70;  // nilai aktif yang dipakai sistem

const int THRESHOLD_TINGGI = 90;
const int THRESHOLD_SEDANG = 70;
const int THRESHOLD_RENDAH = 50;

const int device_id = 1;

// ================= PIN & HARDWARE =================
#define RESET_BUTTON 0
#define SDA_PIN 21
#define SCL_PIN 22
#define SD_CS_PIN 15

// ================= TOPIK MQTT =================
const char* topic_status      = "kelas/alat/status";
const char* topic_rfid        = "kelas/alat/rfid";
const char* topic_perintah    = "kelas/alat/perintah";
const char* topic_audio_data  = "kelas/alat/audio_data";
const char* topic_sync_status = "kelas/alat/sync_status";

WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);
Preferences      preferences;
Adafruit_PN532   nfc(-1, -1);

unsigned long lastStatusTime = 0;
const long    statusInterval = 10000;
int           dummyBattery   = 95;

// ================= STATE SINKRONISASI =================
volatile bool sdSyncAktif    = false;
volatile bool audioSyncAktif = false;
bool     sdAckDiterima       = false;
bool     sdAckBatalkan       = false;
String   sdTargetKelas       = "";
String   audioTargetKelas    = "";
String   sdFileSedangDikirim = "";

// ================= TOMBOL RESET (INTERRUPT + FREERTOS) =================
volatile unsigned long isrButtonPressTime = 0;
volatile bool          isrButtonActive    = false;
TaskHandle_t           resetTaskHandle    = NULL;

void IRAM_ATTR onBootButtonISR() {
  if (digitalRead(RESET_BUTTON) == LOW) {
    isrButtonPressTime = millis();
    isrButtonActive    = true;
  } else {
    isrButtonActive = false;
  }
}

void resetTask(void* param) {
  const TickType_t checkInterval = pdMS_TO_TICKS(50);
  bool sudahWarning = false;

  while (true) {
    vTaskDelay(checkInterval);

    if (!isrButtonActive) {
      sudahWarning = false;
      continue;
    }

    unsigned long durasi = millis() - isrButtonPressTime;

    if (durasi >= 1000 && !sudahWarning) {
      Serial.println("\n⚠️  [RESET] Tahan 3 detik untuk reset...");
      sudahWarning = true;
    }

    if (durasi >= 3000) {
      Serial.println("🔄 [RESET] Tombol ditekan 3 detik — memulai reset!");

      if (sdSyncAktif || audioSyncAktif) {
        Serial.println("🛑 [RESET] Membatalkan sinkronisasi...");
        sdAckBatalkan  = true;
        sdSyncAktif    = false;
        audioSyncAktif = false;
        vTaskDelay(pdMS_TO_TICKS(300));
      }

      Preferences pref;
      pref.begin("catch_note", false);
      pref.clear();
      pref.end();
      Serial.println("✅ [RESET] Memori terhapus. Restart...");

      vTaskDelay(pdMS_TO_TICKS(500));
      ESP.restart();
    }
  }
}

// ================= SD CARD =================
bool initSDCard() {
  Serial.println("🔧 [SD] Mencoba inisialisasi SD Card...");
  Serial.printf("   CS PIN : GPIO %d\n", SD_CS_PIN);

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  delay(100);

  SPI.end();
  delay(50);

  SPI.begin(18, 19, 23, SD_CS_PIN);
  delay(200);
  Serial.println("   SPI.begin() selesai");

  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("   Percobaan SD.begin() %d/3...\n", attempt);
    SD.end();
    delay(100);

    if (SD.begin(SD_CS_PIN)) {
      uint8_t  cardType = SD.cardType();
      uint64_t cardSize = SD.cardSize() / (1024 * 1024);
      Serial.printf("✅ SD Card siap! Tipe=%d, Ukuran=%lluMB\n", cardType, cardSize);
      return true;
    }
    Serial.printf("   ❌ Percobaan %d gagal\n", attempt);
    delay(300);
  }

  Serial.println("❌ Gagal menginisialisasi SD Card setelah 3x percobaan!");
  return false;
}

// ================= STATUS SYNC =================
void kirimStatusSync(const String& status, const String& pesan, int total = 0, int berhasil = 0) {
  StaticJsonDocument<256> doc;
  doc["device_id"] = device_id;
  doc["status"]    = status;
  doc["pesan"]     = pesan;
  doc["total"]     = total;
  doc["berhasil"]  = berhasil;
  char buf[256];
  serializeJson(doc, buf);
  mqttClient.publish(topic_sync_status, buf);
  Serial.println("📤 Status Sync → " + String(buf));
}

// ================= VALIDASI NAMA FILE =================
bool isFormatMHS(const String& nama) {
  if (!nama.startsWith("MHS_") || !nama.endsWith(".txt")) return false;
  String tengah = nama.substring(4, nama.length() - 4);
  int us = tengah.indexOf('_');
  if (us <= 0) return false;
  if (tengah.indexOf('_', us + 1) != -1) return false;
  String b1 = tengah.substring(0, us);
  String b2 = tengah.substring(us + 1);
  if (b1.length() == 0 || b2.length() == 0) return false;
  for (char c : b1) if (!isDigit(c)) return false;
  for (char c : b2) if (!isDigit(c)) return false;
  return true;
}

bool isFormatDSN(const String& nama) {
  if (!nama.startsWith("DSN_") || !nama.endsWith(".txt")) return false;
  String tengah = nama.substring(4, nama.length() - 4);
  if (tengah.length() == 0) return false;
  for (char c : tengah) if (!isDigit(c)) return false;
  return true;
}

bool isFormatDSNWav(const String& nama) {
  if (!nama.startsWith("DSN_") || !nama.endsWith(".wav")) return false;
  String tengah = nama.substring(4, nama.length() - 4);
  if (tengah.length() == 0) return false;
  for (char c : tengah) if (!isDigit(c)) return false;
  return true;
}

bool isFormatMHSWav(const String& nama) {
  if (!nama.startsWith("MHS_") || !nama.endsWith(".wav")) return false;
  String tengah = nama.substring(4, nama.length() - 4);
  int us = tengah.indexOf('_');
  if (us <= 0) return false;
  if (tengah.indexOf('_', us + 1) != -1) return false;
  String b1 = tengah.substring(0, us);
  String b2 = tengah.substring(us + 1);
  if (b1.length() == 0 || b2.length() == 0) return false;
  for (char c : b1) if (!isDigit(c)) return false;
  for (char c : b2) if (!isDigit(c)) return false;
  return true;
}

// ================= BACA FILE TXT → JSON =================
String bacaFileSdKeJson(const String& namaFile, const String& targetKelas) {
  File f = SD.open("/" + namaFile, FILE_READ);
  if (!f) {
    Serial.println("❌ Gagal buka file: " + namaFile);
    return "";
  }

  String baris[4];
  int idx = 0;
  while (f.available() && idx < 4) {
    baris[idx] = f.readStringUntil('\n');
    baris[idx].trim();
    idx++;
  }
  f.close();

  StaticJsonDocument<256> doc;
  doc["file"]         = namaFile;
  doc["target_kelas"] = targetKelas;

  if (isFormatDSN(namaFile)) {
    if (idx < 2) {
      Serial.println("⚠️ DSN file tidak lengkap (butuh 2 baris): " + namaFile);
      return "";
    }
    String tengah = namaFile.substring(4, namaFile.length() - 4);
    doc["no_pertanyaan"] = tengah.toInt();
    doc["tanggal"]       = baris[1];

  } else if (isFormatMHS(namaFile)) {
    if (idx < 4) {
      Serial.println("⚠️ MHS file tidak lengkap (butuh 4 baris): " + namaFile);
      return "";
    }
    String waktuStr = baris[3];
    waktuStr.replace(" ms", "");
    waktuStr.trim();

    doc["no_pertanyaan"] = baris[0].toInt();
    doc["no_jawaban"]    = baris[1].toInt();
    doc["uid"]           = baris[2];
    doc["waktu_diam_ms"] = waktuStr.toInt();

  } else {
    Serial.println("⚠️ Format nama file tidak dikenal: " + namaFile);
    return "";
  }

  char buf[256];
  serializeJson(doc, buf);
  return String(buf);
}

// ================= KIRIM AUDIO VIA HTTPS =================
int kirimAudioHTTP(const String& namaFile, const String& targetKelas) {
  String path = "/" + namaFile;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println("❌ Gagal buka: " + path);
    return -1;
  }
  size_t ukuranFile = f.size();
  Serial.printf("📂 Buka: %s (%u bytes)\n", namaFile.c_str(), ukuranFile);

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
  Serial.printf("📦 Content-Length: %u bytes\n", contentLength);

  WiFiClientSecure client;
  client.setInsecure();

  Serial.printf("🔌 Menghubungkan ke %s:443 ...\n", backend_server);
  if (!client.connect(backend_server, 443)) {
    Serial.println("❌ Gagal konek ke " + String(backend_server) + ":443");
    f.close();
    return -1;
  }
  Serial.println("🔌 Terhubung ke Render, mulai stream...");

  client.printf(
    "POST /api/upload-audio-sd HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %u\r\n"
    "Connection: close\r\n"
    "\r\n",
    backend_server, boundary.c_str(), (unsigned int)contentLength
  );

  client.print(partHeader);

  const size_t CHUNK = 512;
  uint8_t chunkBuf[CHUNK];
  size_t terkirim = 0;
  unsigned long lastPrint = millis();

  while (f.available()) {
    size_t baca    = f.read(chunkBuf, CHUNK);
    size_t ditulis = client.write(chunkBuf, baca);
    terkirim += ditulis;

    if (millis() - lastPrint > 2000) {
      Serial.printf("   ⏳ Progress: %u/%u bytes (%.0f%%)\n",
        terkirim, (unsigned int)ukuranFile,
        (float)terkirim / ukuranFile * 100);
      lastPrint = millis();
    }
  }
  f.close();
  Serial.printf("✅ File selesai distream: %u bytes\n", terkirim);

  client.print(partMid);

  int httpCode = -1;
  String responseLine = "";
  unsigned long timeout = millis();

  while (millis() - timeout < 15000) {
    if (client.available()) {
      responseLine = client.readStringUntil('\n');
      responseLine.trim();
      if (responseLine.startsWith("HTTP/")) {
        httpCode = responseLine.substring(9, 12).toInt();
        Serial.printf("📡 HTTP %d untuk: %s\n", httpCode, namaFile.c_str());
        break;
      }
    }
    delay(10);
  }

  if (httpCode == -1) Serial.println("⏱️ Timeout baca response dari server.");

  String body = "";
  unsigned long bodyTimeout = millis();
  while (client.available() && millis() - bodyTimeout < 3000) {
    body += (char)client.read();
  }
  if (body.length() > 0) {
    int jsonStart = body.indexOf('{');
    if (jsonStart >= 0) Serial.println("   Response: " + body.substring(jsonStart));
  }

  client.stop();
  return httpCode;
}

// ================= SINKRONISASI AUDIO =================
void prosesSinkronisasiAudio(const String& targetKelas) {
  Serial.println("\n🎵 Memulai sinkronisasi AUDIO untuk kelas: " + targetKelas);

  if (!initSDCard()) {
    kirimStatusSync("error", "SD Card tidak dapat diakses (audio sync)");
    audioSyncAktif = false;
    return;
  }

  File root = SD.open("/");
  std::vector<String> daftarAudio;

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String nama   = String(entry.name());
    bool   isDir  = entry.isDirectory();
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

  int total    = daftarAudio.size();
  int berhasil = 0;

  if (total == 0) {
    Serial.println("ℹ️ Tidak ada file audio di SD Card.");
    kirimStatusSync("selesai", "Tidak ada file audio untuk dikirim.", 0, 0);
    audioSyncAktif = false;
    return;
  }

  Serial.printf("📦 Total audio: %d file\n", total);

  for (int i = 0; i < total; i++) {
    if (sdAckBatalkan) {
      kirimStatusSync("dibatalkan", "Sinkronisasi audio dihentikan.", total, berhasil);
      audioSyncAktif = false;
      return;
    }

    String nama = daftarAudio[i];
    Serial.printf("\n🎵 [%d/%d] Mengirim audio: %s\n", i + 1, total, nama.c_str());

    int httpCode = kirimAudioHTTP(nama, targetKelas);

    if (httpCode == 200 || httpCode == 201) {
      Serial.println("✅ Diterima! Menghapus: " + nama);
      SD.remove("/" + nama);
      berhasil++;
      Serial.printf("🗑️  Progress: %d/%d\n", berhasil, total);
    } else {
      Serial.printf("❌ Gagal kirim: %s (HTTP %d)\n", nama.c_str(), httpCode);
      kirimStatusSync("error",
        "Gagal kirim audio: " + nama + ". Ulangi atau batalkan?",
        total, berhasil);

      sdAckDiterima = false;
      sdAckBatalkan = false;
      unsigned long tunggu = millis();
      while (!sdAckDiterima && !sdAckBatalkan && millis() - tunggu < 30000) {
        mqttClient.loop();
        delay(100);
      }

      if (sdAckBatalkan || !sdAckDiterima) {
        kirimStatusSync("dibatalkan", "Sinkronisasi audio dihentikan.", total, berhasil);
        audioSyncAktif = false;
        return;
      }
      Serial.println("↩️  Server: lanjut, skip file ini.");
    }
  }

  Serial.printf("\n🎉 Audio selesai! %d/%d berhasil.\n", berhasil, total);
  kirimStatusSync("selesai", "Semua audio berhasil dikirim.", total, berhasil);
  audioSyncAktif = false;
}

// ================= SINKRONISASI TXT =================
void prosesSinkronisasiSD(const String& targetKelas) {
  Serial.println("\n🔄 Memulai sinkronisasi SD Card untuk kelas: " + targetKelas);

  if (!initSDCard()) {
    kirimStatusSync("error", "SD Card tidak dapat diakses");
    sdSyncAktif = false;
    return;
  }

  File root = SD.open("/");
  std::vector<String> daftarFile;

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String nama  = String(entry.name());
    bool   isDir = entry.isDirectory();
    entry.close();
    if (isDir) continue;

    if (isFormatMHS(nama) || isFormatDSN(nama)) {
      daftarFile.push_back(nama);
      Serial.println("📄 Ditemukan: " + nama);
    } else {
      Serial.println("⏭️  Dilewati (bukan format MHS/DSN txt): " + nama);
    }
  }
  root.close();

  int total    = daftarFile.size();
  int berhasil = 0;

  if (total == 0) {
    Serial.println("ℹ️ Tidak ada file .txt di SD Card.");
    kirimStatusSync("selesai", "Tidak ada file untuk dikirim. SD Card kosong.", 0, 0);
    sdSyncAktif = false;
    return;
  }

  Serial.printf("📦 Total file ditemukan: %d\n", total);

  for (int i = 0; i < total; i++) {
    String namaFile = daftarFile[i];
    Serial.printf("\n📤 [%d/%d] Mengirim: %s\n", i + 1, total, namaFile.c_str());

    String jsonData = bacaFileSdKeJson(namaFile, targetKelas);
    if (jsonData == "") {
      Serial.println("⚠️ File dilewati (format tidak valid): " + namaFile);
      sdFileSedangDikirim = namaFile;
      kirimStatusSync("error",
        "Gagal membaca file: " + namaFile + ". Lanjut atau batalkan?",
        total, berhasil);

      sdAckDiterima = false;
      sdAckBatalkan = false;
      unsigned long tunggu = millis();
      while (!sdAckDiterima && !sdAckBatalkan && millis() - tunggu < 30000) {
        mqttClient.loop();
        delay(100);
      }
      if (sdAckBatalkan || (!sdAckDiterima && !sdAckBatalkan)) {
        kirimStatusSync("dibatalkan", "Sinkronisasi dihentikan.", total, berhasil);
        sdSyncAktif = false;
        return;
      }
      continue;
    }

    sdFileSedangDikirim = namaFile;
    sdAckDiterima       = false;
    sdAckBatalkan       = false;
    mqttClient.setBufferSize(512);
    bool terkirim = mqttClient.publish(topic_audio_data, jsonData.c_str(), true);

    if (!terkirim) {
      Serial.println("❌ Gagal publish ke broker: " + namaFile);
      kirimStatusSync("error",
        "Gagal mengirim ke broker: " + namaFile + ". Ulangi atau batalkan?",
        total, berhasil);

      unsigned long tunggu = millis();
      while (!sdAckDiterima && !sdAckBatalkan && millis() - tunggu < 30000) {
        mqttClient.loop();
        delay(100);
      }
      if (sdAckBatalkan || (!sdAckDiterima && !sdAckBatalkan)) {
        kirimStatusSync("dibatalkan", "Sinkronisasi dihentikan.", total, berhasil);
        sdSyncAktif = false;
        return;
      }
      continue;
    }

    Serial.println("⏳ Menunggu konfirmasi server...");

    unsigned long tunggu = millis();
    while (!sdAckDiterima && !sdAckBatalkan && millis() - tunggu < 15000) {
      mqttClient.loop();
      delay(100);
    }

    if (sdAckBatalkan) {
      kirimStatusSync("dibatalkan", "Sinkronisasi dibatalkan oleh server.", total, berhasil);
      sdSyncAktif = false;
      return;
    }

    if (!sdAckDiterima) {
      Serial.println("⏱️ Timeout! Server tidak merespon: " + namaFile);
      kirimStatusSync("error",
        "Timeout untuk " + namaFile + ". Ulangi atau batalkan?",
        total, berhasil);

      unsigned long tunggu2 = millis();
      while (!sdAckDiterima && !sdAckBatalkan && millis() - tunggu2 < 30000) {
        mqttClient.loop();
        delay(100);
      }
      if (sdAckBatalkan || !sdAckDiterima) {
        kirimStatusSync("dibatalkan", "Sinkronisasi dihentikan karena timeout.", total, berhasil);
        sdSyncAktif = false;
        return;
      }
    }

    Serial.println("✅ Diterima server! Menghapus: " + namaFile);
    SD.remove("/" + namaFile);
    berhasil++;
    Serial.printf("🗑️ Progress: %d/%d\n", berhasil, total);
  }

  Serial.printf("\n🎉 Sinkronisasi selesai! %d/%d berhasil.\n", berhasil, total);
  kirimStatusSync("selesai", "Semua file berhasil dikirim.", total, berhasil);
  sdSyncAktif = false;
}

// ================= MQTT CALLBACK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("\n📩 [PESAN MASUK] Topik: ");
  Serial.println(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println("Isi: " + msg);

  if (String(topic) == topic_perintah) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (err) {
      Serial.println("⚠️ JSON tidak valid: " + String(err.c_str()));
      return;
    }

    String perintah = doc["perintah"] | "";

    if (perintah == "request_sync_wav") {
      String kelas = doc["target_kelas"] | "";
      Serial.println("📥 request_sync_wav → Kelas: " + kelas);
      if (!audioSyncAktif) {
        audioSyncAktif   = true;
        audioTargetKelas = kelas;
      } else {
        Serial.println("⚠️ Audio sync sudah berjalan.");
      }
    }
    else if (perintah == "request_sync_audio") {
      String kelas = doc["target_kelas"] | "";
      Serial.println("📥 request_sync_audio → Kelas: " + kelas);
      if (!sdSyncAktif) {
        sdSyncAktif   = true;
        sdTargetKelas = kelas;
      } else {
        Serial.println("⚠️ TXT sync sudah berjalan.");
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
      Serial.println("🛑 Server meminta pembatalan.");
      sdAckBatalkan = true;
    }
  }
}

// ================= MQTT RECONNECT =================
void reconnect() {
  while (!mqttClient.connected()) {
    Serial.print("Menghubungkan ke HiveMQ Cloud...");
    String clientId = "ESP32-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println(" ✅ Terhubung!");
      mqttClient.subscribe(topic_perintah);
      mqttClient.subscribe(topic_sync_status);
    } else {
      Serial.printf(" ❌ Gagal rc=%d. Retry 5 detik...\n", mqttClient.state());
      delay(5000);
    }
  }
}

// ================= KIRIM STATUS =================
void kirimStatus() {
  StaticJsonDocument<200> doc;
  doc["device_id"] = device_id;
  doc["status"]    = "online";
  doc["battery"]   = dummyBattery;
  doc["threshold"] = threshold_value;   // ← sertakan threshold di status
  char buf[256];
  serializeJson(doc, buf);
  mqttClient.publish(topic_status, buf);
  Serial.print("📤 Status: ");
  Serial.println(buf);
  dummyBattery--;
  if (dummyBattery <= 0) dummyBattery = 100;
}

// ================= KONEKSI WIFI BIASA =================
bool connectWifiBiasa(const char* ssid, const char* pass) {
  Serial.printf("🔄 Konek ke WiFi biasa (%s)...\n", ssid);
  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 20000) {
      Serial.printf("\n❌ Timeout! Status: %d\n", WiFi.status());
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Berhasil konek WiFi biasa!");
  return true;
}

// ================= KONEKSI EDUROAM =================
bool connectEduroam(const char* ssid, const char* nim, const char* pass) {
  Serial.println("\n========== DEBUG EDUROAM ==========");
  Serial.printf("SSID     : '%s'\n", ssid);
  Serial.printf("NIM      : '%s'\n", nim);
  Serial.println("====================================\n");

  if (strlen(ssid) == 0 || strlen(nim) == 0 || strlen(pass) == 0) {
    Serial.println("❌ SSID / NIM / Password kosong!");
    return false;
  }

  WiFi.disconnect(true);
  delay(1000);
  WiFi.mode(WIFI_STA);

  String identity = String(nim);
  if (identity.indexOf("@") == -1) identity += "@itb.ac.id";
  Serial.printf("Identity : '%s'\n", identity.c_str());

  esp_eap_client_set_identity((uint8_t*)identity.c_str(), identity.length());
  esp_eap_client_set_username((uint8_t*)nim, strlen(nim));
  esp_eap_client_set_password((uint8_t*)pass, strlen(pass));
  esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
  esp_wifi_sta_enterprise_enable();

  Serial.println("Mulai WiFi.begin()...");
  WiFi.begin(ssid);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 30000) {
      Serial.printf("\n❌ Timeout! Status WiFi: %d\n", WiFi.status());
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\n✅ Konek eduroam! IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// ================= PORTAL KONFIGURASI =================
void bukaPortal() {
  Serial.println("🌐 Membuka captive portal...");

  // ── Baca nilai tersimpan untuk pre-fill form ──
  preferences.begin("catch_note", true);  // read-only
  String saved_nim_str      = preferences.getString("nim",       "");
  String saved_eap_pass_str = preferences.getString("eap_pass",  "");
  String saved_ssid_str     = preferences.getString("ssid",      "");
  String saved_type_str     = preferences.getString("wifi_type", "biasa");
  String saved_wpass_str    = preferences.getString("wifi_pass", "");
  int    saved_threshold    = preferences.getInt   ("threshold", THRESHOLD_SEDANG);
  preferences.end();

  // Tentukan kategori threshold yang aktif untuk pre-select di form
  String saved_threshold_cat = "sedang";
  if (saved_threshold == THRESHOLD_TINGGI)       saved_threshold_cat = "tinggi";
  else if (saved_threshold == THRESHOLD_RENDAH)  saved_threshold_cat = "rendah";

  WiFi.mode(WIFI_AP);
  WiFi.softAP("CatchNote-Setup");
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("📡 Portal aktif. IP: %s\n", apIP.toString().c_str());

  dnsServer.start(DNS_PORT, "*", apIP);

  WebServer server(80);

  // ── Bangun HTML dengan nilai tersimpan ──
  // Untuk keamanan, password eduroam tidak di-pre-fill (hanya NIM & SSID)
  String chk_biasa    = (saved_type_str == "biasa")    ? "active" : "";
  String chk_eduroam  = (saved_type_str == "eduroam")  ? "active" : "";
  String sec_biasa    = (saved_type_str == "biasa")    ? "show"   : "";
  String sec_eduroam  = (saved_type_str == "eduroam")  ? "show"   : "";
  String initType     = saved_type_str;

  // Threshold radio pre-select
  String chkT = (saved_threshold_cat == "tinggi") ? "checked" : "";
  String chkS = (saved_threshold_cat == "sedang") ? "checked" : "";
  String chkR = (saved_threshold_cat == "rendah") ? "checked" : "";

  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>CatchNote Setup</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=DM+Sans:wght@400;500;600&family=DM+Serif+Display&display=swap');

    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

    :root {
      --pink:     #e8638c;
      --pink-lt:  #fde8ef;
      --pink-md:  #f5b8cc;
      --text:     #2d1a22;
      --muted:    #9c7585;
      --bg:       #fdf4f7;
      --card:     #ffffff;
      --border:   #f0d0dc;
      --radius:   14px;
    }

    body {
      font-family: 'DM Sans', sans-serif;
      background: var(--bg);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 24px 16px;
      background-image: radial-gradient(circle at 20% 20%, #fce4ed 0%, transparent 50%),
                        radial-gradient(circle at 80% 80%, #fde8ef 0%, transparent 50%);
    }

    .card {
      background: var(--card);
      border-radius: 24px;
      padding: 36px 32px;
      width: 100%;
      max-width: 440px;
      box-shadow: 0 4px 40px rgba(232,99,140,0.10);
      border: 1px solid var(--border);
    }

    .logo { text-align: center; margin-bottom: 28px; }
    .logo-icon {
      width: 56px; height: 56px;
      background: linear-gradient(135deg, #e8638c, #f598b4);
      border-radius: 16px;
      display: inline-flex; align-items: center; justify-content: center;
      font-size: 26px;
      margin-bottom: 12px;
      box-shadow: 0 6px 20px rgba(232,99,140,0.30);
    }
    .logo h1 { font-family: 'DM Serif Display', serif; font-size: 22px; color: var(--text); }
    .logo p  { font-size: 13px; color: var(--muted); margin-top: 4px; }

    /* ── Section divider ── */
    .section-title {
      font-size: 11px; font-weight: 600; color: var(--muted);
      text-transform: uppercase; letter-spacing: 1px;
      margin: 24px 0 14px;
      display: flex; align-items: center; gap: 8px;
    }
    .section-title::after {
      content: ''; flex: 1; height: 1px; background: var(--border);
    }

    .toggle-wrap {
      display: flex; background: var(--pink-lt); border-radius: 12px;
      padding: 4px; margin-bottom: 20px; gap: 4px;
    }
    .toggle-btn {
      flex: 1; padding: 10px; border: none; border-radius: 9px;
      font-family: 'DM Sans', sans-serif; font-size: 14px; font-weight: 500;
      cursor: pointer; transition: all 0.2s; background: transparent; color: var(--muted);
    }
    .toggle-btn.active { background: white; color: var(--pink); box-shadow: 0 2px 8px rgba(232,99,140,0.15); }

    .section { display: none; }
    .section.show { display: block; }

    .field { margin-bottom: 16px; }
    .field label {
      display: block; font-size: 12px; font-weight: 600; color: var(--muted);
      text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 6px;
    }
    .field input[type=text],
    .field input[type=password] {
      width: 100%; padding: 12px 14px; border: 1.5px solid var(--border);
      border-radius: var(--radius); font-family: 'DM Sans', sans-serif;
      font-size: 15px; color: var(--text); background: #fff;
      transition: border-color 0.2s, box-shadow 0.2s; outline: none;
    }
    .field input[type=text]:focus,
    .field input[type=password]:focus {
      border-color: var(--pink); box-shadow: 0 0 0 3px rgba(232,99,140,0.10);
    }

    /* ── Threshold cards ── */
    .threshold-grid {
      display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px;
      margin-bottom: 8px;
    }
    .threshold-card {
      position: relative; cursor: pointer;
    }
    .threshold-card input[type=radio] {
      position: absolute; opacity: 0; width: 0; height: 0;
    }
    .threshold-label {
      display: flex; flex-direction: column; align-items: center;
      padding: 14px 8px 12px;
      border: 2px solid var(--border); border-radius: 14px;
      background: #fff; transition: all 0.18s; user-select: none;
      cursor: pointer;
    }
    .threshold-card input[type=radio]:checked + .threshold-label {
      border-color: var(--pink);
      background: var(--pink-lt);
      box-shadow: 0 0 0 3px rgba(232,99,140,0.12);
    }
    .threshold-label .th-icon  { font-size: 22px; margin-bottom: 6px; }
    .threshold-label .th-name  { font-size: 13px; font-weight: 600; color: var(--text); }
    .threshold-label .th-value {
      font-size: 11px; color: var(--muted); margin-top: 3px;
      background: var(--border); border-radius: 20px; padding: 2px 8px;
    }
    .threshold-card input[type=radio]:checked + .threshold-label .th-value {
      background: var(--pink-md); color: white;
    }

    .hint {
      background: var(--pink-lt); border-radius: 10px; padding: 10px 14px;
      font-size: 12.5px; color: var(--pink); margin-bottom: 16px; line-height: 1.5;
    }

    /* ── Saved badge ── */
    .saved-badge {
      display: inline-flex; align-items: center; gap: 5px;
      font-size: 11.5px; color: var(--pink); background: var(--pink-lt);
      border-radius: 20px; padding: 3px 10px; margin-bottom: 10px;
    }

    button[type=submit] {
      width: 100%; padding: 14px;
      background: linear-gradient(135deg, #e8638c, #f598b4);
      color: white; border: none; border-radius: var(--radius);
      font-family: 'DM Sans', sans-serif; font-size: 16px; font-weight: 600;
      cursor: pointer; margin-top: 12px; box-shadow: 0 4px 16px rgba(232,99,140,0.35);
      transition: opacity 0.2s, transform 0.1s;
    }
    button[type=submit]:hover  { opacity: 0.92; }
    button[type=submit]:active { transform: scale(0.98); }
  </style>
</head>
<body>
<div class="card">
  <div class="logo">
    <div class="logo-icon">📋</div>
    <h1>CatchNote</h1>
    <p>Konfigurasi perangkat ESP32</p>
  </div>

  <form method="POST" action="/save">
    <input type="hidden" name="wifi_type" id="wifi_type_val" value=")rawhtml" + initType + R"rawhtml(">

    <!-- ══ BAGIAN WIFI ══ -->
    <div class="section-title">📶 Koneksi WiFi</div>

    <div class="toggle-wrap">
      <button type="button" class="toggle-btn )rawhtml" + chk_biasa + R"rawhtml(" onclick="setTipe('biasa', this)">📶 WiFi Biasa</button>
      <button type="button" class="toggle-btn )rawhtml" + chk_eduroam + R"rawhtml(" onclick="setTipe('eduroam', this)">🏫 Eduroam ITB</button>
    </div>

    <div class="section )rawhtml" + sec_biasa + R"rawhtml(" id="sec-biasa">
      <div class="field">
        <label>Nama WiFi (SSID)</label>
        <input type="text" name="ssid_biasa" placeholder="Nama jaringan WiFi" value=")rawhtml";

  // inject saved SSID untuk WiFi biasa (hanya jika tipe sebelumnya biasa)
  if (saved_type_str == "biasa") html += saved_ssid_str;

  html += R"rawhtml(">
      </div>
      <div class="field">
        <label>Password WiFi</label>
        <input type="password" name="wifi_pass" placeholder="Password jaringan">
      </div>
    </div>

    <div class="section )rawhtml" + sec_eduroam + R"rawhtml(" id="sec-eduroam">
      <div class="hint">Gunakan akun SSO ITB Anda.</div>)rawhtml";

  // Tampilkan badge "tersimpan" jika ada NIM sebelumnya
  if (saved_nim_str.length() > 0) {
    html += "<div class='saved-badge'>✅ Akun tersimpan: " + saved_nim_str + "</div>";
  }

  html += R"rawhtml(
      <div class="field">
        <label>Nama WiFi (SSID)</label>
        <input type="text" name="ssid_eduroam" placeholder="eduroam" value=")rawhtml";

  if (saved_type_str == "eduroam") html += saved_ssid_str;

  html += R"rawhtml(">
      </div>
      <div class="field">
        <label>NIM ITB</label>
        <input type="text" name="nim" placeholder="13xxxxxxx" value=")rawhtml" + saved_nim_str + R"rawhtml(">
      </div>
      <div class="field">
        <label>Password SSO)rawhtml";

  // Tampilkan hint password sudah tersimpan jika ada
  if (saved_eap_pass_str.length() > 0) {
    html += R"rawhtml( <span style="font-weight:400;color:#c47090;font-size:11px;">(kosongkan jika tidak berubah)</span>)rawhtml";
  }

  html += R"rawhtml(</label>
        <input type="password" name="eap_pass" placeholder=")rawhtml";
  html += (saved_eap_pass_str.length() > 0) ? "••••••••" : "Password SSO ITB";
  html += R"rawhtml(">
      </div>
    </div>

    <!-- ══ BAGIAN THRESHOLD ══ -->
    <div class="section-title"> Sensitivitas Deteksi</div>
    <p style="font-size:13px;color:var(--muted);margin-bottom:14px;line-height:1.5;">
      Pilih level sensitivitas untuk deteksi suara. Nilai lebih tinggi = lebih sensitif.
    </p>

    <div class="threshold-grid">
      <!-- Tinggi -->
      <label class="threshold-card">
        <input type="radio" name="threshold_cat" value="tinggi" )rawhtml" + chkT + R"rawhtml(>
        <span class="threshold-label">
          <span class="th-icon">🔴</span>
          <span class="th-name">Tinggi</span>
          <span class="th-value">90</span>
        </span>
      </label>
      <!-- Sedang -->
      <label class="threshold-card">
        <input type="radio" name="threshold_cat" value="sedang" )rawhtml" + chkS + R"rawhtml(>
        <span class="threshold-label">
          <span class="th-icon">🟡</span>
          <span class="th-name">Sedang</span>
          <span class="th-value">70</span>
        </span>
      </label>
      <!-- Rendah -->
      <label class="threshold-card">
        <input type="radio" name="threshold_cat" value="rendah" )rawhtml" + chkR + R"rawhtml(>
        <span class="threshold-label">
          <span class="th-icon">🟢</span>
          <span class="th-name">Rendah</span>
          <span class="th-value">50</span>
        </span>
      </label>
    </div>

    <button type="submit">Simpan &amp; Restart →</button>
  </form>
</div>

<script>
  function setTipe(tipe, btn) {
    document.getElementById('wifi_type_val').value = tipe;
    document.querySelectorAll('.toggle-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    document.querySelectorAll('.section').forEach(s => s.classList.remove('show'));
    document.getElementById('sec-' + tipe).classList.add('show');
  }
</script>
</body>
</html>
)rawhtml";

  // ── Handler utama ──
  server.on("/", HTTP_GET, [&]() {
    server.send(200, "text/html", html);
  });

  // ── Captive Portal Detection ──
  server.on("/generate_204", HTTP_GET, [&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/fwlink", HTTP_GET, [&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/hotspot-detect.html", HTTP_GET, [&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/library/test/success.html", HTTP_GET, [&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/ncsi.txt", HTTP_GET, [&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/connecttest.txt", HTTP_GET, [&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/redirect", HTTP_GET, [&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.onNotFound([&]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  // ── Handler simpan konfigurasi ──
  server.on("/save", HTTP_POST, [&]() {
    String wifiType     = server.arg("wifi_type");
    String nim          = server.arg("nim");
    String eapPassNew   = server.arg("eap_pass");
    String wifiPass     = server.arg("wifi_pass");
    String thresholdCat = server.arg("threshold_cat");

    String ssid = (wifiType == "eduroam")
                  ? server.arg("ssid_eduroam")
                  : server.arg("ssid_biasa");

    // Hitung nilai threshold dari kategori
    int thresholdVal = THRESHOLD_SEDANG;
    if (thresholdCat == "tinggi")      thresholdVal = THRESHOLD_TINGGI;
    else if (thresholdCat == "rendah") thresholdVal = THRESHOLD_RENDAH;

    preferences.begin("catch_note", false);
    preferences.putString("ssid",      ssid);
    preferences.putString("wifi_type", wifiType);
    preferences.putString("wifi_pass", wifiPass);
    preferences.putString("nim",       nim);
    preferences.putInt   ("threshold", thresholdVal);

    // Password eduroam: hanya diperbarui jika user mengisi field baru
    // (bukan placeholder "••••••••"), jika kosong gunakan yang lama
    if (eapPassNew.length() > 0) {
      preferences.putString("eap_pass", eapPassNew);
      Serial.println("🔐 Password eduroam diperbarui.");
    } else {
      Serial.println("🔐 Password eduroam tidak berubah (kosong = pakai lama).");
    }

    preferences.end();

    Serial.printf("💾 Tersimpan → SSID: %s | Tipe: %s | Threshold: %d (%s)\n",
      ssid.c_str(), wifiType.c_str(), thresholdVal, thresholdCat.c_str());

    server.send(200, "text/html",
      "<!DOCTYPE html><html><head>"
      "<meta charset='UTF-8'>"
      "<style>"
      "body{font-family:'DM Sans',sans-serif;background:#fdf4f7;"
      "display:flex;align-items:center;justify-content:center;min-height:100vh;}"
      ".box{background:white;border-radius:24px;padding:40px 32px;text-align:center;"
      "box-shadow:0 4px 40px rgba(232,99,140,.1);max-width:360px;width:100%;}"
      ".icon{font-size:48px;margin-bottom:16px;}"
      "h2{color:#e8638c;margin-bottom:8px;}"
      "p{color:#9c7585;font-size:14px;}"
      "</style></head><body>"
      "<div class='box'><div class='icon'>✅</div>"
      "<h2>Konfigurasi tersimpan!</h2>"
      "<p>ESP32 akan restart dalam beberapa detik...</p>"
      "</div></body></html>"
    );

    delay(2000);
    ESP.restart();
  });

  server.begin();
  Serial.println("✅ Web server aktif, menunggu koneksi...");

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(RESET_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RESET_BUTTON), onBootButtonISR, CHANGE);

  xTaskCreatePinnedToCore(
    resetTask, "ResetTask", 2048, NULL, 5, &resetTaskHandle, 0
  );
  Serial.println("✅ Reset task aktif (Core 0, Prioritas 5)");

  preferences.begin("catch_note", false);
  preferences.getString("nim",       "").toCharArray(eap_nim,    40);
  preferences.getString("eap_pass",  "").toCharArray(eap_pass,   40);
  preferences.getString("ssid",      "").toCharArray(saved_ssid, 40);
  preferences.getString("wifi_type", "biasa").toCharArray(wifi_type, 10);
  preferences.getString("wifi_pass", "").toCharArray(wifi_pass,  64);
  threshold_value = preferences.getInt("threshold", THRESHOLD_SEDANG);  // ← load threshold
  preferences.end();

  Serial.printf("📊 Threshold aktif: %d\n", threshold_value);

  bool isConnected = false;

  if (strlen(saved_ssid) > 0) {
    if (strcmp(wifi_type, "eduroam") == 0) {
      isConnected = connectEduroam(saved_ssid, eap_nim, eap_pass);
    } else {
      isConnected = connectWifiBiasa(saved_ssid, wifi_pass);
    }
  }

  if (!isConnected) bukaPortal();

  Serial.printf("\n✅ Jaringan siap! IP: %s\n", WiFi.localIP().toString().c_str());

  espClient.setInsecure();

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);

  Wire.begin(SDA_PIN, SCL_PIN);
  nfc.begin();
  if (!nfc.getFirmwareVersion()) {
    Serial.println("❌ PN532 tidak ditemukan! Cek kabel.");
    while (1);
  }
  nfc.SAMConfig();
  Serial.println("✅ Sistem siap! Menunggu kartu...\n");
}

// ================= LOOP =================
void loop() {
  if (!mqttClient.connected()) reconnect();
  mqttClient.loop();

  if (sdSyncAktif && sdTargetKelas != "") {
    String kelas = sdTargetKelas;
    sdTargetKelas = "";
    prosesSinkronisasiSD(kelas);
  }

  if (audioSyncAktif && audioTargetKelas != "") {
    String kelas = audioTargetKelas;
    audioTargetKelas = "";
    prosesSinkronisasiAudio(kelas);
  }

  uint8_t uid[7], uidLength;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
    String uidStr = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] < 0x10) uidStr += "0";
      uidStr += String(uid[i], HEX);
      if (i < uidLength - 1) uidStr += "";
    }
    uidStr.toUpperCase();
    Serial.println("\n=== KARTU TERDETEKSI: " + uidStr + " ===");

    StaticJsonDocument<200> doc;
    doc["device_id"] = device_id;
    doc["action"]    = "tap_rfid";
    doc["uid"]       = uidStr;
    doc["battery"]   = dummyBattery;
    doc["threshold"] = threshold_value;  // ← sertakan threshold di data RFID
    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(topic_rfid, buf);
    Serial.println("📤 " + String(buf));
    delay(2000);
  }

  if (millis() - lastStatusTime >= statusInterval) {
    lastStatusTime = millis();
    kirimStatus();
  }
}
