"use strict";

// Klien MQTT singleton + handler connect/error. Dipindahkan apa adanya dari
// server.js (behavior-preserving). Handler "message" tetap di server.js
// untuk saat ini (akan dipecah per-topik di fase berikutnya).
//
// Kredensial broker kini dari environment (lihat src/config/env.js),
// dengan default = nilai lama agar perilaku tidak berubah.

const mqtt = require("mqtt");
const { MQTT } = require("./env");

const mqttClient = mqtt.connect({
  host: MQTT.host,
  port: MQTT.port,
  protocol: MQTT.protocol,
  username: MQTT.username,
  password: MQTT.password,
  rejectUnauthorized: true,
  clientId: `catchnote_server_${Math.random().toString(16).slice(2, 8)}`, // ID unik tiap koneksi
  clean: true,
});

mqttClient.on("connect", () => {
  console.log("✅ Backend terhubung ke HiveMQ Cloud!");
  mqttClient.subscribe(
    [
      "kelas/alat/rfid",
      "kelas/alat/status",
      "kelas/alat/audio_data",
      "kelas/alat/sync_status",
    ],
    (err) => {
      if (!err)
        console.log(
          "✅ Subscribe Berhasil: rfid, status, audio_data, sync_status",
        );
      else console.error("❌ Gagal berlangganan topik:", err);
    },
  );
});

mqttClient.on("error", (err) => {
  console.error("❌ MQTT Error:", err.message);
});

module.exports = { mqttClient };
