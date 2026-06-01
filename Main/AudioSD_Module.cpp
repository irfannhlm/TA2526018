// AudioSD_Module.cpp

#include "AudioSD_Module.h"
#include <SPI.h>
#include <SD.h>
#include <driver/i2s_std.h>
#include <math.h>
#include "MPU6050_Module.h"
#include "I2C_Handler.h"
#include "LCD_Helper.h"

// =========================
// Variabel Global Audio
// =========================
i2s_chan_handle_t rx_handle = nullptr; // Handle channel I2S receive
int16_t *preRecordBuffer    = nullptr; // Pre-buffer rekaman 16-bit PCM
int16_t *processed_buffer   = nullptr; // Buffer hasil proses 16-bit PCM
int32_t *raw_i2s_buffer     = nullptr; // Raw buffer dari I2S 32-bit

extern int active_threshold;

int bufferHead = 0;
bool bufferIsFull = false;
static bool isSdInitialized = false;

// =========================
// High-pass Filter State
// =========================
static float hpf_prev_in = 0;
static float hpf_prev_out = 0;
static const float hpf_alpha = 0.98f;

// =========================
// Tracker File
// =========================
static int qIndex = 0; // Nomor pertanyaan dosen
static int aIndex = 0; // Nomor jawaban mahasiswa

// =========================
// VAD Ringan Hemat RAM
// =========================
struct VadFeatures {
    float avgAbs;
    float rms;
    float zcr;
    float crest;
    int peak;
    int samples;
};

// Skor agar VAD tidak trigger hanya karena 1 frame keras.
static int vadScore = 0;
static unsigned long vadMuteUntil = 0;
static unsigned long vadFirstSpeechMs = 0;

#ifndef VAD_TRIGGER_SCORE
#define VAD_TRIGGER_SCORE 18
#endif

#ifndef VAD_MAX_SCORE
#define VAD_MAX_SCORE 30
#endif

static const float VAD_START_THRESHOLD_MULT  = 1.40f;
static const float VAD_RECORD_THRESHOLD_MULT = 0.60f;

static const float VAD_ZCR_MIN_START  = 0.020f;
static const float VAD_ZCR_MAX_START  = 0.180f;

static const float VAD_ZCR_MIN_RECORD = 0.010f;
static const float VAD_ZCR_MAX_RECORD = 0.320f;

static const float VAD_CREST_REJECT_START  = 6.0f;
static const float VAD_CREST_REJECT_RECORD = 12.0f;

void resetVadState() {
    vadScore = 0;
    vadFirstSpeechMs = 0;
}

unsigned long getVadFirstSpeechMs() {
    return vadFirstSpeechMs;
}

void muteVad(unsigned long ms) {
    vadMuteUntil = millis() + ms;
}

static bool isVadMuted() {
    return millis() < vadMuteUntil;
}

bool isSpeechLikeFrame(const VadFeatures &vf) {
    if (isVadMuted()) return false;
    if (vf.samples <= 0) return false;

    if (vf.avgAbs < active_threshold * VAD_START_THRESHOLD_MULT) return false;

    // Reject suara impulsif keras
    if (vf.peak > active_threshold * 12 && vf.crest > 5.0f) return false;

    // Reject bentuk sinyal terlalu spike
    if (vf.crest > VAD_CREST_REJECT_START) return false;

    // Reject sinyal terlalu low-frequency atau terlalu tajam
    if (vf.zcr < VAD_ZCR_MIN_START || vf.zcr > VAD_ZCR_MAX_START) return false;

    return true;
}

bool isOngoingSpeechFrame(const VadFeatures &vf) {
    if (isVadMuted()) return false;
    if (vf.samples <= 0) return false;

    if (vf.avgAbs < active_threshold * VAD_RECORD_THRESHOLD_MULT) return false;
    if (vf.crest > VAD_CREST_REJECT_RECORD) return false;
    if (vf.zcr < VAD_ZCR_MIN_RECORD || vf.zcr > VAD_ZCR_MAX_RECORD) return false;

    return true;
}

