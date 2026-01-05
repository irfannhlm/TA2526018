#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s_std.h>
#include <Wire.h> 
#include <LiquidCrystal_I2C.h>
#include <Adafruit_PN532.h>

// --- KONFIGURASI PIN ---
#define BUZZER_PIN  12

// SD Card SPI
#define SD_CS   5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_CLK  18

// I2S Microphone
#define I2S_SCK 16
#define I2S_WS  17
#define I2S_SD  4 

// I2C COMMON
#define I2C_SDA 21
#define I2C_SCL 22

// --- SETTINGS ---
#define SAMPLE_RATE 16000 
#define I2S_READ_LEN 1024 
#define MAX_CARDS 100

// --- DATABASE KARTU YANG DIIZINKAN (WHITELIST) ---
// Hanya kartu-kartu ini yang boleh masuk
const uint8_t whitelist[][4] = {
  {0x25, 0x03, 0x40, 0x06},
  {0x07, 0x1D, 0x48, 0x06},
  {0x15, 0xAE, 0x40, 0x06}
};
const int whitelistCount = 3; 

// --- OBJECTS ---
i2s_chan_handle_t rx_handle;
LiquidCrystal_I2C lcd(0x27, 16, 2); 
Adafruit_PN532 nfc(-1, -1); 

// Global Variables
int16_t *preRecordBuffer;       
int16_t *processed_buffer; 
int32_t *raw_i2s_buffer;   
int bufferHead = 0;             
bool bufferIsFull = false;      
int nextFileIndex = 1; 

struct Card {
  uint8_t uid[7];
  uint8_t uidLength;
};
Card knownCards[MAX_CARDS];
int totalSaved = 0;

// Filter Variables
float hpf_prev_in = 0;
float hpf_prev_out = 0;
const float hpf_alpha = 0.98;

#define BIT_SHIFT 14       
#define SILENCE_THRESHOLD 300    
#define SILENCE_LIMIT_MS 2500    
#define MAX_DURASI_DETIK 300    
#define PRE_BUFFER_SEC 3         
#define PRE_BUFFER_SIZE (SAMPLE_RATE * PRE_BUFFER_SEC) 

struct WavHeader {
  char riff_tag[4]; uint32_t riff_length; char wave_tag[4];
  char fmt_tag[4];  uint32_t fmt_length;  uint16_t audio_format;
  uint16_t num_channels; uint32_t sample_rate; uint32_t byte_rate;
  uint16_t block_align;  uint16_t bits_per_sample; char data_tag[4];
  uint32_t data_length;
};

// --- FUNGSI BUZZER ---
void beep(int count, int delayMs) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(delayMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(100);
  }
}

void beepLong() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(1000);
  digitalWrite(BUZZER_PIN, LOW);
}

void writeWavHeader(File &file, int dataSize) {
  WavHeader header;
  memcpy(header.riff_tag, "RIFF", 4); header.riff_length = dataSize + 36;
  memcpy(header.wave_tag, "WAVE", 4); memcpy(header.fmt_tag, "fmt ", 4);
  header.fmt_length = 16; header.audio_format = 1; header.num_channels = 1;
  header.sample_rate = SAMPLE_RATE; header.byte_rate = SAMPLE_RATE * 2;
  header.block_align = 2; header.bits_per_sample = 16;
  memcpy(header.data_tag, "data", 4); header.data_length = dataSize;
  file.seek(0); file.write((uint8_t*)&header, sizeof(WavHeader));
}

void initFileIndex() {
  while (true) {
    String filename = "/rec_" + String(nextFileIndex) + ".wav";
    if (!SD.exists(filename)) break;
    nextFileIndex++;
  }
}

String getFastFilename() {
  String name = "/rec_" + String(nextFileIndex) + ".wav";
  nextFileIndex++; 
  return name;
}

float processAudioBuffer(int32_t *input, int16_t *output, int samples, long &sumLoudness) {
  sumLoudness = 0;
  for (int i = 0; i < samples; i++) {
    int32_t sample = input[i];
    sample = sample >> BIT_SHIFT; 
    float filtered = hpf_alpha * (hpf_prev_out + sample - hpf_prev_in);
    hpf_prev_in = sample;
    hpf_prev_out = filtered;
    if (filtered > 32700) filtered = 32700;
    if (filtered < -32700) filtered = -32700;
    output[i] = (int16_t)filtered;
    sumLoudness += abs((int)filtered);
  }
  return (float)(sumLoudness / samples);
}

