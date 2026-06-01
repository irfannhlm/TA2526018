// Main.ino

#include "Config.h"
#include "PN532_Module.h"
#include "AudioSD_Module.h"
#include "MPU6050_Module.h"
#include "Communication.h"
#include "Wifi_Management.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include "nvs_flash.h"
#include "Adafruit_MAX1704X.h"
#include <Preferences.h>
#include <SD.h>
#include "IdentTiming.h"
#include "I2C_Handler.h"
#include "Buzzer_Module.h"
#include "LCD_Helper.h"

#define MAX_PESERTA        100
#define ERROR_DISPLAY_TIME 2000

#include <sys/time.h>

uint64_t t1EpochMs = 0;
uint64_t t2EpochMs = 0;

uint64_t getEpochMs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return ((uint64_t)tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000);
}

String getTimestampMs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);

  struct tm timeinfo;
  localtime_r(&tv.tv_sec, &timeinfo);

  char tanggalJam[30];
  strftime(tanggalJam, sizeof(tanggalJam), "%d-%m-%Y %H:%M:%S", &timeinfo);

  char finalTime[40];
  snprintf(finalTime, sizeof(finalTime), "%s.%03ld", tanggalJam, tv.tv_usec / 1000);

  return String(finalTime);
}

LiquidCrystal_I2C lcd(0x27, 16, 2);

unsigned long maxRecordMs = 300000; // Default 5 menit
int active_threshold = 500; // Default threshold
int jumlahTerdaftar = 0; // Penghitung jumlah peserta terdaftar

Adafruit_MAX17048 maxlipo;

float batteryVoltage = 0.0;
float batteryPercent = 0.0;

volatile int buttonClicks = 0;
unsigned long lastClickTime = 0;

unsigned long lastBatteryCheck = 0;
const unsigned long BATTERY_CHECK_INTERVAL = 5000; // cek tiap 5 detik

String daftarSudahBicara[MAX_PESERTA];
String listTerdaftar[MAX_PESERTA];
String listBanned[MAX_PESERTA];
int    jumlahPeserta = 0;

enum OperatingMode { MODE_KOMUNIKASI, MODE_DOSEN, MODE_MAHASISWA };
volatile OperatingMode currentMode = MODE_KOMUNIKASI;

enum SystemState { STANDBY, INVALID, UNREGISTERED, BANNED, SD_ERROR, AUTHORIZED, TIMEOUT, RECORDING, NO_QUESTION };
SystemState state     = STANDBY;
SystemState lastState = (SystemState)-1;

const int LONG_PRESS_TIME = 2000;

volatile bool          buttonFlag        = false;
volatile unsigned long lastTriggerTime   = 0;

bool lastMqttStatus = false;

unsigned long stateTimer    = 0;
String        currentUID    = "";
unsigned long waktuMulai    = 0;
unsigned long waktuRespon   = 0;
int           lastCountdown = -1;
unsigned long lastRegTime   = 0;
static String lastWifiLine = "";

// ── Heartbeat: kirim status setiap 10 detik di MODE_KOMUNIKASI ──
static unsigned long lastHeartbeatTime = 0;
const  unsigned long HEARTBEAT_MS      = 10000;

void IRAM_ATTR handleButtonInterrupt() {
  buttonFlag = true;
}

void clearButtonState() {
  buttonFlag = false;
  buttonClicks = 0;
  lastClickTime = 0;
  lastTriggerTime = millis();
}

bool cekHistory(String uid) {
  for (int i = 0; i < jumlahPeserta; i++)
    if (daftarSudahBicara[i] == uid) return true;
  return false;
}

void simpanHistory(String uid) {
  if (jumlahPeserta < MAX_PESERTA)
    daftarSudahBicara[jumlahPeserta++] = uid;
}