bool updateVadTrigger(const VadFeatures &vf) {
    bool speechLike = isSpeechLikeFrame(vf);
    unsigned long now = millis();

    // Estimasi waktu awal frame, bukan waktu setelah frame selesai diproses.
    unsigned long frameDurationMs = 0;
    if (vf.samples > 0) {
        frameDurationMs = ((unsigned long)vf.samples * 1000UL) / SAMPLE_RATE;
    }

    unsigned long frameStartMs = now;
    if (now > frameDurationMs) {
        frameStartMs = now - frameDurationMs;
    }

    if (speechLike) {
        // Catat awal kandidat suara pertama.
        // Ini hanya diset saat skor masih 0 agar tidak bergeser ke frame berikutnya.
        if (vadScore == 0 && vadFirstSpeechMs == 0) {
            vadFirstSpeechMs = frameStartMs;
        }

        vadScore += 2;
    } else {
        // Suara keras impulsif harus cepat menghapus skor.
        if (vf.avgAbs > active_threshold * 2.0f && vf.crest > VAD_CREST_REJECT_START) {
            vadScore -= 8;
        } else {
            vadScore -= 3;
        }

        // Kalau kandidat suara gagal total, hapus waktu awalnya.
        if (vadScore <= 0) {
            vadScore = 0;
            vadFirstSpeechMs = 0;
        }
    }

    if (vadScore > VAD_MAX_SCORE) {
        vadScore = VAD_MAX_SCORE;
    }

    return vadScore >= VAD_TRIGGER_SCORE;
}

void clearPreRecordBuffer() {
    if (preRecordBuffer != nullptr) {
        memset(preRecordBuffer, 0, PRE_BUFFER_SIZE * sizeof(int16_t));
    }

    bufferHead = 0;
    bufferIsFull = false;
}

// WAV Header
struct WavHeader {
    char riff_tag[4];
    uint32_t riff_length;
    char wave_tag[4];
    char fmt_tag[4];
    uint32_t fmt_length;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_tag[4];
    uint32_t data_length;
};

void writeWavHeader(File &file, int dataSize) {
    WavHeader header;
    memcpy(header.riff_tag, "RIFF", 4);
    header.riff_length = dataSize + 36;
    memcpy(header.wave_tag, "WAVE", 4);
    memcpy(header.fmt_tag, "fmt ", 4);
    header.fmt_length = 16;
    header.audio_format = 1;
    header.num_channels = 1;
    header.sample_rate = SAMPLE_RATE;
    header.byte_rate = SAMPLE_RATE * 2;
    header.block_align = 2;
    header.bits_per_sample = 16;
    memcpy(header.data_tag, "data", 4);
    header.data_length = dataSize;

    file.seek(0);
    file.write((uint8_t *)&header, sizeof(WavHeader));
}

// Tracker
void saveTracker() {
    SD.remove("/tracker.txt");
    File f = SD.open("/tracker.txt", FILE_WRITE);
    if (f) {
        f.println(qIndex);
        f.println(aIndex);
        f.close();
    }
}

