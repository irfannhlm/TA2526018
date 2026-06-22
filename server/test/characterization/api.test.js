"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { loadApp, hashPassword } = require("../helpers/loadApp");

const PERINTAH = "kelas/alat/perintah";

function seedAdmin(sb) {
  sb.seed("users", [
    {
      user_id: 1,
      username: "admin",
      password: hashPassword("admin123"),
      role: "admin",
    },
  ]);
}

test("/api/sync-status butuh login (302 tanpa sesi)", async () => {
  const ctx = await loadApp({ seed: seedAdmin });
  try {
    const res = await ctx.request("GET", "/api/sync-status");
    assert.equal(res.status, 302);
  } finally {
    await ctx.close();
  }
});

test("/api/sync-status default idle, reset -> loading", async () => {
  const ctx = await loadApp({ seed: seedAdmin });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    let res = await ctx.request("GET", "/api/sync-status", { cookie });
    assert.equal(res.json().state, "idle");

    res = await ctx.request("POST", "/api/sync-status/reset", { cookie });
    assert.equal(res.json().ok, true);

    res = await ctx.request("GET", "/api/sync-status", { cookie });
    assert.equal(res.json().state, "loading");
  } finally {
    await ctx.close();
  }
});

test("/api/duplicate-queue kosong + resolve item tak ada -> 404", async () => {
  const ctx = await loadApp({ seed: seedAdmin });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    let res = await ctx.request("GET", "/api/duplicate-queue", { cookie });
    assert.deepEqual(res.json().pending, []);

    res = await ctx.request("POST", "/api/duplicate-resolve", {
      cookie,
      body: { qid: 999, action: "skip" },
    });
    assert.equal(res.status, 404);
    assert.equal(res.json().ok, false);
  } finally {
    await ctx.close();
  }
});

test("/api/duplicate-resolve skip (txt_dsn) -> ACK & item resolved", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedAdmin(sb);
      sb.seed("classes", [{ class_id: 10, class_name: "K1" }]);
      sb.seed("questions", [
        { question_id: 9, class_id: 10, number_q: 1, date_id: "2026-01-01" },
      ]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "DSN_2_1.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      tanggal: "13-05-2026",
    });
    const cookie = await ctx.loginAs("admin", "admin123");
    const { pending } = (
      await ctx.request("GET", "/api/duplicate-queue", { cookie })
    ).json();
    assert.equal(pending.length, 1);

    ctx.mqtt.clearPublished();
    const res = await ctx.request("POST", "/api/duplicate-resolve", {
      cookie,
      body: { qid: pending[0].qid, action: "skip" },
    });
    assert.equal(res.json().action, "skip");
    assert.equal(ctx.mqtt.lastPublished(PERINTAH).payload.perintah, "ack_file");

    const after = (
      await ctx.request("GET", "/api/duplicate-queue", { cookie })
    ).json();
    assert.equal(after.pending.length, 0);
  } finally {
    await ctx.close();
  }
});

test("/api/duplicate-resolve replace (txt_dsn) -> update date_id + ACK", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedAdmin(sb);
      sb.seed("classes", [{ class_id: 10, class_name: "K1" }]);
      sb.seed("questions", [
        { question_id: 9, class_id: 10, number_q: 1, date_id: "2026-01-01" },
      ]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "DSN_2_1.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      tanggal: "13-05-2026",
    });
    const cookie = await ctx.loginAs("admin", "admin123");
    const { pending } = (
      await ctx.request("GET", "/api/duplicate-queue", { cookie })
    ).json();

    const res = await ctx.request("POST", "/api/duplicate-resolve", {
      cookie,
      body: { qid: pending[0].qid, action: "replace" },
    });
    assert.equal(res.json().action, "replace");
    const q = ctx.supabase.rows("questions").find((x) => x.question_id === 9);
    assert.equal(q.date_id, "2026-05-13");
  } finally {
    await ctx.close();
  }
});

test("Persistensi: duplicate_queue dari DB muncul di /api/duplicate-queue", async () => {
  // Simulasi "setelah restart": item sudah ada di tabel (bukan RAM).
  const ctx = await loadApp({
    seed(sb) {
      seedAdmin(sb);
      sb.seed("duplicate_queue", [
        {
          qid: 5,
          file: "DSN_2_1.txt",
          tipe: "txt_dsn",
          target_kelas: "K1",
          payload: { no_pertanyaan: 1 },
          resolved_at: null,
        },
      ]);
    },
  });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    const res = await ctx.request("GET", "/api/duplicate-queue", { cookie });
    const { pending } = res.json();
    assert.equal(pending.length, 1);
    assert.equal(pending[0].qid, 5);
    assert.equal(pending[0].tipe, "txt_dsn");
    assert.equal(pending[0].no_pertanyaan, 1); // dari payload jsonb
  } finally {
    await ctx.close();
  }
});

test("Validasi: /api/duplicate-resolve action invalid -> 400", async () => {
  const ctx = await loadApp({ seed: seedAdmin });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    const res = await ctx.request("POST", "/api/duplicate-resolve", {
      cookie,
      body: { qid: 1, action: "ngawur" },
    });
    assert.equal(res.status, 400);
  } finally {
    await ctx.close();
  }
});

