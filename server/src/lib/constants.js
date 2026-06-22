"use strict";

// Konstanta lintas-modul. Nilai sengaja sama persis dengan yang sebelumnya
// hardcode di server.js (refactor ini behavior-preserving).

// Primary key tiap tabel Supabase.
const TABLE_PK = {
  answers: "answer_id",
  questions: "question_id",
  students: "student_id",
  classes: "class_id",
  devices: "device_id",
  users: "user_id",
  class_students: "class_student_id",
  duplicate_queue: "qid",
};

// Nilai ambang kebisingan preset (dikirim ke alat seperti set_timer).
// Selain preset ini, dosen bisa memilih "custom" dan memasukkan nilai sendiri.
const THRESHOLD_VALUES = {
  hening: 200,
  normal: 300,
  bising: 400,
};

// Bucket Supabase Storage untuk file audio.
const STORAGE_BUCKET = "audio-catchnote";

// Cost factor bcrypt (sama dengan nilai lama di server.js).
const SALT_ROUNDS = 10;

// Peran pengguna.
const ROLES = { ADMIN: "admin", DOSEN: "dosen" };

module.exports = {
  TABLE_PK,
  THRESHOLD_VALUES,
  STORAGE_BUCKET,
  SALT_ROUNDS,
  ROLES,
};
