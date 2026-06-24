// AudioSD_Module.cpp

#include "AudioSD_Module.h"
#include <SPI.h>
#include <SD.h>
#include <driver/i2s_std.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
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

// Didefinisikan di Main.ino. Cek permintaan stop manual rekaman (double click+).
extern bool pollManualStopRecording();

static int bufferHead = 0;
static bool bufferIsFull = false;
static volatile bool isSdInitialized = false;
static volatile bool sdRecoveryRequested = false;
static volatile bool sdRecoveryRunning = false;
static volatile bool sdRecoveryPaused = false;
static TaskHandle_t sdRecoveryTaskHandle = nullptr;
static unsigned long lastSdProbeMs = 0;
static unsigned long lastSdInitAttemptMs = 0;
static const unsigned long SD_PROBE_INTERVAL_MS = 500;
static const unsigned long SD_RETRY_INTERVAL_MS = 5000;

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

#define RECORDING_CAPTURE_SAMPLES (I2S_READ_LEN / 4)
#define RECORDING_CAPTURE_QUEUE_DEPTH 64
#define RECORDING_CAPTURE_READ_TIMEOUT_MS 20
#define RECORDING_CAPTURE_SEND_WAIT_MS 10
#define RECORDING_PRE_ROLL_SAMPLES ((int)PRE_BUFFER_SIZE)
#define RECORDING_SD_WRITE_BUFFER_BYTES 4096

struct RecordingAudioChunk {
    int samples;
    int16_t pcm[RECORDING_CAPTURE_SAMPLES];
    VadFeatures vf;
};

struct RecordingWriteBuffer {
    uint8_t *data;
    size_t used;
    size_t capacity;
};

struct RecordingGainStats {
    uint32_t samplesProcessed;
    uint32_t clippedSamples;
    int maxAbsBeforeGain;
    int maxAbsAfterGain;
};

struct RecordingSilenceTracker {
    unsigned long pendingSilenceMs;
    unsigned long pendingSpeechMs;
};

enum RecordingVadReason : uint8_t {
    RECORDING_VAD_SPEECH = 0,
    RECORDING_VAD_MUTED,
    RECORDING_VAD_EMPTY,
    RECORDING_VAD_THRESHOLD_OFF,
    RECORDING_VAD_CREST,
    RECORDING_VAD_LOW_ZCR,
    RECORDING_VAD_HIGH_ZCR,
    RECORDING_VAD_QUIET,
    RECORDING_VAD_REASON_COUNT
};

struct RecordingVadStats {
    unsigned long initialResponseMs;
    unsigned long speechMs;
    unsigned long nonSpeechMs;
    unsigned long committedSilenceMs;
    unsigned long ignoredShortSilenceMs;
    unsigned long absorbedSpeechBlipMs;
    unsigned long maxCommittedSegmentMs;
    uint32_t committedSegmentCount;
    uint32_t ignoredShortSegmentCount;
    uint32_t absorbedSpeechBlips;
    unsigned long reasonMs[RECORDING_VAD_REASON_COUNT];
    uint32_t reasonFrames[RECORDING_VAD_REASON_COUNT];
};

static QueueHandle_t recordingCaptureQueue = nullptr;
static TaskHandle_t recordingCaptureTaskHandle = nullptr;
static volatile bool recordingCaptureShouldRun = false;
static volatile uint32_t recordingDroppedChunks = 0; // diagnosis: chunk dibuang saat queue penuh
static volatile UBaseType_t recordingCaptureDepth = RECORDING_CAPTURE_QUEUE_DEPTH;
static uint8_t recordingSdWriteBuffer[RECORDING_SD_WRITE_BUFFER_BYTES];

// Skor agar VAD tidak trigger hanya karena 1 frame keras.
static int vadScore = 0;
static unsigned long vadMuteUntil = 0;
static unsigned long vadFirstSpeechMs = 0;
static unsigned long vadFirstSoftSpeechMs = 0;

#ifndef VAD_TRIGGER_SCORE
#define VAD_TRIGGER_SCORE 22
#endif

#ifndef VAD_MAX_SCORE
#define VAD_MAX_SCORE 30
#endif

static const float VAD_START_THRESHOLD_MULT  = 1.40f;
static const float VAD_RECORD_THRESHOLD_MULT = 0.32f;
static const float VAD_RECORD_RMS_THRESHOLD_MULT = 0.42f;
static const float VAD_RECORD_PEAK_MULT = 2.2f;

static const float VAD_ZCR_MIN_START  = 0.020f;
static const float VAD_ZCR_MAX_START  = 0.260f;
static const float VAD_ZCR_HIGH_START = 0.180f;

static const float VAD_ZCR_MIN_RECORD = 0.006f;
static const float VAD_ZCR_MAX_RECORD = 0.360f;

static const float VAD_CREST_REJECT_START  = 5.0f;
static const float VAD_CREST_REJECT_RECORD = 12.0f;

