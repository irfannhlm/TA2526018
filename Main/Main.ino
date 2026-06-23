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
#include "I2C_Handler.h"
#include "Buzzer_Module.h"
#include "LCD_Helper.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define MAX_PESERTA        100
#define ERROR_DISPLAY_TIME 2000

LiquidCrystal_I2C lcd(0x27, 16, 2);

unsigned long maxRecordMs = 300000; // Default 5 menit
int active_threshold = 500; // Default threshold
int jumlahTerdaftar = 0; // Penghitung jumlah peserta terdaftar

Adafruit_MAX17048 maxlipo;

float batteryVoltage = 0.0;
float batteryPercent = 0.0;
float batteryOffset = 0.058; // Offset tegangan untuk kalibrasi (~58mV)

volatile unsigned long buttonCooldownUntil = 0;
volatile bool buttonSuppressUntilReleased = false;
volatile uint32_t buttonResetSeq = 0;
static TaskHandle_t buttonTaskHandle = nullptr;
static QueueHandle_t buttonEventQueue = nullptr;
static portMUX_TYPE buttonMux = portMUX_INITIALIZER_UNLOCKED;

unsigned long lastBatteryCheck = 0;
const unsigned long BATTERY_CHECK_INTERVAL = 5000; // cek tiap 5 detik

String daftarSudahBicara[MAX_PESERTA];
String listTerdaftar[MAX_PESERTA];
String listBanned[MAX_PESERTA];
int    jumlahPeserta = 0;

enum OperatingMode { MODE_KOMUNIKASI, MODE_DOSEN, MODE_MAHASISWA };
volatile OperatingMode currentMode = MODE_KOMUNIKASI;

enum SystemState { STANDBY, INVALID, UNREGISTERED, BANNED, SD_ERROR, RFID_ERROR, DATA_ERROR, AUTHORIZED_BEEP, AUTHORIZED_SETTLE, AUTHORIZED, TIMEOUT, RECORDING, NO_QUESTION };
SystemState state     = STANDBY;
SystemState lastState = (SystemState)-1;

const unsigned long BUTTON_POLL_MS           = 10;
const unsigned long BUTTON_DEBOUNCE_MS       = 30;
const unsigned long BUTTON_MIN_CLICK_MS      = 20;
const unsigned long CLICK_EVALUATE_MS        = 300;
const unsigned long BUTTON_POST_ACTION_COOLDOWN_MS = 350;
const unsigned long LONG_PRESS_TIME          = 3000;
const int BUTTON_MAX_CLICK_COUNT             = 3;
const uint16_t AUTHORIZED_BEEP_ON_MS         = 150;
const uint16_t AUTHORIZED_BEEP_OFF_MS        = 40;
const unsigned long AUTHORIZED_SETTLE_MS     = 200;

bool pendingWifiReconnect = false;

bool lastMqttStatus = false;

unsigned long stateTimer    = 0;
String        currentUID    = "";
unsigned long waktuMulai    = 0;
unsigned long waktuRespon   = 0;
unsigned long authorizedSettleStarted = 0;
int           lastCountdown = -1;
unsigned long lastRegTime   = 0;
static String lastWifiLine = "";

// Heartbeat: kirim status setiap 10 detik di MODE_KOMUNIKASI
static unsigned long lastHeartbeatTime = 0;
const  unsigned long HEARTBEAT_MS      = 10000;

enum ButtonEventType : uint8_t {
  BUTTON_EVENT_CLICKS = 1,
  BUTTON_EVENT_HOLD   = 2
};

struct ButtonEvent {
  uint8_t type;
  uint8_t clicks;
};

void resetButtonInput(const char* reason, unsigned long cooldownMs) {
  unsigned long now = millis();
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  portENTER_CRITICAL(&buttonMux);
  buttonSuppressUntilReleased = pressed;
  buttonCooldownUntil = (cooldownMs > 0) ? (now + cooldownMs) : 0;
  buttonResetSeq++;
  portEXIT_CRITICAL(&buttonMux);

  if (buttonEventQueue != nullptr) {
    xQueueReset(buttonEventQueue);
  }
}

void clearButtonState() {
  resetButtonInput("clear", 0);
}

void consumeButtonGesture(const char* reason) {
  resetButtonInput(reason, BUTTON_POST_ACTION_COOLDOWN_MS);
}

void dropButtonGesture(const char* reason) {
  resetButtonInput(reason, 0);
}

// Beep penanda ganti mode: switch (1/2 click) = 1 beep pendek, cycle (3 click) = 2 beep.
// Non-blocking, di-flush oleh waitBuzzerDone() di setOperatingMode sebelum init.
void beepSwitch(bool isCycle) {
  playBuzzer(isCycle ? 2 : 1, 60, 60);
}

// Dipanggil dari loop rekam untuk mendeteksi
// permintaan stop manual: double click atau lebih (>=2). Non-blocking.
bool pollManualStopRecording() {
  if (buttonEventQueue == nullptr) return false;

  ButtonEvent event;
  if (xQueueReceive(buttonEventQueue, &event, 0) != pdPASS) return false;

  return (event.type == BUTTON_EVENT_CLICKS && event.clicks >= 2);
}

void queueButtonEvent(uint8_t type, uint8_t clicks) {
  if (buttonEventQueue == nullptr) return;

  ButtonEvent event = {
    type,
    clicks
  };

  xQueueSend(buttonEventQueue, &event, 0);
}