void initFileIndex() {
    Serial.println("Membaca Index dari SD Card...");
    unsigned long startScan = millis();

    // 1. Coba baca dari file tracker.
    if (SD.exists("/tracker.txt")) {
        File f = SD.open("/tracker.txt", FILE_READ);
        if (f) {
            qIndex = f.readStringUntil('\n').toInt();
            aIndex = f.readStringUntil('\n').toInt();
            f.close();

            if (qIndex >= 0 && aIndex >= 0) {
                Serial.printf("[BOOT CEPAT] Index Q: %d, A: %d\n", qIndex, aIndex);
                Serial.print("Waktu Boot: ");
                Serial.print(millis() - startScan);
                Serial.println(" ms");
                return;
            }
        }
    }

    // 2. Jika tracker tidak ada / tidak valid, lakukan full scan.
    Serial.println("[WARNING] Tracker tidak ditemukan/tidak valid. Memulai full scan...");

    File root = SD.open("/");
    if (!root) {
        Serial.println("[ERROR] Gagal membuka root SD untuk scan index.");
        qIndex = 0;
        aIndex = 0;
        return;
    }

    int maxQ = 0;
    int maxA = 0;

    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;

        String name = String(entry.name());
        entry.close();

        // Beberapa implementasi SD bisa mengembalikan nama dengan awalan "/".
        if (name.startsWith("/")) {
            name.remove(0, 1);
        }

        int dotPos = name.lastIndexOf('.');
        if (dotPos == -1) continue;

        // Format: DSN_<DEVICE_ID>_<Q>.wav
        if (name.startsWith("DSN_")) {
            int lastU = name.lastIndexOf('_');

            if (lastU != -1 && lastU < dotPos) {
                int q = name.substring(lastU + 1, dotPos).toInt();

                if (q > maxQ) {
                    maxQ = q;
                }
            }
        }

        // Format: MHS_<DEVICE_ID>_<Q>_<A>.wav
        else if (name.startsWith("MHS_")) {
            int lastU = name.lastIndexOf('_');
            int prevU = name.lastIndexOf('_', lastU - 1);

            if (prevU != -1 && lastU != -1 && lastU < dotPos) {
                int q = name.substring(prevU + 1, lastU).toInt();
                int a = name.substring(lastU + 1, dotPos).toInt();

                if (q > maxQ) {
                    maxQ = q;
                }

                if (a > maxA) {
                    maxA = a;
                }
            }
        }
    }

    root.close();

    qIndex = maxQ;
    aIndex = maxA;

    saveTracker();

    Serial.printf("Scan selesai dalam: %lu ms\n", millis() - startScan);
    Serial.printf("Index diset -> Q: %d, A: %d\n", qIndex, aIndex);
}

bool cekAdaPertanyaan() {
    File root = SD.open("/");
    if (!root) return false;

    bool adaDSN = false;

    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;

        String name = String(entry.name());
        entry.close();

        // Samakan format nama file, karena kadang SD mengembalikan "/nama.wav"
        if (name.startsWith("/")) {
            name.remove(0, 1);
        }

        name.trim();

        if (name.startsWith("DSN_") && name.endsWith(".wav")) {
            adaDSN = true;
            break;
        }
    }

    root.close();
    return adaDSN;
}

// Init / Deinit Audio
void deinitAudio() {
    if (rx_handle != nullptr) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
    }

    if (preRecordBuffer != nullptr) {
        free(preRecordBuffer);
        preRecordBuffer = nullptr;
    }
    if (raw_i2s_buffer != nullptr) {
        free(raw_i2s_buffer);
        raw_i2s_buffer = nullptr;
    }
    if (processed_buffer != nullptr) {
        free(processed_buffer);
        processed_buffer = nullptr;
    }

    bufferHead = 0;
    bufferIsFull = false;
    resetVadState();

    Serial.println("[RAM] Memori Audio (I2S & Buffers) berhasil dibebaskan.");
}