static const float VAD_PEAK_REJECT_MULT = 16.0f;
static const float VAD_IMPACT_AVG_MULT  = 2.0f;
static const float VAD_IMPACT_CREST_MIN = 3.5f;
static const float VAD_THUMP_AVG_MULT   = 8.0f;
static const float VAD_THUMP_PEAK_MULT  = 30.0f;
static const float VAD_THUMP_ZCR_MAX    = 0.070f;
static const float VAD_THUMP_CREST_MAX  = 3.5f;
static const float VAD_HIGH_ZCR_THRESHOLD_MULT = 1.8f;
static const float RECORDING_WAV_GAIN = 2.0f;

// Tail trim: potong noise di ekor rekaman (gedebuk tangkapan / klik tombol stop manual)
static const unsigned long RECORDING_THROW_TAIL_TRIM_MS  = 700UL; // tangkapan (gedebuk) bisa memantul/lag queue
static const unsigned long RECORDING_MANUAL_TAIL_TRIM_MS = 600UL; // klik + 300ms CLICK_EVALUATE_MS sebelum loop berhenti

static const unsigned long VAD_IMPACT_MUTE_MS = 320UL;
static const unsigned long VAD_RECORD_MIN_SILENCE_MS = 1350UL;
static const unsigned long VAD_RECORD_SPEECH_CONFIRM_MS = 200UL;

static int16_t recordingGainBuffer[RECORDING_CAPTURE_SAMPLES];

void resetVadState() {
    vadScore = 0;
    vadFirstSpeechMs = 0;
    vadFirstSoftSpeechMs = 0;
}

unsigned long getVadFirstSpeechMs() {
    return vadFirstSpeechMs;
}

unsigned long getVadFirstSoftSpeechMs() {
    return vadFirstSoftSpeechMs;
}

void muteVad(unsigned long ms) {
    vadMuteUntil = millis() + ms;
}

static bool isVadMuted() {
    return millis() < vadMuteUntil;
}

static const char *classifyImpactFrame(const VadFeatures &vf) {
    if (vf.samples <= 0) return nullptr;
    if (active_threshold <= 0) return nullptr;

    bool loudEnough = vf.avgAbs >= active_threshold * VAD_IMPACT_AVG_MULT;
    if (!loudEnough) return nullptr;

    bool currentRejectSpike = vf.peak > active_threshold * 12 && vf.crest > 5.0f;
    bool sharpCrest = vf.crest > VAD_CREST_REJECT_START;
    bool hardPeak = vf.peak > active_threshold * VAD_PEAK_REJECT_MULT &&
                    vf.crest >= VAD_IMPACT_CREST_MIN;
    bool denseThump = vf.avgAbs > active_threshold * 3.0f &&
                      vf.peak > active_threshold * 10 &&
                      vf.crest >= VAD_IMPACT_CREST_MIN;
    bool lowBoomThump = vf.avgAbs > active_threshold * VAD_THUMP_AVG_MULT &&
                        vf.peak > active_threshold * VAD_THUMP_PEAK_MULT &&
                        vf.zcr <= VAD_THUMP_ZCR_MAX &&
                        vf.crest <= VAD_THUMP_CREST_MAX;

    if (lowBoomThump) return "impact_thump";
    if (currentRejectSpike || sharpCrest || hardPeak || denseThump) return "impact_spike";
    return nullptr;
}

static bool classifySpeechFrame(const VadFeatures &vf, const char **reasonOut) {
    const char *reason = "reject";
    bool speechLike = false;

    if (isVadMuted()) {
        if (reasonOut != nullptr) {
            *reasonOut = "muted";
        }
        return false;
    }

    if (vf.samples <= 0) {
        reason = "empty";
    } else if (vf.avgAbs < active_threshold * VAD_START_THRESHOLD_MULT) {
        reason = "quiet";
    } else {
        const char *impactReason = classifyImpactFrame(vf);
        if (impactReason != nullptr) {
            reason = impactReason;
        } else if (vf.peak > active_threshold * 12 && vf.crest > 5.0f) {
            reason = "impact_spike";
        } else if (vf.crest > VAD_CREST_REJECT_START) {
            reason = "crest_reject";
        } else if (vf.zcr < VAD_ZCR_MIN_START) {
            reason = "low_zcr";
        } else if (vf.zcr > VAD_ZCR_MAX_START) {
            reason = "high_zcr_reject";
        } else if (vf.zcr > VAD_ZCR_HIGH_START) {
            if (vf.avgAbs >= active_threshold * VAD_HIGH_ZCR_THRESHOLD_MULT) {
                speechLike = true;
                reason = "high_zcr_speech";
            } else {
                reason = "high_zcr_quiet";
            }
        } else {
            speechLike = true;
            reason = "speech";
        }
    }

    if (reasonOut != nullptr) {
        *reasonOut = reason;
    }
    return speechLike;
}

static const char *recordingVadReasonName(RecordingVadReason reason) {
    switch (reason) {
        case RECORDING_VAD_SPEECH: return "speech";
        case RECORDING_VAD_MUTED: return "muted";
        case RECORDING_VAD_EMPTY: return "empty";
        case RECORDING_VAD_THRESHOLD_OFF: return "threshold_off";
        case RECORDING_VAD_CREST: return "crest";
        case RECORDING_VAD_LOW_ZCR: return "low_zcr";
        case RECORDING_VAD_HIGH_ZCR: return "high_zcr";
        case RECORDING_VAD_QUIET: return "quiet";
        default: return "unknown";
    }
}