void buttonPollingTask(void* param) {
  bool rawLast = digitalRead(BUTTON_PIN);
  bool stableState = rawLast;
  unsigned long rawChangedAt = millis();
  unsigned long pressStartedAt = (stableState == LOW) ? millis() : 0;
  unsigned long lastReleaseAt = 0;
  uint8_t clickCount = 0;
  bool sequenceNoise = false;
  bool holdReported = false;
  uint32_t localResetSeq = buttonResetSeq;

  while (true) {
    unsigned long now = millis();
    bool raw = digitalRead(BUTTON_PIN);
    uint32_t resetSeqSnapshot = 0;

    portENTER_CRITICAL(&buttonMux);
    resetSeqSnapshot = buttonResetSeq;
    portEXIT_CRITICAL(&buttonMux);

    if (resetSeqSnapshot != localResetSeq) {
      localResetSeq = resetSeqSnapshot;
      rawLast = raw;
      stableState = raw;
      rawChangedAt = now;
      pressStartedAt = (stableState == LOW) ? now : 0;
      lastReleaseAt = 0;
      clickCount = 0;
      sequenceNoise = false;
      holdReported = false;
    }

    if (raw != rawLast) {
      rawLast = raw;
      rawChangedAt = now;
    }

    if (raw != stableState && now - rawChangedAt >= BUTTON_DEBOUNCE_MS) {
      stableState = raw;

      if (stableState == LOW) {
        pressStartedAt = now;
        holdReported = false;
      } else {
        bool suppressed = false;
        bool cooldownActive = false;
        unsigned long pressDuration = (pressStartedAt > 0) ? (now - pressStartedAt) : 0;

        portENTER_CRITICAL(&buttonMux);
        suppressed = buttonSuppressUntilReleased;
        if (buttonSuppressUntilReleased) {
          buttonSuppressUntilReleased = false;
        }
        cooldownActive = (buttonCooldownUntil != 0 && now < buttonCooldownUntil);
        portEXIT_CRITICAL(&buttonMux);

        if (!holdReported &&
            !suppressed &&
            !cooldownActive &&
            pressDuration >= BUTTON_MIN_CLICK_MS) {
          if (clickCount < BUTTON_MAX_CLICK_COUNT + 1) {
            clickCount++;
          }
          lastReleaseAt = now;

          if (clickCount > BUTTON_MAX_CLICK_COUNT) {
            sequenceNoise = true;
          }
        }

        if (suppressed || cooldownActive) {
          clickCount = 0;
          lastReleaseAt = 0;
          sequenceNoise = false;
        }

        pressStartedAt = 0;
        holdReported = false;
      }
    }

    bool suppressBlocksHold = false;
    bool cooldownBlocksHold = false;
    portENTER_CRITICAL(&buttonMux);
    suppressBlocksHold = buttonSuppressUntilReleased;
    cooldownBlocksHold = (buttonCooldownUntil != 0 && now < buttonCooldownUntil);
    portEXIT_CRITICAL(&buttonMux);

    if (stableState == LOW &&
        pressStartedAt > 0 &&
        !holdReported &&
        !suppressBlocksHold &&
        !cooldownBlocksHold &&
        now - pressStartedAt >= LONG_PRESS_TIME) {
      portENTER_CRITICAL(&buttonMux);
      buttonSuppressUntilReleased = true;
      buttonCooldownUntil = now + BUTTON_POST_ACTION_COOLDOWN_MS;
      portEXIT_CRITICAL(&buttonMux);

      if (buttonEventQueue != nullptr) {
        xQueueReset(buttonEventQueue);
      }

      queueButtonEvent(BUTTON_EVENT_HOLD, 0);
      clickCount = 0;
      lastReleaseAt = 0;
      sequenceNoise = false;
      holdReported = true;
    }

    if (stableState == HIGH &&
        clickCount > 0 &&
        lastReleaseAt > 0 &&
        now - lastReleaseAt > CLICK_EVALUATE_MS) {
      if (!sequenceNoise && clickCount <= BUTTON_MAX_CLICK_COUNT) {
        queueButtonEvent(BUTTON_EVENT_CLICKS, clickCount);
      }

      clickCount = 0;
      lastReleaseAt = 0;
      sequenceNoise = false;
    }

    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

void startButtonTask() {
  if (buttonTaskHandle != nullptr) return;

  if (buttonEventQueue == nullptr) {
    buttonEventQueue = xQueueCreate(4, sizeof(ButtonEvent));
    if (buttonEventQueue == nullptr) {
      return;
    }
  } else {
    xQueueReset(buttonEventQueue);
  }

  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  portENTER_CRITICAL(&buttonMux);
  buttonSuppressUntilReleased = pressed;
  buttonCooldownUntil = 0;
  buttonResetSeq++;
  portEXIT_CRITICAL(&buttonMux);

  BaseType_t ok = xTaskCreatePinnedToCore(
    buttonPollingTask,
    "ButtonTask",
    2048,
    nullptr,
    2,
    &buttonTaskHandle,
    1
  );

  if (ok != pdPASS) {
    buttonTaskHandle = nullptr;
  }
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

bool isModeChangeAllowed() {
  switch (state) {
    case STANDBY:
    case INVALID:
    case UNREGISTERED:
    case BANNED:
    case SD_ERROR:
    case RFID_ERROR:
    case DATA_ERROR:
    case TIMEOUT:
    case NO_QUESTION:
    case AUTHORIZED_BEEP:
    case AUTHORIZED_SETTLE:
    case AUTHORIZED:
      return true;

    // Saat rekaman bukan switch mode, tapi manual stop rekam
    case RECORDING:
      return false;
  }

  return false;
}

void masukModeDebug() {
  lcd.clear();
  lcdPrint16(0, "   MODE SETUP  ");
  lcdPrint16(1, "  WIFI CONFIG  ");

  playBuzzer(1, 60, 60); // switch (1/2 click) = 1 beep pendek
  waitBuzzerDone();

  stopMQTT();
  stopWiFi();
  delay(300);

  lcd.clear();
  lcdPrint16(0, "WIFI: CatchNote");
  lcdPrint16(1, "IP: 192.168.4.1");

  bukaPortal(); 
  lastState = (SystemState)-1;
}

//  prepareDeepSleep
void prepareDeepSleep() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  MEMATIKAN...  ");
  delay(1000);
  stopWiFi();
  playBuzzer(1, 500, 80);
  waitBuzzerDone();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  esp_deep_sleep_start();
}

DataLoadStatus muatDaftarEligible() {
  if (!ensureSdReady("load_eligible", true)) {
    return DATA_LOAD_SD_ERROR;
  }

  if (!SD.exists("/eligible.txt")) {
    jumlahTerdaftar = 0;
    return DATA_LOAD_EMPTY;
  }

  File file = SD.open("/eligible.txt", FILE_READ);
  if (!file) {
    markSdLost("load_eligible_open");
    return DATA_LOAD_SD_ERROR;
  }

  String loadedList[MAX_PESERTA];
  int loadedCount = 0;

  while (file.available() && loadedCount < MAX_PESERTA) {
    String line = file.readStringUntil('\n');
    line.trim(); 
    
    if (line.length() > 0) {
      loadedList[loadedCount] = line;
      loadedCount++;
    }
  }
  file.close();

  if (!ensureSdReady("load_eligible_done", true)) {
    return DATA_LOAD_SD_ERROR;
  }

  jumlahTerdaftar = loadedCount;
  for (int i = 0; i < loadedCount; i++) {
    listTerdaftar[i] = loadedList[i];
  }

  return (jumlahTerdaftar > 0) ? DATA_LOAD_OK : DATA_LOAD_EMPTY;
}

// Fungsi Pengecekan UID di daftar kelas
bool cekTerdaftar(String uid) {
  if (jumlahTerdaftar == 0) return false; // Tolak semua jika daftar kosong
  for (int i = 0; i < jumlahTerdaftar; i++) {
    if (listTerdaftar[i] == uid) return true; 
  }
  return false;
}

int jumlahBanned = 0;

DataLoadStatus muatDaftarBanned() {
  if (!ensureSdReady("load_banned", true)) {
    return DATA_LOAD_SD_ERROR;
  }

  if (!SD.exists("/banned.txt")) {
    jumlahBanned = 0;
    return DATA_LOAD_EMPTY;
  }

  File file = SD.open("/banned.txt", FILE_READ);
  if (!file) {
    markSdLost("load_banned_open");
    return DATA_LOAD_SD_ERROR;
  }

  String loadedList[MAX_PESERTA];
  int loadedCount = 0;

  while (file.available() && loadedCount < MAX_PESERTA) {
    String line = file.readStringUntil('\n');
    line.trim(); 
    
    if (line.length() > 0) {
      loadedList[loadedCount] = line;
      loadedCount++;
    }
  }
  file.close();

  if (!ensureSdReady("load_banned_done", true)) {
    return DATA_LOAD_SD_ERROR;
  }

  jumlahBanned = loadedCount;
  for (int i = 0; i < loadedCount; i++) {
    listBanned[i] = loadedList[i];
  }

  return (jumlahBanned > 0) ? DATA_LOAD_OK : DATA_LOAD_EMPTY;
}

void masukSdError(const char* konteks) {
  currentUID = "";
  resetVadState();
  stateTimer = millis();
  state = SD_ERROR;
  lastState = (SystemState)-1;
}

bool pastikanSdOperasional(const char* konteks, bool forceProbe) {
  if (ensureSdReady(konteks, forceProbe)) return true;

  masukSdError(konteks);
  return false;
}

void tandaiDataMahasiswaKosong() {
  if (jumlahTerdaftar > 0) return;

  if (!isSdReady()) {
    masukSdError("eligible_cache_without_sd");
    return;
  }

  stateTimer = millis();
  state = DATA_ERROR;
  lastState = (SystemState)-1;
}

bool cekBanned(String uid) {
  if (jumlahBanned == 0) return false; 
  for (int i = 0; i < jumlahBanned; i++) {
    if (listBanned[i] == uid) return true; 
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

  static bool portalScreenShown = false;

  // OVERLAY notifikasi "PENGATURAN DIPERBARUI" 3 detik (non-blocking)
  static unsigned long pengaturanShownUntil = 0;
  if (pengaturanDiperbarui) {
    pengaturanDiperbarui = false;
    pengaturanShownUntil = millis() + 3000;   // tampil 3 detik
    playBuzzer(1, 60, 60);                     // 1 beep pendek halus
    lcd.clear();
    lcdPrint16(0, "   PENGATURAN   ");
    lcdPrint16(1, "   DIPERBARUI   ");
    lastState = (SystemState)-1;               // paksa refresh layar normal setelahnya
  }
  if (millis() < pengaturanShownUntil) {
    return;   // tahan overlay, jangan render layar lain
  }

  if (currentMode == MODE_KOMUNIKASI && isWifiPortalActive()) {
    if (!portalScreenShown) {
      lcd.clear();
      lcdPrint16(0, "WIFI: CatchNote");
      lcdPrint16(1, "IP: 192.168.4.1");
      portalScreenShown = true;
    }
    return;
  }

  portalScreenShown = false;

  if (currentMode == MODE_KOMUNIKASI &&
    state == STANDBY &&
    String(saved_ssid).length() < 2) {

  lcdPrint16(0, "    OFFLINE   ");
  lcdPrint16(1, "WIFI BELUM DISET");
  return;
  }

  // WIFI RETRY / BELUM CONNECT
  if (currentMode == MODE_KOMUNIKASI &&
      state == STANDBY &&
      isWiFiConnecting()) {

    if (animFrame != lastAnimFrame || state != lastState) {
      lcdPrint16(0, "  MODE: ONLINE  ");
      lcdPrint16(1, "  KONEK WIFI" + animDots());

      lastAnimFrame = animFrame;
      lastState = state;
    }

    return;
  }

  if (currentMode == MODE_KOMUNIKASI &&
      state == STANDBY &&
      isTimeSyncing()) {

    if (animFrame != lastAnimFrame || state != lastState) {
      lcdPrint16(0, "PENGATURAN WAKTU");
      lcdPrint16(1, " MOHON TUNGGU" + animDots());

      lastAnimFrame = animFrame;
      lastState = state;
    }

    return;
  }

  if (currentMode == MODE_KOMUNIKASI &&
      state == STANDBY &&
      WiFi.status() != WL_CONNECTED &&
      !wifiDibatalkan &&
      !isWiFiConnecting()) {
    
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

  bool mqttBusy    = isMQTTConnecting();
  bool mqttOkForUi = (!mqttBusy) && mqttClient.connected();   

  // MQTT / SERVER CONNECTING, WIFI SUDAH CONNECT
  if (currentMode == MODE_KOMUNIKASI &&
      state == STANDBY &&
      WiFi.status() == WL_CONNECTED &&
      !wifiDibatalkan &&
      !mqttOkForUi) {

    if (animFrame != lastAnimFrame ||
        state != lastState ||
        lastMqttStatus != mqttOkForUi) {

      lcdPrint16(0, "  KONEK SERVER  ");
      lcdPrint16(1, " MOHON TUNGGU" + animDots());

      lastAnimFrame = animFrame;
      lastMqttStatus = mqttOkForUi;
      lastState = state;
    }

    return;
  }

  // EARLY-RETURN GATE
  bool mqttChanged = (currentMode == MODE_KOMUNIKASI) &&
                     !wifiDibatalkan &&
                     (mqttOkForUi != lastMqttStatus);

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
    lastMqttStatus = mqttOkForUi;
  }

  lastWifiLine = "";
  lastAnimFrame = animFrame;

  lcd.clear();

  lastCountdown = countdownSekarang;
  lastTimeout = timeoutSekarang;

  switch (state) {

    case STANDBY:
      if (currentMode == MODE_MAHASISWA) {
        lcdShowModeMahasiswa();
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
          lcdShowModeOnline();
        }
      }
      break;

    case AUTHORIZED:
      lcdPrint16(0, "  MULAI BICARA ");
      lcdPrint16(1, "BATAL DALAM " + twoDigit(timeoutSekarang) + "s");
      break;

    case AUTHORIZED_BEEP:
      lcdPrint16(0, "  IZIN REKAM  ");
      lcdPrint16(1, " MOHON TUNGGU ");
      break;

    case AUTHORIZED_SETTLE:
      lcdPrint16(0, " SIAPKAN SUARA ");
      lcdPrint16(1, " MOHON TUNGGU ");
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

    case RFID_ERROR:
      lcdPrint16(0, "  RFID ERROR   ");
      lcdPrint16(1, "  CEK SENSOR   ");
      break;

    case DATA_ERROR:
      lcdPrint16(0, "DATA KLS KOSONG.");
      lcdPrint16(1, "   SYNC DULU    ");
      break;

    case NO_QUESTION:
      lcdPrint16(0, "BELUM ADA SOAL ");
      lcdPrint16(1, "DOSEN REKAM DULU");
      break;
  }

  lastState = state;
}

void setOperatingMode(int nextModeValue) {
  OperatingMode nextMode = (OperatingMode) nextModeValue;
  OperatingMode oldMode = currentMode;

  if (oldMode == nextMode) return;

  pendingWifiReconnect = false;

  if (oldMode == MODE_KOMUNIKASI && nextMode != MODE_KOMUNIKASI) {
    if (isWifiPortalActive()) {
      stopWifiPortal();
    }
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

  // updateBuzzer() baru jalan lagi setelah kembali ke loop().
  waitBuzzerDone();

  if (currentMode == MODE_DOSEN) {
    stopWiFi();
    delay(50); // jeda settle radio sebelum power-up SD/I2C
    resumeSdRecovery();
    initAudioSD();
    pastikanSdOperasional("mode_dosen", true);
  }
  else if (currentMode == MODE_MAHASISWA) {
    stopWiFi();
    delay(50); // jeda settle radio sebelum power-up SD/I2C
    resumeSdRecovery();
    // PN532 di-init lazy oleh state machine STANDBY supaya ganti mode tidak blocking.
    initAudioSD();

    if (pastikanSdOperasional("mode_mahasiswa", true)) {
      DataLoadStatus eligibleStatus = muatDaftarEligible();
      DataLoadStatus bannedStatus = muatDaftarBanned();

      if (eligibleStatus == DATA_LOAD_SD_ERROR || bannedStatus == DATA_LOAD_SD_ERROR) {
        masukSdError("mode_mahasiswa_load");
      } else if (eligibleStatus == DATA_LOAD_EMPTY) {
        tandaiDataMahasiswaKosong();
      }
    }
  }
  else if (currentMode == MODE_KOMUNIKASI) {
    deinitAudio();
    pauseSdRecovery();
    startWiFi();
    lastRegTime       = 0;
    lastHeartbeatTime = 0;
  }

  lastState = (SystemState)-1;
  consumeButtonGesture("mode_change");
}

void checkModeButton() {
  if (buttonEventQueue == nullptr) return;

  ButtonEvent event;
  if (xQueueReceive(buttonEventQueue, &event, 0) != pdPASS) return;

  if (event.type == BUTTON_EVENT_HOLD) {
    resetButtonInput("hold_deep_sleep", BUTTON_POST_ACTION_COOLDOWN_MS);
    prepareDeepSleep();
    return;
  }

  if (event.type != BUTTON_EVENT_CLICKS || event.clicks == 0) return;

  int clicks = event.clicks;
  if (clicks > BUTTON_MAX_CLICK_COUNT) {
    dropButtonGesture("noise_clicks");
    return;
  }

  if (!isModeChangeAllowed()) {
    dropButtonGesture("state_busy");
    return;
  }

  if ((clicks == 1 || clicks == 2) && currentMode == MODE_KOMUNIKASI) {
    // Single/double click di MODE_KOMUNIKASI: ONLINE <-> SET WIFI
    if (isWifiPortalActive()) {
      // SET WIFI -> MODE ONLINE
      consumeButtonGesture("comm_portal_to_online");
      stopWifiPortal();

      lcd.clear();
      lcdPrint16(0, "  MODE: ONLINE  ");
      lcdPrint16(1, "  KONEK WIFI");

      playBuzzer(1, 60, 60);
      waitBuzzerDone();

      pendingWifiReconnect = true;
      lastWifiAttempt = millis();
      wifiDibatalkan = false;
      lastWifiLine = "";
      lastState = (SystemState)-1;
    } 
    else {
      // MODE ONLINE -> SET WIFI
      consumeButtonGesture("comm_online_to_portal");
      masukModeDebug();
    }
  }

  else if (clicks == 1) {
    dropButtonGesture("single_ignored");
  }

  else if (clicks == 2) {
    // Double click di cycle rekam:
    // DOSEN <-> MAHASISWA (switch = 1 beep pendek)
    consumeButtonGesture("double_mode_action");
    beepSwitch(false);
    if (currentMode == MODE_DOSEN) {
      setOperatingMode(MODE_MAHASISWA);
    }
    else if (currentMode == MODE_MAHASISWA) {
      setOperatingMode(MODE_DOSEN);
    }
  }

  else if (clicks == 3) {
    // Triple click (switch cycle = 2 beep pendek):
    // CYCLE PEREKAMAN -> KOMUNIKASI
    // KOMUNIKASI -> DOSEN
    consumeButtonGesture("triple_mode_action");
    beepSwitch(true);
    if (currentMode == MODE_KOMUNIKASI) {
      if (isWifiPortalActive()) {
        stopWifiPortal();
      }
      setOperatingMode(MODE_DOSEN);
    }
    else {
      setOperatingMode(MODE_KOMUNIKASI);
    }
  }
}

// VARIABEL GLOBAL & KONFIGURASI BATERAI

// Variabel untuk filter dan memori persentase
float smoothedVoltage = 0.0;
float lastReportedPercent = 100.0; 

// LOOK-UP TABLE (LUT) BATERAI
struct BatteryProfile {
  float voltage;
  float percentage;
};

// Calibrated for Wintonic 2500mAh + MAX17048 ~50mV offset
const int NUM_LUT_POINTS = 21; 
const BatteryProfile lipo_LUT[NUM_LUT_POINTS] = {
  {4.220, 100.0},
  {4.124, 95.0},
  {4.086, 90.0},
  {4.065, 85.0},
  {4.034, 80.0},
  {3.995, 75.0},
  {3.950, 70.0},
  {3.903, 65.0},
  {3.852, 60.0},
  {3.800, 55.0},
  {3.751, 50.0},
  {3.706, 45.0},
  {3.668, 40.0},
  {3.636, 35.0},
  {3.611, 30.0},
  {3.584, 25.0},
  {3.554, 20.0},
  {3.498, 15.0},
  {3.428, 10.0},
  {3.214, 5.0},
  {3.000, 0.0}    // Batas bawah / cutoff baterai
};

// FUNGSI INTERPOLASI
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

// FUNGSI UTAMA PENGECEKAN BATERAI
void checkBattery() {

  // PEMBACAAN DAN PENGOLAHAN SENSOR (Tiap 5 Detik)
  if (millis() - lastBatteryCheck >= BATTERY_CHECK_INTERVAL) {
    lastBatteryCheck = millis();

    bool batteryReadOK = false;

    // Baca MAX17048 dengan proteksi mutex I2C
    if (lockI2C(20)) {
      batteryVoltage = maxlipo.cellVoltage() + batteryOffset;
      unlockI2C();
      batteryReadOK = true;
    }

    // Kalau I2C sedang sibuk, skip pembacaan baterai siklus
    // LED lanjut pakai nilai terakhir
    if (batteryReadOK) {
      if (isnan(batteryVoltage)) {
        // Jika tidak ada baterai / USB power
        batteryPercent = NAN;
        smoothedVoltage = 0.0;
        lastReportedPercent = 100.0;
      } 
      else {
        // Low-pass filter / EMA pada tegangan
        if (smoothedVoltage == 0.0) {
          smoothedVoltage = batteryVoltage;
        } else {
          smoothedVoltage = (0.1 * batteryVoltage) + (0.9 * smoothedVoltage);
        }

        // Hitung persentase dari LUT
        float rawPercent = getBatteryPercentageFromLUT(smoothedVoltage);

        // Monotonic logic agar persentase tidak naik-turun sendiri
        if (rawPercent < lastReportedPercent) {
          lastReportedPercent = rawPercent;
        } 
        else if (rawPercent - lastReportedPercent > 10.0) {
          // Lonjakan besar dianggap indikasi charging / dicolok
          lastReportedPercent = rawPercent;
        }

        batteryPercent = lastReportedPercent;
      }

      // Cetak status hanya kalau pembacaan berhasil

      if (isnan(batteryPercent)) {
      } else {
      }

    }
  }

  // UPDATE LED BATERAI (Jalan terus di loop)
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

  // UPDATE LED WIFI
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_WIFI, HIGH);
  } else {
    digitalWrite(LED_WIFI, LOW);
  }
}

void mulaiAuthorizedWindow(const String &uid) {
  currentUID = uid;
  authorizedSettleStarted = 0;

  resetVadState();
  clearPreRecordBuffer();
  resetAudioFilters();

  waktuMulai = millis();

  lastCountdown = -1;
  lastState = (SystemState)-1;

  state = AUTHORIZED;
}

void mulaiAuthorizedBeep(const String &uid) {
  currentUID = uid;

  resetVadState();
  clearPreRecordBuffer();
  resetAudioFilters();

  lastCountdown = -1;
  lastState = (SystemState)-1;

  authorizedSettleStarted = 0;
  state = AUTHORIZED_BEEP;
  playBuzzer(1, AUTHORIZED_BEEP_ON_MS, AUTHORIZED_BEEP_OFF_MS);
}

//  SETUP
void setup() {
  Serial.begin(115200);
  delay(50);   // beri waktu monitor attach

  pinMode(EN_POWER, OUTPUT);
  digitalWrite(EN_POWER, HIGH); // nyalakan power rail

  // NVS wajib pertama
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
  } else {
  }
    
  batteryVoltage = maxlipo.cellVoltage() + batteryOffset;
  batteryPercent = maxlipo.cellPercent();

  lcd.init();
  lcd.backlight();
  lcdInitCustomChars();
  lcd.clear();
  lcdPrint16(0, "   BOOTING...   ");
  lcdPrint16(1, "                ");

  initPN532();
  initAudioSD();
  muatDaftarEligible();
  muatDaftarBanned();
  initMPU6050();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  initBuzzer();
  startButtonTask();
  delay(1000);

  // Portal konfigurasi WiFi
  initWifiPortal();

  Preferences prefs;
  prefs.begin("catch_note", false);
  maxRecordMs = prefs.getULong("max_record_ms", 300000); 
  active_threshold = prefs.getInt("threshold", 300);
  prefs.end();

  // Setup MQTT server (dari nilai yang disimpan portal)
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

  // Default boot mulai di MODE_KOMUNIKASI
  currentMode = MODE_KOMUNIKASI;
  state = STANDBY;
  
  // Mode komunikasi memakai WiFi/MQTT; audio dilepas agar RAM longgar.
  deinitAudio();
  ensurePN532Ready(0);
  startWiFi();
  pendingWifiReconnect = false;
  wifiDibatalkan = false;
  lastWifiLine = "";
  lastMqttStatus = mqttClient.connected();
  waktuMulai = millis();

  // Reset timer komunikasi untuk heartbeat/registrasi awal.
  lastRegTime = 0;
  lastHeartbeatTime = 0;
  
  // Paksa layar untuk update di putaran loop pertama
  lastState = (SystemState)-1; 
} 

