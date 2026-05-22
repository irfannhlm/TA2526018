"use strict";

// Skema input zod untuk endpoint /api/*.

const { z } = require("zod");

const uploadAudioSdSchema = z.object({
  nama_file: z.string().min(1).max(128),
  target_kelas: z.string().min(1).max(128),
});

const uploadAudioSchema = z.object({
  log_id: z.coerce.number().int().positive(),
});

const duplicateResolveSchema = z.object({
  qid: z.coerce.number().int().positive(),
  action: z.enum(["skip", "replace"]),
});

const sdSyncKeputusanSchema = z.object({
  keputusan: z.enum(["ulangi", "batalkan"]),
});

module.exports = {
  uploadAudioSdSchema,
  uploadAudioSchema,
  duplicateResolveSchema,
  sdSyncKeputusanSchema,
};