bool isOngoingSpeechFrame(const VadFeatures &vf, RecordingVadReason *reasonOut = nullptr) {
    RecordingVadReason reason = RECORDING_VAD_QUIET;
    bool speech = false;

    if (isVadMuted()) {
        reason = RECORDING_VAD_MUTED;
    } else if (vf.samples <= 0) {
        reason = RECORDING_VAD_EMPTY;
    } else if (active_threshold <= 0) {
        reason = RECORDING_VAD_THRESHOLD_OFF;
    } else if (vf.crest > VAD_CREST_REJECT_RECORD) {
        reason = RECORDING_VAD_CREST;
    } else if (vf.zcr < VAD_ZCR_MIN_RECORD) {
        reason = RECORDING_VAD_LOW_ZCR;
    } else if (vf.zcr > VAD_ZCR_MAX_RECORD) {
        reason = RECORDING_VAD_HIGH_ZCR;
    } else {
        bool avgEnergy = vf.avgAbs >= active_threshold * VAD_RECORD_THRESHOLD_MULT;
        bool rmsEnergy = vf.rms >= active_threshold * VAD_RECORD_RMS_THRESHOLD_MULT;
        bool peakEnergy = vf.peak >= active_threshold * VAD_RECORD_PEAK_MULT;

        if (avgEnergy || rmsEnergy || peakEnergy) {
            reason = RECORDING_VAD_SPEECH;
            speech = true;
        } else {
            reason = RECORDING_VAD_QUIET;
        }
    }

    if (reasonOut != nullptr) {
        *reasonOut = reason;
    }

    return speech;
}

static void resetRecordingSilenceTracker(RecordingSilenceTracker &tracker) {
    tracker.pendingSilenceMs = 0;
    tracker.pendingSpeechMs = 0;
}

static void resetRecordingVadStats(RecordingVadStats &stats, unsigned long initialResponseMs) {
    stats.initialResponseMs = initialResponseMs;
    stats.speechMs = 0;
    stats.nonSpeechMs = 0;
    stats.committedSilenceMs = 0;
    stats.ignoredShortSilenceMs = 0;
    stats.absorbedSpeechBlipMs = 0;
    stats.maxCommittedSegmentMs = 0;
    stats.committedSegmentCount = 0;
    stats.ignoredShortSegmentCount = 0;
    stats.absorbedSpeechBlips = 0;

    for (int i = 0; i < RECORDING_VAD_REASON_COUNT; i++) {
        stats.reasonMs[i] = 0;
        stats.reasonFrames[i] = 0;
    }
}

static void absorbPendingSpeechBlip(RecordingSilenceTracker &tracker, RecordingVadStats &stats) {
    if (tracker.pendingSpeechMs == 0) return;

    tracker.pendingSilenceMs += tracker.pendingSpeechMs;
    stats.absorbedSpeechBlipMs += tracker.pendingSpeechMs;
    stats.absorbedSpeechBlips++;
    tracker.pendingSpeechMs = 0;
}

static void finishRecordingSilenceSegment(
    RecordingSilenceTracker &tracker,
    RecordingVadStats &stats,
    unsigned long &totalWaktuBerpikir
) {
    absorbPendingSpeechBlip(tracker, stats);

    unsigned long segmentMs = tracker.pendingSilenceMs;
    if (segmentMs == 0) return;

    tracker.pendingSilenceMs = 0;

    if (segmentMs >= VAD_RECORD_MIN_SILENCE_MS) {
        totalWaktuBerpikir += segmentMs;
        stats.committedSilenceMs += segmentMs;
        stats.committedSegmentCount++;

        if (segmentMs > stats.maxCommittedSegmentMs) {
            stats.maxCommittedSegmentMs = segmentMs;
        }
        return;
    }

    stats.ignoredShortSilenceMs += segmentMs;
    stats.ignoredShortSegmentCount++;
}

static void updateRecordingSilenceMetadata(
    RecordingSilenceTracker &tracker,
    RecordingVadStats &stats,
    bool speechNow,
    unsigned long frameDurationMs,
    unsigned long &totalWaktuBerpikir
) {
    if (frameDurationMs == 0) return;

    if (speechNow) {
        if (tracker.pendingSilenceMs > 0) {
            tracker.pendingSpeechMs += frameDurationMs;

            if (tracker.pendingSpeechMs >= VAD_RECORD_SPEECH_CONFIRM_MS) {
                tracker.pendingSpeechMs = 0;
                finishRecordingSilenceSegment(tracker, stats, totalWaktuBerpikir);
            }
        }
        return;
    }

    absorbPendingSpeechBlip(tracker, stats);
    tracker.pendingSilenceMs += frameDurationMs;
}