void loop() {
  if (currentMode == MODE_KOMUNIKASI && isWifiPortalActive()) {
    updateBuzzer();

    if (isWifiPortalActive()) {
      handleWifiPortal();
    }

    checkModeButton();

    return;
  }

  updateBuzzer();

  if (pendingWifiReconnect) {
    pendingWifiReconnect = false;
    startWiFi();
    lastRegTime       = 0;
    lastHeartbeatTime = 0;
    lastState         = (SystemState)-1;
  }

  checkModeButton();
  checkBattery();
  updateLCD();

  // MODE_KOMUNIKASI: MQTT + heartbeat
  if (currentMode == MODE_KOMUNIKASI) {
    if (isWifiPortalActive()) {
      handleWifiPortal();
    } 
    else {
      handleWiFi();

      if (WiFi.status() == WL_CONNECTED && !isTimeSyncing()) {
        handleMQTT();

        if (!isMQTTConnecting() &&
            mqttClient.connected() &&
            millis() - lastHeartbeatTime >= HEARTBEAT_MS) {
          lastHeartbeatTime = millis();
          kirimStatus("online");
        }
      }
    }
  }

  // VAD (Voice Activity Detection) pre-buffer
  // Pre-buffer tetap aktif di luar MODE_KOMUNIKASI, kecuali saat beep/settle.
  // Trigger VAD hanya dipakai saat state == AUTHORIZED.
  float maxLoudness = 0;
  bool vadTriggered = false;
  unsigned long vadFirstSpeechMsThisLoop = 0;
  unsigned long vadFirstSoftSpeechMsThisLoop = 0;
  unsigned long vadHardTriggerMsThisLoop = 0;

  if (currentMode != MODE_KOMUNIKASI && state != RECORDING &&
      state != AUTHORIZED_BEEP &&
      state != AUTHORIZED_SETTLE &&
      rx_handle != nullptr &&
      raw_i2s_buffer != nullptr &&
      processed_buffer != nullptr &&
      preRecordBuffer != nullptr) {

    size_t bytes_read = 0;

    while (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN,
                            &bytes_read, 0) == ESP_OK && bytes_read > 0) {

      int samples = bytes_read / 4;

      bool frameTriggered = updateAudioPreBufferAndVad(samples, maxLoudness);

      // VAD score hanya berlaku ketika sudah authorized.
      // Di STANDBY, suara luar tetap masuk pre-buffer, tapi tidak membangun skor trigger.
      if (state == AUTHORIZED && frameTriggered) {
        vadTriggered = true;
        vadHardTriggerMsThisLoop = millis();

        vadFirstSpeechMsThisLoop = getVadFirstSpeechMs();
        vadFirstSoftSpeechMsThisLoop = getVadFirstSoftSpeechMs();

        if (vadFirstSpeechMsThisLoop == 0) {
          vadFirstSpeechMsThisLoop = millis();
        }

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
          if (!pastikanSdOperasional("mhs_standby", false)) {
            break;
          }

          if (!ensurePN532Ready()) {
            playBuzzer(3, 80, 80);
            stateTimer = millis();
            state = RFID_ERROR;
            break;
          }

          String idKartu = scanUID();
          if (idKartu != "") {

            QuestionStatus questionStatus = QUESTION_SD_ERROR;
            if (pastikanSdOperasional("mhs_uid", true)) {
              questionStatus = cekStatusPertanyaan();
            }

            // CEK 0: Apakah SD Card terbaca dengan baik?
            if (questionStatus == QUESTION_SD_ERROR) {
              playBuzzer(1, 300, 80);
              masukSdError("mhs_uid_or_question");
            }

            // CEK 0.5: Apakah Dosen sudah bikin soal?
            else if (questionStatus == QUESTION_NONE) {
              playBuzzer(2, 150, 100);
              stateTimer = millis();
              state = NO_QUESTION;
            }

            // CEK 0.75: Cache kelas wajib ada agar mode Mahasiswa tetap strict.
            else if (jumlahTerdaftar == 0) {
              playBuzzer(2, 80, 80);
              stateTimer = millis();
              state = DATA_ERROR;
            }

            // CEK 1: Apakah dia peserta kelas ini
            else if (!cekTerdaftar(idKartu)) {
              playBuzzer(2, 80, 80);
              stateTimer = millis();
              state = UNREGISTERED;
            }

            // CEK 2: Apakah dia kena BANNED dari web
            else if (cekBanned(idKartu)) {
              playBuzzer(2, 80, 80);
              stateTimer = millis();
              state = BANNED;
            }

            // CEK 3: Apakah dia sudah bicara di sesi lokal SAAT INI
            else if (cekHistory(idKartu)) {
              // identTimer_stop(DEC_INVALID);
              playBuzzer(2, 80, 80);
              stateTimer = millis();
              state = INVALID; 
            } 

            // LOLOS SEMUA CEK: Izinkan Merekam
            else {
              mulaiAuthorizedBeep(idKartu);
            }
          }
          break;
        }

        // MODE DOSEN
        case MODE_DOSEN: {
          currentUID = "DOSEN";
          if (millis() - waktuMulai > (unsigned long)(DOSEN_COUNTDOWN * 1000)) {
            
            // CEK SD CARD
            if (!pastikanSdOperasional("dosen_record_start", true)) {
              playBuzzer(1, 300, 80);
              waktuMulai = millis(); // Reset countdown dosen setelah error
            } 
            else {
              mulaiAuthorizedWindow("DOSEN");
            }
          }
          break;
        }

        // MODE KOMUNIKASI
        case MODE_KOMUNIKASI: {
          if (isWifiPortalActive()) {
            break;
          }

          // Sinkronisasi TXT (trigger dari mqttCallback)
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

          // Sinkronisasi Audio WAV (trigger dari mqttCallback)
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

          // Mencegah PN532 mengganggu I2C LCD saat layar masih "KONEK SERVER".
          if (WiFi.status() != WL_CONNECTED || isTimeSyncing() || isMQTTConnecting()) {
            break;
          }
          if (!mqttClient.connected()) {
            break;
          }

          // Scan RFID → kirim registrasi
          if (!ensurePN532Ready()) {
            playBuzzer(3, 80, 80);
            stateTimer = millis();
            state = RFID_ERROR;
            break;
          }

          String idKartu = scanUIDPeriodik(100);
          if (idKartu != "") {
            if (millis() - lastRegTime > 2000) { // Anti-spam 2 detik
              if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
                sendRegistration(idKartu);
                lastRegTime = millis();
                playBuzzer(1, 60, 60);
              } else {
                lcdPrint16(1, " Koneksi Gagal! ");
                playBuzzer(2, 60, 90);   // 2 beep pendek
                waitBuzzerDone();        
                lastState = (SystemState)-1;
              }
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
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) {
        if (!isSdReady()) {
          requestSdRecovery("sd_error_recovery");
          break;
        }

        if (currentMode == MODE_MAHASISWA) {
          DataLoadStatus eligibleStatus = muatDaftarEligible();
          DataLoadStatus bannedStatus = muatDaftarBanned();

          if (eligibleStatus == DATA_LOAD_SD_ERROR || bannedStatus == DATA_LOAD_SD_ERROR) {
            masukSdError("sd_recovery_load");
          } else if (eligibleStatus == DATA_LOAD_EMPTY) {
            tandaiDataMahasiswaKosong();
          } else {
            state = STANDBY;
            lastState = (SystemState)-1;
          }
        } else {
          state = STANDBY;
          lastState = (SystemState)-1;
        }
      }
      break;
    }

    case RFID_ERROR: {
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) state = STANDBY;
      break;
    }

    case DATA_ERROR: {
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) state = STANDBY;
      break;
    }

    case NO_QUESTION: { 
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) state = STANDBY;
      break;
    }

    case AUTHORIZED_BEEP: {
      if (!isBuzzerBusy()) {
        if (!pastikanSdOperasional("authorized_beep", true)) {
          break;
        }

        String uid = currentUID;
        if (uid == "") {
          authorizedSettleStarted = 0;
          state = STANDBY;
        } else {
          authorizedSettleStarted = millis();
          drainAudioInput();
          clearPreRecordBuffer();
          resetVadState();
          resetAudioFilters();
          lastState = (SystemState)-1;
          state = AUTHORIZED_SETTLE;
        }
      }
      break;
    }

    case AUTHORIZED_SETTLE: {
      if (!pastikanSdOperasional("authorized_settle", false)) {
        break;
      }

      drainAudioInput();
      clearPreRecordBuffer();

      if (currentUID == "") {
        authorizedSettleStarted = 0;
        state = STANDBY;
        break;
      }

      if (authorizedSettleStarted == 0) {
        authorizedSettleStarted = millis();
      }

      if (millis() - authorizedSettleStarted >= AUTHORIZED_SETTLE_MS) {
        String uid = currentUID;
        authorizedSettleStarted = 0;
        drainAudioInput();
        clearPreRecordBuffer();
        resetVadState();
        resetAudioFilters();
        mulaiAuthorizedWindow(uid);
      }
      break;
    }

    case AUTHORIZED: {
      if (!pastikanSdOperasional("authorized_wait", false)) {
        break;
      }

      if (vadTriggered) {
        unsigned long softFirstSpeechMs = vadFirstSoftSpeechMsThisLoop;
        unsigned long hardFirstSpeechMs = vadFirstSpeechMsThisLoop;
        unsigned long hardTriggerMs = vadHardTriggerMsThisLoop;

        if (softFirstSpeechMs == 0) {
          softFirstSpeechMs = getVadFirstSoftSpeechMs();
        }

        if (hardFirstSpeechMs == 0) {
          hardFirstSpeechMs = getVadFirstSpeechMs();
        }

        if (hardTriggerMs == 0) {
          hardTriggerMs = millis();
        }

        unsigned long firstSpeechMs = softFirstSpeechMs;

        if (firstSpeechMs == 0) {
          firstSpeechMs = hardFirstSpeechMs;
        }

        if (firstSpeechMs == 0) {
          firstSpeechMs = hardTriggerMs;
        }

        if (firstSpeechMs < waktuMulai) {
          firstSpeechMs = waktuMulai;
        }

        unsigned long kandidatWaktuRespon = firstSpeechMs - waktuMulai;

        if (kandidatWaktuRespon <= TIMEOUT_BICARA) {
          waktuRespon = kandidatWaktuRespon;

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

      RecordingResult recordingResult = rekamSuara(currentUID, waktuRespon);

      clearButtonState(); // buang tombol yang kepencet selama rekaman
      resetAudioFilters();

      if (recordingResult == RECORDING_SD_ERROR) {
        playBuzzer(1, 300, 80);
        masukSdError("recording_result");
        break;
      }

      playBuzzer(2, 60, 60);

      if (currentMode == MODE_MAHASISWA) simpanHistory(currentUID);

      if (recordingResult == RECORDING_THROWN && currentMode == MODE_DOSEN) {
          setOperatingMode(MODE_MAHASISWA);
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
