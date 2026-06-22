# Audio Pipeline — Detail Teknis

Dokumen ini menjelaskan cara kerja perekaman suara pada alat (ESP32 + mikrofon
I2S **INMP441** + kartu SD), dengan fokus pada **I2S DMA buffer** dan
**streaming ke SD card**, lalu menandai bagian kode yang **paling kritis** —
yang kalau salah satu meleset, hasil rekaman jadi senyap, cempreng, kecepatannya
salah, atau putus-putus.

Logika audio ada di [AudioSD_Module.cpp](AudioSD_Module.cpp), konstanta di
[Config.h](Config.h), dan loop pre-buffer/VAD di [Main.ino](Main.ino).

---

## 1. Ringkasan & Alur Data

```
INMP441 (mic I2S, 24-bit)
   │  (BCLK/WS/SD)
   ▼
I2S peripheral (slot 32-bit, MONO, LEFT)
   │
   ▼
DMA ring buffer  (16 desc × 512 frame ≈ 0,5 dtk)
   │  i2s_channel_read()
   ▼
processAudioBufferVAD()        →  >> BIT_SHIFT, HPF/DC-block, limiter, cast int16
   │                              (sekalian hitung fitur VAD: avgAbs/rms/zcr/crest/peak)
   ▼
preRecordBuffer (ring 1,5 dtk)  ── pre-roll: audio SEBELUM trigger ikut terekam
   │
   │  [VAD trigger → mulai rekam]
   ▼
recordingCaptureTask (core 0)  →  FreeRTOS queue (depth 64)
   │                              decoupling capture dari latensi tulis SD
   ▼
writeRecordingChunk()          →  gain ×3 + buffer tulis 4 KB
   │
   ▼
File .wav di SD card  (header 44 byte ditulis ulang di akhir)
```

Singkatnya: mic → I2S → DMA → baca → proses jadi PCM 16-bit → simpan di ring
pre-roll → saat suara terdeteksi, chunk dialirkan lewat queue ke task penulis →
ditulis ke SD sebagai WAV.

---

## 2. Konfigurasi I2S & DMA Buffer