void initAudioSD() {
    deinitAudio();

    preRecordBuffer = (int16_t *)malloc(PRE_BUFFER_SIZE * sizeof(int16_t));
    raw_i2s_buffer = (int32_t *)malloc(I2S_READ_LEN);
    processed_buffer = (int16_t *)malloc((I2S_READ_LEN / 4) * sizeof(int16_t));

    if (!preRecordBuffer || !raw_i2s_buffer || !processed_buffer) {
        Serial.println("Gagal Alokasi RAM Audio");
        Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
        Serial.printf("Max alloc heap: %u\n", ESP.getMaxAllocHeap());
        deinitAudio();
        return;
    }

    memset(preRecordBuffer, 0, PRE_BUFFER_SIZE * sizeof(int16_t));
    resetVadState();
    resetAudioFilters();

    if (!isSdInitialized) {
        SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

        if (!SD.begin(SD_CS, SPI, 4000000)) {
            Serial.println("SD Card Gagal");
        } else {
            isSdInitialized = true;
            initFileIndex();
        }
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 12;
    chan_cfg.dma_frame_num = 512;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK || rx_handle == nullptr) {
        Serial.printf("I2S channel gagal dibuat: %d\n", err);
        deinitAudio();
        return;
    }

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

    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("I2S init std mode gagal: %d\n", err);
        deinitAudio();
        return;
    }

    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        Serial.printf("I2S enable gagal: %d\n", err);
        deinitAudio();
        return;
    }

    Serial.println("[RAM] Memori Audio berhasil dirakit ulang.");
    Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
    Serial.printf("Max alloc heap: %u\n", ESP.getMaxAllocHeap());
}

void resetAudioFilters() {
    hpf_prev_in = 0;
    hpf_prev_out = 0;
}

// =========================
// Audio Processing + VAD Feature Extraction
// =========================
float processAudioBufferVAD(
    int32_t *input,
    int16_t *output,
    int samples,
    long &sumLoudness,
    VadFeatures &vf
) {
    sumLoudness = 0;

    if (samples <= 0) {
        vf.avgAbs = 0;
        vf.rms = 0;
        vf.zcr = 0;
        vf.crest = 0;
        vf.peak = 0;
        vf.samples = 0;
        return 0;
    }

    int64_t sumSquare = 0;
    int peak = 0;
    int zeroCross = 0;
    int16_t prev = 0;
    bool hasPrev = false;

    for (int i = 0; i < samples; i++) {
        int32_t sample = input[i] >> BIT_SHIFT;

        // High-pass filter / DC blocking.
        float filtered = hpf_alpha * (hpf_prev_out + sample - hpf_prev_in);
        hpf_prev_in = sample;
        hpf_prev_out = filtered;

        // Hard limiter untuk mencegah clipping overflow saat konversi ke int16.
        if (filtered > 32700) filtered = 32700;
        if (filtered < -32700) filtered = -32700;

        int16_t y = (int16_t)filtered;
        output[i] = y;

        int absY = abs((int)y);
        sumLoudness += absY;
        sumSquare += (int64_t)y * (int64_t)y;

        if (absY > peak) peak = absY;

        if (hasPrev) {
            if ((prev >= 0 && y < 0) || (prev < 0 && y >= 0)) {
                zeroCross++;
            }
        }
        prev = y;
        hasPrev = true;
    }

    float avgAbs = (float)sumLoudness / (float)samples;
    float rms = sqrtf((float)sumSquare / (float)samples);
    float zcr = (samples > 1) ? ((float)zeroCross / (float)(samples - 1)) : 0.0f;
    float crest = (rms > 1.0f) ? ((float)peak / rms) : 0.0f;

    vf.avgAbs = avgAbs;
    vf.rms = rms;
    vf.zcr = zcr;
    vf.crest = crest;
    vf.peak = peak;
    vf.samples = samples;

    return avgAbs;
}


float processAudioBuffer(int32_t *input, int16_t *output, int samples, long &sumLoudness) {
    VadFeatures vf;
    return processAudioBufferVAD(input, output, samples, sumLoudness, vf);
}