// ════════════════════════════════════════════════
//  checkModeButton
// ════════════════════════════════════════════════
void checkModeButton() {
  // 1. Tangkap flag dari interrupt (Pasti terjadi saat tombol mulai ditekan)
  if (buttonFlag) {
    buttonFlag = false;
    // CUKUP pakai jeda waktu (250ms), hilangkan pengecekan posisi jari.
    // Ini memastikan klik yang cepat di mode MHS (loop lambat) tetap terekam!
    if (millis() - lastTriggerTime > 250) { 
      lastTriggerTime = millis();
      buttonClicks++;
      lastClickTime = millis();
    }
  }

  // 2. Deteksi Hold untuk Deep Sleep (NON-BLOCKING)
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (buttonClicks > 0 && (millis() - lastTriggerTime > LONG_PRESS_TIME)) {
      buttonClicks = 0; 
      prepareDeepSleep();
    }
  }

  // 3. Evaluasi aksi setelah jeda 350ms dari klik terakhir
  if (buttonClicks > 0 && digitalRead(BUTTON_PIN) == HIGH && (millis() - lastClickTime > 350)) {
    if (buttonClicks >= 3) {
      masukModeDebug(); // Triple click
    } 
    // Terima 1 atau 2 klik sebagai aksi ganti mode (anti-bounce aman)
    else if (buttonClicks == 1 || buttonClicks == 2) {
      gantiMode();      
    }
    buttonClicks = 0; // Reset counter
  }
}

void gantiMode() {
  OperatingMode oldMode = currentMode;
  OperatingMode nextMode;

  if      (currentMode == MODE_KOMUNIKASI) nextMode = MODE_DOSEN;
  else if (currentMode == MODE_DOSEN)      nextMode = MODE_MAHASISWA;
  else                                     nextMode = MODE_KOMUNIKASI;

  if (oldMode == MODE_KOMUNIKASI && nextMode != MODE_KOMUNIKASI) {
    stopMQTT();
  }

  currentMode     = nextMode;
  state           = STANDBY;
  lastState       = (SystemState)-1;
  lastCountdown   = -1;
  lastWifiLine    = "";
  lastMqttStatus  = mqttClient.connected();
  lastWifiAttempt = millis();
  wifiDibatalkan  = false;
  waktuMulai      = millis();

  if (currentMode != MODE_KOMUNIKASI) {
    updateLCD();
  }

  // Bunyi mode langsung, sebelum proses berat
  playBuzzer((uint8_t)currentMode + 1, 40, 40);
  waitBuzzerDone();

  if (currentMode == MODE_DOSEN) {
    stopWiFi();
    delay(200);
    initAudioSD();
  }
  else if (currentMode == MODE_KOMUNIKASI) {
    deinitAudio();
    startWiFi();
    lastRegTime       = 0;
    lastHeartbeatTime = 0;
  }
  else {
    stopWiFi();
    delay(200); 
    initAudioSD();
    muatDaftarEligible();
    muatDaftarBanned();
  }
}

void masukModeDebug() {
  lcd.clear();
  lcdPrint16(0, "   MODE SETUP  ");
  lcdPrint16(1, "  WIFI CONFIG  ");
  
  playBuzzer(2, 100, 100);
  waitBuzzerDone();
  
  stopWiFi();
  delay(1500);

  // Hapus SSID saja agar alat dipaksa masuk AP Mode jika di-restart, 
  // tapi fungsi bukaPortal() tetap bisa pre-fill dari data terakhir (Nim/Tipe).
  Preferences prefs;
  prefs.begin("catch_note", false);
  prefs.remove("ssid");
  prefs.end();

  lcd.clear();
  lcdPrint16(0, "WIFI: CatchNote");
  lcdPrint16(1, "IP: 192.168.4.1");

  bukaPortal(); 
}

//  prepareDeepSleep
void prepareDeepSleep() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  MEMATIKAN...  ");
  delay(1000);
  stopWiFi();
  // lcd.noBacklight();
  // lcd.noDisplay();
  playBuzzer(1, 500, 80);
  waitBuzzerDone();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  // powerDownPN532();
  Serial.println("Entering Deep Sleep...");
  esp_deep_sleep_start();
}

void muatDaftarEligible() {
  jumlahTerdaftar = 0; // Reset isi array
  
  if (!SD.exists("/eligible.txt")) {
    Serial.println("File /eligible.txt tidak ditemukan. Daftar kelas kosong!");
    return;
  }

  File file = SD.open("/eligible.txt", FILE_READ);
  if (!file) {
    Serial.println("Gagal membaca /eligible.txt");
    return;
  }

  while (file.available() && jumlahTerdaftar < MAX_PESERTA) {
    String line = file.readStringUntil('\n');
    line.trim(); 
    
    if (line.length() > 0) {
      listTerdaftar[jumlahTerdaftar] = line;
      jumlahTerdaftar++;
    }
  }
  file.close();
  
  Serial.println("====================================");
  Serial.printf("Berhasil memuat %d UID ke memori Kelas\n", jumlahTerdaftar);
  Serial.println("====================================");
}

