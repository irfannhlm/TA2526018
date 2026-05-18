"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { loadApp, hashPassword } = require("../helpers/loadApp");

const BUCKET = "audio-catchnote";

function seedClass(sb) {
  sb.seed("classes", [{ class_id: 10, class_name: "K1" }]);
  sb.seed("users", [
    {
      user_id: 1,
      username: "admin",
      password: hashPassword("admin123"),
      role: "admin",
    },
  ]);
}

test("POST /api/upload-audio-sd DSN baru -> 200 + simpan question + url storage", async () => {
  const ctx = await loadApp({ seed: seedClass });
  try {
    const res = await ctx.requestMultipart("POST", "/api/upload-audio-sd", {
      fields: { nama_file: "DSN_2_1.wav", target_kelas: "K1" },
      file: { filename: "DSN_2_1.wav" },
    });
    assert.equal(res.status, 200);
    const body = res.json();
    assert.equal(body.success, true);
    assert.match(body.audio_file_path, /^https:\/\/fake\.storage\//);

    const qs = ctx.supabase.rows("questions");
    assert.equal(qs.length, 1);
    assert.equal(qs[0].number_q, 1);
    assert.match(qs[0].audio_file_path, /^https:\/\/fake\.storage\//);
  } finally {
    await ctx.close();
  }
});

test("POST /api/upload-audio-sd MHS -> simpan answer dengan audio", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedClass(sb);
      sb.seed("questions", [
        { question_id: 7, class_id: 10, number_q: 1, transcript_text: "" },
      ]);
    },
  });
  try {
    const res = await ctx.requestMultipart("POST", "/api/upload-audio-sd", {
      fields: { nama_file: "MHS_2_1_2.wav", target_kelas: "K1" },
      file: { filename: "MHS_2_1_2.wav" },
    });
    assert.equal(res.status, 200);
    const answers = ctx.supabase.rows("answers");
    assert.equal(answers.length, 1);
    assert.equal(answers[0].number_a, 2);
    assert.match(answers[0].audio_file_path, /^https:\/\/fake\.storage\//);
  } finally {
    await ctx.close();
  }
});

test("POST /api/upload-audio-sd nama file invalid -> 400", async () => {
  const ctx = await loadApp({ seed: seedClass });
  try {
    const res = await ctx.requestMultipart("POST", "/api/upload-audio-sd", {
      fields: { nama_file: "SALAH.wav", target_kelas: "K1" },
      file: { filename: "SALAH.wav" },
    });
    assert.equal(res.status, 400);
    assert.equal(res.json().success, false);
  } finally {
    await ctx.close();
  }
});

test("POST /api/upload-audio-sd tanpa file -> 400", async () => {
  const ctx = await loadApp({ seed: seedClass });
  try {
    const res = await ctx.requestMultipart("POST", "/api/upload-audio-sd", {
      fields: { nama_file: "DSN_2_1.wav", target_kelas: "K1" },
    });
    assert.equal(res.status, 400);
  } finally {
    await ctx.close();
  }
});

test("POST /api/upload-audio-sd duplikat WAV -> pending di queue", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedClass(sb);
      sb.seedStorage(BUCKET, ["K1/DSN_2_1.wav"]);
    },
  });
  try {
    const res = await ctx.requestMultipart("POST", "/api/upload-audio-sd", {
      fields: { nama_file: "DSN_2_1.wav", target_kelas: "K1" },
      file: { filename: "DSN_2_1.wav" },
    });
    assert.equal(res.status, 200);
    const body = res.json();
    assert.equal(body.pending, true);
    assert.ok(body.qid);

    const cookie = await ctx.loginAs("admin", "admin123");
    const q = await ctx.request("GET", "/api/duplicate-queue", { cookie });
    const { pending } = q.json();
    assert.equal(pending.length, 1);
    assert.equal(pending[0].tipe, "wav");
  } finally {
    await ctx.close();
  }
});

test("POST /api/upload-audio (manual) -> update answers.audio_file_path", async () => {
  const ctx = await loadApp({
    seed(sb) {
      sb.seed("answers", [
        { answer_id: 5, question_id: 1, transcript_text: "" },
      ]);
    },
  });
  let createdFile;
  try {
    const res = await ctx.requestMultipart("POST", "/api/upload-audio", {
      fields: { log_id: "5" },
      file: { filename: "rec.wav" },
    });
    assert.equal(res.status, 200);
    const body = res.json();
    assert.equal(body.status, "success");
    assert.match(body.url, /^\/recordings\//);
    createdFile = body.url.split("/").pop();

    const ans = ctx.supabase.rows("answers").find((a) => a.answer_id === 5);
    assert.equal(ans.audio_file_path, body.url);
  } finally {
    await ctx.close();
    if (createdFile) {
      const p = path.join(
        __dirname,
        "..",
        "..",
        "public",
        "recordings",
        createdFile,
      );
      if (fs.existsSync(p)) fs.unlinkSync(p);
    }
  }
});
