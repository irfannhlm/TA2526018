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
OperatingMode currentMode = MODE_MAHASISWA;

enum SystemState {
  STANDBY,
  INVALID,
  AUTHORIZED,
  RECORDING
};

SystemState state = STANDBY;
SystemState lastState = (SystemState)-1;

unsigned long stateTimer = 0;
String currentUID = "";

unsigned long waktuMulai = 0;
unsigned long waktuRespon = 0;

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
  static unsigned long lastDebounce = 0;
  if (digitalRead(BUTTON_PIN) == LOW && (millis() - lastDebounce > 500)) {
    
    // Berpindah mode secara berurutan (Cycling)
    if (currentMode == MODE_MAHASISWA) {
      currentMode = MODE_DOSEN;
    } else if (currentMode == MODE_DOSEN) {
      currentMode = MODE_KOMUNIKASI;
    } else {
      currentMode = MODE_MAHASISWA;
    }

    state = STANDBY; // Reset state sistem setiap ganti mode
    lastState = (SystemState)-1; // LCD refresh
    
    // Feedback buzzer antar pergantian mode
    for(int i=0; i <= (int)currentMode; i++) {
      digitalWrite(BUZZER_PIN, HIGH); delay(40); 
      digitalWrite(BUZZER_PIN, LOW);  delay(40);
    }
    
    lastDebounce = millis();
  }
}

// UPDATE LCD
void updateLCD() {

  if (state == lastState) return;
  lcd.clear();

  switch (state) {

    case STANDBY:
      lcd.setCursor(0, 0);
      switch (currentMode) {
        case MODE_MAHASISWA:  lcd.print("[MODE: MHS]");    break;
        case MODE_DOSEN:      lcd.print("[MODE: DOSEN]");  break;
        // case MODE_KOMUNIKASI: lcd.print("[MODE: KOM]"); break;
      }

      lcd.setCursor(0, 1);
      switch (currentMode) {
        case MODE_MAHASISWA:  lcd.print("  MENUNGGU TAG  ");     break;
        case MODE_DOSEN:      lcd.print(" MENUNGGU SUARA ");   break;
        // case MODE_KOMUNIKASI: lcd.print("MENCARI PEER...");  break; 
      }
      break;

    case INVALID:
      lcd.setCursor(0,0);
      lcd.print("  TIDAK VALID  ");
      lcd.setCursor(0,1);
      lcd.print("  SUDAH BICARA  ");
      break;

    case AUTHORIZED:
      lcd.setCursor(0,0);
      lcd.print("ID:");
      lcd.print(currentUID.substring(0,10));
      lcd.setCursor(0,1);
      lcd.print("SILAKAN BICARA");
      break;

    case RECORDING:
      lcd.setCursor(0,0);
      lcd.print("RECORDING...");
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
          // MODE DOSEN: Langsung bypass tanpa scan kartu
          currentUID = "DOSEN"; 
          waktuMulai = millis();
          state = AUTHORIZED; 
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
      // Logika menunggu suara
      size_t bytes_read;
      if (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN, &bytes_read, 50) == ESP_OK) {
        long dummySum;
        int samples = bytes_read / 4;
        float avgLoudness = processAudioBuffer(raw_i2s_buffer, processed_buffer, samples, dummySum);

        // Isi pre-record buffer
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
      break;
    }
  }
}