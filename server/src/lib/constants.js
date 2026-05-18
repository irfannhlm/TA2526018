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
};

// Nilai ambang kebisingan (dikirim ke alat seperti set_timer).
const THRESHOLD_VALUES = {
  hening: 175,
  normal: 300,
  agak_bising: 400,
  sangat_bising: 750,
};

// Bucket Supabase Storage untuk file audio.
const STORAGE_BUCKET = "audio-catchnote";

// Cost factor bcrypt (sama dengan nilai lama di server.js).
const SALT_ROUNDS = 10;

module.exports = { TABLE_PK, THRESHOLD_VALUES, STORAGE_BUCKET, SALT_ROUNDS };
