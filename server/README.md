# CatchNote — Server

Backend dokumentasi diskusi kelas: perangkat ESP32 (RFID + audio SD)
mengirim data via **MQTT/HTTP**, server menyimpan ke **Supabase**,
transkrip otomatis via **Deepgram**, dan menyajikan dashboard
admin/dosen (EJS).

## Menjalankan

```bash
cd server
npm install
cp .env.example .env   # lalu isi nilainya
npm start              # http://localhost:3000
```

Node.js **>= 18** (dikembangkan di Node 22).

## Environment

Lihat [.env.example](.env.example). Wajib: `SUPABASE_URL`, `SUPABASE_KEY`,
`SESSION_SECRET` — server **menolak start** bila salah satu kosong
(fail-fast di `src/config/env.js`). Kredensial MQTT kini lewat ENV
(default lama tetap dipakai bila kosong).

## Skrip

| Perintah               | Fungsi                                                  |
| ---------------------- | ------------------------------------------------------- |
| `npm start`            | Jalankan server (`node server.js`)                      |
| `npm test`             | Characterization test (`node:test`, dependency di-mock) |
| `npm run lint`         | ESLint                                                  |
| `npm run format`       | Prettier `--write`                                      |
| `npm run format:check` | Prettier cek format                                     |

## Arsitektur

Berlapis, satu arah: **transport (HTTP/MQTT) → router/handler → service →
repository → Supabase**. `server.js` hanya **bootstrap** (71 baris);
seluruh logika di `src/` (25 modul).

```
server.js                 bootstrap: start server, background job, graceful shutdown
src/
  app.js                  composition root (Express, helmet, sesi, router, error mw)
  config/
    env.js                validasi ENV fail-fast + kredensial MQTT
    supabase.js           klien Supabase singleton
    mqtt.js               klien MQTT singleton + connect/error
  data/
    baseRepo.js           helper sbSelect/sbInsert/sbUpdate/sbDelete
  state.js                state RAM bersama (sesi, sync, antrian duplikat)
  lib/
    constants.js          TABLE_PK, THRESHOLD_VALUES, STORAGE_BUCKET, SALT_ROUNDS
    utils.js              toInt
    fileName.js           parseNamaFile
    asyncHandler.js       pembungkus async → error-middleware
  mqtt/
    messageHandler.js     dispatcher per-topik
    handlers/             deviceStatus · rfid · audioData · syncStatus
  services/
    audio.service.js      simpan URL audio ke DB
    storage.service.js    upload ke Supabase Storage
  http/
    middleware/           auth · upload (multer) · error (terpusat)
    routes/               api · auth · pilihKelas · admin · dosen
```

### Penanganan error

Route memakai `asyncHandler`; error tak terduga diteruskan ke
**error-middleware terpusat** ([src/http/middleware/error.js](src/http/middleware/error.js))
yang menjawab seragam (500 JSON untuk `/api/*`, selainnya teks).

### Keamanan

- `helmet` (CSP dimatikan karena view EJS lama pakai skrip inline).
- **CSRF** (double-submit cookie, `csrf-csrf`) untuk semua form
  admin/dosen; endpoint ESP32 (`/api/upload-audio*`) dikecualikan.
- Cookie sesi `httpOnly` + `sameSite=lax` + `secure` (di produksi),
  `trust proxy` aktif.
- Rate-limit pada `/login`; validasi input semua endpoint mutasi &
  upload dengan `zod`.
- **Otorisasi kepemilikan**: dosen hanya bisa ubah/hapus data kelas
  yang dia ampu (admin bypass).
- Password `bcrypt` (min 8 karakter saat pembuatan user).
- MQTT perintah dikirim QoS 1 (`src/mqtt/publisher.js`) agar tidak
  hilang. Kredensial broker via ENV.

Catatan: `MemoryStore` sesi tidak untuk produksi (hilang saat restart)
— ganti store bila deploy.

## Migrasi database

Sebelum menjalankan versi ini, jalankan SQL di Supabase Studio:
[docs/sql/duplicate_queue.sql](docs/sql/duplicate_queue.sql) — tabel
`duplicate_queue` (antrian duplikat kini persisten, bukan di RAM, agar
tidak hilang saat server restart).

## Test

47 _characterization test_ mengunci perilaku (auth, MQTT, upload, API,
mutasi admin/dosen, CSRF-bypass, ownership, validasi) dengan **semua
dependency di-mock** (tanpa jaringan). Detail & batasan:
[test/README.md](test/README.md).

```bash
npm test   # 47 passed
```
