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

#define MAX_PESERTA        50
#define ERROR_DISPLAY_TIME 2000

LiquidCrystal_I2C lcd(0x27, 16, 2);

Adafruit_MAX17048 maxlipo;

float batteryVoltage = 0.0;
float batteryPercent = 0.0;

unsigned long lastBatteryCheck = 0;
const unsigned long BATTERY_CHECK_INTERVAL = 5000; // cek tiap 5 detik

String daftarSudahBicara[MAX_PESERTA];
int    jumlahPeserta = 0;

enum OperatingMode { MODE_MAHASISWA, MODE_DOSEN, MODE_KOMUNIKASI };
volatile OperatingMode currentMode = MODE_MAHASISWA;

enum SystemState { STANDBY, INVALID, AUTHORIZED, TIMEOUT, RECORDING };
SystemState state     = STANDBY;
SystemState lastState = (SystemState)-1;

const int LONG_PRESS_TIME = 2000;

volatile bool          buttonFlag        = false;
volatile unsigned long lastTriggerTime   = 0;

unsigned long stateTimer    = 0;
String        currentUID    = "";
unsigned long waktuMulai    = 0;
unsigned long waktuRespon   = 0;
int           lastCountdown = -1;
unsigned long lastRegTime   = 0;

// ── Heartbeat: kirim status setiap 10 detik di MODE_KOMUNIKASI ──
static unsigned long lastHeartbeatTime = 0;
const  unsigned long HEARTBEAT_MS      = 10000;

void IRAM_ATTR handleButtonInterrupt() {
  buttonFlag = true;
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
  if (!buttonFlag) return;
  if (millis() - lastTriggerTime < 200) { buttonFlag = false; return; }
  lastTriggerTime = millis();

  // Hold untuk Deep Sleep
  unsigned long startHold = millis();
  while (digitalRead(BUTTON_PIN) == LOW) {
    if (millis() - startHold > LONG_PRESS_TIME) prepareDeepSleep();
  }

  // Maju ke mode berikutnya
  OperatingMode nextMode;
  if      (currentMode == MODE_MAHASISWA) nextMode = MODE_DOSEN;
  else if (currentMode == MODE_DOSEN)     nextMode = MODE_KOMUNIKASI;
  else                                    nextMode = MODE_MAHASISWA;

  currentMode  = nextMode;
  state        = STANDBY;
  lastState    = (SystemState)-1;
  lastCountdown = -1;
  waktuMulai   = millis();

  updateLCD();

  // Buzzer: jumlah beep = nomor mode
  for (int i = 0; i <= (int)currentMode; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(40);
    digitalWrite(BUZZER_PIN, LOW);  delay(40);
  }

  buttonFlag = false;

  if (currentMode == MODE_DOSEN) {
    stopWiFi();
    delay(200);
  }
  else if (currentMode == MODE_KOMUNIKASI) {
    deinitAudio();
    delay(300);
    startWiFi();
    lastRegTime       = 0;
    lastHeartbeatTime = 0; // Reset heartbeat saat masuk mode KOM
  }
  else { // MODE_MAHASISWA
    stopWiFi();
    delay(500);
    Serial.println("\n=== STATUS MEMORI (WiFi OFF) ===");
    Serial.printf("RAM Bebas    : %d bytes (%.1f KB)\n", ESP.getFreeHeap(), ESP.getFreeHeap() / 1024.0);
    Serial.printf("Blok Terbesar: %d bytes (%.1f KB)\n", ESP.getMaxAllocHeap(), ESP.getMaxAllocHeap() / 1024.0);
    Serial.println("================================");
    initAudioSD();
  }
}

// ════════════════════════════════════════════════
//  prepareDeepSleep
// ════════════════════════════════════════════════
void prepareDeepSleep() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  MEMATIKAN...  ");
  delay(1000);
  stopWiFi();
  lcd.noBacklight();
  lcd.noDisplay();
  digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  Serial.println("Entering Deep Sleep...");
  esp_deep_sleep_start();
}

