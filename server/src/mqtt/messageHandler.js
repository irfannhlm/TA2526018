"use strict";

// Dispatcher pesan MQTT: parse payload, log, lalu rute ke handler per-topik.
// Setara perilaku lama (satu blok if-per-topik) karena topik selalu satu
// nilai. Dependency disuntik via ctx (sbSelect/sbInsert/sbUpdate/
// parseNamaFile/mqttClient/state).

const TOPICS = require("../lib/topics");
const deviceStatus = require("./handlers/deviceStatus");
const rfid = require("./handlers/rfid");
const audioData = require("./handlers/audioData");
const syncStatus = require("./handlers/syncStatus");

const TOPIC_HANDLERS = {
  [TOPICS.STATUS]: deviceStatus,
  [TOPICS.RFID]: rfid,
  [TOPICS.AUDIO_DATA]: audioData,
  [TOPICS.SYNC_STATUS]: syncStatus,
};

function createMessageHandler(ctx) {
  return async (topic, message) => {
    try {
      const payload = JSON.parse(message.toString());
      console.log(`\n📩 Pesan Masuk [${topic}]:`, payload);

      const handle = TOPIC_HANDLERS[topic];
      if (handle) await handle(payload, ctx);
    } catch (error) {
      console.error("❌ MQTT Error:", error);
    }
  };
}

module.exports = { createMessageHandler };