// Fungsi Pengecekan UID di daftar kelas
bool cekTerdaftar(String uid) {
  if (jumlahTerdaftar == 0) return false; // Tolak semua jika daftar kosong
  for (int i = 0; i < jumlahTerdaftar; i++) {
    if (listTerdaftar[i] == uid) return true; // Ditemukan!
  }
  return false;
}

int jumlahBanned = 0;

void muatDaftarBanned() {
  jumlahBanned = 0; // Reset isi array
  
  if (!SD.exists("/banned.txt")) {
    Serial.println("ℹ️ File /banned.txt kosong/tidak ada. Semua mahasiswa diizinkan.");
    return;
  }

  File file = SD.open("/banned.txt", FILE_READ);
  if (!file) {
    Serial.println("Gagal membaca /banned.txt");
    return;
  }

  while (file.available() && jumlahBanned < MAX_PESERTA) {
    String line = file.readStringUntil('\n');
    line.trim(); 
    
    if (line.length() > 0) {
      listBanned[jumlahBanned] = line;
      jumlahBanned++;
    }
  }
  file.close();

  Serial.printf("Berhasil memuat %d UID ke memori BANNED\n", jumlahBanned);

}

bool cekBanned(String uid) {
  if (jumlahBanned == 0) return false; 
  for (int i = 0; i < jumlahBanned; i++) {
    if (listBanned[i] == uid) return true; // Ditemukan di daftar blacklist!
  }
  return false;
}

