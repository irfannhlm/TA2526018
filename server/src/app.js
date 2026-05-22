"use strict";

// ================= COMPOSITION ROOT =================
// Merakit aplikasi Express: konfigurasi, sesi, handler MQTT, router
// per-domain, error-middleware. TIDAK memanggil app.listen (itu tugas
// server.js). Seluruh logika ada di modul-modul di bawah src/.

const express = require("express");
const path = require("path");
const session = require("express-session");
const MemoryStore = require("memorystore")(session);
const env = require("./config/env"); // validasi ENV fail-fast (paling awal)

const { supabase } = require("./config/supabase");
const { sbSelect, sbInsert, sbUpdate } = require("./data/baseRepo");
const { state } = require("./state");
const { mqttClient } = require("./config/mqtt");
const { parseNamaFile } = require("./lib/fileName");
const { createMessageHandler } = require("./mqtt/messageHandler");

const app = express();
const ROOT = path.join(__dirname, ".."); // folder server/

// Hormati X-Forwarded-* dari reverse proxy (Render/Railway/Nginx) supaya
// cookie `secure` bekerja di produksi.
app.set("trust proxy", 1);

// ================= KEAMANAN (HELMET) =================
// CSP dimatikan karena view EJS lama memakai script/style inline;
// proteksi header lain (X-Frame-Options, noSniff, dll.) tetap aktif.
app.use(require("helmet")({ contentSecurityPolicy: false }));

// ================= EXPRESS SETUP =================
app.set("view engine", "ejs");
app.set("views", path.join(ROOT, "views"));
app.use(express.urlencoded({ extended: true }));
app.use(express.json());
app.use(require("cookie-parser")(env.SESSION_SECRET));
app.use(express.static(path.join(ROOT, "public")));

// ================= SESSION =================
app.use(
  session({
    secret: env.SESSION_SECRET,
    resave: false,
    // true: pengunjung anonim (halaman login) tetap dapat cookie sesi,
    // sehingga sessionID stabil dari render form sampai submit. Ini
    // SYARAT agar token CSRF (yang terikat sessionID) valid saat login.
    saveUninitialized: true,
    store: new MemoryStore({
      checkPeriod: 86400000, // bersihkan sesi kedaluwarsa tiap 24 jam
    }),
    cookie: {
      httpOnly: true,
      maxAge: 8 * 60 * 60 * 1000,
      sameSite: "lax",
      secure: process.env.NODE_ENV === "production",
    },
  }),
);

// ================= CSRF (double-submit cookie) =================
app.use(require("./http/middleware/csrf").csrfMiddleware);

// ================= CEK KONEKSI SUPABASE =================
(async () => {
  try {
    const { error } = await supabase
      .from("devices")
      .select("device_id")
      .limit(1);
    if (error) throw error;
    console.log("✅ Berhasil terhubung ke Supabase!");
  } catch (err) {
    console.error("❌ Error koneksi Supabase:", err.message);
  }
})();

// Reset status alat saat startup
(async () => {
  try {
    await sbUpdate("devices", {}, { status: "offline", battery_level: null });
    console.log("🔄 System Startup: Status alat di-reset.");
  } catch (err) {
    console.error("❌ Gagal reset status devices:", err.message);
  }
})();

// ================= HANDLER PESAN MQTT =================
mqttClient.on(
  "message",
  createMessageHandler({
    sbSelect,
    sbInsert,
    sbUpdate,
    parseNamaFile,
    mqttClient,
    state,
  }),
);

// ================= ROUTER PER-DOMAIN =================
app.use(require("./http/routes/api.routes"));
app.use(require("./http/routes/auth.routes"));
app.use(require("./http/routes/pilihKelas.routes"));
app.use(require("./http/routes/admin.routes"));
app.use(require("./http/routes/dosen.routes"));

// ================= ERROR HANDLER TERPUSAT (harus paling akhir) =================
app.use(require("./http/middleware/error"));

module.exports = { app };
