#include "Config.h"
#include "PN532_Module.h"
#include "AudioSD_Module.h"
#include "MPU6050_Module.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#define MAX_PESERTA 50
#define ERROR_DISPLAY_TIME 2000

LiquidCrystal_I2C lcd(0x27, 16, 2);

String daftarSudahBicara[MAX_PESERTA];
int jumlahPeserta = 0;

enum OperatingMode { MODE_MAHASISWA, MODE_DOSEN, MODE_KOMUNIKASI };
volatile OperatingMode currentMode = MODE_MAHASISWA;

enum SystemState {
  STANDBY,
  INVALID,
  AUTHORIZED,
  RECORDING
};

SystemState state = STANDBY;
SystemState lastState = (SystemState)-1;

const int LONG_PRESS_TIME = 2000; // 2 detik untuk Deep Sleep

volatile bool buttonFlag = false;
volatile unsigned long lastTriggerTime = 0;

unsigned long stateTimer = 0;
String currentUID = "";

unsigned long waktuMulai = 0;
unsigned long waktuRespon = 0;
int lastCountdown = -1;

void IRAM_ATTR handleButtonInterrupt() {
  buttonFlag = true;
}

// Cek apakah sudah pernah bicara
bool cekHistory(String uid) {
  for (int i = 0; i < jumlahPeserta; i++) {
    if (daftarSudahBicara[i] == uid) return true;
  }
  return false;
}

// Update history
void simpanHistory(String uid) {
  if (jumlahPeserta < MAX_PESERTA) {
    daftarSudahBicara[jumlahPeserta++] = uid;
  }
}

// Switching Mode
void checkModeButton() {
  if (!buttonFlag) return; // Jika tidak ada interrupt, langsung keluar

  // Debouncing sederhana
  if (millis() - lastTriggerTime < 200) {
    buttonFlag = false;
    return;
  }

  // LOGIKA HOLD UNTUK DEEP SLEEP
  unsigned long startHold = millis();
  while (digitalRead(BUTTON_PIN) == LOW) {
    if (millis() - startHold > LONG_PRESS_TIME) {
      prepareDeepSleep();
    }
  }

  // LOGIKA KLIK SWITCHING MODE-
  if (currentMode == MODE_MAHASISWA) {
    currentMode = MODE_DOSEN;
  } else if (currentMode == MODE_DOSEN) {
    currentMode = MODE_KOMUNIKASI;
  } else {
    currentMode = MODE_MAHASISWA;
  }

  state = STANDBY;
  lastState = (SystemState)-1;
  lastCountdown = -1; // Agar LCD langsung refresh ke angka countdown
  waktuMulai = millis(); // Reset waktuMulai tiap ganti mode

  // Feedback Buzzer
  for(int i=0; i <= (int)currentMode; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(40); 
    digitalWrite(BUZZER_PIN, LOW);  delay(40);
  }

  lastTriggerTime = millis();
  buttonFlag = false; // Reset flag
}

void prepareDeepSleep() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("MEMATIKAN...");
  delay(1000);
  lcd.noBacklight();
  lcd.noDisplay();

  // Feedback buzzer panjang
  digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);

  // Membangunkan saat pin kembali ke LOW (ditekan)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0); 

  Serial.println("Entering Deep Sleep");
  esp_deep_sleep_start();
}