void updateLCD() {
  unsigned long durasi = millis() - waktuMulai;
  
  // Hitung countdown DOSEN
  int countdownSekarang = DOSEN_COUNTDOWN - (durasi / 1000);
  if (countdownSekarang < 0) countdownSekarang = 0;

  // Hitung countdown TIMEOUT dengan Pembulatan Ke Atas (Ceiling)
  // Agar tidak langsung lompat ke 4 detik karena terpotong delay(100) buzzer
  int timeoutSekarang = 0;
  if (durasi < TIMEOUT_BICARA) {
    timeoutSekarang = (TIMEOUT_BICARA - durasi + 999) / 1000; 
  }

  static wl_status_t lastWifiStatus = WL_DISCONNECTED;
  static bool        lastMqttStatus = false;
  static int         lastTimeout    = -1; 

  bool wifiChanged = (currentMode == MODE_KOMUNIKASI) && (WiFi.status() != lastWifiStatus);
  bool mqttChanged = (currentMode == MODE_KOMUNIKASI) && (mqttClient.connected() != lastMqttStatus);
  
  bool isCountdownDosen = (state == STANDBY && currentMode == MODE_DOSEN && countdownSekarang > 0);
  bool isCountdownAuth  = (state == AUTHORIZED);

  // --- PERBAIKAN LOGIKA REFRESH LAYAR ---
  // Hanya blokir refresh jika state TIDAK berubah
  if (state == lastState) {
    if (isCountdownDosen && countdownSekarang == lastCountdown) return;
    if (isCountdownAuth && timeoutSekarang == lastTimeout) return;
    if (!isCountdownDosen && !isCountdownAuth && !wifiChanged && !mqttChanged) return;
  }
  // --------------------------------------

  // Jika kode sampai di sini, artinya HARUS refresh layar
  if (currentMode == MODE_KOMUNIKASI) {
    lastWifiStatus = WiFi.status();
    lastMqttStatus = mqttClient.connected();
  }

  lcd.clear();
  lastCountdown = countdownSekarang;
  lastTimeout   = timeoutSekarang;

  switch (state) {
    case STANDBY:
      if (currentMode == MODE_MAHASISWA) {
        lcd.setCursor(0, 0); lcd.print("  [MODE: MHS]   ");
        lcd.setCursor(0, 1); lcd.print("  MENUNGGU TAG  ");
      }
      else if (currentMode == MODE_DOSEN) {
        lcd.setCursor(0, 0); lcd.print("  [MODE: DSN]   ");
        lcd.setCursor(0, 1); lcd.print(" Cooldown: " + String(countdownSekarang) + "s ");
      }
      else { // MODE_KOMUNIKASI
        lcd.setCursor(0, 0); lcd.print("  [MODE: KOM]   ");
        
        if (WiFi.status() != WL_CONNECTED) {
            lcd.setCursor(0, 1); 
            lcd.print(" Connecting...  "); // (Bisa Anda buat animasi juga kalau mau, tapi karena ada startWiFi() ini jarang terlihat lama)
        } 
        else if (!mqttClient.connected()) {
            lcd.setCursor(0, 1); 
            lcd.print("MQTT Connecting."); 
        }
        else {
            lcd.setCursor(0, 1); 
            lcd.print("Standby Tag/Task");
        }
      }
      break;

    case AUTHORIZED:
      lcd.setCursor(0, 0);
      lcd.print("ID: ");
      lcd.print(currentMode == MODE_DOSEN ? "DOSEN" : currentUID.substring(0, 8));
      lcd.setCursor(0, 1); 
      lcd.print("TIMEOUT: "); lcd.print(timeoutSekarang); lcd.print("s ");
      break;

    case RECORDING:
      lcd.setCursor(0, 0); lcd.print(" >>> RECORDING  ");
      lcd.setCursor(0, 1); lcd.print("    ------    ");
      break;

    case INVALID:
      lcd.setCursor(0, 0); lcd.print("   TIDAK VALID  ");
      lcd.setCursor(0, 1); lcd.print("  SUDAH BICARA  ");
      break;

    case TIMEOUT:
      lcd.setCursor(0, 0); lcd.print("  WAKTU HABIS   ");
      lcd.setCursor(0, 1); lcd.print(" BATAL MEREKAM  ");
      break;
  }

  lastState = state;
}