// --- FUNGSI REKAM (TIDAK DIUBAH) ---
void rekamSuara() {
  String filename = getFastFilename();
  lcd.clear(); lcd.setCursor(0, 0); lcd.print("REC START...");
  
  File file = SD.open(filename, FILE_WRITE);
  if(!file) { lcd.clear(); lcd.print("Err SD Wrt"); return; }
  
  uint8_t blank[44] = {0}; file.write(blank, 44);
  
  if (bufferIsFull) {
    int part1Len = PRE_BUFFER_SIZE - bufferHead;
    file.write((uint8_t*)&preRecordBuffer[bufferHead], part1Len * 2);
    file.write((uint8_t*)&preRecordBuffer[0], bufferHead * 2);
  } else {
    file.write((uint8_t*)&preRecordBuffer[0], bufferHead * 2);
  }
  bufferHead = 0; bufferIsFull = false;
 
  unsigned long startTime = millis();
  unsigned long lastSoundTime = millis();
  unsigned long lastLCDUpdate = 0; 
  bool isRecording = true;
  size_t bytes_read;
  bool timeOutTriggered = false;

  while (isRecording) {
    unsigned long now = millis();
    long elapsedSec = (now - startTime) / 1000;
    long sisaDurasiMax = MAX_DURASI_DETIK - elapsedSec; 
    
    if (sisaDurasiMax <= 0) { 
        isRecording = false; 
        timeOutTriggered = true; 
    }

    if(i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN, &bytes_read, 100) == ESP_OK) {
        if (bytes_read > 0) {
          int samples = bytes_read / 4;
          long sumLoudness = 0;
          float avgLoudness = processAudioBuffer(raw_i2s_buffer, processed_buffer, samples, sumLoudness);
          file.write((uint8_t *)processed_buffer, samples * 2);

          if (avgLoudness > SILENCE_THRESHOLD) { 
            lastSoundTime = now; 
          } else { 
            if (now - lastSoundTime > SILENCE_LIMIT_MS) {
              isRecording = false; 
            }
          }

          if (now - lastLCDUpdate > 200) { 
            long timeSinceLastSound = now - lastSoundTime;
            long sisaWaktuHening = SILENCE_LIMIT_MS - timeSinceLastSound;
            if (sisaWaktuHening < 0) sisaWaktuHening = 0;

            lcd.setCursor(0, 0); 
            lcd.print("Sisa Wkt: "); lcd.print(sisaDurasiMax); lcd.print("s   "); 
            lcd.setCursor(0, 1); 
            lcd.print("Hening: "); lcd.print(sisaWaktuHening / 1000.0, 1); lcd.print("s   ");

            lastLCDUpdate = now;
          }
        }
    }
  }

  long dataSize = file.size() - 44;
  writeWavHeader(file, dataSize);
  file.close();

  if (timeOutTriggered) { 
      lcd.clear(); lcd.print("WAKTU HABIS!"); 
      beepLong(); 
  }
  
  lcd.clear(); lcd.setCursor(0, 0); lcd.print("SAVED!");
  delay(1000); 
  lcd.clear(); lcd.setCursor(0, 0); lcd.print("STANDBY MODE");
  delay(1000);
}

// Fungsi Bantu Compare UID
bool compareUid(uint8_t* uid1, uint8_t len1, const uint8_t* uid2, uint8_t len2) {
  if (len1 != len2) return false;
  for (int i = 0; i < len1; i++) if (uid1[i] != uid2[i]) return false;
  return true;
}

