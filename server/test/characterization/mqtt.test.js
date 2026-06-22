"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { loadApp, hashPassword } = require("../helpers/loadApp");

const PERINTAH = "kelas/alat/perintah";

function seedBase(sb) {
  sb.seed("users", [
    {
      user_id: 1,
      username: "admin",
      password: hashPassword("admin123"),
      role: "admin",
    },
  ]);
  sb.seed("classes", [{ class_id: 10, class_name: "K1" }]);
  sb.seed("students", [
    { student_id: 1, name: "Budi", nim: "111", rfid_uid: "UID-A" },
  ]);
  sb.seed("class_students", [
    { class_student_id: 1, student_id: 1, class_id: 10 },
  ]);
  sb.seed("devices", [
    { device_id: 2, name: "Alat-2", status: "offline", battery_level: null },
  ]);
}

test("MQTT status: update status & battery device", async () => {
  const ctx = await loadApp({ seed: seedBase });
  try {
    await ctx.mqtt.deliver("kelas/alat/status", {
      device_id: 2,
      status: "online",
      battery: 88,
    });
    const dev = ctx.supabase.rows("devices").find((d) => d.device_id === 2);
    assert.equal(dev.status, "online");
    assert.equal(dev.battery_level, 88);
  } finally {
    await ctx.close();
  }
});

test("MQTT rfid tap: catat scan in-memory tanpa menulis ke questions/answers", async () => {
  const ctx = await loadApp({ seed: seedBase });
  try {
    await ctx.mqtt.deliver("kelas/alat/rfid", {
      action: "tap_rfid",
      uid: "UID-A",
    });
    assert.equal(ctx.supabase.rows("questions").length, 0);
    assert.equal(ctx.supabase.rows("answers").length, 0);

    const res = await ctx.request("GET", "/api/realtime-data?kelas=K1");
    assert.equal(res.status, 200);
    const data = res.json();
    assert.equal(data.scannedList[0].uid, "UID-A");
    assert.equal(data.scannedList[0].name, "Budi");
  } finally {
    await ctx.close();
  }
});

test("MQTT audio_data DSN baru: insert question + kirim ACK", async () => {
  const ctx = await loadApp({ seed: seedBase });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "DSN_2_1.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      tanggal: "13-05-2026",
      uid: "DOSEN",
    });
    const qs = ctx.supabase.rows("questions");
    assert.equal(qs.length, 1);
    assert.equal(qs[0].number_q, 1);
    assert.equal(qs[0].date_id, "2026-05-13");
    assert.equal(qs[0].device_id, 2);

    const ack = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(ack.payload.perintah, "ack_file");
    assert.equal(ack.payload.file, "DSN_2_1.txt");
  } finally {
    await ctx.close();
  }
});

test("MQTT audio_data DSN duplikat: ditahan di queue, TANPA ACK", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedBase(sb);
      sb.seed("questions", [
        {
          question_id: 99,
          class_id: 10,
          number_q: 1,
          date_id: "2026-01-01",
          transcript_text: "",
        },
      ]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "DSN_2_1.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      tanggal: "13-05-2026",
      uid: "DOSEN",
    });
    assert.equal(ctx.mqtt.lastPublished(PERINTAH), null, "tidak boleh ACK");

    const cookie = await ctx.loginAs("admin", "admin123");
    const res = await ctx.request("GET", "/api/duplicate-queue", { cookie });
    const { pending } = res.json();
    assert.equal(pending.length, 1);
    assert.equal(pending[0].tipe, "txt_dsn");
  } finally {
    await ctx.close();
  }
});

test("MQTT audio_data: file duplikat sama dikirim 2x -> tetap 1 antrian", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedBase(sb);
      sb.seed("questions", [
        {
          question_id: 99,
          class_id: 10,
          number_q: 1,
          date_id: "2026-01-01",
          transcript_text: "",
        },
      ]);
    },
  });
  try {
    const payload = {
      file: "DSN_2_1.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      tanggal: "13-05-2026",
      uid: "DOSEN",
    };
    await ctx.mqtt.deliver("kelas/alat/audio_data", payload);
    await ctx.mqtt.deliver("kelas/alat/audio_data", payload); // kiriman ulang

    const cookie = await ctx.loginAs("admin", "admin123");
    const { pending } = (
      await ctx.request("GET", "/api/duplicate-queue", { cookie })
    ).json();
    assert.equal(pending.length, 1, "tidak boleh menggandakan antrian");
  } finally {
    await ctx.close();
  }
});

test("MQTT audio_data MHS baru: insert answer + ACK", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedBase(sb);
      sb.seed("questions", [
        { question_id: 50, class_id: 10, number_q: 1, transcript_text: "" },
      ]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "MHS_2_1_3.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      no_jawaban: 3,
      uid: "UID-A",
      waktu_diam_ms: 4000,
    });
    const answers = ctx.supabase.rows("answers");
    assert.equal(answers.length, 1);
    assert.equal(answers[0].number_a, 3);
    assert.equal(answers[0].duration_answer, 4);
    assert.equal(answers[0].student_id, 1);

    const ack = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(ack.payload.perintah, "ack_file");
  } finally {
    await ctx.close();
  }
});

