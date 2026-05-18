"use strict";

// ================= BOOTSTRAP =================
// Titik masuk proses: rakit app (src/app.js), jalankan background job
// (Deepgram sweep) & HTTP server, plus graceful shutdown. Saat di-require
// dari test, app dipakai tanpa listen.

const { app } = require("./src/app");
const { supabase } = require("./src/config/supabase");
const { sbUpdate } = require("./src/data/baseRepo");
const { mqttClient } = require("./src/config/mqtt");
const { sweepUntranscribed } = require("./Deepgramservice");

let sweepTimeout = null;
let sweepInterval = null;
let httpServer = null;

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
  startBackgroundJobs();
  startServer();
}

module.exports = { app, startServer, startBackgroundJobs, shutdown };
