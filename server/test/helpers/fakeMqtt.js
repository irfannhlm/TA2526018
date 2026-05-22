"use strict";

// Fake module "mqtt". connect() mengembalikan client palsu berbasis
// EventEmitter: publish dicatat, pesan masuk bisa dipicu manual lewat
// deliver() (memanggil handler "message" yang didaftarkan server.js dan
// menunggu promise-nya selesai).

const { EventEmitter } = require("events");

class FakeMqttClient extends EventEmitter {
  constructor() {
    super();
    this.published = []; // { topic, payload(string) }
    this.subscriptions = [];
    this.ended = false;
  }
  subscribe(topics, cb) {
    this.subscriptions.push(topics);
    if (typeof cb === "function") cb(null);
    return this;
  }
  publish(topic, payload, optsOrCb, maybeCb) {
    // Dukung kedua bentuk: publish(t,p,cb) & publish(t,p,opts,cb).
    const cb = typeof optsOrCb === "function" ? optsOrCb : maybeCb;
    this.published.push({ topic, payload: String(payload) });
    if (typeof cb === "function") cb(null);
    return this;
  }
  end() {
    this.ended = true;
  }

  // ---- util test ----
  /** Picu satu pesan MQTT masuk dan tunggu handler async selesai. */
  async deliver(topic, objOrString) {
    const message = Buffer.from(
      typeof objOrString === "string"
        ? objOrString
        : JSON.stringify(objOrString),
    );
    const listeners = this.listeners("message");
    for (const fn of listeners) await fn(topic, message);
  }
  /** Pesan terakhir yang dipublish (opsional difilter per-topik). */
  lastPublished(topic) {
    const list = topic
      ? this.published.filter((p) => p.topic === topic)
      : this.published;
    const last = list[list.length - 1];
    return last
      ? { topic: last.topic, payload: JSON.parse(last.payload) }
      : null;
  }
  clearPublished() {
    this.published = [];
  }
}

function makeFakeMqttModule() {
  const client = new FakeMqttClient();
  const module = { connect: () => client };
  return { module, client };
}

module.exports = { FakeMqttClient, makeFakeMqttModule };