bool updateVadTrigger(const VadFeatures &vf) {
    const char *impactReason = classifyImpactFrame(vf);
    if (impactReason != nullptr) {
        vadScore = 0;
        vadFirstSpeechMs = 0;
        vadFirstSoftSpeechMs = 0;
        muteVad(VAD_IMPACT_MUTE_MS);
        return false;
    }

    bool muted = isVadMuted();
    const char *reason = muted ? "muted" : "reject";
    bool speechLike = muted ? false : classifySpeechFrame(vf, &reason);

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

    bool softSpeechLike = muted ? false : isOngoingSpeechFrame(vf, nullptr);
    if (softSpeechLike && vadFirstSoftSpeechMs == 0) {
        vadFirstSoftSpeechMs = frameStartMs;
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

void drainAudioInput() {
    if (rx_handle == nullptr || raw_i2s_buffer == nullptr) {
        return;
    }

    size_t bytesRead = 0;
    int chunksDrained = 0;
    const int maxDrainChunks = 32;

    do {
        bytesRead = 0;
        esp_err_t err = i2s_channel_read(
            rx_handle,
            raw_i2s_buffer,
            I2S_READ_LEN,
            &bytesRead,
            0
        );

        if (err != ESP_OK || bytesRead == 0) {
            break;
        }

        chunksDrained++;
    } while (chunksDrained < maxDrainChunks);
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

// Hitung berapa byte audio yang dipotong dari ekor (untuk buang noise tangkapan/klik).
// Selalu menyisakan minimal 1 detik audio, dan align 2 byte (16-bit mono).
static size_t audioTailTrimBytes(unsigned long trimMs, size_t totalDataWritten) {
    const size_t minKeepBytes = (size_t)SAMPLE_RATE * sizeof(int16_t); // simpan min 1 detik audio
    if (totalDataWritten <= minKeepBytes) return 0;                    // rekaman pendek: jangan trim

    uint64_t bytes = ((uint64_t)trimMs * SAMPLE_RATE * sizeof(int16_t)) / 1000ULL;
    bytes -= (bytes % sizeof(int16_t));                                // align 2 byte (16-bit)

    size_t maxTrim = totalDataWritten - minKeepBytes;
    if (bytes > maxTrim) bytes = maxTrim;
    return (size_t)bytes;
}

// Tracker
bool saveTracker() {
    if (!ensureSdReady("saveTracker", true)) {
        return false;
    }

    SD.remove("/tracker.txt");
    File f = SD.open("/tracker.txt", FILE_WRITE);
    if (f) {
        f.println(qIndex);
        f.println(aIndex);
        f.close();
    } else {
        markSdLost("saveTracker_open");
        return false;
    }

    return ensureSdReady("saveTracker_done", true);
}

void initFileIndex() {
    unsigned long startScan = millis();

    // 1. Coba baca dari file tracker.
    if (SD.exists("/tracker.txt")) {
        File f = SD.open("/tracker.txt", FILE_READ);
        if (f) {
            qIndex = f.readStringUntil('\n').toInt();
            aIndex = f.readStringUntil('\n').toInt();
            f.close();

            if (qIndex >= 0 && aIndex >= 0) {
                return;
            }
        }
    }

    // 2. Jika tracker tidak ada / tidak valid, lakukan full scan.

    File root = SD.open("/");
    if (!root) {
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

}

static void sdRecoveryTask(void* param) {
    while (true) {
        if (sdRecoveryPaused || !sdRecoveryRequested || isSdInitialized) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        unsigned long now = millis();
        if (lastSdInitAttemptMs != 0 &&
            now - lastSdInitAttemptMs < SD_RETRY_INTERVAL_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        sdRecoveryRunning = true;
        lastSdInitAttemptMs = now;
        unsigned long attemptStartedAt = millis();

        SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
        bool mounted = SD.begin(SD_CS, SPI, 4000000);

        if (mounted) {
            File root = SD.open("/");
            mounted = (bool)root;
            if (root) root.close();
        }

        if (mounted) {
            isSdInitialized = true;
            lastSdProbeMs = millis();
            lastSdInitAttemptMs = 0;
            initFileIndex();
            if (isSdInitialized) {
                sdRecoveryRequested = false;
            } else {
                sdRecoveryRequested = true;
            }
        } else {
            SD.end();
            isSdInitialized = false;
            lastSdProbeMs = 0;
        }

        if (mounted && isSdInitialized) {
            sdRecoveryRunning = false;
            sdRecoveryTaskHandle = nullptr;
            vTaskDelete(nullptr);
            return;
        }

        sdRecoveryRunning = false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void startSdRecoveryTask() {
    if (sdRecoveryTaskHandle != nullptr) return;

    BaseType_t ok = xTaskCreatePinnedToCore(
        sdRecoveryTask,
        "SDRecovery",
        6144,
        nullptr,
        1,
        &sdRecoveryTaskHandle,
        0
    );

    if (ok != pdPASS) {
        sdRecoveryTaskHandle = nullptr;
    }
}

void requestSdRecovery(const char* context) {
    if (isSdInitialized || sdRecoveryRunning) return;
    if (sdRecoveryRequested) {
        startSdRecoveryTask();
        return;
    }

    sdRecoveryRequested = true;
    startSdRecoveryTask();
}

void pauseSdRecovery() {
    sdRecoveryPaused = true;
}

void resumeSdRecovery() {
    sdRecoveryPaused = false;
}

bool isSdReady() {
    return isSdInitialized && !sdRecoveryRunning;
}

void markSdLost(const char* reason) {
    SD.end();
    isSdInitialized = false;
    lastSdProbeMs = 0;
    lastSdInitAttemptMs = millis();
    sdRecoveryRequested = true;
    startSdRecoveryTask();
}

bool ensureSdReady(const char* context, bool forceProbe) {
    unsigned long now = millis();

    if (sdRecoveryRunning && xTaskGetCurrentTaskHandle() != sdRecoveryTaskHandle) {
        return false;
    }

    if (!isSdInitialized) {
        requestSdRecovery(context);
        return false;
    }

    if (!forceProbe && lastSdProbeMs != 0 && now - lastSdProbeMs < SD_PROBE_INTERVAL_MS) {
        return true;
    }

    File root = SD.open("/");
    if (!root) {
        markSdLost(context);
        return false;
    }
    root.close();
    lastSdProbeMs = now;
    return true;
}

QuestionStatus cekStatusPertanyaan() {
    if (!ensureSdReady("cekAdaPertanyaan", true)) {
        return QUESTION_SD_ERROR;
    }

    File root = SD.open("/");
    if (!root) {
        markSdLost("cekAdaPertanyaan_root");
        return QUESTION_SD_ERROR;
    }

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
    return adaDSN ? QUESTION_OK : QUESTION_NONE;
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

}

void initAudioSD() {
    deinitAudio();

    preRecordBuffer = (int16_t *)malloc(PRE_BUFFER_SIZE * sizeof(int16_t));
    raw_i2s_buffer = (int32_t *)malloc(I2S_READ_LEN);
    processed_buffer = (int16_t *)malloc((I2S_READ_LEN / 4) * sizeof(int16_t));

    if (!preRecordBuffer || !raw_i2s_buffer || !processed_buffer) {
        deinitAudio();
        return;
    }

    memset(preRecordBuffer, 0, PRE_BUFFER_SIZE * sizeof(int16_t));
    resetVadState();
    resetAudioFilters();

    ensureSdReady("initAudioSD");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 512;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK || rx_handle == nullptr) {
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
        deinitAudio();
        return;
    }

    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        deinitAudio();
        return;
    }

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

        // Hard limiter untuk mencegah clipping overflow
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

static void recordingCaptureTask(void *param) {
    (void)param;

    RecordingAudioChunk chunk;
    int32_t localRaw[RECORDING_CAPTURE_SAMPLES];

    while (recordingCaptureShouldRun) {
        if (rx_handle == nullptr || recordingCaptureQueue == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t bytesRead = 0;
        esp_err_t err = i2s_channel_read(
            rx_handle,
            localRaw,
            I2S_READ_LEN,
            &bytesRead,
            RECORDING_CAPTURE_READ_TIMEOUT_MS
        );

        if (err != ESP_OK || bytesRead == 0) {
            continue;
        }

        int samples = bytesRead / sizeof(int32_t);
        if (samples <= 0) {
            continue;
        }
        if (samples > RECORDING_CAPTURE_SAMPLES) {
            samples = RECORDING_CAPTURE_SAMPLES;
        }

        long sumLoudness = 0;
        processAudioBufferVAD(localRaw, chunk.pcm, samples, sumLoudness, chunk.vf);
        chunk.samples = samples;

        if (xQueueSend(recordingCaptureQueue, &chunk,
                       pdMS_TO_TICKS(RECORDING_CAPTURE_SEND_WAIT_MS)) != pdTRUE) {
            recordingDroppedChunks++;   // queue penuh → chunk dibuang (penyebab klik)
        }
    }

    recordingCaptureTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

static bool startRecordingCaptureTask() {
    if (recordingCaptureTaskHandle != nullptr) {
        return true;
    }

    if (recordingCaptureQueue != nullptr) {
        vQueueDelete(recordingCaptureQueue);
        recordingCaptureQueue = nullptr;
    }

    recordingCaptureDepth = RECORDING_CAPTURE_QUEUE_DEPTH;

    const UBaseType_t depthOptions[] = {RECORDING_CAPTURE_QUEUE_DEPTH, 48, 40, 32, 24};
    recordingCaptureQueue = nullptr;
    for (size_t i = 0; i < sizeof(depthOptions) / sizeof(depthOptions[0]); i++) {
        recordingCaptureDepth = depthOptions[i];
        recordingCaptureQueue = xQueueCreate(recordingCaptureDepth, sizeof(RecordingAudioChunk));
        if (recordingCaptureQueue != nullptr) {
            break;
        }
    }

    if (recordingCaptureQueue == nullptr) {
        recordingCaptureDepth = 0;
        return false;
    }

    recordingCaptureShouldRun = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        recordingCaptureTask,
        "AudioCapture",
        4096,
        nullptr,
        3,
        &recordingCaptureTaskHandle,
        0
    );

    if (ok != pdPASS) {
        recordingCaptureShouldRun = false;
        recordingCaptureTaskHandle = nullptr;
        vQueueDelete(recordingCaptureQueue);
        recordingCaptureQueue = nullptr;
        recordingCaptureDepth = 0;
        return false;
    }

    return true;
}

static void stopRecordingCaptureTask() {
    recordingCaptureShouldRun = false;

    unsigned long startWait = millis();
    while (recordingCaptureTaskHandle != nullptr && millis() - startWait < 250) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (recordingCaptureTaskHandle != nullptr) {
        vTaskDelete(recordingCaptureTaskHandle);
        recordingCaptureTaskHandle = nullptr;
    }

    if (recordingCaptureQueue != nullptr) {
        vQueueDelete(recordingCaptureQueue);
        recordingCaptureQueue = nullptr;
    }
}

static bool readRecordingCaptureChunk(RecordingAudioChunk &chunk, TickType_t waitTicks) {
    if (recordingCaptureQueue == nullptr) {
        return false;
    }

    return xQueueReceive(recordingCaptureQueue, &chunk, waitTicks) == pdPASS;
}

static bool flushRecordingWriteBuffer(
    File &file,
    RecordingWriteBuffer &writeBuffer,
    size_t &totalDataWritten
) {
    if (writeBuffer.used == 0) {
        return true;
    }

    size_t written = file.write(writeBuffer.data, writeBuffer.used);
    totalDataWritten += written;

    if (written != writeBuffer.used) {
        writeBuffer.used = 0;
        return false;
    }

    writeBuffer.used = 0;
    return true;
}

static bool writeRecordingPcmBuffered(
    File &file,
    RecordingWriteBuffer &writeBuffer,
    const uint8_t *data,
    size_t len,
    size_t &totalDataWritten
) {
    size_t offset = 0;

    while (offset < len) {
        size_t space = writeBuffer.capacity - writeBuffer.used;
        if (space == 0) {
            if (!flushRecordingWriteBuffer(file, writeBuffer, totalDataWritten)) {
                return false;
            }
            space = writeBuffer.capacity;
        }

        size_t copyLen = len - offset;
        if (copyLen > space) {
            copyLen = space;
        }

        memcpy(writeBuffer.data + writeBuffer.used, data + offset, copyLen);
        writeBuffer.used += copyLen;
        offset += copyLen;
    }

    return true;
}

static void resetRecordingGainStats(RecordingGainStats &stats) {
    stats.samplesProcessed = 0;
    stats.clippedSamples = 0;
    stats.maxAbsBeforeGain = 0;
    stats.maxAbsAfterGain = 0;
}

static int absInt16AsInt(int16_t sample) {
    int value = (int)sample;
    return (value < 0) ? -value : value;
}

static int16_t applyRecordingWavGain(int16_t sample, RecordingGainStats &stats) {
    int absBefore = absInt16AsInt(sample);
    if (absBefore > stats.maxAbsBeforeGain) {
        stats.maxAbsBeforeGain = absBefore;
    }

    long amplified = lroundf((float)sample * RECORDING_WAV_GAIN);
    bool clipped = false;

    if (amplified > 32767L) {
        amplified = 32767L;
        clipped = true;
    } else if (amplified < -32768L) {
        amplified = -32768L;
        clipped = true;
    }

    int16_t output = (int16_t)amplified;
    int absAfter = absInt16AsInt(output);
    if (absAfter > stats.maxAbsAfterGain) {
        stats.maxAbsAfterGain = absAfter;
    }

    stats.samplesProcessed++;
    if (clipped) {
        stats.clippedSamples++;
    }

    return output;
}

static bool writeRecordingPcmWithGain(
    File &file,
    RecordingWriteBuffer &writeBuffer,
    const int16_t *pcm,
    int samples,
    size_t &totalDataWritten,
    RecordingGainStats &gainStats
) {
    if (samples <= 0 || pcm == nullptr) {
        return true;
    }

    int offset = 0;
    while (offset < samples) {
        int batch = samples - offset;
        if (batch > RECORDING_CAPTURE_SAMPLES) {
            batch = RECORDING_CAPTURE_SAMPLES;
        }

        for (int i = 0; i < batch; i++) {
            recordingGainBuffer[i] = applyRecordingWavGain(pcm[offset + i], gainStats);
        }

        if (!writeRecordingPcmBuffered(
                file,
                writeBuffer,
                (uint8_t *)recordingGainBuffer,
                batch * sizeof(int16_t),
                totalDataWritten)) {
            return false;
        }

        offset += batch;
    }

    return true;
}

static bool writePreRollBuffer(
    File &file,
    RecordingWriteBuffer &writeBuffer,
    size_t &totalDataWritten,
    RecordingGainStats &gainStats
) {
    if (preRecordBuffer == nullptr) {
        return true;
    }

    int preBufferSize = (int)PRE_BUFFER_SIZE;
    int available = bufferIsFull ? preBufferSize : bufferHead;
    int targetSamples = (int)RECORDING_PRE_ROLL_SAMPLES;

    if (targetSamples > preBufferSize) {
        targetSamples = preBufferSize;
    }

    if (available <= 0 || targetSamples <= 0) {
        return true;
    }

    int samplesToWrite = targetSamples;
    if (samplesToWrite > available) {
        samplesToWrite = available;
    }

    int start = 0;
    if (bufferIsFull) {
        start = bufferHead - samplesToWrite;
        while (start < 0) {
            start += preBufferSize;
        }
    } else {
        start = available - samplesToWrite;
    }

    int firstLen = samplesToWrite;
    if (start + firstLen > preBufferSize) {
        firstLen = preBufferSize - start;
    }

    if (!writeRecordingPcmWithGain(
            file,
            writeBuffer,
            &preRecordBuffer[start],
            firstLen,
            totalDataWritten,
            gainStats)) {
        return false;
    }

    int remaining = samplesToWrite - firstLen;
    if (remaining > 0) {
        if (!writeRecordingPcmWithGain(
                file,
                writeBuffer,
                &preRecordBuffer[0],
                remaining,
                totalDataWritten,
                gainStats)) {
            return false;
        }
    }

    return true;
}

static bool writeRecordingChunk(
    File &file,
    RecordingWriteBuffer &writeBuffer,
    const int16_t *pcm,
    int samples,
    const VadFeatures &vf,
    size_t &totalDataWritten,
    unsigned long startTime,
    unsigned long &totalWaktuBerpikir,
    RecordingSilenceTracker &silenceTracker,
    RecordingVadStats &vadStats,
    RecordingGainStats &gainStats,
    bool &sdWriteFailed
) {
    if (samples <= 0 || pcm == nullptr) {
        return true;
    }

    if (!writeRecordingPcmWithGain(
            file,
            writeBuffer,
            pcm,
            samples,
            totalDataWritten,
            gainStats)) {
        sdWriteFailed = true;
        return false;
    }

    unsigned long now = millis();
    unsigned long elapsed = now - startTime;

    long sisaMs = (elapsed >= maxRecordMs) ? 0 : (long)(maxRecordMs - elapsed);

    int menit = sisaMs / 60000;
    int detik = (sisaMs % 60000) / 1000;

    // Update LCD tiap 1000 ms.
    static unsigned long lastLCD = 0;
    if (now - lastLCD > 1000) {
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

    // Saat recording, VAD hanya dipakai untuk mengukur apakah frame ini speech atau hening.
    unsigned long frameDurationMs = 0;
    if (samples > 0) {
        frameDurationMs = ((unsigned long)samples * 1000UL) / SAMPLE_RATE;
    }

    RecordingVadReason vadReason = RECORDING_VAD_QUIET;
    bool speechNow = isOngoingSpeechFrame(vf, &vadReason);
    int reasonIndex = (int)vadReason;

    if (reasonIndex >= 0 && reasonIndex < RECORDING_VAD_REASON_COUNT) {
        vadStats.reasonMs[reasonIndex] += frameDurationMs;
        vadStats.reasonFrames[reasonIndex]++;
    }

    if (speechNow) {
        vadStats.speechMs += frameDurationMs;
    } else {
        vadStats.nonSpeechMs += frameDurationMs;
    }

    updateRecordingSilenceMetadata(
        silenceTracker,
        vadStats,
        speechNow,
        frameDurationMs,
        totalWaktuBerpikir
    );

    return elapsed < maxRecordMs;
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

// Metadata
bool tulisMetadata(String filename, String uid, unsigned long waktuBerpikir, int currentQ, int currentA) {
    if (!ensureSdReady("metadata_start", true)) {
        return false;
    }

    String metaFile = filename;
    metaFile.replace(".wav", ".txt");

    File file = SD.open(metaFile, FILE_WRITE);
    if (!file) {
        markSdLost("metadata_open");
        return false;
    }

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

    if (!ensureSdReady("metadata_done", true)) {
        return false;
    }

    return true;
}

// Rekam Suara
RecordingResult rekamSuara(String uid, unsigned long waktuBerpikir) {
    if (!ensureSdReady("record_start", true)) {
        return RECORDING_SD_ERROR;
    }

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

    recordingDroppedChunks = 0;
    bool useCaptureQueue = startRecordingCaptureTask();
    unsigned long startTime = millis();

    File file = SD.open(filename, FILE_WRITE);
    if (!file) {
        markSdLost("record_open");
        if (useCaptureQueue) {
            stopRecordingCaptureTask();
        }
        return RECORDING_SD_ERROR;
    }

    uint8_t blank[44] = {0};
    if (file.write(blank, 44) != 44) {
        if (useCaptureQueue) {
            stopRecordingCaptureTask();
        }
        file.close();
        markSdLost("record_header");
        return RECORDING_SD_ERROR;
    }

    size_t totalDataWritten = 0;
    RecordingWriteBuffer writeBuffer = {
        recordingSdWriteBuffer,
        0,
        RECORDING_SD_WRITE_BUFFER_BYTES
    };
    RecordingGainStats gainStats;
    resetRecordingGainStats(gainStats);

    if (!writePreRollBuffer(file, writeBuffer, totalDataWritten, gainStats)) {
        if (useCaptureQueue) {
            stopRecordingCaptureTask();
        }
        file.close();
        markSdLost("record_preroll");
        return RECORDING_SD_ERROR;
    }

    unsigned long totalWaktuBerpikir = waktuBerpikir; // hidden, untuk metadata
    RecordingSilenceTracker silenceTracker;
    static RecordingVadStats vadStats;
    resetRecordingSilenceTracker(silenceTracker);
    resetRecordingVadStats(vadStats, waktuBerpikir);

    bool isRecording = true;
    bool thrown = false;
    bool manualStopped = false;
    bool sdWriteFailed = false;
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

        // Stop manual: double click (atau lebih) saat rekaman. Mode tidak pindah.
        if (pollManualStopRecording()) {
            manualStopped = true;
            isRecording = false;
            break;
        }

        if (useCaptureQueue) {
            RecordingAudioChunk chunk;
            if (readRecordingCaptureChunk(chunk, pdMS_TO_TICKS(100))) {
                if (!writeRecordingChunk(
                    file,
                    writeBuffer,
                    chunk.pcm,
                    chunk.samples,
                    chunk.vf,
                    totalDataWritten,
                    startTime,
                    totalWaktuBerpikir,
                    silenceTracker,
                    vadStats,
                    gainStats,
                    sdWriteFailed
                )) {
                    isRecording = false;
                }
            } else if (millis() - startTime >= maxRecordMs) {
                isRecording = false;
            }
        }
        else if (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN, &bytes_read, 100) == ESP_OK && bytes_read > 0) {
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

            if (!writeRecordingChunk(
                file,
                writeBuffer,
                processed_buffer,
                samples,
                vf,
                totalDataWritten,
                startTime,
                totalWaktuBerpikir,
                silenceTracker,
                vadStats,
                gainStats,
                sdWriteFailed
            )) {
                isRecording = false;
            }
        }
    }

    if (useCaptureQueue) {
        stopRecordingCaptureTask();
    }
    stopThrowDetectionTask();
    resetFlyingFlag();

    if (!flushRecordingWriteBuffer(file, writeBuffer, totalDataWritten)) {
        sdWriteFailed = true;
    }

    size_t trimBytes = 0;
    if (totalDataWritten > 0) {
        // Potong ekor rekaman saat berhenti karena lemparan/stop manual,
        // supaya noise tangkapan ("gedebuk") atau klik tombol tidak ikut terbaca.
        unsigned long trimMs = thrown        ? RECORDING_THROW_TAIL_TRIM_MS
                             : manualStopped ? RECORDING_MANUAL_TAIL_TRIM_MS
                             : 0UL;
        trimBytes            = audioTailTrimBytes(trimMs, totalDataWritten);
        size_t finalDataSize = totalDataWritten - trimBytes;

        writeWavHeader(file, (int)finalDataSize);
        file.flush();
    }

    file.close();

    if (sdWriteFailed || !ensureSdReady("record_finalize", true)) {
        markSdLost("record_write_failed");
        return RECORDING_SD_ERROR;
    }

    // Selaraskan waktuBerpikir dengan audio ter-trim: jeda diam di ekor yang dipotong jangan dihitung.
    if (trimBytes > 0) {
        const size_t bytesPerMs = (size_t)SAMPLE_RATE * sizeof(int16_t) / 1000; // 32 byte/ms
        unsigned long trimMsEff = (unsigned long)(trimBytes / bytesPerMs);
        if (silenceTracker.pendingSilenceMs > trimMsEff)
            silenceTracker.pendingSilenceMs -= trimMsEff;
        else
            silenceTracker.pendingSilenceMs = 0;
    }

    finishRecordingSilenceSegment(silenceTracker, vadStats, totalWaktuBerpikir);

    if (!tulisMetadata(filename, uid, totalWaktuBerpikir, currentQ, currentA)) {
        return RECORDING_SD_ERROR;
    }

    int oldQ = qIndex;
    int oldA = aIndex;
    if (prefix == "DSN") {
        qIndex = currentQ;
    } else {
        aIndex = currentA;
    }

    if (!saveTracker()) {
        qIndex = oldQ;
        aIndex = oldA;
        return RECORDING_SD_ERROR;
    }

    // Diagnosis "spiky": bukti clipping (gain) vs chunk ke-drop (queue).
    Serial.printf(
        "[REC] bytes=%u clipped=%u/%u (%.1f%%) maxPreGain=%d maxPostGain=%d dropped=%u\n",
        (unsigned)totalDataWritten,
        (unsigned)gainStats.clippedSamples, (unsigned)gainStats.samplesProcessed,
        gainStats.samplesProcessed ? (100.0f * gainStats.clippedSamples / gainStats.samplesProcessed) : 0.0f,
        gainStats.maxAbsBeforeGain, gainStats.maxAbsAfterGain,
        (unsigned)recordingDroppedChunks);

    lcd.clear();

    if (thrown) {
        lcd.setCursor(0, 0);
        lcd.print(" ALAT DILEMPAR ");
        lcd.setCursor(0, 1);
        lcd.print("REKAMAN SELESAI");
    }
    else if (manualStopped) {
        lcd.setCursor(0, 0);
        lcd.print("  STOP MANUAL  ");
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

    return thrown ? RECORDING_THROWN : RECORDING_OK;
}