test("MQTT audio_data: nama file tak dikenal -> ACK & tidak menyimpan", async () => {
  const ctx = await loadApp({ seed: seedBase });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "RANDOM.txt",
      target_kelas: "K1",
    });
    assert.equal(ctx.supabase.rows("questions").length, 0);
    const ack = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(ack.payload.perintah, "ack_file");
    assert.equal(ack.payload.file, "RANDOM.txt");
  } finally {
    await ctx.close();
  }
});

test("MQTT sync_status: perbarui state yang bisa dipolling", async () => {
  const ctx = await loadApp({ seed: seedBase });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");

    await ctx.mqtt.deliver("kelas/alat/sync_status", {
      status: "progress",
      pesan: "jalan",
      total: 5,
      berhasil: 2,
    });
    let res = await ctx.request("GET", "/api/sync-status", { cookie });
    assert.equal(res.json().state, "loading");

    await ctx.mqtt.deliver("kelas/alat/sync_status", {
      status: "selesai",
      pesan: "ok",
      total: 5,
      berhasil: 5,
    });
    res = await ctx.request("GET", "/api/sync-status", { cookie });
    assert.equal(res.json().state, "done");
  } finally {
    await ctx.close();
  }
});

test("TC-MQTT-10 audio_data MHS duplikat (sudah ada student) -> queue, TANPA ACK", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedBase(sb);
      sb.seed("questions", [{ question_id: 50, class_id: 10, number_q: 1 }]);
      sb.seed("answers", [
        { answer_id: 70, question_id: 50, number_a: 3, student_id: 1, duration_answer: 4 },
      ]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "MHS_2_1_3.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      no_jawaban: 3,
      uid: "UID-A",
      waktu_diam_ms: 9000,
    });
    assert.equal(ctx.mqtt.lastPublished(PERINTAH), null, "tidak boleh ACK");

    const cookie = await ctx.loginAs("admin", "admin123");
    const { pending } = (
      await ctx.request("GET", "/api/duplicate-queue", { cookie })
    ).json();
    assert.equal(pending.length, 1);
    assert.equal(pending[0].tipe, "txt_mhs");
  } finally {
    await ctx.close();
  }
});

test("TC-MQTT-11 audio_data DSN ada tapi date_id kosong -> update date_id + ACK", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedBase(sb);
      sb.seed("questions", [
        { question_id: 50, class_id: 10, number_q: 1, transcript_text: "" },
      ]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "DSN_2_1.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      tanggal: "13-05-2026",
      uid: "DOSEN",
    });
    const q = ctx.supabase.rows("questions").find((x) => x.question_id === 50);
    assert.equal(q.date_id, "2026-05-13");
    assert.equal(q.device_id, 2); // device_id terisi dari nama file
    const ack = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(ack.payload.perintah, "ack_file");
  } finally {
    await ctx.close();
  }
});

test("TC-MQTT-12 audio_data MHS ada tapi kosong -> update student+duration + ACK", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedBase(sb);
      sb.seed("questions", [{ question_id: 50, class_id: 10, number_q: 1 }]);
      sb.seed("answers", [
        { answer_id: 70, question_id: 50, number_a: 3, student_id: null, duration_answer: null },
      ]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "MHS_2_1_3.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      no_jawaban: 3,
      uid: "UID-A",
      waktu_diam_ms: 5000,
    });
    const a = ctx.supabase.rows("answers").find((x) => x.answer_id === 70);
    assert.equal(a.student_id, 1);
    assert.equal(a.duration_answer, 5);
    const ack = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(ack.payload.perintah, "ack_file");
  } finally {
    await ctx.close();
  }
});

test("TC-MQTT-13 audio_data MHS uid tak terdaftar -> answer student_id null + ACK", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedBase(sb);
      sb.seed("questions", [{ question_id: 50, class_id: 10, number_q: 1 }]);
    },
  });
  try {
    await ctx.mqtt.deliver("kelas/alat/audio_data", {
      file: "MHS_2_1_3.txt",
      target_kelas: "K1",
      no_pertanyaan: 1,
      no_jawaban: 3,
      uid: "GHOST",
      waktu_diam_ms: 2000,
    });
    const answers = ctx.supabase.rows("answers");
    assert.equal(answers.length, 1);
    assert.equal(answers[0].student_id, null);
    const ack = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(ack.payload.perintah, "ack_file");
  } finally {
    await ctx.close();
  }
});

test("TC-MQTT-14 sync_status: error -> error, dibatalkan -> cancelled", async () => {
  const ctx = await loadApp({ seed: seedBase });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");

    await ctx.mqtt.deliver("kelas/alat/sync_status", { status: "error", pesan: "gagal" });
    let res = await ctx.request("GET", "/api/sync-status", { cookie });
    assert.equal(res.json().state, "error");

    await ctx.mqtt.deliver("kelas/alat/sync_status", { status: "dibatalkan", pesan: "batal" });
    res = await ctx.request("GET", "/api/sync-status", { cookie });
    assert.equal(res.json().state, "cancelled");
  } finally {
    await ctx.close();
  }
});

test("MQTT payload rusak: tidak melempar error (di-catch)", async () => {
  const ctx = await loadApp({ seed: seedBase });
  try {
    await ctx.mqtt.deliver("kelas/alat/status", "bukan-json{");
    assert.ok(true, "handler tidak boleh throw keluar");
  } finally {
    await ctx.close();
  }
});
