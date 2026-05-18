"use strict";

// Konfigurasi Multer (upload audio). Dipindahkan apa adanya dari server.js
// (behavior-preserving). Path tetap menunjuk ke folder di root server/.

const path = require("path");
const fs = require("fs");
const multer = require("multer");

const SERVER_ROOT = path.join(__dirname, "..", "..", "..");

const uploadDir = path.join(SERVER_ROOT, "public/recordings");
if (!fs.existsSync(uploadDir)) fs.mkdirSync(uploadDir, { recursive: true });

const localStorage = multer.diskStorage({
  destination: (req, file, cb) => cb(null, uploadDir),
  filename: (req, file, cb) =>
    cb(null, `audio-${Date.now()}${path.extname(file.originalname)}`),
});
const upload = multer({ storage: localStorage });

const tempDir = path.join(SERVER_ROOT, "temp_audio");
if (!fs.existsSync(tempDir)) fs.mkdirSync(tempDir, { recursive: true });
const uploadTemp = multer({ dest: tempDir });

// Upload kecil di memori (mis. file CSV impor mahasiswa).
const uploadMemory = multer({
  storage: multer.memoryStorage(),
  limits: { fileSize: 2 * 1024 * 1024 },
});

module.exports = { upload, uploadTemp, uploadMemory, uploadDir, tempDir };
