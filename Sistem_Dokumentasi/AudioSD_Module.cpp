#include "AudioSD_Module.h"
#include <SPI.h>
#include <SD.h>
#include <driver/i2s_std.h>
#include "MPU6050_Module.h"

// Definisi Variabel Global
i2s_chan_handle_t rx_handle = nullptr; // Handle channel I2S (receive)
int16_t *preRecordBuffer    = nullptr; // Pre buffer rekaman
int16_t *processed_buffer   = nullptr; // Buffer hasil proses (PCM 16-bit)
int32_t *raw_i2s_buffer     = nullptr; // Raw buffer dari I2S (32-bit)

int bufferHead = 0;
bool bufferIsFull = false;

// Variabel Internal
static float hpf_prev_in = 0;
static float hpf_prev_out = 0;
static const float hpf_alpha = 0.98;
static int nextFileIndex = 1;

// Struktur wav header
struct WavHeader {
  char riff_tag[4]; uint32_t riff_length; char wave_tag[4];
  char fmt_tag[4];  uint32_t fmt_length;  uint16_t audio_format;
  uint16_t num_channels; uint32_t sample_rate; uint32_t byte_rate;
  uint16_t block_align;  uint16_t bits_per_sample; char data_tag[4];
  uint32_t data_length;
};

// Fungsi penulis header wav
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

// Inisialisasi nama file rekaman
void initFileIndex() {
  while (true) {
    String filename = "/rec_" + String(nextFileIndex) + ".wav";
    if (!SD.exists(filename)) break;
    nextFileIndex++;
  }
}

// Fungsi mengambil nama file terbaru
String getFastFilename(String prefix) {
  String name = "/" + prefix + "_" + String(nextFileIndex) + ".wav";
  nextFileIndex++; 
  return name;
}

// Inisialisasi modul Audio dan SD Card
void initAudioSD() {
  // Alokasi memori buffer
  preRecordBuffer = (int16_t *)malloc(PRE_BUFFER_SIZE * sizeof(int16_t));
  raw_i2s_buffer = (int32_t *)malloc(I2S_READ_LEN); 
  processed_buffer = (int16_t *)malloc((I2S_READ_LEN / 4) * 2);

  if (!preRecordBuffer || !raw_i2s_buffer || !processed_buffer) {
    Serial.println("Gagal Alokasi RAM Audio");
    return;
  }

  memset(preRecordBuffer, 0, PRE_BUFFER_SIZE * sizeof(int16_t));

  // Inisialisasi SD Card
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS); 

  if (!SD.begin(SD_CS, SPI, 20000000)) {
    Serial.println("SD Card Gagal");
  }

  initFileIndex();

  // Konfigurasi I2S untuk INMP441 (32-bit MSB, mono)
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
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
}

// Reset filter
void resetAudioFilters() {
    hpf_prev_in = 0;
    hpf_prev_out = 0;
}

// Proses buffer
float processAudioBuffer(int32_t *input, int16_t *output, int samples, long &sumLoudness) {
  sumLoudness = 0;
  for (int i = 0; i < samples; i++) {
    int32_t sample = input[i] >> BIT_SHIFT; 

    // High Pass Filter (DC Blocking)
    float filtered = hpf_alpha * (hpf_prev_out + sample - hpf_prev_in);
    
    hpf_prev_in = sample;
    hpf_prev_out = filtered;

    // Hard Limiter
    if (filtered > 32700) filtered = 32700;
    if (filtered < -32700) filtered = -32700;

    output[i] = (int16_t)filtered;
    sumLoudness += abs((int)filtered);
  }
  return (float)(sumLoudness / samples);
}

void tulisMetadata(String filename, String uid, unsigned long waktuBerpikir) {
    // Mengubah .wav menjadi .txt
    String metaFile = filename;
    metaFile.replace(".wav", ".txt");

    File file = SD.open(metaFile, FILE_WRITE);
    if (file) {
        file.println(uid);
        file.print(waktuBerpikir); file.println(" ms");
        file.close();
    }
}
// Funsgi record
void rekamSuara(String uid, unsigned long waktuBerpikir) {
    String prefix = (uid == "DOSEN") ? "DSN" : "MHS";

    String filename = getFastFilename(prefix);
    tulisMetadata(filename, uid, waktuBerpikir);

    File file = SD.open(filename, FILE_WRITE);
    if(!file) return;

    uint8_t blank[44] = {0}; file.write(blank, 44);
    
    // Tulis Pre Buffer 3 detik 
    if (bufferIsFull) {
      int part1Len = PRE_BUFFER_SIZE - bufferHead;
      file.write((uint8_t*)&preRecordBuffer[bufferHead], part1Len * 2);
      file.write((uint8_t*)&preRecordBuffer[0], bufferHead * 2);
    } else {
      file.write((uint8_t*)&preRecordBuffer[0], bufferHead * 2);
    }

    unsigned long startTime = millis();
    unsigned long lastSoundTime = millis();
    bool isRecording = true;
    bool thrown = false; 
    size_t bytes_read;

while (isRecording) {

  if (isDeviceFlying()) {
      thrown = true;
      isRecording = false;
      resetFlyingFlag();
      break;
  }
  if (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN, &bytes_read, 100) == ESP_OK) {

    int samples = bytes_read / 4;
    long sumLoudness = 0;

    float avgLoudness = processAudioBuffer(
        raw_i2s_buffer,
        processed_buffer,
        samples,
        sumLoudness);

    // Tulis 16-bit PCM ke SD
    file.write((uint8_t *)processed_buffer, samples * 2);

    unsigned long now = millis();
    unsigned long elapsed = now - startTime;

    // Hitung sisa waktu
    long sisaMs = MAX_RECORD_MS - elapsed;
    if (sisaMs < 0) sisaMs = 0;

    int menit = sisaMs / 60000;
    int detik = (sisaMs % 60000) / 1000;

    // Silence detection
    float sisaHening =
      (SILENCE_LIMIT_MS - (now - lastSoundTime)) / 1000.0;

    if (sisaHening < 0) sisaHening = 0;

    // Update LCD tiap 250 ms
    static unsigned long lastLCD = 0;
    if (now - lastLCD > 250) {

      lcd.setCursor(0, 0);
      lcd.print("SISA: ");

      if (menit < 10) lcd.print("0");
      lcd.print(menit);
      lcd.print(":");

      if (detik < 10) lcd.print("0");
      lcd.print(detik);
      lcd.print("   ");

      lcd.setCursor(0, 1);
      lcd.print("STOP IN: ");
      lcd.print(sisaHening, 1);
      lcd.print("s   ");

      lastLCD = now;
    }

    // LOGIKA STOP
    // Reset timer hening jika ada suara
    if (avgLoudness > SILENCE_THRESHOLD) {
      lastSoundTime = now;
    }
    else if (now - lastSoundTime > SILENCE_LIMIT_MS) {
      isRecording = false;
    }

    // Stop jika durasi habis
    if (elapsed >= MAX_RECORD_MS) {
      isRecording = false;
    }
  }
}

  // Save File
  lcd.clear(); 

  if (thrown) {
    lcd.setCursor(0, 0);
    lcd.print(" ALAT DILEMPAR "); 
  } else {
    lcd.setCursor(0, 0);
    lcd.print(" RECORD SELESAI ");
  }

  lcd.setCursor(0, 1);
  lcd.print(" SAVING FILE... ");

  long dataSize = file.size() - 44;
  writeWavHeader(file, dataSize);
  file.close();

  delay(1500); 
}