Sumber: `initAudioSD()` di [AudioSD_Module.cpp:855](AudioSD_Module.cpp#L855).

### DMA buffer
```c
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
chan_cfg.dma_desc_num  = 16;   // jumlah deskriptor DMA
chan_cfg.dma_frame_num = 512;  // frame per deskriptor
```
[AudioSD_Module.cpp:873-875](AudioSD_Module.cpp#L873)

- Total kapasitas DMA = `16 × 512 = 8192 frame`. Pada `SAMPLE_RATE = 16000`,
  itu **≈ 0,512 detik** audio yang bisa "ditahan" hardware sebelum di-baca CPU.
- **Inilah bantalan utama** yang mencegah sampel hilang ketika CPU sedang sibuk
  (misalnya saat menunggu tulis ke SD). DMA terus mengisi buffer ini di latar
  belakang tanpa campur tangan CPU.

### Slot / format
```c
.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
...
std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
```
[AudioSD_Module.cpp:883-895](AudioSD_Module.cpp#L885)

- INMP441 mengeluarkan data **24-bit, MSB-aligned, di dalam slot 32-bit** → karena
  itu lebar slot harus **32-bit**.
- INMP441 dengan pin **L/R di-ground** mengeluarkan datanya di kanal **LEFT**, maka
  `slot_mask = I2S_STD_SLOT_LEFT`. **Salah mask/kanal → hasil senyap atau noise.**

### GPIO (RX-only)
```c
.gpio_cfg = { .mclk = UNUSED, .bclk = I2S_SCK, .ws = I2S_WS, .dout = UNUSED, .din = I2S_SD }
```
Pin diambil dari [Config.h:9-12](Config.h#L9): `I2S_WS=16`, `I2S_SD=4`, `I2S_SCK=17`.
`dout` tidak dipakai karena kita hanya merekam (receive).

### Alokasi buffer
[AudioSD_Module.cpp:858-860](AudioSD_Module.cpp#L858)
```c
preRecordBuffer = malloc(PRE_BUFFER_SIZE * sizeof(int16_t)); // ring pre-roll, 24000 sampel = 1,5 dtk
raw_i2s_buffer  = malloc(I2S_READ_LEN);                      // 1024 byte = 256 sampel int32 mentah
processed_buffer= malloc((I2S_READ_LEN / 4) * sizeof(int16_t)); // 256 sampel int16 hasil proses
```
Perhatikan: `raw_i2s_buffer` menampung sampel **32-bit mentah**, sedangkan
`processed_buffer` menampung hasil **16-bit**. Ukuran keduanya saling terkait —
salah satu salah → buffer overrun.

---

## 3. Membaca I2S (Dua Jalur)

Pipeline membaca I2S lewat dua jalur berbeda tergantung state:

### Jalur A — Listen / pre-buffer (sebelum rekam)
[Main.ino:1223](Main.ino#L1223)
```c
while (i2s_channel_read(rx_handle, raw_i2s_buffer, I2S_READ_LEN, &bytes_read, 0) == ESP_OK
       && bytes_read > 0) {
    int samples = bytes_read / 4;
    bool frameTriggered = updateAudioPreBufferAndVad(samples, maxLoudness);
    ...
}
```
- Timeout **0** (non-blocking): kuras semua data yang sudah ada di DMA tiap loop,
  lalu berhenti. Tidak memblokir loop utama.
- Tiap frame masuk `updateAudioPreBufferAndVad()` → diproses + disimpan ke ring
  pre-roll + update skor VAD.

### Jalur B — Saat merekam
Saat rekaman aktif, capture dipisah ke **task FreeRTOS tersendiri** supaya tidak
terganggu latensi tulis SD:

`recordingCaptureTask` [AudioSD_Module.cpp:989](AudioSD_Module.cpp#L989) (prioritas
3, pinned ke **core 0**):
```c
esp_err_t err = i2s_channel_read(rx_handle, localRaw, I2S_READ_LEN, &bytesRead,
                                 RECORDING_CAPTURE_READ_TIMEOUT_MS /*20ms*/);
...
int samples = bytesRead / sizeof(int32_t);
processAudioBufferVAD(localRaw, chunk.pcm, samples, sumLoudness, chunk.vf);
xQueueSend(recordingCaptureQueue, &chunk, ...);   // kirim ke loop penulis
```
Loop perekam di `rekamSuara()` lalu **pop** chunk dari queue dan menulisnya ke SD.
Ini **decoupling**: capture jalan terus walau tulis SD sesaat lambat.

### Konversi sampel yang mengikat semuanya
`samples = bytes_read / 4` karena tiap sampel **4 byte (int32)**. Konstanta ini
muncul konsisten di [AudioSD_Module.cpp:1014](AudioSD_Module.cpp#L1014),
[Main.ino:1226](Main.ino#L1226), dan [AudioSD_Module.cpp:1600](AudioSD_Module.cpp#L1600).
Kalau pembaginya salah (mis. `/2`), jumlah sampel salah → buffer overrun / data acak.

---

## 4. Pemrosesan Sinyal — `processAudioBufferVAD()`

[AudioSD_Module.cpp:919](AudioSD_Module.cpp#L919). Untuk tiap sampel mentah:

```c
int32_t sample = input[i] >> BIT_SHIFT;                       // (1) turunkan skala
float filtered = hpf_alpha * (hpf_prev_out + sample - hpf_prev_in); // (2) HPF / DC-block
hpf_prev_in = sample; hpf_prev_out = filtered;
if (filtered >  32700) filtered =  32700;                     // (3) hard limiter
if (filtered < -32700) filtered = -32700;
int16_t y = (int16_t)filtered;                               // (4) jadi PCM 16-bit
output[i] = y;
```

1. **`>> BIT_SHIFT` (=14)** — [AudioSD_Module.cpp:945](AudioSD_Module.cpp#L945).
   Data INMP441 24-bit MSB-aligned di slot 32-bit; shift 14 menurunkan skalanya ke
   rentang ~int16. **Ini tombol "kenyaringan/distorsi" terbesar**: shift kebesaran
   → terlalu pelan; shift kekecilan → pecah/clip.
2. **HPF / DC-block** dengan `hpf_alpha = 0.98`
   [AudioSD_Module.cpp:948](AudioSD_Module.cpp#L948). INMP441 punya DC offset
   bawaan; filter ini membuangnya. Tanpa HPF → gelombang miring ke satu sisi →
   clipping asimetris / dengung.
3. **Hard limiter ±32700** mencegah overflow saat cast ke int16.
4. Selain menghasilkan PCM, fungsi ini juga menghitung **fitur VAD** (avgAbs, rms,
   zcr, crest, peak) yang dipakai untuk deteksi suara manusia.

---

## 5. Pre-roll Ring Buffer (audio sebelum trigger)

Supaya kata pertama tidak terpotong, alat selalu menyimpan **~1,5 detik terakhir**
audio di ring buffer `preRecordBuffer`, bahkan sebelum perekaman resmi dimulai.

### Mengisi ring
`updateAudioPreBufferAndVad()` [AudioSD_Module.cpp:1409](AudioSD_Module.cpp#L1409):
```c
for (int i = 0; i < samples; i++) {
    preRecordBuffer[bufferHead] = processed_buffer[i];
    bufferHead++;
    if (bufferHead >= PRE_BUFFER_SIZE) { bufferHead = 0; bufferIsFull = true; }
}
```
`bufferHead` berputar (circular); `bufferIsFull` menandai ring sudah penuh sekali
putaran.

### Menumpahkan ring ke file saat rekam dimulai
`writePreRollBuffer()` [AudioSD_Module.cpp:1246](AudioSD_Module.cpp#L1246) menghitung
titik `start` dan menulis isi ring dengan benar menangani **wrap-around** (bisa jadi
dua segmen: dari `start` ke akhir buffer, lalu dari awal buffer ke `bufferHead`).
Salah aritmetika pointer di sini → awal rekaman berisi data lama/acak atau "pop".

---

## 6. Streaming ke SD & Format WAV — `rekamSuara()`

[AudioSD_Module.cpp:1482](AudioSD_Module.cpp#L1482).

### Urutan
1. Tentukan nama file (`DSN_<id>_<q>.wav` atau `MHS_<id>_<q>_<a>.wav`).
2. Start `recordingCaptureTask`.
3. Buka file, tulis **44 byte header kosong** sebagai placeholder
   [AudioSD_Module.cpp:1517](AudioSD_Module.cpp#L1517) — ukuran data belum diketahui,
   jadi header asli ditulis belakangan.
4. Tumpahkan pre-roll buffer (`writePreRollBuffer`).
5. Loop: pop chunk dari queue → `writeRecordingChunk`.

### Penulisan ter-buffer (kunci kelancaran)
Tiap chunk: `applyRecordingWavGain` (gain `RECORDING_WAV_GAIN = 3.0`, clip ±32767)
→ `writeRecordingPcmBuffered` ke **buffer 4096 byte**, lalu **flush hanya saat
penuh** (`flushRecordingWriteBuffer`).
[AudioSD_Module.cpp:1113-1163](AudioSD_Module.cpp#L1113)

Alasannya: SD card efisien menulis dalam blok besar. Kalau menulis per-sampel
(potongan kecil), tulisan jadi lambat → chunk menumpuk di queue → akhirnya
**dibuang** → audio putus-putus / klik.

Partial write ditangani: jika `written != writeBuffer.used` → `sdWriteFailed = true`
[AudioSD_Module.cpp:1125](AudioSD_Module.cpp#L1125).

### Menutup file & header WAV
Di akhir [AudioSD_Module.cpp:1639-1657](AudioSD_Module.cpp#L1639): flush sisa buffer,
hitung tail-trim (buang noise "gedebuk"/klik di ekor), lalu:
```c
writeWavHeader(file, (int)finalDataSize);  // seek(0) + tulis header asli
file.flush(); file.close();
```
`writeWavHeader()` [AudioSD_Module.cpp:520](AudioSD_Module.cpp#L520) melakukan
`file.seek(0)` lalu menimpa placeholder 44 byte. Nilai yang **wajib benar**:
`audio_format=1`(PCM), `num_channels=1`, `sample_rate=SAMPLE_RATE`, `bits=16`,
`byte_rate=SAMPLE_RATE*2`, `block_align=2`, dan `data_length = byte audio aktual`.
Salah satu meleset → player memutar dengan pitch/kecepatan/durasi salah, atau file
dianggap rusak.

### Diagnostik
Setelah rekam, ada log diagnosis [AudioSD_Module.cpp:1695](AudioSD_Module.cpp#L1695):
```
[REC] bytes=... clipped=x/y (z%) maxPreGain=... maxPostGain=... dropped=...
```
- `clipped%` tinggi → masalah **gain** (suara cempreng/pecah).
- `dropped` > 0 → **queue penuh** (SD lambat → audio putus-putus).

---

## 7. Bagian KUNCI — kalau salah, rekaman kacau

Urut dari yang paling sering jadi biang masalah:

| # | Bagian | Lokasi | Kalau salah |
|---|--------|--------|-------------|
| 1 | `slot_mask = I2S_STD_SLOT_LEFT` + MONO + slot 32-bit | [AudioSD_Module.cpp:883-895](AudioSD_Module.cpp#L885) | Senyap / noise (baca kanal kosong) |
| 2 | `input[i] >> BIT_SHIFT` (BIT_SHIFT=14) | [AudioSD_Module.cpp:945](AudioSD_Module.cpp#L945) | Terlalu pelan (shift kebesaran) / distorsi-clip (kekecilan) |
| 3 | `samples = bytes_read / 4` (int32) | [:1014](AudioSD_Module.cpp#L1014), [Main.ino:1226](Main.ino#L1226), [:1600](AudioSD_Module.cpp#L1600) | Jumlah sampel salah → overrun / data acak |
| 4 | DMA `dma_desc_num=16` / `dma_frame_num=512` | [AudioSD_Module.cpp:874-875](AudioSD_Module.cpp#L874) | Bantalan ~0,5 dtk; kekecilan → overflow / gap |
| 5 | Decoupling queue + `recordingCaptureTask` (depth 64, prio 3, core 0) | [:989](AudioSD_Module.cpp#L989), [:1026](AudioSD_Module.cpp#L1026) | Tanpa ini / queue kekecilan → `recordingDroppedChunks` naik = klik/putus |
| 6 | Buffer tulis 4096 B + flush/partial-write | [:1113-1163](AudioSD_Module.cpp#L1113) | Flush per-sampel → SD telat → chunk dibuang |
| 7 | Header WAV: placeholder 44 B + `seek(0)` rewrite + `data_length` benar | [:520-538](AudioSD_Module.cpp#L520), [:1653](AudioSD_Module.cpp#L1653) | Pitch/kecepatan/durasi salah, file garbled |
| 8 | HPF / DC-block `hpf_alpha=0.98` | [:948](AudioSD_Module.cpp#L948) | Clipping asimetris / dengung / offset |
| 9 | `RECORDING_WAV_GAIN=3.0` + clip | [:167](AudioSD_Module.cpp#L167), [:1186](AudioSD_Module.cpp#L1186) | Kebesaran = clipping "cempreng" |
| 10 | Aritmetika wrap pre-roll `writePreRollBuffer` | [:1273-1309](AudioSD_Module.cpp#L1273) | Awal rekaman berisi data lama/acak / pop |
| 11 | Ukuran alokasi buffer di `initAudioSD` | [:858-860](AudioSD_Module.cpp#L858) | Mismatch ukuran → buffer overrun |

---

## 8. Tabel Konstanta

Nilai dari [Config.h](Config.h) dan [AudioSD_Module.cpp](AudioSD_Module.cpp).

| Konstanta | Nilai | Lokasi | Arti | Dampak kalau salah |
|-----------|-------|--------|------|--------------------|
| `SAMPLE_RATE` | 16000 | [Config.h:29](Config.h#L29) | Laju cuplik (Hz) | Harus sama di I2S clock & header WAV; beda → pitch/kecepatan salah |
| `I2S_READ_LEN` | 1024 (byte) = 256 sampel ≈ 16 ms | [Config.h:30](Config.h#L30) | Ukuran 1 kali baca I2S (dalam **byte**) | Salah hitung → overrun; ini byte, bukan jumlah sampel |
| `BIT_SHIFT` | 14 | [Config.h:31](Config.h#L31) | Skala data 24-bit → ~16-bit | Kebesaran=pelan, kekecilan=clip |
| `PRE_BUFFER_SEC` / `PRE_BUFFER_SIZE` | 1.5 / 24000 sampel | [Config.h:33-34](Config.h#L33) | Panjang pre-roll | Kekecilan → awal suara terpotong |
| `dma_desc_num` | 16 | [AudioSD_Module.cpp:874](AudioSD_Module.cpp#L874) | Jumlah deskriptor DMA | Kekecilan → overflow/gap |
| `dma_frame_num` | 512 | [AudioSD_Module.cpp:875](AudioSD_Module.cpp#L875) | Frame per deskriptor (total ≈0,5 dtk) | Kekecilan → overflow/gap |
| `RECORDING_CAPTURE_QUEUE_DEPTH` | 64 | [AudioSD_Module.cpp:66](AudioSD_Module.cpp#L66) | Kedalaman queue capture→penulis | Kekecilan → chunk dibuang = klik |
| `RECORDING_SD_WRITE_BUFFER_BYTES` | 4096 | [AudioSD_Module.cpp:70](AudioSD_Module.cpp#L70) | Buffer akumulasi sebelum tulis SD | Kekecilan → tulis kecil-kecil = SD lambat |
| `RECORDING_WAV_GAIN` | 3.0 | [AudioSD_Module.cpp:167](AudioSD_Module.cpp#L167) | Penguatan digital saat simpan | Kebesaran=clip cempreng, kekecilan=pelan |
| `hpf_alpha` | 0.98 | [AudioSD_Module.cpp:45](AudioSD_Module.cpp#L45) | Koefisien HPF/DC-block | Salah → DC offset / dengung |
| Pin I2S (`WS/SD/SCK`) | 16 / 4 / 17 | [Config.h:10-12](Config.h#L10) | Wiring mic | Salah → senyap/noise |
| Pin SD (`SCK/MISO/MOSI/CS`) | 18 / 19 / 23 / 5 | [Config.h:15-18](Config.h#L15) | Wiring SD (SPI) | Salah → SD gagal mount / tulis |

---

## 9. Tabel Gejala → Penyebab

Panduan cepat: kalau hasil rekaman terdengar seperti kolom kiri, periksa kolom
tengah/kanan.

| Gejala hasil rekaman | Penyebab kemungkinan | Lokasi & cara cek |
|----------------------|----------------------|-------------------|
| **Senyap / flat (datar)** | `slot_mask`/kanal salah, pin `din` salah, atau `BIT_SHIFT` kebesaran | [slot:895](AudioSD_Module.cpp#L895), [shift:945](AudioSD_Module.cpp#L945); cek `maxPreGain` di log `[REC]` (kalau ~0 → mic tidak terbaca) |
| **Cempreng / pecah (clipping)** | `RECORDING_WAV_GAIN` kebesaran / sinyal sudah keras | [gain:167](AudioSD_Module.cpp#L167), [apply:1186](AudioSD_Module.cpp#L1186); cek `clipped%` di log `[REC]` (tinggi → turunkan gain) |
| **Putus-putus / klik** | Queue penuh (SD lambat) → chunk dibuang | [drop:1026-1029](AudioSD_Module.cpp#L1026); cek `dropped` di log `[REC]` (>0 → perbesar queue/DMA/buffer SD) |
| **Kecepatan/pitch salah, durasi salah** | Field header WAV (`sample_rate`/`data_length`) salah | [header:529](AudioSD_Module.cpp#L529), [rewrite:1653](AudioSD_Module.cpp#L1653) |
| **Volume terlalu pelan** | `BIT_SHIFT` kebesaran atau `GAIN` kekecilan | [shift:945](AudioSD_Module.cpp#L945), [gain:167](AudioSD_Module.cpp#L167) |
| **Dengung / DC offset / "gedebuk" di awal** | HPF mati/salah atau pre-roll salah | [hpf:948](AudioSD_Module.cpp#L948), [preroll:1273](AudioSD_Module.cpp#L1273) |
| **Audio awal hilang / terpotong** | Pre-roll kosong atau di-clear sebelum waktunya | [clearPreRecordBuffer:467](AudioSD_Module.cpp#L467), [writePreRollBuffer:1246](AudioSD_Module.cpp#L1246) |
| **Suara kresek-kresek/acak menyeluruh** | `samples = bytes_read/4` salah, atau ukuran buffer mismatch | [div:1014](AudioSD_Module.cpp#L1014), [alloc:858-860](AudioSD_Module.cpp#L858) |
| **File tidak bisa dibuka / 0 byte** | SD gagal (mount/tulis) — bukan masalah audio | `ensureSdReady`/`markSdLost`, cek wiring SPI [Config.h:15-18](Config.h#L15) |

---

### Catatan diagnostik praktis

Saat ragu, baca baris log serial `[REC]` ([AudioSD_Module.cpp:1695](AudioSD_Module.cpp#L1695)):
ia langsung memisahkan dua penyebab tersering — **clipping** (masalah gain) vs
**dropped chunk** (masalah kecepatan tulis SD / ukuran buffer).