void updateLCD() {
  unsigned long durasi = millis() - waktuMulai;
  
  int countdownSekarang = DOSEN_COUNTDOWN - (durasi / 1000);
  if (countdownSekarang < 0) countdownSekarang = 0;

  int timeoutSekarang = 0;
  if (durasi < TIMEOUT_BICARA) {
    timeoutSekarang = (TIMEOUT_BICARA - durasi + 999) / 1000;
  }

  static int lastTimeout = -1;
  static int lastAnimFrame = -1;

  int animFrame = (millis() / 500) % 4;

  if (currentMode == MODE_KOMUNIKASI &&
    state == STANDBY &&
    String(saved_ssid).length() < 2) {

  lcdPrint16(0, " MODE: OFFLINE ");
  lcdPrint16(1, "WIFI BELUM DISET");
  return;
  }

  // ── WIFI RETRY / BELUM CONNECT ──
  if (currentMode == MODE_KOMUNIKASI &&
      state == STANDBY &&
      WiFi.status() != WL_CONNECTED &&
      !wifiDibatalkan) {
    
    int sisa = (5000 - (int)(millis() - lastWifiAttempt)) / 1000;
    if (sisa < 0) sisa = 0;

    String wifiLine = "COBA LAGI " + twoDigit(sisa) + "s" + animDots();

    // Update hanya saat teks berubah, tanpa clear terus-menerus
    if (wifiLine != lastWifiLine || state != lastState) {
      lcdPrint16(0, "  WIFI GAGAL   ");
      lcdPrint16(1, wifiLine);
      lastWifiLine = wifiLine;
      lastState = state;
    }

    return;
  }

  // ── MQTT / SERVER CONNECTING, WIFI SUDAH CONNECT ──
  if (currentMode == MODE_KOMUNIKASI &&
      state == STANDBY &&
      WiFi.status() == WL_CONNECTED &&
      !wifiDibatalkan &&
      !mqttClient.connected()) {

    if (animFrame != lastAnimFrame ||
        state != lastState ||
        lastMqttStatus != mqttClient.connected()) {

      lcdPrint16(0, "  KONEK SERVER  ");
      lcdPrint16(1, " MOHON TUNGGU" + animDots());

      lastAnimFrame = animFrame;
      lastMqttStatus = mqttClient.connected();
      lastState = state;
    }

    return;
  }

  // ── EARLY-RETURN GATE ──
  bool mqttChanged = (currentMode == MODE_KOMUNIKASI) &&
                     !wifiDibatalkan &&
                     (mqttClient.connected() != lastMqttStatus);

  bool isCountdownDosen = (state == STANDBY &&
                           currentMode == MODE_DOSEN &&
                           countdownSekarang > 0);

  bool isCountdownAuth = (state == AUTHORIZED);

  if (state == lastState) {
    if (isCountdownDosen && countdownSekarang == lastCountdown) return;
    if (isCountdownAuth && timeoutSekarang == lastTimeout) return;
    if (!isCountdownDosen && !isCountdownAuth && !mqttChanged) return;
  }

  if (currentMode == MODE_KOMUNIKASI) {
    lastMqttStatus = mqttClient.connected();
  }

  lastWifiLine = "";
  lastAnimFrame = animFrame;

  lcd.clear();

  lastCountdown = countdownSekarang;
  lastTimeout = timeoutSekarang;

  switch (state) {

    case STANDBY:
      if (currentMode == MODE_MAHASISWA) {
        lcdPrint16(0, "MODE: MAHASISWA");
        lcdPrint16(1, "SCAN KARTU ANDA ");
      }

      else if (currentMode == MODE_DOSEN) {
        lcdPrint16(0, "  MODE: DOSEN  ");
        lcdPrint16(1, "TUNGGU " + twoDigit(countdownSekarang) + " DETIK");
      }

      else { // MODE_KOMUNIKASI, WiFi dan MQTT sudah siap
        if (wifiDibatalkan) {
          lcdPrint16(0, "  DIBATALKAN   ");
          lcdPrint16(1, " MOHON TUNGGU" + animDots());
        } else {
          lcdPrint16(0, "  MODE: ONLINE  ");
          lcdPrint16(1, "   TAP KARTU  ");
        }
      }
      break;

    case AUTHORIZED:
      lcdPrint16(0, "  MULAI BICARA ");
      lcdPrint16(1, "BATAL DALAM " + twoDigit(timeoutSekarang) + "s");
      break;

    case RECORDING:
      lcdPrint16(0, " SEDANG MEREKAM");
      lcdPrint16(1, "    ------      ");
      break;

    case INVALID:
      lcdPrint16(0, "    DITOLAK    ");
      lcdPrint16(1, "  SUDAH BICARA ");
      break;

    case UNREGISTERED:
      lcdPrint16(0, "    DITOLAK    ");
      lcdPrint16(1, "TIDAK TERDAFTAR");
      break;

    case TIMEOUT:
      lcdPrint16(0, "  WAKTU HABIS  ");
      lcdPrint16(1, " GILIRAN BATAL ");
      break;

    case BANNED:
      lcdPrint16(0, "    DITOLAK    ");
      lcdPrint16(1, "  KUOTA HABIS  ");
      break;

    case SD_ERROR:
      lcdPrint16(0, "  SISTEM ERROR  ");
      lcdPrint16(1, " SD CARD GAGAL ");
      break;

    case NO_QUESTION:
      lcdPrint16(0, "BELUM ADA SOAL ");
      lcdPrint16(1, "DOSEN REKAM DULU");
      break;
  }

  lastState = state;
}


// ==========================================
// 1. VARIABEL GLOBAL & KONFIGURASI BATERAI
// ==========================================

// Variabel untuk filter dan memori persentase
float smoothedVoltage = 0.0;
float lastReportedPercent = 100.0; 

// (Asumsi variabel ini sudah ada di kodemu sebelumnya)
// float batteryVoltage;
// float batteryPercent;

// ==========================================
// 2. LOOK-UP TABLE (LUT) BATERAI
// ==========================================
struct BatteryProfile {
  float voltage;
  float percentage;
};

const int NUM_LUT_POINTS = 11;
const BatteryProfile lipo_LUT[NUM_LUT_POINTS] = {
  {4.20, 100.0},
  {4.06, 90.0},
  {3.98, 80.0},
  {3.92, 70.0},
  {3.87, 60.0},
  {3.82, 50.0},
  {3.79, 40.0},
  {3.77, 30.0},
  {3.74, 20.0},
  {3.68, 10.0},
  {3.20, 0.0}   // Batas bawah / cutoff baterai
};

