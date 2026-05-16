// AudioSD_Module.cpp

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
static bool isSdInitialized = false; // TAMBAHAN BARU: Flag agar SD Card tidak init berkali-kali

// Variabel Internal
static float hpf_prev_in = 0;
static float hpf_prev_out = 0;
static const float hpf_alpha = 0.98;

// Variabel Tracker File
static int qIndex = 0; // Nomor pertanyaan dosen
static int aIndex = 0; // Nomor jawaban mahasiswa

void saveTracker() {
    SD.remove("/tracker.txt");
    File f = SD.open("/tracker.txt", FILE_WRITE);
    if (f) {
        f.println(qIndex);
        f.println(aIndex);
        f.close();
    }
}

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
// Inisialisasi nama file rekaman dengan Tracker File
void initFileIndex() {
    Serial.println("Membaca Index dari SD Card...");
    unsigned long startScan = millis();

    // 1. Coba baca dari file tracker
    if (SD.exists("/tracker.txt")) {
        File f = SD.open("/tracker.txt", FILE_READ);
        if (f) {
            qIndex = f.readStringUntil('\n').toInt();
            aIndex = f.readStringUntil('\n').toInt();
            f.close();
            
            if (qIndex >= 0 && aIndex >= 0) {
                Serial.printf("[BOOT CEPAT] Index Q: %d, A: %d\n", qIndex, aIndex);
                Serial.print("Waktu Boot: "); Serial.print(millis() - startScan); Serial.println(" ms");
                return; 
            }
        }
    }

    // 2. Jika file tidak ada, lakukan Full Scan
    Serial.println("[WARNING] Tracker tidak ditemukan. Memulai full scan...");
    File root = SD.open("/");
    int maxQ = 0;
    int maxA = 0;

    while (true) {
        File entry = root.openNextFile();
        if (!entry) break; 
        
        String name = String(entry.name());
        
        if (name.startsWith("DSN_")) {
            // Format: DSN_{device_id}_{no_pertanyaan}.ext
            int firstU = name.indexOf('_');
            int secondU = name.indexOf('_', firstU + 1);
            int dotPos = name.lastIndexOf('.');
            if (firstU != -1 && secondU != -1 && dotPos != -1) {
                int q = name.substring(secondU + 1, dotPos).toInt();
                if (q > maxQ) maxQ = q;
            }
        } else if (name.startsWith("MHS_")) {
            // Format: MHS_{device_id}_{no_pertanyaan}_{no_jawaban}.ext
            int firstU = name.indexOf('_');
            int secondU = name.indexOf('_', firstU + 1);
            int thirdU = name.indexOf('_', secondU + 1);
            int dotPos = name.lastIndexOf('.');
            if (firstU != -1 && secondU != -1 && thirdU != -1 && dotPos != -1) {
                int q = name.substring(secondU + 1, thirdU).toInt();
                int a = name.substring(thirdU + 1, dotPos).toInt();
                if (q > maxQ) maxQ = q;
                if (a > maxA) maxA = a;
            }
        }
        entry.close();
    }
    root.close();

    qIndex = maxQ;
    aIndex = maxA;
    
    // 3. Buat file tracker baru
    saveTracker();

    Serial.printf("Scan selesai dalam: %lu ms\n", millis() - startScan);
    Serial.printf("Index diset -> Q: %d, A: %d\n", qIndex, aIndex);
}

// Membongkar memori Audio untuk membuat space mode komunikasi
void deinitAudio() {
    // 1. Matikan dan bebaskan channel I2S (DMA)
    if (rx_handle != nullptr) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
    }

    // 2. Bebaskan RAM dari malloc
    if (preRecordBuffer != nullptr) { free(preRecordBuffer); preRecordBuffer = nullptr; }
    if (raw_i2s_buffer != nullptr) { free(raw_i2s_buffer); raw_i2s_buffer = nullptr; }
    if (processed_buffer != nullptr) { free(processed_buffer); processed_buffer = nullptr; }
    
    bufferHead = 0;
    bufferIsFull = false;

    Serial.println("[RAM] Memori Audio (I2S & Buffers) berhasil dibebaskan.");
}

// Inisialisasi modul Audio dan SD Card
// Inisialisasi modul Audio dan SD Card
void initAudioSD() {
    deinitAudio(); // Bersihkan sisa memori jika ada

    // Alokasi memori buffer
    preRecordBuffer = (int16_t *)malloc(PRE_BUFFER_SIZE * sizeof(int16_t));
    raw_i2s_buffer = (int32_t *)malloc(I2S_READ_LEN); 
    processed_buffer = (int16_t *)malloc((I2S_READ_LEN / 4) * 2);

    if (!preRecordBuffer || !raw_i2s_buffer || !processed_buffer) {
        Serial.println("Gagal Alokasi RAM Audio");
        return;
    }

    memset(preRecordBuffer, 0, PRE_BUFFER_SIZE * sizeof(int16_t));

    // Inisialisasi SD Card HANYA SEKALI
    if (!isSdInitialized) {
        SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS); 
        // Menggunakan 4MHz untuk mencegah brownout saat WiFi nyala
        if (!SD.begin(SD_CS, SPI, 4000000)) { 
            Serial.println("SD Card Gagal");
        } else {
            isSdInitialized = true;
            initFileIndex();
        }
    }


    // Konfigurasi I2S untuk INMP441 (32-bit MSB, mono)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    chan_cfg.dma_desc_num = 12;    // Lebih hemat RAM
    chan_cfg.dma_frame_num = 512; 

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

    Serial.println("[RAM] Memori Audio berhasil dirakit ulang.");
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