void checkBattery() {

  // Baca baterai tiap 5 detik
  if (millis() - lastBatteryCheck < BATTERY_CHECK_INTERVAL) return;
  lastBatteryCheck = millis();

  batteryVoltage = maxlipo.cellVoltage();
  batteryPercent = maxlipo.cellPercent();

  Serial.println("=== STATUS BATERAI ===");
  Serial.printf("Voltage : %.2f V\n", batteryVoltage);
  Serial.printf("Battery : %.1f %%\n", batteryPercent);
  Serial.println("======================");

  // LED merah baterai low
  if (batteryPercent < 20.0) {
    digitalWrite(LED_BATTERY, HIGH);
  } else {
    digitalWrite(LED_BATTERY, LOW);
  }

  // LED WiFi
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_WIFI, HIGH);
  } else {
    digitalWrite(LED_WIFI, LOW);
  }
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // 1. NVS wajib pertama
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  Wire.begin(I2C_SDA, I2C_SCL);

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
  initMPU6050();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonInterrupt, FALLING);
  delay(1000);

  // 2. Portal konfigurasi WiFi
  initWifiPortal();

  // 3. Setup MQTT server (dari nilai yang disimpan portal)
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

  // 4. Matikan WiFi — default mulai di MODE_MAHASISWA
  WiFi.mode(WIFI_OFF);
  state = STANDBY;

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  [MODE: MHS]   ");
  lcd.setCursor(0, 1); lcd.print("  MENUNGGU TAG  ");
}