// ==========================================
// 3. FUNGSI INTERPOLASI
// ==========================================
float getBatteryPercentageFromLUT(float v) {
  if (v >= lipo_LUT[0].voltage) return 100.0;
  if (v <= lipo_LUT[NUM_LUT_POINTS - 1].voltage) return 0.0;

  for (int i = 0; i < NUM_LUT_POINTS - 1; i++) {
    if (v <= lipo_LUT[i].voltage && v >= lipo_LUT[i+1].voltage) {
      float v1 = lipo_LUT[i].voltage;
      float p1 = lipo_LUT[i].percentage;
      float v2 = lipo_LUT[i+1].voltage;
      float p2 = lipo_LUT[i+1].percentage;

      return p1 + ((v - v1) * (p2 - p1) / (v2 - v1));
    }
  }
  return 0.0; 
}

// ==========================================
// 4. FUNGSI UTAMA PENGECEKAN BATERAI
// ==========================================
void checkBattery() {

  // --- A. PEMBACAAN DAN PENGOLAHAN SENSOR (Tiap 5 Detik) ---
  if (millis() - lastBatteryCheck >= BATTERY_CHECK_INTERVAL) {
    lastBatteryCheck = millis();

    bool batteryReadOK = false;

    // Baca MAX17048 dengan proteksi mutex I2C
    if (lockI2C(20)) {
      batteryVoltage = maxlipo.cellVoltage();
      unlockI2C();
      batteryReadOK = true;
    }

    // Kalau I2C sedang sibuk, skip pembacaan baterai siklus ini.
    // LED tetap lanjut pakai nilai terakhir.
    if (batteryReadOK) {
      if (isnan(batteryVoltage)) {
        // Jika tidak ada baterai / USB power
        batteryPercent = NAN;
        smoothedVoltage = 0.0;
        lastReportedPercent = 100.0;
      } 
      else {
        // 1. Low-pass filter / EMA pada tegangan
        if (smoothedVoltage == 0.0) {
          smoothedVoltage = batteryVoltage;
        } else {
          smoothedVoltage = (0.1 * batteryVoltage) + (0.9 * smoothedVoltage);
        }

        // 2. Hitung persentase dari LUT
        float rawPercent = getBatteryPercentageFromLUT(smoothedVoltage);

        // 3. Monotonic logic agar persentase tidak naik-turun sendiri
        if (rawPercent < lastReportedPercent) {
          lastReportedPercent = rawPercent;
        } 
        else if (rawPercent - lastReportedPercent > 10.0) {
          // Lonjakan besar dianggap indikasi charging / dicolok
          lastReportedPercent = rawPercent;
        }

        batteryPercent = lastReportedPercent;
      }

      // --- Cetak status hanya kalau pembacaan berhasil ---
      Serial.println("=== STATUS BATERAI ===");
      Serial.printf("Raw Voltage : %.2f V\n", batteryVoltage);
      Serial.printf("Smooth Volt : %.2f V\n", smoothedVoltage);

      if (isnan(batteryPercent)) {
        Serial.println("Battery : USB Power / No Battery");
      } else {
        Serial.printf("Battery : %.1f %%\n", batteryPercent);
      }

      Serial.println("======================");
    }
  }

  // --- B. UPDATE LED BATERAI (Jalan terus di loop) ---
  if (!isnan(batteryPercent) && batteryPercent < 20.0) {
    // Mode baterai lemah: kedip tiap 500 ms
    static unsigned long lastBlinkTime = 0;
    static bool ledState = false;

    if (millis() - lastBlinkTime >= 500) {
      lastBlinkTime = millis();
      ledState = !ledState;
      digitalWrite(LED_BATTERY, ledState ? HIGH : LOW);
    }
  } 
  else {
    // Mode normal / USB: LED nyala terus
    digitalWrite(LED_BATTERY, HIGH);
  }

  // --- C. UPDATE LED WIFI ---
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_WIFI, HIGH);
  } else {
    digitalWrite(LED_WIFI, LOW);
  }
}

