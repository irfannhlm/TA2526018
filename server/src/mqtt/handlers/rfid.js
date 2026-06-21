"use strict";

// Topik: kelas/alat/rfid — catat tap kartu RFID ke daftar scan sesi
// (in-memory) untuk ditampilkan di dashboard secara real-time.
//
// Catatan desain: handler ini sengaja TIDAK menulis ke tabel `questions`
// maupun `answers`. Pembuatan baris pada kedua tabel tersebut sepenuhnya
// digerakkan oleh alur audio (HTTP upload `/api/upload-audio-sd` dan MQTT
// `kelas/alat/audio_data`), bukan oleh tap kartu. Dengan demikian tap RFID
// hanya berperan sebagai sinyal kehadiran real-time, tanpa menimbulkan
// efek samping persisten pada basis data.
module.exports = async function rfid(payload, ctx) {
  const { state } = ctx;

  const { uid, action } = payload;
  if (action === "tap_rfid" && uid) {
    console.log(`📝 RFID Tap: ${uid}`);
    state.scanCounter++;
    state.sessionData.scannedList.unshift({
      id: state.scanCounter,
      uid,
      time: new Date().toLocaleTimeString(),
    });
  }
};
