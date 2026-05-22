"use strict";

// ================= BOOTSTRAP =================
// Titik masuk proses: rakit app (src/app.js), jalankan background job
// (Deepgram sweep) & HTTP server, plus graceful shutdown. Saat di-require
// dari test, app dipakai tanpa listen.

const fs = require("fs");
const path = require("path");
const { app } = require("./src/app");
const { supabase } = require("./src/config/supabase");
const { sbUpdate } = require("./src/data/baseRepo");
const { mqttClient } = require("./src/config/mqtt");
const { sweepUntranscribed } = require("./Deepgramservice");

let sweepTimeout = null;
let sweepInterval = null;
let httpServer = null;

// Bersihkan file sementara di temp_audio/ yang lebih tua dari 24 jam
// (sisa upload yang gagal/crash di tengah jalan).
function cleanupTempAudio() {
  const dir = path.join(__dirname, "temp_audio");
  const maxAgeMs = 24 * 60 * 60 * 1000;
  try {
    for (const name of fs.readdirSync(dir)) {
      if (name === ".gitkeep") continue;
      const p = path.join(dir, name);
      try {
        const st = fs.statSync(p);
        if (st.isFile() && Date.now() - st.mtimeMs > maxAgeMs) {
          fs.unlinkSync(p);
        }
      } catch (_) {
        /* abaikan file yang gagal dibaca */
      }
    }
  } catch (_) {
    /* folder belum ada — abaikan */
  }
}

// ================= DEEPGRAM AUTO-SWEEP =================
// Sweep pertama 10 detik setelah server nyala, lalu tiap 5 menit.
function startBackgroundJobs() {
  sweepTimeout = setTimeout(() => {
    sweepUntranscribed(supabase, sbUpdate);
  }, 10_000);

  sweepInterval = setInterval(
    () => {
      sweepUntranscribed(supabase, sbUpdate);
    },
    5 * 60 * 1000,
  ); // setiap 5 menit
}

// ================= START SERVER =================
function startServer() {
  const PORT = process.env.PORT || 3000;
  httpServer = app.listen(PORT, "0.0.0.0", () => {
    console.log("==================================================");
    console.log(`🚀 SERVER RUNNING!`);
    console.log(`💻 Akses Web via Laptop: http://localhost:${PORT}`);
    console.log("==================================================");
  });
  return httpServer;
}

// ================= GRACEFUL SHUTDOWN =================
function shutdown(signal) {
  console.log(`\n🛑 ${signal} diterima — mematikan server dengan rapi...`);
  if (sweepTimeout) clearTimeout(sweepTimeout);
  if (sweepInterval) clearInterval(sweepInterval);
  try {
    mqttClient.end();
  } catch (_) {
    /* abaikan */
  }
  const done = () => process.exit(0);
  if (httpServer) httpServer.close(done);
  else done();
  // jaring pengaman bila close menggantung
  setTimeout(() => process.exit(0), 5000).unref();
}

// Hanya jalankan saat dieksekusi langsung (`node server.js` / `npm start`).
// Saat di-`require` dari test, lewati agar bisa pakai `app` tanpa listen.
if (require.main === module) {
  process.on("SIGINT", () => shutdown("SIGINT"));
  process.on("SIGTERM", () => shutdown("SIGTERM"));
  cleanupTempAudio();
  startBackgroundJobs();
  startServer();
}

module.exports = { app, startServer, startBackgroundJobs, shutdown };