void updateLCD() {
  unsigned long durasi = millis() - waktuMulai;
  int countdownSekarang = DOSEN_COUNTDOWN - (durasi / 1000);
  if (countdownSekarang < 0) countdownSekarang = 0;

  // Refresh jika countdown berubah di Mode Dosen Standby
  bool isCountdownDosen = (state == STANDBY && currentMode == MODE_DOSEN && countdownSekarang > 0);
  
  if (state == lastState && !isCountdownDosen) return;
  if (isCountdownDosen && countdownSekarang == lastCountdown) return;

  lcd.clear();
  lastCountdown = countdownSekarang;

  switch (state) {
    case STANDBY:
        if (currentMode == MODE_MAHASISWA) {
        lcd.setCursor(0, 0); lcd.print("  [MODE: MHS]  ");
        lcd.setCursor(0, 1); lcd.print("  MENUNGGU TAG  ");
      }
      else if (currentMode == MODE_DOSEN) {
        lcd.setCursor(0, 0); lcd.print(" [MODE: DOSEN] ");
        lcd.setCursor(0, 1); lcd.print(" Countdown: " + String(countdownSekarang) + "s");
      } 
      else if (currentMode == MODE_KOMUNIKASI) {
        lcd.setCursor(0, 0); lcd.print("  [MODE: KOM]  ");
        lcd.setCursor(0, 1); lcd.print(" Connecting... "); 
      }
      break;

    case AUTHORIZED:
      lcd.setCursor(0, 0);
      lcd.print("ID: ");
      lcd.print(currentMode == MODE_DOSEN ? "DOSEN" : currentUID.substring(0, 8));
      lcd.setCursor(0, 1);
      lcd.print("SILAKAN BICARA");
      break;

    case RECORDING:
      lcd.setCursor(0, 0);
      lcd.print(" >>> RECORDING" );
      lcd.setCursor(0, 1);
      lcd.print("     ------     ");
      break;

    case INVALID:
      lcd.setCursor(0, 0);
      lcd.print("   TIDAK VALID  ");
      lcd.setCursor(0, 1);
      lcd.print("  SUDAH BICARA  ");
      break;
  }

  lastState = state;
}

// SETUP
void setup() {

  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  lcd.init();
  lcd.backlight();
  lcd.print("   BOOTING...   ");

  initPN532();   
  initAudioSD();
  initMPU6050();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonInterrupt, FALLING); // Trigger saat tombol ditekan (FALLING karena pull-up)
  delay(1000);

  state = STANDBY;
}


// Main loop
void loop() {

  checkModeButton();
  updateLCD();

  switch (state) {
    case STANDBY: {
      switch (currentMode) {
        
        case MODE_MAHASISWA: {
          String idKartu = scanUID();
          if (idKartu != "") {
            if (cekHistory(idKartu)) {
              stateTimer = millis(); 
              state = INVALID;
            } else {
              currentUID = idKartu;
              waktuMulai = millis();
              state = AUTHORIZED;
              // Feedback suara sukses tap
              digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);
            }
          }
          break;
        }

        case MODE_DOSEN: {
          // Set UID dulu agar saat pindah state sudah siap
          currentUID = "DOSEN"; 

          // Hanya pindah ke AUTHORIZED jika sudah lewat countdown
          if (millis() - waktuMulai > (DOSEN_COUNTDOWN * 1000)) {
            state = AUTHORIZED;
            waktuMulai = millis(); 
          }
          break;
        }

        case MODE_KOMUNIKASI: {
          // Logic Mode Komunikasi 
          break;
        }
      }
      break;
    }

    case INVALID: {
      if (millis() - stateTimer > ERROR_DISPLAY_TIME) {
        state = STANDBY;
      }
      break;
    }

case AUTHORIZED: {
      // Logika VAD
      size_t bytes_read;
      if (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN, &bytes_read, 50) == ESP_OK) {
        long dummySum;
        int samples = bytes_read / 4;
        float avgLoudness = processAudioBuffer(raw_i2s_buffer, processed_buffer, samples, dummySum);

        // Update pre-record buffer
        for (int i = 0; i < samples; i++) {
          preRecordBuffer[bufferHead] = processed_buffer[i];
          bufferHead++;
          if (bufferHead >= PRE_BUFFER_SIZE) { bufferHead = 0; bufferIsFull = true; }
        }

        if (avgLoudness > SILENCE_THRESHOLD) {
          waktuRespon = millis() - waktuMulai;
          state = RECORDING;
        }
      }
      break;
    }

    case RECORDING: {
      rekamSuara(currentUID, waktuRespon);
      resetAudioFilters();
      
      // Simpan history hanya jika yang bicara adalah Mahasiswa
      if (currentMode == MODE_MAHASISWA) {
        simpanHistory(currentUID);
      }

      currentUID = "";
      state = STANDBY;

      waktuMulai = millis(); // Reset timer
      lastCountdown = -1;    // Paksa LCD untuk refresh tampilan countdown
      break;
    }
  }
}