// Helper untuk pre-buffer di Main. Setelah i2s_channel_read() mengisi raw_i2s_buffer,
// panggil fungsi ini untuk proses audio, simpan ke preRecordBuffer, dan update skor VAD.
// Return true jika VAD sudah cukup yakin ada suara manusia.
bool updateAudioPreBufferAndVad(int samples, float &maxLoudness) {
    if (samples <= 0 || raw_i2s_buffer == nullptr || processed_buffer == nullptr || preRecordBuffer == nullptr) {
        return false;
    }

    long dummySum = 0;
    VadFeatures vf;
    float chunkLoudness = processAudioBufferVAD(raw_i2s_buffer, processed_buffer, samples, dummySum, vf);

    static unsigned long lastVadLog = 0;
    if (millis() - lastVadLog > 300) {
    Serial.printf("[VAD] avg=%.1f rms=%.1f zcr=%.3f crest=%.2f peak=%d score=%d th=%d\n",
                  vf.avgAbs, vf.rms, vf.zcr, vf.crest, vf.peak, vadScore, active_threshold);
    lastVadLog = millis();
}

    if (chunkLoudness > maxLoudness) {
        maxLoudness = chunkLoudness;
    }

    bool vadTriggered = updateVadTrigger(vf);

    for (int i = 0; i < samples; i++) {
        preRecordBuffer[bufferHead] = processed_buffer[i];
        bufferHead++;
        if (bufferHead >= PRE_BUFFER_SIZE) {
            bufferHead = 0;
            bufferIsFull = true;
        }
    }

    return vadTriggered;
}

// =========================
// Metadata
// =========================
void tulisMetadata(String filename, String uid, unsigned long waktuBerpikir, int currentQ, int currentA) {
    String metaFile = filename;
    metaFile.replace(".wav", ".txt");

    File file = SD.open(metaFile, FILE_WRITE);
    if (file) {
        if (uid == "DOSEN") {
            String tanggal = "DD-MM-YYYY";
            if (SD.exists("/tanggal.txt")) {
                File tFile = SD.open("/tanggal.txt", FILE_READ);
                if (tFile) {
                    tanggal = tFile.readStringUntil('\n');
                    tanggal.trim();
                    tFile.close();
                }
            }

            file.println(currentQ);
            file.println(tanggal);
        } else {
            file.println(currentQ);
            file.println(currentA);
            file.println(uid);
            file.println(waktuBerpikir);
        }
        file.close();
    }
}