test("/api/sd-sync-keputusan ulangi/batalkan/invalid", async () => {
  const ctx = await loadApp({ seed: seedAdmin });
  try {
    let res = await ctx.request("POST", "/api/sd-sync-keputusan", {
      body: { keputusan: "ulangi" },
    });
    assert.equal(res.json().success, true);
    assert.equal(ctx.mqtt.lastPublished(PERINTAH).payload.perintah, "ack_file");

    res = await ctx.request("POST", "/api/sd-sync-keputusan", {
      body: { keputusan: "batalkan" },
    });
    assert.equal(
      ctx.mqtt.lastPublished(PERINTAH).payload.perintah,
      "batalkan_sync",
    );

    res = await ctx.request("POST", "/api/sd-sync-keputusan", {
      body: { keputusan: "ngawur" },
    });
    assert.equal(res.status, 400);
  } finally {
    await ctx.close();
  }
});

test("/api/realtime-data tanpa kelas -> scannedList kosong + devices ter-map", async () => {
  const ctx = await loadApp({
    seed(sb) {
      sb.seed("devices", [
        { device_id: 1, name: "A1", status: "online", battery_level: 90 },
      ]);
    },
  });
  try {
    const res = await ctx.request("GET", "/api/realtime-data");
    assert.equal(res.status, 200);
    const data = res.json();
    assert.deepEqual(data.scannedList, []);
    // Perilaku nyata: IIFE startup server me-reset SEMUA device jadi
    // status "offline" & battery null saat boot (server.js:166-173).
    assert.deepEqual(data.devices, [
      { id: 1, name: "A1", status: "offline", battery: null },
    ]);
  } finally {
    await ctx.close();
  }
});

test("/api/realtime-data: mahasiswa beda kelas -> isWrongClass true", async () => {
  const ctx = await loadApp({
    seed(sb) {
      sb.seed("classes", [
        { class_id: 10, class_name: "K1" },
        { class_id: 20, class_name: "K2" },
      ]);
      sb.seed("students", [
        { student_id: 1, name: "Siti", nim: "9", rfid_uid: "U1" },
      ]);
      sb.seed("class_students", [
        { class_student_id: 1, student_id: 1, class_id: 20 },
      ]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/rfid", {
      action: "tap_rfid",
      uid: "U1",
    });
    const res = await ctx.request("GET", "/api/realtime-data?kelas=K1");
    const data = res.json();
    const row = data.scannedList.find((x) => x.uid === "U1");
    assert.equal(row.isRegistered, true);
    assert.equal(row.isWrongClass, true);
  } finally {
    await ctx.close();
  }
});

test("TC-API-10 realtime-data: uid tak terdaftar -> isRegistered false", async () => {
  const ctx = await loadApp({
    seed(sb) {
      sb.seed("classes", [{ class_id: 10, class_name: "K1" }]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/rfid", { action: "tap_rfid", uid: "TIDAK-ADA" });
    const res = await ctx.request("GET", "/api/realtime-data?kelas=K1");
    const row = res.json().scannedList.find((x) => x.uid === "TIDAK-ADA");
    assert.equal(row.isRegistered, false);
    assert.equal(row.name, "Tidak Terdaftar");
  } finally {
    await ctx.close();
  }
});

test("TC-API-11 upload-audio-sd kelas tak ditemukan -> 200 + warning, tetap tersimpan", async () => {
  const ctx = await loadApp({ seed: seedAdmin });
  try {
    const res = await ctx.requestMultipart("POST", "/api/upload-audio-sd", {
      fields: { nama_file: "DSN_2_1.wav", target_kelas: "KELAS-GHOST" },
      file: { filename: "DSN_2_1.wav" },
    });
    assert.equal(res.status, 200);
    const j = res.json();
    assert.equal(j.success, true);
    assert.match(j.warning || "", /tidak ditemukan/i);
    assert.ok(j.audio_file_path, "tetap mengembalikan URL audio tersimpan");
    // Tidak ada question dibuat karena kelas tak ada
    assert.equal(ctx.supabase.rows("questions").length, 0);
  } finally {
    await ctx.close();
  }
});

test("/api/realtime-logs tanpa kelas -> logs kosong; dengan data -> ada entri", async () => {
  const ctx = await loadApp({
    seed(sb) {
      sb.seed("classes", [{ class_id: 10, class_name: "K1" }]);
      sb.seed("students", [
        { student_id: 1, name: "Andi", nim: "7", rfid_uid: "UA" },
      ]);
      sb.seed("class_students", [
        { class_student_id: 1, student_id: 1, class_id: 10 },
      ]);
      sb.seed("questions", [
        {
          question_id: 1,
          class_id: 10,
          transcript_text: "Apa itu X?",
          created_at: "2026-05-10T00:00:00.000Z",
        },
      ]);
      sb.seed("answers", [
        {
          answer_id: 1,
          question_id: 1,
          student_id: 1,
          transcript_text: "X adalah Y",
          class_id: 10,
        },
      ]);
    },
  });
  try {
    let res = await ctx.request("GET", "/api/realtime-logs");
    assert.deepEqual(res.json().logs, []);

    res = await ctx.request("GET", "/api/realtime-logs?kelas=K1");
    const { logs } = res.json();
    assert.equal(logs.length, 1);
    assert.equal(logs[0].question, "Apa itu X?");
    assert.equal(logs[0].transcript, "X adalah Y");
    assert.equal(logs[0].name, "Andi");
  } finally {
    await ctx.close();
  }
});