void mulaiAuthorizedWindow(const String &uid) {
  currentUID = uid;

  resetVadState();
  clearPreRecordBuffer();
  resetAudioFilters();

  waktuMulai = millis();

  // Timestamp absolut hanya untuk log/debug.
  // Interval utama tetap pakai millis().
  t1EpochMs = getEpochMs();

  lastCountdown = -1;
  lastState = (SystemState)-1;

  state = AUTHORIZED;

  Serial.println("------------- T1 -------------");
  Serial.printf("[T1] Authorized UID : %s\n", currentUID.c_str());
  Serial.printf("[T1] Timestamp      : %s\n", getTimestampMs().c_str());
  Serial.printf("[T1] epoch_ms       : %llu\n", (unsigned long long)t1EpochMs);
  Serial.printf("[T1] millis         : %lu\n", waktuMulai);
  Serial.println("------------------------------");
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup() {
  pinMode(EN_POWER, OUTPUT);
  digitalWrite(EN_POWER, HIGH); // nyalakan power rail

  Serial.begin(115200);

  // 1. NVS wajib pertama
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  Wire.begin(I2C_SDA, I2C_SCL);

  initI2CMutex();

  pinMode(LED_BATTERY, OUTPUT);
  pinMode(LED_WIFI, OUTPUT);

  digitalWrite(LED_BATTERY, LOW);
  digitalWrite(LED_WIFI, LOW);

  // Init MAX17048
  if (!maxlipo.begin()) {
    Serial.println("MAX17048 tidak terdeteksi!");
  } else {
    Serial.println("MAX17048 Ready");
  }
    
  batteryVoltage = maxlipo.cellVoltage();
  batteryPercent = maxlipo.cellPercent();

  Serial.printf("Battery awal: %.1f%% | %.2fV\n",
                batteryPercent,
                batteryVoltage);

  lcd.init();
  lcd.backlight();
  lcd.print("   BOOTING...   ");

  initPN532();
  initAudioSD();
  muatDaftarEligible();
  muatDaftarBanned();
  initMPU6050();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  initBuzzer();
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonInterrupt, FALLING);
  delay(1000);

  // 2. Portal konfigurasi WiFi
  initWifiPortal();

  Preferences prefs;
  prefs.begin("catch_note", false);
  maxRecordMs = prefs.getULong("max_record_ms", 300000); 
  active_threshold = prefs.getInt("threshold", 300);
  prefs.end();
  Serial.printf("Batas Waktu Bicara aktif: %lu ms\n", maxRecordMs);

  // 3. Setup MQTT server (dari nilai yang disimpan portal)
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

  // 4. Default mulai di MODE_KOMUNIKASI
  state = STANDBY;
  
  // Karena masuk mode komunikasi, bebaskan RAM dari modul Audio
  deinitAudio();
  
  // Pastikan WiFi menyala dan siap terkoneksi
  startWiFi(); 
  
  // Reset timer komunikasi
  lastRegTime = 0;
  lastHeartbeatTime = 0;
  
  // Paksa layar untuk update di putaran loop pertama
  lastState = (SystemState)-1; 
} 

void printListTerdaftar() {
  Serial.println("===== LIST TERDAFTAR =====");
  Serial.printf("Jumlah terdaftar: %d\n", jumlahTerdaftar);

  for (int i = 0; i < jumlahTerdaftar; i++) {
    Serial.print(i);
    Serial.print(" | [");
    Serial.print(listTerdaftar[i]);
    Serial.print("] len=");
    Serial.println(listTerdaftar[i].length());
  }

  Serial.println("==========================");
}

