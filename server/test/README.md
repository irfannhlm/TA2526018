# Characterization Test — Jaring Pengaman Refactor

Test ini **mengunci perilaku `server.js` saat ini** (sebelum refactor), supaya
tiap fase refactor bisa diverifikasi tidak mengubah perilaku. Ini _characterization
test_: yang dites adalah perilaku nyata yang ada sekarang — bukan perilaku ideal.

## Menjalankan

```bash
npm test
```

(= `node --test --test-concurrency=1 "test/**/*.test.js"`)

`--test-concurrency=1` disengaja: tiap test memuat ulang `server.js` (state RAM
modul: `sessionData`, `duplicateQueue`, `scanCounter`) dan membuka port acak,
jadi dijalankan serial agar terisolasi.

## Pendekatan

- **Black-box**: aplikasi dijalankan apa adanya lewat `app` Express + handler
  MQTT, lalu diassert response HTTP, pesan MQTT yang dipublish, dan state DB.
- **Semua dependency eksternal di-mock** (tanpa jaringan):
  - `@supabase/supabase-js` → `helpers/fakeSupabase.js` (in-memory).
  - `mqtt` → `helpers/fakeMqtt.js` (publish dicatat, pesan masuk dipicu manual).
  - `./Deepgramservice` → stub (panggilan dicatat).
- `helpers/loadApp.js` meng-inject mock ke `require.cache`, set ENV dummy,
  fresh-require `server.js` tiap test, listen di port acak.

## Seam di server.js

Satu-satunya perubahan untuk testability (perilaku `npm start` tetap sama):
`app.listen` + background job hanya jalan bila `require.main === module`;
`server.js` meng-export `{ app, startServer, startBackgroundJobs }`.

## Cakupan

Difokuskan ke area paling berisiko untuk refactor:

- Auth & guard (`/login`, `requireLogin`, `requireRole`, `/logout`)
- Handler MQTT (status, rfid, audio_data DSN/MHS + duplikat + ACK, sync_status)
- Upload audio (`/api/upload-audio-sd`, `/api/upload-audio`) + antrian duplikat
- API realtime, sync-status, sd-sync-keputusan, duplicate-resolve
- Mutasi admin/dosen + smoke render halaman admin/dosen/pilih-kelas

## Batas (diketahui)

`fakeSupabase` meniru pola query yang dipakai `server.js` saja (filter datar +
embedded-select sesuai skema proyek), **bukan** emulator PostgREST penuh.
Branch yang bergantung waktu (mis. auto-timeout sync-status 60 dtk) tidak dites.