// =========================
// Rekam Suara
// =========================
bool rekamSuara(String uid, unsigned long waktuBerpikir) {
    String prefix = (uid == "DOSEN") ? "DSN" : "MHS";
    int currentQ = qIndex;
    int currentA = aIndex;

    if (prefix == "DSN") {
        currentQ++;
    } else {
        currentA++;
    }

    String filename;
    if (prefix == "DSN") {
        filename = "/DSN_" + String(DEVICE_ID) + "_" + String(currentQ) + ".wav";
    } else {
        filename = "/MHS_" + String(DEVICE_ID) + "_" + String(currentQ) + "_" + String(currentA) + ".wav";
    }

    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println("Gagal membuka file untuk rekam");
        return false;
    }

    uint8_t blank[44] = {0};
    file.write(blank, 44);

    size_t totalDataWritten = 0;

    // Tulis pre-buffer terlebih dahulu agar awal suara tidak terpotong.
    if (preRecordBuffer != nullptr) {
        if (bufferIsFull) {
            int part1Len = PRE_BUFFER_SIZE - bufferHead;
            totalDataWritten += file.write((uint8_t *)&preRecordBuffer[bufferHead], part1Len * sizeof(int16_t));
            totalDataWritten += file.write((uint8_t *)&preRecordBuffer[0], bufferHead * sizeof(int16_t));
        } else {
            totalDataWritten += file.write((uint8_t *)&preRecordBuffer[0], bufferHead * sizeof(int16_t));
        }
    }

    unsigned long startTime = millis();
    unsigned long totalWaktuBerpikir = waktuBerpikir; // hidden, untuk metadata
    bool isRecording = true;
    bool thrown = false;
    size_t bytes_read = 0;

    // Saat sudah mulai rekam, skor VAD untuk trigger awal tidak dibutuhkan.
    resetVadState();

    startThrowDetectionTask();

    while (isRecording) {
        if (throwDetected) {
            thrown = true;
            isRecording = false;
            break;
        }

        if (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN, &bytes_read, 100) == ESP_OK && bytes_read > 0) {
            int samples = bytes_read / 4;
            long sumLoudness = 0;

            VadFeatures vf;
            float avgLoudness = processAudioBufferVAD(
                raw_i2s_buffer,
                processed_buffer,
                samples,
                sumLoudness,
                vf
            );

            (void)avgLoudness; // avgLoudness tetap dihitung untuk kompatibilitas/debug jika dibutuhkan.

            // Tulis 16-bit PCM ke SD.
            size_t expected = samples * sizeof(int16_t);
            size_t written = file.write((uint8_t *)processed_buffer, expected);
            totalDataWritten += written;

            if (written != expected) {
                Serial.printf("Error: SD write tidak lengkap! expected=%u written=%u\n",
                            (unsigned)expected,
                            (unsigned)written);
                isRecording = false;
            }

            unsigned long now = millis();
            unsigned long elapsed = now - startTime;

            long sisaMs = maxRecordMs - elapsed;
            if (sisaMs < 0) sisaMs = 0;

            int menit = sisaMs / 60000;
            int detik = (sisaMs % 60000) / 1000;

            // Update LCD tiap 500 ms.
            static unsigned long lastLCD = 0;
            if (now - lastLCD > 500) {
                if (lockI2C(20)) {
                    lcd.setCursor(0, 0);
                    lcd.print(" SEDANG MEREKAM");

                    lcd.setCursor(0, 1);
                    lcd.print("SISA WAKTU ");

                    if (menit < 10) lcd.print("0");
                    lcd.print(menit);
                    lcd.print(":");

                    if (detik < 10) lcd.print("0");
                    lcd.print(detik);

                    unlockI2C();
                }

                lastLCD = now;
            }

            // Pengecekan RAM tiap 2 detik, tidak setiap loop.
            static unsigned long lastRamCheck = 0;
            if (now - lastRamCheck > 2000) {
                Serial.printf("[AUDIO] RAM Sisa saat Merekam: %u bytes (%.2f KB)\n",
                              ESP.getFreeHeap(), ESP.getFreeHeap() / 1024.0f);
                Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
                Serial.printf("Max alloc heap: %u\n", ESP.getMaxAllocHeap());
                lastRamCheck = now;
            }

            // Saat recording, VAD hanya dipakai untuk mengukur apakah frame ini speech atau hening.
            unsigned long frameDurationMs = 0;
            if (samples > 0) {
                frameDurationMs = ((unsigned long)samples * 1000UL) / SAMPLE_RATE;
            }

            bool speechNow = isOngoingSpeechFrame(vf);
            if (!speechNow) {
                totalWaktuBerpikir += frameDurationMs;
            }

            if (elapsed >= maxRecordMs) {
                isRecording = false;
            }
        }
    }

    stopThrowDetectionTask();
    resetFlyingFlag();

    if (totalDataWritten > 0) {
        long dataSize = file.size() - 44;
        writeWavHeader(file, dataSize);
        file.flush();
    }

    file.close();

    if (prefix == "DSN") {
        qIndex = currentQ;
    } else {
        aIndex = currentA;
    }

    saveTracker();

    Serial.println("--------- METADATA ---------");
    Serial.printf("[META] UID                : %s\n", uid.c_str());
    Serial.printf("[META] waktuResponAwal    : %lu ms\n", waktuBerpikir);
    Serial.printf("[META] waktuBerpikirTotal : %lu ms\n", totalWaktuBerpikir);
    Serial.println("----------------------------");

    tulisMetadata(filename, uid, totalWaktuBerpikir, currentQ, currentA);

    lcd.clear();

    if (thrown) {
        lcd.setCursor(0, 0);
        lcd.print(" ALAT DILEMPAR ");
        lcd.setCursor(0, 1);
        lcd.print("REKAMAN SELESAI");
    } 
    else {
        lcd.setCursor(0, 0);
        lcd.print("  WAKTU HABIS  ");
        lcd.setCursor(0, 1);
        lcd.print("REKAMAN SELESAI");
    }

    delay(1500);

    return thrown;
}
