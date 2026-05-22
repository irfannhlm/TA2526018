"use strict";

// Nama topik MQTT terpusat agar tidak ada string yang tersebar/typo.
module.exports = {
  PERINTAH: "kelas/alat/perintah", // server -> alat (perintah & ACK)
  STATUS: "kelas/alat/status", // alat -> server (status & baterai)
  RFID: "kelas/alat/rfid", // alat -> server (tap kartu)
  AUDIO_DATA: "kelas/alat/audio_data", // alat -> server (metadata SD)
  SYNC_STATUS: "kelas/alat/sync_status", // alat -> server (progres sync)
};