void loop() {
  updateBuzzer();
  checkBattery();
  checkModeButton();
  updateLCD();

  // ── MODE_KOMUNIKASI: MQTT + heartbeat ──
  if (currentMode == MODE_KOMUNIKASI) {
    handleWiFi();
    handleMQTT();

    // Heartbeat kirimStatus setiap 10 detik
  if (!isMQTTConnecting() &&
      mqttClient.connected() &&
      millis() - lastHeartbeatTime >= HEARTBEAT_MS) {
      lastHeartbeatTime = millis();
      kirimStatus("online");
    }
  }

  // ── VAD (Voice Activity Detection) pre-buffer ──
  // Pre-buffer tetap aktif di luar MODE_KOMUNIKASI.
  // Trigger VAD hanya dipakai saat state == AUTHORIZED.
  float maxLoudness = 0;
  bool vadTriggered = false;
  unsigned long vadFirstSpeechMsThisLoop = 0;

  if (currentMode != MODE_KOMUNIKASI && state != RECORDING &&
      rx_handle != nullptr &&
      raw_i2s_buffer != nullptr &&
      processed_buffer != nullptr &&
      preRecordBuffer != nullptr) {

    size_t bytes_read = 0;

    while (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN,
                            &bytes_read, 0) == ESP_OK && bytes_read > 0) {

      int samples = bytes_read / 4;

      bool frameTriggered = updateAudioPreBufferAndVad(samples, maxLoudness);

      // VAD score hanya boleh berlaku ketika user/dosen sudah authorized.
      // Di STANDBY, suara luar tetap masuk pre-buffer, tapi tidak boleh membangun skor trigger.
      if (state == AUTHORIZED && frameTriggered) {
        vadTriggered = true;

        // Ambil waktu awal suara secepat mungkin.
        vadFirstSpeechMsThisLoop = getVadFirstSpeechMs();

        if (vadFirstSpeechMsThisLoop == 0) {
          vadFirstSpeechMsThisLoop = millis();
        }

        // Begitu sudah trigger, tidak perlu drain semua frame lagi.
        // Lebih cepat masuk ke state RECORDING.
        break;
      } 
      else if (state != AUTHORIZED) {
        resetVadState();
      }
    }
  }


  //  STATE MACHINE
  switch (state) {

    case STANDBY: {
      switch (currentMode) {

        case MODE_MAHASISWA: {
          identTimer_start();
          // printListTerdaftar();
          String idKartu = scanUID();
          if (idKartu != "") {
            
            // CEK 0: Apakah SD Card terbaca dengan baik?
            if (SD.cardType() == CARD_NONE) {
              // Feedback error panjang
              identTimer_stop(DEC_SD_ERROR);
              playBuzzer(1, 300, 80);
              stateTimer = millis();
              state = SD_ERROR;
            }

            // CEK 0.5: Apakah Dosen sudah bikin soal? 
            else if (!cekAdaPertanyaan()) {
              identTimer_stop(DEC_NO_QUESTION);
              playBuzzer(2, 150, 100);
              stateTimer = millis();
              state = NO_QUESTION;
            }

            // CEK 1: Apakah dia peserta kelas ini
            else if (!cekTerdaftar(idKartu)) {
              identTimer_stop(DEC_UNREGISTERED);
              playBuzzer(2, 80, 80);
              stateTimer = millis();
              state = UNREGISTERED;
            }

            // CEK 2: Apakah dia kena BANNED dari web
            else if (cekBanned(idKartu)) {
              identTimer_stop(DEC_BANNED);
              playBuzzer(2, 80, 80);
              stateTimer = millis();
              state = BANNED; 
            }

            // CEK 3: Apakah dia sudah bicara di sesi lokal SAAT INI
            else if (cekHistory(idKartu)) {
              identTimer_stop(DEC_INVALID);
              playBuzzer(2, 80, 80);
              stateTimer = millis();
              state = INVALID; 
            } 

            // LOLOS SEMUA CEK: Izinkan Merekam
            else {
              identTimer_stop(DEC_AUTHORIZED);
              mulaiAuthorizedWindow(idKartu);
            }
          }
          break;
        }

        // ── MODE DOSEN ──
        case MODE_DOSEN: {
          currentUID = "DOSEN";
          if (millis() - waktuMulai > (unsigned long)(DOSEN_COUNTDOWN * 1000)) {
            
            // CEK SD CARD
            if (SD.cardType() == CARD_NONE) {
              playBuzzer(1, 300, 80);
              stateTimer = millis();
              state = SD_ERROR;
              waktuMulai = millis(); // Reset countdown dosen setelah error
            } 
            else {
              mulaiAuthorizedWindow("DOSEN");
            }
          }
          break;
        }

        // ── MODE KOMUNIKASI ──
        case MODE_KOMUNIKASI: {

          // 1. Sinkronisasi TXT (trigger dari mqttCallback)
          if (sdSyncAktif && sdTargetKelas != "") {
            lcd.clear();
            lcdPrint16(0, "   SYNC DATA   ");
            lcdPrint16(1, " MOHON TUNGGU" + animDots());

            String kelas  = sdTargetKelas;
            sdTargetKelas = "";
            prosesSinkronisasiSD(kelas);

            lcd.clear();
            lastState = (SystemState)-1; // Paksa refresh LCD setelah sync
          }

          // 2. Sinkronisasi Audio WAV (trigger dari mqttCallback) ← BARU
          if (audioSyncAktif && audioTargetKelas != "") {
            lcd.clear();
            lcdPrint16(0, "  SYNC AUDIO   ");
            lcdPrint16(1, " MOHON TUNGGU" + animDots());

            String kelas     = audioTargetKelas;
            audioTargetKelas = "";
            prosesSinkronisasiAudio(kelas);

            lcd.clear();
            lastState = (SystemState)-1; // Paksa refresh LCD setelah sync
          }

          // 3. Scan RFID → kirim registrasi
          String idKartu = scanUIDPeriodik(100);
          if (idKartu != "") {
            if (millis() - lastRegTime > 2000) { // Anti-spam 2 detik
              if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
                sendRegistration(idKartu);
                lastRegTime = millis();
                Serial.println("Registrasi terkirim: " + idKartu);
                playBuzzer(1, 60, 60);
              } else {
                Serial.println("Gagal! Cek koneksi WiFi/MQTT.");
                lcd.setCursor(0, 1); lcd.print("  CONN ERROR!   ");
                playBuzzer(3, 80, 80);
                delay(1000);
                lastState = (SystemState)-1;
              }
            } else {
              Serial.println("Anti-spam aktif, tunggu sebentar...");
            }
          }
          break;
        }

      } // end switch(currentMode)
      break;
    }

    case INVALID: {
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) state = STANDBY;
      break;
    }

    case UNREGISTERED: { 
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) state = STANDBY;
      break;
    }

    case BANNED: { 
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) state = STANDBY;
      break;
    }

    case SD_ERROR: { 
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) state = STANDBY;
      break;
    }

    case NO_QUESTION: { 
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) state = STANDBY;
      break;
    }

    case AUTHORIZED: {
      if (vadTriggered) {
        unsigned long firstSpeechMs = vadFirstSpeechMsThisLoop;

        if (firstSpeechMs == 0) {
          firstSpeechMs = getVadFirstSpeechMs();
        }

        if (firstSpeechMs == 0) {
          firstSpeechMs = millis();
        }

        // Kalau karena edge case firstSpeechMs lebih kecil dari waktuMulai,
        // jangan sampai unsigned underflow.
        if (firstSpeechMs < waktuMulai) {
          firstSpeechMs = waktuMulai;
        }

        unsigned long kandidatWaktuRespon = firstSpeechMs - waktuMulai;

        // Suara dianggap sah kalau awal suaranya masih dalam batas timeout.
        // Ini lebih adil daripada mengecek waktu saat VAD baru trigger.
        if (kandidatWaktuRespon <= TIMEOUT_BICARA) {
          waktuRespon = kandidatWaktuRespon;

          t2EpochMs = t1EpochMs + waktuRespon;

          Serial.println("------------- T2 -------------");
          Serial.printf("[T2] Suara mulai terdeteksi\n");
          Serial.printf("[T2] firstSpeechMs : %lu\n", firstSpeechMs);
          Serial.printf("[T2] epoch_ms est.  : %llu\n", (unsigned long long)t2EpochMs);
          Serial.println("------------ HASIL -----------");
          Serial.printf("[RESULT] waktuRespon : %lu ms\n", waktuRespon);
          Serial.println("==============================");

          resetVadState();
          state = RECORDING;
        } 
        else {
          currentUID = "";
          stateTimer = millis();
          state = TIMEOUT;
          resetVadState();

          playBuzzer(1, 500, 80);
        }
      } 
      else if (millis() - waktuMulai > TIMEOUT_BICARA) {
        currentUID = "";
        stateTimer = millis();
        state = TIMEOUT;
        resetVadState();

        playBuzzer(1, 500, 80);
      }

      break;
    }

    case TIMEOUT: {
      // Tunggu selama ERROR_DISPLAY_TIME (2 detik) lalu kembali ke awal
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) {
        state = STANDBY;
        waktuMulai = millis(); // Reset waktu agar countdown dosen mulai dari awal lagi jika mode Dosen
        lastCountdown = -1;    // Paksa layar update
      }
      break;
    }

    case RECORDING: {
      clearButtonState(); // buang input tombol sebelum rekaman

      bool alatDilempar = rekamSuara(currentUID, waktuRespon);

      clearButtonState(); // buang tombol yang kepencet selama rekaman
      resetAudioFilters();

      playBuzzer(2, 60, 60);

      if (currentMode == MODE_MAHASISWA) simpanHistory(currentUID);

      if (alatDilempar && currentMode == MODE_DOSEN) {
          gantiMode(); 
      } else {
          currentUID    = "";
          state         = STANDBY;
          waktuMulai    = millis();
          lastCountdown = -1;
      }
      break;
    }
  }
}    