void tulisMetadata(String filename, String uid, unsigned long waktuBerpikir, int currentQ, int currentA) {
    String metaFile = filename;
    metaFile.replace(".wav", ".txt");

    File file = SD.open(metaFile, FILE_WRITE);
    if (file) {
        if (uid == "DOSEN") {
            // --- Format DSN_x.txt (2 Baris) ---
            String tanggal = "DD-MM-YYYY"; // Fallback jika belum pernah connect wifi
            if (SD.exists("/tanggal.txt")) {
                File tFile = SD.open("/tanggal.txt", FILE_READ);
                if (tFile) {
                    tanggal = tFile.readStringUntil('\n');
                    tanggal.trim(); 
                    tFile.close();
                }
            }
            file.println(currentQ);  // Baris 1: qIndex
            file.println(tanggal);   // Baris 2: Tanggal dari tanggal.txt

        } else {
            // --- Format MHS_x_y.txt (4 Baris Tetap) ---
            file.println(currentQ);       // Baris 1: qIndex
            file.println(currentA);       // Baris 2: aIndex
            file.println(uid);            // Baris 3: UID Mahasiswa
            file.println(waktuBerpikir);  // Baris 4: Waktu diam (ms)
        }
        file.close();
    }
}

// Fungsi record
void rekamSuara(String uid, unsigned long waktuBerpikir) {
    String prefix = (uid == "DOSEN") ? "DSN" : "MHS";
    int currentQ = qIndex;
    int currentA = aIndex;

    if (prefix == "DSN") {
        currentQ++; // Dosen membuat pertanyaan baru
    } else {
        currentA++; // Mahasiswa membuat jawaban baru
    }

    String filename;
    if (prefix == "DSN") {
        filename = "/DSN_" + String(DEVICE_ID) + "_" + String(currentQ) + ".wav";
    } else {
        filename = "/MHS_" + String(DEVICE_ID) + "_" + String(currentQ) + "_" + String(currentA) + ".wav";
    }
    
    File file = SD.open(filename, FILE_WRITE); 
    if(!file) {
        Serial.println("Gagal membuka file untuk rekam");
        return;
    }

    uint8_t blank[44] = {0}; 
    file.write(blank, 44);

    size_t totalDataWritten = 0; // Tambahkan counter data
    
    // Tulis Pre Buffer 3 detik 
    if (bufferIsFull) {
          int part1Len = PRE_BUFFER_SIZE - bufferHead;
          totalDataWritten += file.write((uint8_t*)&preRecordBuffer[bufferHead], part1Len * 2);
          totalDataWritten += file.write((uint8_t*)&preRecordBuffer[0], bufferHead * 2);
        } else {
          totalDataWritten += file.write((uint8_t*)&preRecordBuffer[0], bufferHead * 2);
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
    size_t written = file.write((uint8_t *)processed_buffer, samples * 2);
    totalDataWritten += written;

    if (written == 0 && samples > 0) {
        Serial.println("Error: Gagal menulis ke SD!");
        isRecording = false;
    }

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

    // PENGECEKAN RAM TANPA MENGGANGGU AUDIO (Tiap 2 Detik)
    static unsigned long lastRamCheck = 0; 
    if (now - lastRamCheck > 2000) { 
        Serial.printf("[AUDIO] RAM Sisa saat Merekam: %d bytes (%.2f KB)\n", 
                      ESP.getFreeHeap(), ESP.getFreeHeap() / 1024.0);
        lastRamCheck = now;
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

  if (totalDataWritten > 0) {
        lcd.clear();
        lcd.setCursor(0, 1);
        lcd.print(" SAVING FILE... ");
        long dataSize = file.size() - 44;
        writeWavHeader(file, dataSize);
        file.flush(); // Paksa data keluar dari buffer RAM ke fisik SD
        Serial.print("File Saved: "); Serial.print(filename);
        Serial.print(" Size: "); Serial.println(file.size());
    }

  file.close();
  if (prefix == "DSN") {
      qIndex = currentQ;
  } else {
      aIndex = currentA;
  }

  saveTracker();
  
  tulisMetadata(filename, uid, waktuBerpikir, currentQ, currentA);

  lcd.clear(); 

  if (thrown) {
    lcd.setCursor(0, 0);
    lcd.print(" ALAT DILEMPAR "); 
  } else {
    lcd.setCursor(0, 0);
    lcd.print(" RECORD SELESAI ");
  }

  delay(1500); 
}