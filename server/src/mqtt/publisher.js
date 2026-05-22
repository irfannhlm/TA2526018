"use strict";

// Pengirim perintah ke alat. Selalu publish ke topik PERINTAH dengan
// QoS 1 (at-least-once) supaya pesan penting (ACK, set_timer, sync, dst.)
// tidak hilang diam-diam saat koneksi flaky. Mengembalikan Promise<boolean>.

const { mqttClient } = require("../config/mqtt");
const TOPICS = require("../lib/topics");

function publishCommand(payload, opts = {}) {
  const message =
    typeof payload === "string" ? payload : JSON.stringify(payload);
  return new Promise((resolve) => {
    mqttClient.publish(TOPICS.PERINTAH, message, { qos: 1, ...opts }, (err) => {
      if (err) {
        console.error("❌ Gagal publish perintah:", err.message || err);
        return resolve(false);
      }
      console.log(`📤 Perintah terkirim (qos1): ${message}`);
      resolve(true);
    });
  });
}

module.exports = { publishCommand };
