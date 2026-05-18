"use strict";

// Sentralisasi & validasi environment variable (fail-fast).
// Di-require paling awal oleh src/app.js: bila ENV wajib hilang, proses
// berhenti dengan pesan jelas (bukan crash misterius saat runtime).

require("dotenv").config();

const REQUIRED = ["SUPABASE_URL", "SUPABASE_KEY", "SESSION_SECRET"];
const missing = REQUIRED.filter((k) => !process.env[k]);
if (missing.length > 0) {
  const msg = `Environment variable wajib belum diset: ${missing.join(", ")}`;
  console.error(`❌ ${msg}. Lihat server/.env.example`);
  throw new Error(msg);
}

module.exports = {
  SUPABASE_URL: process.env.SUPABASE_URL,
  SUPABASE_KEY: process.env.SUPABASE_KEY,
  SESSION_SECRET: process.env.SESSION_SECRET,
  PORT: Number(process.env.PORT) || 3000,
  DEEPGRAM_KEY: process.env.DEEPGRAM_KEY || "",
  // Kredensial broker MQTT — dulu hardcode di src/config/mqtt.js.
  // Default = nilai lama agar perilaku tidak berubah bila ENV kosong,
  // tapi sekarang BISA dioverride via .env (lihat .env.example).
  MQTT: {
    host:
      process.env.MQTT_HOST ||
      "c4bbf4787735464dadc96ca13e4a9c6b.s1.eu.hivemq.cloud",
    port: Number(process.env.MQTT_PORT) || 8883,
    protocol: process.env.MQTT_PROTOCOL || "mqtts",
    username: process.env.MQTT_USERNAME || "catchnote",
    password: process.env.MQTT_PASSWORD || "Ta2526018",
  },
};
