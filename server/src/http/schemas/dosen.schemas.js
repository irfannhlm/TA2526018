"use strict";

// Skema input zod untuk endpoint /dosen/*. Sengaja longgar pada
// field bebas (transcript, dll.) namun ketat pada ID/nilai/enum.

const { z } = require("zod");

const optStr = z
  .string()
  .optional()
  .or(z.literal("").transform(() => undefined));

// nilai: 1-100 atau kosong/null (untuk membatalkan nilai).
const nilaiSchema = z.object({
  nilai: z
    .union([z.literal(""), z.null(), z.coerce.number().int().min(1).max(100)])
    .optional(),
});

const answerPatchSchema = z.object({
  transcript: z.string().optional(),
  student_id: z.union([z.literal(""), z.coerce.number().int()]).optional(),
  duration_answer: z
    .union([z.literal(""), z.coerce.number().nonnegative()])
    .optional(),
});

const questionPatchSchema = z.object({
  transcript: z.string().optional(),
});

const editLogSchema = z.object({
  id: z.coerce.number().int().positive(),
  student_id: optStr,
  edit_date: z.string().min(1),
  edit_time: z.string().min(1),
  transcript: z.string().optional(),
  question: z.string().optional(),
  current_class: optStr,
});

const updateSettingsSchema = z.object({
  status: optStr,
  mode: optStr,
  timer: z
    .union([z.literal(""), z.coerce.number().int().positive()])
    .optional(),
  threshold: z
    .enum(["hening", "normal", "agak_bising", "sangat_bising"])
    .optional()
    .or(z.literal("").transform(() => undefined)),
  current_class: optStr,
});

const syncUidSchema = z.object({
  mode: z.string().optional(),
  current_class: optStr,
});

const requestAudioSchema = z.object({
  id: z.coerce.number().int().positive(),
  current_class: optStr,
});

const requestSyncSchema = z.object({
  current_class: optStr,
});

const addStudentSchema = z.object({
  name: z.string().min(1),
  nim: z.string().min(1).max(64),
  rfid: z.string().min(1).max(64),
  kelas: optStr,
  current_class: optStr,
});

const manageUidSchema = z.object({
  action: z.enum(["delete", "delete_all", "add_single", "add_date"]),
  uid: optStr,
  entry_id: z.union([z.literal(""), z.coerce.number().int()]).optional(),
  // student_id boleh tunggal (legacy) atau array (multi-checklist).
  student_id: z
    .union([
      z.literal(""),
      z.coerce.number().int(),
      z.array(z.coerce.number().int()),
    ])
    .optional(),
  target_date: optStr,
  current_class: optStr,
});

module.exports = {
  nilaiSchema,
  answerPatchSchema,
  questionPatchSchema,
  editLogSchema,
  updateSettingsSchema,
  syncUidSchema,
  requestAudioSchema,
  requestSyncSchema,
  addStudentSchema,
  manageUidSchema,
};