void loop() {
  checkBattery();
  handleResetButton();
  checkModeButton();
  updateLCD();

  // ── MODE_KOMUNIKASI: MQTT + heartbeat ──
  if (currentMode == MODE_KOMUNIKASI) {
    handleMQTT();

    // Heartbeat kirimStatus setiap 10 detik
    if (mqttClient.connected() && millis() - lastHeartbeatTime >= HEARTBEAT_MS) {
      lastHeartbeatTime = millis();
      kirimStatus("online");
    }
  }

  // ── VAD (Voice Activity Detection) pre-buffer ──
  // Hanya aktif di luar MODE_KOMUNIKASI dan saat tidak sedang rekam
  float maxLoudness = 0;
  if (currentMode != MODE_KOMUNIKASI && state != RECORDING) {
    size_t bytes_read = 0;
    while (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN,
                            &bytes_read, 0) == ESP_OK && bytes_read > 0) {
      long  dummySum = 0;
      int   samples  = bytes_read / 4;
      float chunkLoudness = processAudioBuffer(raw_i2s_buffer, processed_buffer, samples, dummySum);
      if (chunkLoudness > maxLoudness) maxLoudness = chunkLoudness;
      for (int i = 0; i < samples; i++) {
        preRecordBuffer[bufferHead] = processed_buffer[i];
        bufferHead++;
        if (bufferHead >= PRE_BUFFER_SIZE) { bufferHead = 0; bufferIsFull = true; }
      }
    }
  }


  //  STATE MACHINE
  switch (state) {

    case STANDBY: {
      switch (currentMode) {

        // ── MODE MAHASISWA ──
        case MODE_MAHASISWA: {
          String idKartu = scanUID();
          if (idKartu != "") {
            if (cekHistory(idKartu)) {
              // Feedback gagal
              digitalWrite(BUZZER_PIN, HIGH); delay(80);
              digitalWrite(BUZZER_PIN, LOW);  delay(80);
              digitalWrite(BUZZER_PIN, HIGH); delay(80);
              digitalWrite(BUZZER_PIN, LOW);

              stateTimer = millis();
              state = INVALID;
            } else {
              currentUID = idKartu;
              waktuMulai = millis();
              state = AUTHORIZED;
              digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);
            }
          }
          break;
        }

        // ── MODE DOSEN ──
        case MODE_DOSEN: {
          currentUID = "DOSEN";
          if (millis() - waktuMulai > (unsigned long)(DOSEN_COUNTDOWN * 1000)) {
            state = AUTHORIZED;
            waktuMulai = millis();
          }
          break;
        }

        // ── MODE KOMUNIKASI ──
        case MODE_KOMUNIKASI: {

          // 1. Sinkronisasi TXT (trigger dari mqttCallback)
          if (sdSyncAktif && sdTargetKelas != "") {
            lcd.clear();
            lcd.setCursor(0, 0); lcd.print("  SYNC DATA...  ");
            lcd.setCursor(0, 1); lcd.print(" MOHON TUNGGU   ");

            String kelas  = sdTargetKelas;
            sdTargetKelas = "";
            prosesSinkronisasiSD(kelas);

            lcd.clear();
            lastState = (SystemState)-1; // Paksa refresh LCD setelah sync
          }

          // 2. Sinkronisasi Audio WAV (trigger dari mqttCallback) ← BARU
          if (audioSyncAktif && audioTargetKelas != "") {
            lcd.clear();
            lcd.setCursor(0, 0); lcd.print("  SYNC AUDIO... ");
            lcd.setCursor(0, 1); lcd.print(" MOHON TUNGGU   ");

            String kelas     = audioTargetKelas;
            audioTargetKelas = "";
            prosesSinkronisasiAudio(kelas);

            lcd.clear();
            lastState = (SystemState)-1; // Paksa refresh LCD setelah sync
          }

          // 3. Scan RFID → kirim registrasi
          String idKartu = scanUID();
          if (idKartu != "") {
            if (millis() - lastRegTime > 2000) { // Anti-spam 2 detik
              if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
                sendRegistration(idKartu);
                lastRegTime = millis();
                Serial.println("Registrasi terkirim: " + idKartu);
              } else {
                Serial.println("Gagal! Cek koneksi WiFi/MQTT.");
                lcd.setCursor(0, 1); lcd.print("  CONN ERROR!   ");
                delay(1500);
                lastState = (SystemState)-1;
              }
            } else {
              Serial.println("⏳ Anti-spam aktif, tunggu sebentar...");
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

    case AUTHORIZED: {
      if (maxLoudness > SILENCE_THRESHOLD) {
        waktuRespon = millis() - waktuMulai;
        state = RECORDING;
      } 
      // Logika pembatalan jika waktu tunggu habis
      else if (millis() - waktuMulai > TIMEOUT_BICARA) {
        currentUID = "";          // Jangan masukkan ke histori
        stateTimer = millis();    // Mulai timer pesan error
        state = TIMEOUT;      // Lempar ke state timeout
        
        // Bunyi panjang sebagai notifikasi gagal
        digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);
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
      rekamSuara(currentUID, waktuRespon);
      resetAudioFilters();

      // Feedback Record Saved
      digitalWrite(BUZZER_PIN, HIGH); delay(60);
      digitalWrite(BUZZER_PIN, LOW);  delay(60);
      digitalWrite(BUZZER_PIN, HIGH); delay(60);
      digitalWrite(BUZZER_PIN, LOW);

      if (currentMode == MODE_MAHASISWA) simpanHistory(currentUID);
      currentUID    = "";
      state         = STANDBY;
      waktuMulai    = millis();
      lastCountdown = -1; // Paksa refresh countdown LCD
      break;
    }

  } // end switch(state)
}
