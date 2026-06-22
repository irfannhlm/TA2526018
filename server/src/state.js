"use strict";

// State RAM yang dibagi antara handler MQTT dan route HTTP.
// Nilai awal sama persis dengan deklarasi lama di server.js
// (behavior-preserving). Diakses via objek `state` agar reassignment
// (mis. state.syncStatus = {...}) terlihat lintas-modul.

const state = {
  // ----- DATA SESI -----
  sessionData: {
    status: "stopped",
    mode: "dosen_rec",
    maxTime: 5,
    threshold: "normal",
    thresholdCustom: null,
    scannedList: [],
  },
  scanCounter: 0,

  // ----- STATUS SYNC SD CARD -----
  syncStatus: {
    state: "idle", // "idle" | "loading" | "done" | "error" | "cancelled"
    pesan: "",
    total: 0,
    berhasil: 0,
    updatedAt: Date.now(),
  },
  // Antrian duplikat kini persisten di Supabase (lihat
  // src/data/duplicateQueue.repo.js), tidak lagi di RAM.
};

module.exports = { state };