void runAudioSession() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("SILAHKAN");
  lcd.setCursor(0, 1); lcd.print("BERBICARA...");
  
  hpf_prev_in = 0; hpf_prev_out = 0;
  bool sessionActive = true;
  unsigned long waitStart = millis();
  const unsigned long WAIT_TIMEOUT = 15000; 

  while (sessionActive) {
    size_t bytes_read;
    if (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN, &bytes_read, 100) == ESP_OK) {
       long dummySum;
       int samples = bytes_read / 4;
       float avgLoudness = processAudioBuffer(raw_i2s_buffer, processed_buffer, samples, dummySum); 

       for (int i=0; i<samples; i++) {
           preRecordBuffer[bufferHead] = processed_buffer[i];
           bufferHead++;
           if (bufferHead >= PRE_BUFFER_SIZE) { bufferHead = 0; bufferIsFull = true; }
       }
       
       if (avgLoudness > SILENCE_THRESHOLD) { 
           rekamSuara(); 
           sessionActive = false; 
       }
    }
    
    if (millis() - waitStart > WAIT_TIMEOUT) {
      lcd.clear(); lcd.print("TIDAK ADA SUARA"); 
      beep(2, 200); 
      delay(1000); 
      sessionActive = false;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

  // --- AUDIO FIX (PERTAHANKAN INI AGAR SUARA TIDAK HANCUR) ---
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000); // Speed 400kHz penting untuk audio jernih saat LCD update

  lcd.init(); lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("BOOTING...");

  preRecordBuffer = (int16_t *)malloc(PRE_BUFFER_SIZE * sizeof(int16_t));
  raw_i2s_buffer = (int32_t *)malloc(I2S_READ_LEN); 
  processed_buffer = (int16_t *)malloc((I2S_READ_LEN / 4) * 2); 
  if (!preRecordBuffer || !raw_i2s_buffer || !processed_buffer) {
    lcd.setCursor(0, 1); lcd.print("RAM FULL!"); while(1); 
  }
  memset(preRecordBuffer, 0, PRE_BUFFER_SIZE * sizeof(int16_t));

  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 20000000)) { 
     lcd.setCursor(0, 1); lcd.print("SD FAIL"); while(1); 
  }
  initFileIndex(); 

  // --- AUDIO FIX BUFFER BESAR ---
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 16;     // Pertahankan buffer besar
  chan_cfg.dma_frame_num = 512;   
  chan_cfg.auto_clear = true;
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)I2S_SCK,
          .ws = (gpio_num_t)I2S_WS,
          .dout = I2S_GPIO_UNUSED,
          .din = (gpio_num_t)I2S_SD,
      },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; 
  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);

  lcd.setCursor(0, 1); lcd.print("Init NFC...");
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    lcd.clear(); lcd.print("NFC Error!");
    while (1); 
  }
  nfc.SAMConfig();

  lcd.clear(); lcd.setCursor(0, 0); lcd.print("STANDBY...");
  delay(1000);
}

void loop() {
  lcd.setCursor(0, 0); lcd.print("TAP KARTU ANDA  ");
  lcd.setCursor(0, 1); lcd.print("UNTUK REKAM     ");

  boolean success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;

  // Baca NFC
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, &uid[0], &uidLength, 50);

  if (success) {
    
    // --- 1. CEK APAKAH UID ADA DI WHITELIST? ---
    bool isWhitelisted = false;
    for (int i = 0; i < whitelistCount; i++) {
        // Cek 4 byte pertama (sesuai format UID di atas)
        if (compareUid(uid, uidLength, whitelist[i], 4)) {
            isWhitelisted = true;
            break;
        }
    }

    if (!isWhitelisted) {
        // JIKA TIDAK ADA DI DAFTAR IZIN -> TOLAK
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("AKSES DITOLAK");
        lcd.setCursor(0, 1); lcd.print("ID TIDAK DIKENAL");
        beep(4, 50); // Beep error cepat
        delay(2000);
        return; // Kembali minta tap kartu
    }

    // --- 2. JIKA DI WHITELIST, CEK APAKAH SUDAH PERNAH REKAM? ---
    int foundIndex = -1;
    for (int i = 0; i < totalSaved; i++) {
      if (compareUid(uid, uidLength, knownCards[i].uid, knownCards[i].uidLength)) {
        foundIndex = i; break;
      }
    }

    if (foundIndex != -1) {
      // SUDAH PERNAH DIGUNAKAN
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("KARTU SUDAH");
      lcd.setCursor(0, 1); lcd.print("TERDAFTAR!");
      beep(3, 100); 
      delay(2000); 
    } else {
      // BOLEH MASUK (Whitelist OK & Belum pernah pakai)
      if (totalSaved < MAX_CARDS) {
        memcpy(knownCards[totalSaved].uid, uid, uidLength);
        knownCards[totalSaved].uidLength = uidLength;
        totalSaved++;
        
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("AKSES DITERIMA");
        lcd.setCursor(0, 1); lcd.print("SILAHKAN...");
        beep(1, 200);
        delay(1000);
        
        // Pindah ke Mode Audio
        runAudioSession();
      } else {
        lcd.clear(); lcd.print("MEMORI PENUH!"); beep(5, 50); delay(2000);
      }
    }
  }
}
    