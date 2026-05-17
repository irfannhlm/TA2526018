const express = require("express");
const app = express();
const path = require("path");
const mqtt = require("mqtt");
const multer = require("multer");
const fs = require("fs");
const { createClient } = require("@supabase/supabase-js");
require("dotenv").config();
const {
  transcribeAnswer,
  transcribeQuestion,
  sweepUntranscribed,
} = require("./Deepgramservice");
const bcrypt = require("bcrypt");
const SALT_ROUNDS = 10;
const session = require("express-session");
const MemoryStore = require("memorystore")(session);

// ================= KONFIGURASI SUPABASE =================
const supabaseUrl = process.env.SUPABASE_URL.replace(
  /\/rest\/v1\/?$/,
  "",
).replace(/\/$/, "");
const supabaseKey = process.env.SUPABASE_KEY;
const supabase = createClient(supabaseUrl, supabaseKey);
console.log("✅ Supabase client dikonfigurasi:", supabaseUrl);

// ================= HELPER SUPABASE =================
async function sbSelect(table, filters = {}, columns = "*", extra = {}) {
  let q = supabase.from(table).select(columns);
  for (const [col, val] of Object.entries(filters)) {
    if (Array.isArray(val)) q = q.in(col, val);
    else q = q.eq(col, val);
  }
  if (extra.order)
    q = q.order(extra.order.col, { ascending: extra.order.asc ?? true });
  if (extra.limit) q = q.limit(extra.limit);
  const { data, error } = await q;
  if (error) throw error;
  return data || [];
}

async function sbInsert(table, row) {
  const { data, error } = await supabase
    .from(table)
    .insert(row)
    .select()
    .single();
  if (error) throw error;
  return data;
}

const TABLE_PK = {
  answers: "answer_id",
  questions: "question_id",
  students: "student_id",
  classes: "class_id",
  devices: "device_id",
  users: "user_id",
  class_students: "class_student_id",
};

async function sbUpdate(table, filters, updates) {
  let q = supabase.from(table).update(updates);
  const entries = Object.entries(filters);
  if (entries.length === 0) {
    q = q.neq(TABLE_PK[table] || "id", -1);
  } else {
    for (const [col, val] of entries) {
      if (Array.isArray(val)) q = q.in(col, val);
      else q = q.eq(col, val);
    }
  }
  const { data, error } = await q.select();
  if (error) throw error;
  return data || [];
}

async function sbDelete(table, filters) {
  let q = supabase.from(table).delete();
  for (const [col, val] of Object.entries(filters)) {
    if (Array.isArray(val)) q = q.in(col, val);
    else q = q.eq(col, val);
  }
  const { error } = await q;
  if (error) throw error;
}

function toInt(val) {
  if (val === null || val === undefined || val === "") return null;
  const n = parseInt(val, 10);
  return isNaN(n) ? null : n;
}

// ================= HELPER SUPABASE STORAGE =================
const STORAGE_BUCKET = "audio-catchnote";

async function uploadToSupabaseStorage(localFilePath, storagePath) {
  const fileBuffer = fs.readFileSync(localFilePath);
  const { data, error } = await supabase.storage
    .from(STORAGE_BUCKET)
    .upload(storagePath, fileBuffer, {
      contentType: "audio/wav",
      upsert: true,
    });
  if (error) throw error;

  const { data: urlData } = supabase.storage
    .from(STORAGE_BUCKET)
    .getPublicUrl(storagePath);
  return urlData.publicUrl;
}

// ================= EXPRESS SETUP =================
app.set("view engine", "ejs");
app.set("views", path.join(__dirname, "views"));
app.use(express.urlencoded({ extended: true }));
app.use(express.json());
app.use(express.static(path.join(__dirname, "public")));

// ================= SESSION =================
app.use(
  session({
    secret: process.env.SESSION_SECRET || "catchnote-secret-key",
    resave: false,
    saveUninitialized: false,
    store: new MemoryStore({
      checkPeriod: 86400000, // bersihkan sesi kedaluwarsa tiap 24 jam
    }),
    cookie: {
      httpOnly: true,
      maxAge: 8 * 60 * 60 * 1000,
    },
  }),
);

// ================= MIDDLEWARE AUTH =================
function requireLogin(req, res, next) {
  if (!req.session.user) return res.redirect("/");
  next();
}

function requireRole(...roles) {
  return (req, res, next) => {
    if (!req.session.user) return res.redirect("/");
    if (!roles.includes(req.session.user.role)) return res.redirect("/");
    next();
  };
}

// ================= CEK KONEKSI SUPABASE =================
(async () => {
  try {
    const { error } = await supabase
      .from("devices")
      .select("device_id")
      .limit(1);
    if (error) throw error;
    console.log("✅ Berhasil terhubung ke Supabase!");
  } catch (err) {
    console.error("❌ Error koneksi Supabase:", err.message);
  }
})();

// Reset status alat saat startup
(async () => {
  try {
    await sbUpdate("devices", {}, { status: "offline", battery_level: null });
    console.log("🔄 System Startup: Status alat di-reset.");
  } catch (err) {
    console.error("❌ Gagal reset status devices:", err.message);
  }
})();

// ================= DATA SESI (RAM) =================
let sessionData = {
  status: "stopped",
  mode: "dosen_rec",
  maxTime: 5,
  threshold: "normal",
  scannedList: [],
};

// Nilai ambang kebisingan (hardcode) — dikirim ke alat seperti set_timer
const THRESHOLD_VALUES = {
  hening: 175,
  normal: 300,
  agak_bising: 400,
  sangat_bising: 750,
};

let scanCounter = 0;

// ================= STATUS SYNC SD CARD (RAM) =================
let syncStatus = {
  state: "idle", // "idle" | "loading" | "done" | "error" | "cancelled"
  pesan: "",
  total: 0,
  berhasil: 0,
  updatedAt: Date.now(),
};

// ================= ANTRIAN DUPLIKAT (RAM) =================
let duplicateQueue = [];
let duplicateQueueCounter = 0;

// ================= KONEKSI MQTT =================
const mqttClient = mqtt.connect({
  host: "c4bbf4787735464dadc96ca13e4a9c6b.s1.eu.hivemq.cloud",
  port: 8883,
  protocol: "mqtts",
  username: "catchnote",
  password: "Ta2526018",
  rejectUnauthorized: true,
  clientId: `catchnote_server_${Math.random().toString(16).slice(2, 8)}`, // ID unik tiap koneksi
  clean: true,
});

mqttClient.on("connect", () => {
  console.log("✅ Backend terhubung ke HiveMQ Cloud!");
  mqttClient.subscribe(
    [
      "kelas/alat/rfid",
      "kelas/alat/status",
      "kelas/alat/audio_data",
      "kelas/alat/sync_status",
    ],
    (err) => {
      if (!err)
        console.log(
          "✅ Subscribe Berhasil: rfid, status, audio_data, sync_status",
        );
      else console.error("❌ Gagal berlangganan topik:", err);
    },
  );
});

mqttClient.on("error", (err) => {
  console.error("❌ MQTT Error:", err.message);
});

mqttClient.on("message", async (topic, message) => {
  try {
    const payload = JSON.parse(message.toString());
    console.log(`\n📩 Pesan Masuk [${topic}]:`, payload);

    // ── Handler: status device ──
    if (topic === "kelas/alat/status") {
      const { device_id, status, battery } = payload;
      const updated = await sbUpdate(
        "devices",
        { device_id },
        { status, battery_level: battery },
      );
      if (!updated.length)
        console.log(`⚠️ GAGAL: Tidak ada device dengan ID ${device_id}`);
      else console.log(`✅ BERHASIL: Status ${device_id} sekarang ${status}`);
    }

    // ── Handler: tap RFID ──
    if (topic === "kelas/alat/rfid") {
      const { uid, action } = payload;
      if (action === "tap_rfid" && uid) {
        console.log(`📝 RFID Tap: ${uid}`);
        scanCounter++;
        sessionData.scannedList.unshift({
          id: scanCounter,
          uid,
          time: new Date().toLocaleTimeString(),
        });

        const students = await sbSelect("students", { rfid_uid: uid });
        if (students.length > 0) {
          const student = students[0];
          const classRel = await sbSelect("class_students", {
            student_id: student.student_id,
          });
          if (classRel.length > 0) {
            const classId = classRel[0].class_id;
            const questions = await sbSelect(
              "questions",
              { class_id: classId },
              "*",
              { order: { col: "created_at", asc: false }, limit: 1 },
            );
            let qId = questions.length > 0 ? questions[0].question_id : null;
            if (!qId) {
              const newQ = await sbInsert("questions", {
                class_id: classId,
                device_id: null,
                transcript_text: "Pertanyaan baru",
              });
              qId = newQ.question_id;
            }
            await sbInsert("answers", {
              question_id: qId,
              student_id: student.student_id,
              transcript_text: "",
              class_id: classId,
            });
            console.log(`✅ Log diskusi tersimpan untuk: ${student.name}`);
          }
        }
      }
    }

    // ── Handler: data TXT dari SD Card ESP32 ──
    if (topic === "kelas/alat/audio_data") {
      const {
        file,
        target_kelas,
        no_pertanyaan,
        no_jawaban,
        uid,
        waktu_diam_ms,
        tanggal,
      } = payload;

      console.log(`\n📂 [SD SYNC TXT] File diterima: ${file}`);
      console.log(`   Kelas        : ${target_kelas}`);
      console.log(`   No Pertanyaan: ${no_pertanyaan}`);

      const info = parseNamaFile(file);
      if (!info) {
        console.warn(`⚠️ Format nama file tidak dikenal: ${file}`);
        mqttClient.publish(
          "kelas/alat/perintah",
          JSON.stringify({ perintah: "ack_file", file }),
        );
        return;
      }

      try {
        const classes = await sbSelect("classes", { class_name: target_kelas });
        const classId = classes.length > 0 ? classes[0].class_id : null;

        if (!classId) {
          console.warn(
            `⚠️ Kelas "${target_kelas}" tidak ditemukan di database.`,
          );
          mqttClient.publish(
            "kelas/alat/perintah",
            JSON.stringify({ perintah: "ack_file", file }),
          );
          return;
        }

        if (info.tipe === "dsn") {
          // DSN_{device_id}_{no_pertanyaan}.txt dibaca ESP (2 baris):
          //   Baris 1 → no_pertanyaan (integer)
          //   Baris 2 → tanggal       (string, format DD-MM-YYYY)
          //
          // Payload MQTT yang dikirim ESP (dari bacaFileSdKeJson):
          //   {
          //     "file": "DSN_2_1.txt",
          //     "target_kelas": "...",
          //     "device_id": 2,
          //     "no_pertanyaan": 1,
          //     "tanggal": "13-05-2026",
          //     "uid": "DOSEN"
          //   }
          //
          // device_id dan number_q di DB diambil dari parseNamaFile(file)

          const numberQ = parseInt(no_pertanyaan);
          // BARU
          const rawTanggal = (tanggal ?? "").trim();
          const parsedTanggal = (() => {
            const parts = rawTanggal.split("-");
            if (
              parts.length === 3 &&
              parts[0].length <= 2 &&
              parts[2].length === 4
            )
              return `${parts[2]}-${parts[1].padStart(2, "0")}-${parts[0].padStart(2, "0")}`;
            return rawTanggal;
          })();

          console.log(`   number_q (dari payload) : ${numberQ}`);
          console.log(`   number_q (dari namafile): ${info.no_pertanyaan}`);
          console.log(`   Tanggal                 : ${parsedTanggal}`);

          // Validasi: kedua sumber number_q harus konsisten
          if (isNaN(numberQ)) {
            console.warn(
              `⚠️ [DSN TXT] Field 'no_pertanyaan' tidak valid di payload: ${no_pertanyaan}`,
            );
            mqttClient.publish(
              "kelas/alat/perintah",
              JSON.stringify({ perintah: "ack_file", file }),
            );
            return;
          }
          if (!parsedTanggal) {
            console.warn(
              `⚠️ [DSN TXT] Field 'tanggal' kosong atau tidak ada di payload`,
            );
            mqttClient.publish(
              "kelas/alat/perintah",
              JSON.stringify({ perintah: "ack_file", file }),
            );
            return;
          }
          if (numberQ !== info.no_pertanyaan) {
            console.warn(
              `⚠️ [DSN TXT] Inkonsistensi: no_pertanyaan payload (${numberQ}) ≠ nama file (${info.no_pertanyaan}). Menggunakan payload.`,
            );
          }

          const existing = await sbSelect("questions", {
            class_id: classId,
            number_q: numberQ,
          });

          // CEK DUPLIKAT: hanya jika date_id sudah terisi sebelumnya
          if (existing.length > 0 && existing[0].date_id) {
            duplicateQueueCounter++;
            duplicateQueue.push({
              qid: duplicateQueueCounter,
              file,
              tipe: "txt_dsn",
              target_kelas,
              no_pertanyaan: numberQ,
              tanggal: parsedTanggal,
              existingId: existing[0].question_id,
              existingPreview: `Pertanyaan #${numberQ} | Tanggal lama: ${existing[0].date_id} → baru: ${parsedTanggal}`,
              resolvedAt: null,
            });
            console.log(
              `⏸️ [TXT DSN] Duplikat ditahan — menunggu keputusan user. qid=${duplicateQueueCounter}`,
            );
            return; // Tahan, tidak ACK
          }

          // Tidak duplikat — proses normal
          if (existing.length > 0) {
            // Update date_id (dan device_id jika belum terisi) pada pertanyaan yang sudah ada
            const updateFields = { date_id: parsedTanggal };
            if (info.device_id != null && existing[0].device_id == null) {
              updateFields.device_id = info.device_id;
            }
            await sbUpdate(
              "questions",
              { question_id: existing[0].question_id },
              updateFields,
            );
            console.log(
              `✅ [DSN TXT] questions.id=${existing[0].question_id} → date_id="${parsedTanggal}" diperbarui.`,
            );
          } else {
            // Buat question baru
            await sbInsert("questions", {
              class_id: classId,
              device_id: info.device_id ?? null,
              number_q: numberQ,
              date_id: parsedTanggal,
              transcript_text: "",
            });
            console.log(
              `✅ [DSN TXT] question baru dibuat: number_q=${numberQ}, date_id="${parsedTanggal}".`,
            );
          }
        } else if (info.tipe === "mhs") {
          console.log(`   No Jawaban   : ${no_jawaban}`);
          console.log(`   UID          : ${uid}`);
          console.log(`   Waktu Diam   : ${waktu_diam_ms} ms`);

          const students = await sbSelect("students", { rfid_uid: uid });
          const student = students.length > 0 ? students[0] : null;
          if (!student)
            console.warn(`⚠️ UID ${uid} tidak ditemukan di database.`);

          let qRows = await sbSelect("questions", {
            class_id: classId,
            number_q: info.no_pertanyaan,
          });
          let qId;
          if (qRows.length > 0) {
            qId = qRows[0].question_id;
          } else {
            const newQ = await sbInsert("questions", {
              class_id: classId,
              device_id: info.device_id ?? null,
              number_q: info.no_pertanyaan,
              transcript_text: "",
            });
            qId = newQ.question_id;
          }

          const aRows = await sbSelect("answers", {
            question_id: qId,
            number_a: info.no_jawaban,
          });

          // CEK DUPLIKAT: jika sudah ada data jawaban
          if (
            aRows.length > 0 &&
            (aRows[0].student_id || aRows[0].duration_answer)
          ) {
            duplicateQueueCounter++;
            duplicateQueue.push({
              qid: duplicateQueueCounter,
              file,
              tipe: "txt_mhs",
              target_kelas,
              no_pertanyaan: info.no_pertanyaan,
              no_jawaban: info.no_jawaban,
              uid,
              waktu_diam_ms,
              classId,
              studentId: student ? student.student_id : null,
              existingAnswerId: aRows[0].answer_id,
              existingPreview: `Q${info.no_pertanyaan} A${info.no_jawaban} | ${student?.name || uid}`,
              resolvedAt: null,
            });
            console.log(
              `⏸️ [TXT MHS] Duplikat ditahan — menunggu keputusan user. qid=${duplicateQueueCounter}`,
            );
            return; // Tahan, tidak ACK
          }

          // Tidak duplikat — proses normal
          if (aRows.length > 0) {
            await sbUpdate(
              "answers",
              { answer_id: aRows[0].answer_id },
              {
                student_id: student ? student.student_id : null,
                duration_answer: waktu_diam_ms / 1000,
                class_id: classId,
              },
            );
            console.log(
              `✅ [MHS TXT] answers.id=${aRows[0].answer_id} diperbarui.`,
            );
          } else {
            await sbInsert("answers", {
              question_id: qId,
              student_id: student ? student.student_id : null,
              transcript_text: "",
              duration_answer: waktu_diam_ms / 1000,
              number_a: info.no_jawaban,
              class_id: classId,
            });
            console.log(
              `✅ [MHS TXT] answer baru | Q${info.no_pertanyaan} A${info.no_jawaban} | student=${student?.name || "unknown"}`,
            );
          }
        }

        // ACK ke ESP hanya jika tidak duplikat
        const ackPayload = JSON.stringify({ perintah: "ack_file", file });
        mqttClient.publish("kelas/alat/perintah", ackPayload, (err) => {
          if (err) console.error("❌ Gagal kirim ACK:", err);
          else console.log(`📤 ACK dikirim untuk file: ${file}`);
        });
      } catch (dbErr) {
        console.error(
          "❌ [SD SYNC TXT] Gagal simpan ke database:",
          dbErr.message,
        );
      }
    }

    // ── Handler: sync_status dari ESP ──
    if (topic === "kelas/alat/sync_status") {
      const { status, pesan, total, berhasil } = payload;
      const timestamp = new Date().toLocaleTimeString("id-ID");

      // Update syncStatus agar bisa di-polling frontend
      syncStatus = {
        state:
          status === "selesai"
            ? "done"
            : status === "error"
              ? "error"
              : status === "dibatalkan"
                ? "cancelled"
                : status === "progress"
                  ? "loading"
                  : "idle",
        pesan: pesan || "",
        total: total ?? 0,
        berhasil: berhasil ?? 0,
        updatedAt: Date.now(),
      };

      if (status === "selesai") {
        console.log(`\n🎉 [SD SYNC] SELESAI [${timestamp}]`);
        console.log(`   Total : ${total} file`);
        console.log(`   Sukses: ${berhasil} file`);
        console.log(`   Pesan : ${pesan}`);
      } else if (status === "error") {
        console.warn(`\n⚠️ [SD SYNC] ERROR [${timestamp}]: ${pesan}`);
        console.warn(`   Progress: ${berhasil}/${total}`);
      } else if (status === "dibatalkan") {
        console.log(`\n🛑 [SD SYNC] DIBATALKAN [${timestamp}]: ${pesan}`);
      } else if (status === "progress") {
        console.log(`\n🔄 [SD SYNC] Progress: ${berhasil}/${total} — ${pesan}`);
      }
    }
  } catch (error) {
    console.error("❌ MQTT Error:", error);
  }
});

// ================= HELPER: Parse nama file dari ESP =================
function parseNamaFile(namaFile) {
  const dsnMatch = namaFile.match(/^DSN_(\d+)_(\d+)\.(wav|txt)$/i);
  if (dsnMatch) {
    return {
      tipe: "dsn",
      device_id: parseInt(dsnMatch[1]),
      no_pertanyaan: parseInt(dsnMatch[2]),
    };
  }
  const mhsMatch = namaFile.match(/^MHS_(\d+)_(\d+)_(\d+)\.(wav|txt)$/i);
  if (mhsMatch) {
    return {
      tipe: "mhs",
      device_id: parseInt(mhsMatch[1]),
      no_pertanyaan: parseInt(mhsMatch[2]),
      no_jawaban: parseInt(mhsMatch[3]),
    };
  }
  return null;
}
const parseNamaAudio = parseNamaFile;

// ================= HELPER: Simpan URL audio ke DB =================
async function simpanAudioKeDB(info, classId, audioUrl) {
  if (info.tipe === "dsn") {
    const qRows = await sbSelect("questions", {
      class_id: classId,
      number_q: info.no_pertanyaan,
    });
    if (qRows.length > 0) {
      const updateFields = { audio_file_path: audioUrl };
      if (info.device_id != null && qRows[0].device_id == null) {
        updateFields.device_id = info.device_id;
      }
      await sbUpdate(
        "questions",
        { question_id: qRows[0].question_id },
        updateFields,
      );
      console.log(
        `✅ [DSN WAV] audio_file_path disimpan di questions.id=${qRows[0].question_id}`,
      );
    } else {
      const newQ = await sbInsert("questions", {
        class_id: classId,
        device_id: info.device_id ?? null,
        number_q: info.no_pertanyaan,
        audio_file_path: audioUrl,
        transcript_text: "",
      });
      console.log(`✅ [DSN WAV] question baru dibuat id=${newQ.question_id}`);
    }
  } else if (info.tipe === "mhs") {
    let qRows = await sbSelect("questions", {
      class_id: classId,
      number_q: info.no_pertanyaan,
    });
    let qId;
    if (qRows.length > 0) {
      qId = qRows[0].question_id;
    } else {
      const newQ = await sbInsert("questions", {
        class_id: classId,
        device_id: info.device_id ?? null,
        number_q: info.no_pertanyaan,
        transcript_text: "",
      });
      qId = newQ.question_id;
      console.log(`✅ [MHS WAV] question baru dibuat id=${qId}`);
    }

    const aRows = await sbSelect("answers", {
      question_id: qId,
      number_a: info.no_jawaban,
    });
    if (aRows.length > 0) {
      await sbUpdate(
        "answers",
        { answer_id: aRows[0].answer_id },
        { audio_file_path: audioUrl },
      );
      console.log(
        `✅ [MHS WAV] audio_file_path disimpan di answers.id=${aRows[0].answer_id}`,
      );
    } else {
      const newA = await sbInsert("answers", {
        question_id: qId,
        transcript_text: "",
        audio_file_path: audioUrl,
        number_a: info.no_jawaban,
        class_id: classId,
      });
      console.log(`✅ [MHS WAV] answer baru dibuat id=${newA.answer_id}`);
    }
  }
}

// ================= KONFIGURASI UPLOAD (MULTER) =================
const uploadDir = path.join(__dirname, "public/recordings");
if (!fs.existsSync(uploadDir)) fs.mkdirSync(uploadDir, { recursive: true });

const localStorage = multer.diskStorage({
  destination: (req, file, cb) => cb(null, uploadDir),
  filename: (req, file, cb) =>
    cb(null, `audio-${Date.now()}${path.extname(file.originalname)}`),
});
const upload = multer({ storage: localStorage });

const tempDir = path.join(__dirname, "temp_audio");
if (!fs.existsSync(tempDir)) fs.mkdirSync(tempDir, { recursive: true });
const uploadTemp = multer({ dest: tempDir });

// ================= ENDPOINT: Terima audio WAV dari ESP32 via HTTP =================
app.post(
  "/api/upload-audio-sd",
  uploadTemp.single("audio"),
  async (req, res) => {
    const tempPath = req.file?.path;
    const namaFile = req.body.nama_file || req.file?.originalname || "";
    const targetKelas = req.body.target_kelas || "";

    console.log(
      `\n🎵 [AUDIO SD] Menerima: ${namaFile} | Kelas: ${targetKelas}`,
    );

    if (!tempPath || !namaFile) {
      if (tempPath) fs.unlinkSync(tempPath);
      return res
        .status(400)
        .json({ success: false, message: "File atau nama_file tidak ada." });
    }

    const info = parseNamaFile(namaFile);
    if (!info) {
      fs.unlinkSync(tempPath);
      return res.status(400).json({
        success: false,
        message: `Format nama file tidak dikenal: ${namaFile}`,
      });
    }

    try {
      const folderStorage = targetKelas.replace(/\s+/g, "_");
      const storagePath = `${folderStorage}/${namaFile}`;

      // CEK DUPLIKAT WAV di Supabase Storage
      const { data: existingFiles } = await supabase.storage
        .from(STORAGE_BUCKET)
        .list(folderStorage, { search: namaFile });

      const fileExists =
        existingFiles && existingFiles.some((f) => f.name === namaFile);

      if (fileExists) {
        const classes = await sbSelect("classes", { class_name: targetKelas });
        const classId = classes.length > 0 ? classes[0].class_id : null;

        duplicateQueueCounter++;
        const qid = duplicateQueueCounter;
        duplicateQueue.push({
          qid,
          file: namaFile,
          tipe: "wav",
          target_kelas: targetKelas,
          tempPath, // simpan path temp untuk diproses nanti
          storagePath,
          classId,
          info,
          existingPreview: `File audio: ${namaFile} | Kelas: ${targetKelas}`,
          resolvedAt: null,
        });
        console.log(
          `⏸️ [WAV] Duplikat ditahan — menunggu keputusan user. qid=${qid}`,
        );
        return res.status(200).json({
          success: true,
          pending: true,
          qid,
          message: "File duplikat, menunggu keputusan user.",
        });
      }

      // Tidak duplikat — upload normal
      console.log(`☁️  Upload ke Supabase Storage: ${storagePath}`);
      const audioUrl = await uploadToSupabaseStorage(tempPath, storagePath);
      console.log(`✅ Supabase Storage OK: ${audioUrl}`);
      fs.unlinkSync(tempPath);

      const classes = await sbSelect("classes", { class_name: targetKelas });
      const classId = classes.length > 0 ? classes[0].class_id : null;

      if (!classId) {
        console.warn(`⚠️ Kelas "${targetKelas}" tidak ditemukan di DB.`);
        return res.status(200).json({
          success: true,
          audio_file_path: audioUrl,
          warning: `Kelas tidak ditemukan di DB: ${targetKelas}`,
        });
      }

      await simpanAudioKeDB(info, classId, audioUrl);

      // ── Auto-transcribe langsung setelah audio tersimpan ──
      // Jalankan di background (tidak blocking response ke ESP32)
      setImmediate(async () => {
        try {
          if (info.tipe === "dsn") {
            const qRows = await sbSelect("questions", {
              class_id: classId,
              number_q: info.no_pertanyaan,
            });
            if (
              qRows.length > 0 &&
              (!qRows[0].transcript_text ||
                qRows[0].transcript_text.trim() === "")
            ) {
              await transcribeQuestion(
                sbUpdate,
                qRows[0].question_id,
                audioUrl,
              );
            }
          } else if (info.tipe === "mhs") {
            const qRows = await sbSelect("questions", {
              class_id: classId,
              number_q: info.no_pertanyaan,
            });
            if (qRows.length > 0) {
              const aRows = await sbSelect("answers", {
                question_id: qRows[0].question_id,
                number_a: info.no_jawaban,
              });
              if (
                aRows.length > 0 &&
                (!aRows[0].transcript_text ||
                  aRows[0].transcript_text.trim() === "")
              ) {
                await transcribeAnswer(sbUpdate, aRows[0].answer_id, audioUrl);
              }
            }
          }
        } catch (e) {
          console.error("❌ [Auto-transcribe] Error:", e.message);
        }
      });

      res
        .status(200)
        .json({ success: true, audio_file_path: audioUrl, file: namaFile });
    } catch (err) {
      console.error("❌ [AUDIO SD] Error:", err.message);
      if (tempPath && fs.existsSync(tempPath)) fs.unlinkSync(tempPath);
      res.status(500).json({ success: false, message: err.message });
    }
  },
);

// ================= API REALTIME =================
app.get("/api/realtime-data", async (req, res) => {
  try {
    const currentClass = req.query.kelas || null;

    const devices = await sbSelect(
      "devices",
      {},
      "device_id,name,status,battery_level",
    );
    const mappedDevices = devices.map((d) => ({
      id: d.device_id,
      name: d.name,
      status: d.status,
      battery: d.battery_level,
    }));

    const allStudents = await sbSelect("students");

    let studentsInClass = [];
    if (currentClass) {
      const { data, error } = await supabase
        .from("students")
        .select(
          "student_id, class_students!inner(class_id), classes!inner(class_name)",
        )
        .eq("classes.class_name", currentClass);
      if (!error) studentsInClass = data || [];
    }

    const mappedList = sessionData.scannedList.map((item) => {
      const student = allStudents.find((s) => s.rfid_uid === item.uid);
      let isWrongClass = false;
      if (student && currentClass) {
        const isInClass = studentsInClass.find(
          (x) => x.student_id === student.student_id,
        );
        if (!isInClass) isWrongClass = true;
      }
      return {
        id: item.id,
        uid: item.uid,
        time: item.time || "",
        name: student ? student.name : "Tidak Terdaftar",
        nim: student ? student.nim : "-",
        isRegistered: !!student,
        isWrongClass,
      };
    });

    res.json({ scannedList: mappedList, devices: mappedDevices });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "Gagal mengambil data realtime" });
  }
});

// ================= API REALTIME LOGS =================
app.get("/api/realtime-logs", async (req, res) => {
  try {
    const currentClass = req.query.kelas || null;
    if (!currentClass) return res.json({ logs: [] });

    const { data: classData, error: classErr } = await supabase
      .from("classes")
      .select("class_id")
      .eq("class_name", currentClass)
      .single();
    if (classErr || !classData) return res.json({ logs: [] });

    const { data: csData } = await supabase
      .from("class_students")
      .select("student_id, students(student_id,name,nim)")
      .eq("class_id", classData.class_id);

    const studentIds = (csData || []).map((r) => r.student_id);

    const { data: questions, error: qErr } = await supabase
      .from("questions")
      .select(
        `
        question_id,
        transcript_text,
        created_at,
        audio_file_path,
        answers(answer_id, transcript_text, audio_file_path, duration_answer, nilai, student_id, students(name,nim))
      `,
      )
      .eq("class_id", classData.class_id)
      .order("created_at", { ascending: false });

    if (qErr) throw qErr;

    const formatted = [];
    for (const q of (questions || [])) {
      const relevantAnswers = (q.answers || []).filter((a) =>
        studentIds.includes(a.student_id),
      );
      if (relevantAnswers.length > 0) {
        for (const a of relevantAnswers) {
          formatted.push({
            id: a.answer_id,
            question_id: q.question_id,
            student_id: a.student_id,
            date_obj: q.created_at ? new Date(q.created_at).toISOString() : null,
            name: a.students?.name,
            nim: a.students?.nim,
            question: q.transcript_text,
            question_audio_url: q.audio_file_path || null,
            transcript: a.transcript_text,
            audio_url: a.audio_file_path,
            duration_answer: a.duration_answer,
            nilai: a.nilai ?? null,
          });
        }
      } else {
        formatted.push({
          id: null,
          question_id: q.question_id,
          date_obj: q.created_at ? new Date(q.created_at).toISOString() : null,
          name: null,
          nim: null,
          question: q.transcript_text,
          question_audio_url: q.audio_file_path || null,
          transcript: null,
          audio_url: null,
          duration_answer: null,
        });
      }
    }

    res.json({ logs: formatted });
  } catch (err) {
    console.error("❌ Gagal ambil realtime logs:", err.message);
    res.status(500).json({ error: "Gagal mengambil data logs" });
  }
});

// ================= API: STATUS SYNC SD CARD =================
app.get("/api/sync-status", requireLogin, (req, res) => {
  // Auto-timeout jika loading lebih dari 60 detik tanpa update
  const elapsed = Date.now() - syncStatus.updatedAt;
  if (syncStatus.state === "loading" && elapsed > 60000) {
    syncStatus.state = "error";
    syncStatus.pesan = "Timeout — perangkat tidak merespons.";
  }
  res.json(syncStatus);
});

app.post("/api/sync-status/reset", requireLogin, (req, res) => {
  syncStatus = {
    state: "loading",
    pesan: "Mengirim permintaan ke perangkat...",
    total: 0,
    berhasil: 0,
    updatedAt: Date.now(),
  };
  res.json({ ok: true });
});

// ================= API: DUPLIKAT QUEUE =================

// Ambil semua duplikat yang belum diputuskan
app.get("/api/duplicate-queue", requireLogin, (req, res) => {
  const pending = duplicateQueue.filter((d) => !d.resolvedAt);
  res.json({ pending });
});

// User memutuskan: replace atau skip
app.post("/api/duplicate-resolve", requireLogin, async (req, res) => {
  const { qid, action } = req.body; // action: "replace" | "skip"
  const item = duplicateQueue.find(
    (d) => d.qid === parseInt(qid) && !d.resolvedAt,
  );

  if (!item)
    return res
      .status(404)
      .json({ ok: false, message: "Item tidak ditemukan." });

  item.resolvedAt = Date.now();

  try {
    if (action === "skip") {
      // Buang file temp jika ada
      if (item.tempPath && fs.existsSync(item.tempPath))
        fs.unlinkSync(item.tempPath);
      // ACK ke ESP agar lanjut kirim file berikutnya (untuk tipe TXT)
      if (item.tipe !== "wav") {
        mqttClient.publish(
          "kelas/alat/perintah",
          JSON.stringify({ perintah: "ack_file", file: item.file }),
        );
      }
      console.log(`🗑️ [DUPLIKAT] SKIP: ${item.file}`);
      return res.json({ ok: true, action: "skip" });
    }

    // action === "replace"
    if (item.tipe === "wav") {
      const audioUrl = await uploadToSupabaseStorage(
        item.tempPath,
        item.storagePath,
      );
      if (fs.existsSync(item.tempPath)) fs.unlinkSync(item.tempPath);
      if (item.classId)
        await simpanAudioKeDB(item.info, item.classId, audioUrl);
      console.log(`🔄 [DUPLIKAT] REPLACE WAV: ${item.file}`);
    } else if (item.tipe === "txt_dsn") {
      await sbUpdate(
        "questions",
        { question_id: item.existingId },
        { date_id: item.tanggal },
      );
      mqttClient.publish(
        "kelas/alat/perintah",
        JSON.stringify({ perintah: "ack_file", file: item.file }),
      );
      console.log(
        `🔄 [DUPLIKAT] REPLACE TXT DSN: ${item.file} → date_id="${item.tanggal}"`,
      );
    } else if (item.tipe === "txt_mhs") {
      await sbUpdate(
        "answers",
        { answer_id: item.existingAnswerId },
        {
          student_id: item.studentId,
          duration_answer: item.waktu_diam_ms
            ? item.waktu_diam_ms / 1000
            : null,
          class_id: item.classId,
        },
      );
      mqttClient.publish(
        "kelas/alat/perintah",
        JSON.stringify({ perintah: "ack_file", file: item.file }),
      );
      console.log(`🔄 [DUPLIKAT] REPLACE TXT MHS: ${item.file}`);
    }

    res.json({ ok: true, action: "replace" });
  } catch (err) {
    console.error("❌ [DUPLIKAT RESOLVE] Error:", err.message);
    res.status(500).json({ ok: false, message: err.message });
  }
});

// ================= ROUTES =================
app.get("/", (req, res) => res.render("login", { error: null }));

app.post("/login", async (req, res) => {
  const { username, password } = req.body;
  try {
    const rows = await sbSelect("users", { username });
    const user = rows[0];
    if (!user)
      return res.render("login", { error: "Username tidak ditemukan." });

    const isMatch = await bcrypt.compare(password, user.password);
    if (!isMatch) return res.render("login", { error: "Password salah." });

    req.session.user = {
      user_id: user.user_id,
      username: user.username,
      role: user.role,
    };

    const redirectTo = user.role === "admin" ? "/admin" : "/pilih-kelas";
    return res.redirect(redirectTo);
  } catch (err) {
    console.error(err);
    res.render("login", { error: "Terjadi kesalahan server." });
  }
});

app.post("/logout", (req, res) => {
  req.session.destroy(() => res.redirect("/"));
});

// ================= PILIH KELAS =================
app.get("/pilih-kelas", requireLogin, async (req, res) => {
  const { username, role, user_id } = req.session.user;

  try {
    let classes;
    if (role === "dosen") {
      const { data } = await supabase
        .from("classes")
        .select(
          "class_id, class_name, class_code, lecturer_name, class_students(student_id)",
        )
        .eq("lecturer_user_id", user_id)
        .order("class_name");

      classes = (data || []).map((c) => ({
        id: c.class_id,
        name: c.class_name,
        code: c.class_code,
        lecturer: c.lecturer_name,
        student_count: c.class_students?.length || 0,
      }));
    } else {
      const { data } = await supabase
        .from("classes")
        .select(
          "class_id, class_name, class_code, lecturer_name, class_students(student_id)",
        )
        .order("class_name");

      classes = (data || []).map((c) => ({
        id: c.class_id,
        name: c.class_name,
        code: c.class_code,
        lecturer: c.lecturer_name,
        student_count: c.class_students?.length || 0,
      }));
    }
    res.render("pilih-kelas", { classes, role, username });
  } catch (err) {
    console.error(err);
    res.send("Gagal memuat daftar kelas.");
  }
});

// ================= ADMIN ROUTE =================
app.get("/admin", requireRole("admin"), async (req, res) => {
  const { username } = req.session.user;
  const currentClass = req.query.kelas || null;
  const currentDeviceId = req.query.device;

  try {
    const { data: allClassesRaw } = await supabase
      .from("classes")
      .select(
        "class_id, class_name, class_code, lecturer_name, lecturer_user_id, class_students(student_id)",
      )
      .order("class_name");
    const allClasses = (allClassesRaw || []).map((c) => ({
      id: c.class_id,
      name: c.class_name,
      code: c.class_code,
      lecturer: c.lecturer_name,
      lecturer_user_id: c.lecturer_user_id,
      student_count: c.class_students?.length || 0,
    }));

    const users = await sbSelect("users");
    const dosenUsers = await sbSelect(
      "users",
      { role: "dosen" },
      "user_id,username",
    );

    if (!currentClass) {
      return res.render("admin", {
        students: [],
        studentsNotInClass: [],
        devices: [],
        currentDevice: { id: "N/A", name: "Belum Ada Perangkat", status: "offline", battery: null },
        classes: allClasses,
        currentClass: null,
        session: sessionData,
        currentList: [],
        users,
        dosenUsers,
        logs: [],
        username,
      });
    }

    const devicesRaw = await sbSelect(
      "devices",
      {},
      "device_id,name,status,battery_level",
    );
    const devices = devicesRaw.map((d) => ({
      id: d.device_id,
      name: d.name,
      status: d.status,
      battery: d.battery_level,
    }));

    const classRec = allClasses.find((c) => c.name === currentClass);

    if (!classRec) {
      return res.render("admin", {
        students: [],
        studentsNotInClass: [],
        devices,
        currentDevice: devices[0] || {
          id: "N/A",
          name: "Belum Ada Perangkat",
          status: "offline",
          battery: null,
        },
        classes: allClasses,
        currentClass,
        session: sessionData,
        currentList: [],
        users,
        dosenUsers,
        logs: [],
        username,
      });
    }

    const { data: csRows } = await supabase
      .from("class_students")
      .select("student_id, students(*)")
      .eq("class_id", classRec.id);

    const studentsInClass = (csRows || []).map((r) => ({
      ...r.students,
      rfid: r.students?.rfid_uid,
      kelas: currentClass,
    }));

    const studentsInClassIds = studentsInClass.map((s) => s.student_id);

    let studentsNotInClass = [];
    const { data: allCsRows } = await supabase
      .from("class_students")
      .select("student_id, students(*), classes(class_name)")
      .not("class_id", "eq", classRec.id);
    studentsNotInClass = (allCsRows || []).map((r) => ({
      ...r.students,
      kelas: r.classes?.class_name || "-",
    }));

    let classLogs = [];
    if (studentsInClassIds.length > 0) {
      const { data: logs } = await supabase
        .from("answers")
        .select(
          "answer_id, transcript_text, audio_file_path, student_id, students(name,nim), questions(transcript_text,created_at)",
        )
        .in("student_id", studentsInClassIds);
      classLogs = (logs || []).map((l) => ({
        id: l.answer_id,
        date_obj: l.questions?.created_at,
        name: l.students?.name,
        nim: l.students?.nim,
        question: l.questions?.transcript_text,
        transcript: l.transcript_text,
        audio_url: l.audio_file_path,
      }));
    }

    const allStudents = await sbSelect("students");

    if (!currentDeviceId && devices.length > 0) {
      return res.redirect(
        `/admin?kelas=${currentClass}&device=${devices[0].id}`,
      );
    }

    const currentDevice = devices.find(
      (d) => d.id === parseInt(currentDeviceId),
    ) ||
      devices[0] || {
        id: "N/A",
        name: "Belum Ada Perangkat",
        status: "offline",
        battery: null,
      };

    const mappedList = sessionData.scannedList.map((item) => {
      const student = allStudents.find((s) => s.rfid_uid === item.uid);
      let studentClass = "-";
      if (student) {
        const isDiKelasIni = studentsInClass.find(
          (x) => x.student_id === student.student_id,
        );
        studentClass = isDiKelasIni ? isDiKelasIni.kelas : "Beda Kelas";
      }
      return {
        id: item.id,
        uid: item.uid,
        name: student ? student.name : "Tidak Terdaftar",
        nim: student ? student.nim : "-",
        kelas: studentClass,
        isRegistered: !!student,
        isWrongClass: student && studentClass !== currentClass,
      };
    });

    res.render("admin", {
      students: studentsInClass,
      studentsNotInClass,
      devices,
      currentDevice,
      classes: allClasses,
      currentClass,
      session: sessionData,
      currentList: mappedList,
      users,
      dosenUsers,
      logs: classLogs,
      username,
    });
  } catch (err) {
    console.error("❌ Error di Route Admin:", err);
    res.send("Gagal memuat halaman Admin.");
  }
});

// TAMBAH KELAS
app.post("/admin/add-class", requireRole("admin"), async (req, res) => {
  const { name, code, lecturer, lecturer_user_id, current_class } = req.body;
  try {
    await sbInsert("classes", {
      class_name: name,
      class_code: code,
      lecturer_name: lecturer || "",
      lecturer_user_id: toInt(lecturer_user_id),
    });
    const target = current_class
      ? `/admin?kelas=${current_class}`
      : `/pilih-kelas`;
    res.redirect(target);
  } catch (err) {
    console.error(err);
    res.send("Gagal menambah kelas.");
  }
});

// EDIT KELAS
app.post("/admin/edit-class", requireRole("admin"), async (req, res) => {
  const { id, name, code, lecturer, lecturer_user_id, current_class } =
    req.body;
  try {
    await sbUpdate(
      "classes",
      { class_id: toInt(id) },
      {
        class_name: name,
        class_code: code,
        lecturer_name: lecturer || "",
        lecturer_user_id: toInt(lecturer_user_id),
      },
    );
    const target = current_class
      ? `/admin?kelas=${current_class}`
      : `/pilih-kelas`;
    res.redirect(target);
  } catch (err) {
    console.error(err);
    res.send("Gagal mengupdate kelas.");
  }
});

// HAPUS KELAS
app.post("/admin/delete-class", requireRole("admin"), async (req, res) => {
  const { id, current_class } = req.body;
  try {
    const csRows = await sbSelect(
      "class_students",
      { class_id: toInt(id) },
      "student_id",
    );
    const studentIds = csRows.map((r) => r.student_id);
    await sbDelete("classes", { class_id: toInt(id) });
    if (studentIds.length > 0)
      await sbDelete("students", { student_id: studentIds });

    const remaining = await sbSelect("classes", {}, "class_name", { limit: 1 });
    if (current_class) {
      const stillExists = await sbSelect("classes", {
        class_name: current_class,
      });
      if (stillExists.length > 0) {
        return res.redirect(`/admin?kelas=${current_class}`);
      }
    }
    if (remaining.length > 0) {
      res.redirect(`/admin?kelas=${remaining[0].class_name}`);
    } else {
      res.redirect(`/pilih-kelas`);
    }
  } catch (err) {
    console.error(err);
    res.send("Gagal menghapus kelas.");
  }
});

// RESET ALL DATA
app.post("/admin/reset-all", requireRole("admin"), async (req, res) => {
  try {
    await supabase.from("answers").delete().neq("answer_id", 0);
    await supabase.from("questions").delete().neq("question_id", 0);
    await supabase.from("class_students").delete().neq("class_student_id", 0);
    await supabase.from("students").delete().neq("student_id", 0);
    await supabase.from("classes").delete().neq("class_id", 0);
    sessionData.scannedList = [];
    res.redirect(`/pilih-kelas`);
  } catch (err) {
    console.error(err);
    res.send("Gagal mereset semua data.");
  }
});

// HAPUS ENTRI RFID (admin)
app.post("/admin/manage-uid", requireRole("admin"), async (req, res) => {
  const { action, uid, entry_id, current_class } = req.body;
  if (action === "delete") {
    if (entry_id) {
      const idx = sessionData.scannedList.findIndex(
        (item) => item.id === parseInt(entry_id),
      );
      if (idx !== -1) sessionData.scannedList.splice(idx, 1);
    } else {
      const idx = sessionData.scannedList.findIndex((item) => item.uid === uid);
      if (idx !== -1) sessionData.scannedList.splice(idx, 1);
    }
  }
  const target = current_class
    ? `/admin?kelas=${current_class}`
    : `/pilih-kelas`;
  res.redirect(target);
});

// TAMBAH MAHASISWA BARU
app.post("/admin/add", requireRole("admin"), async (req, res) => {
  const { name, nim, rfid, kelas, current_class } = req.body;
  const targetClass = kelas || current_class;
  try {
    const clsRows = await sbSelect("classes", { class_name: targetClass });
    if (clsRows.length === 0) return res.send("Kelas tidak ditemukan");
    const classId = clsRows[0].class_id;
    const newStudent = await sbInsert("students", {
      name,
      nim,
      rfid_uid: rfid,
    });
    await sbInsert("class_students", {
      student_id: newStudent.student_id,
      class_id: classId,
    });
    res.redirect(`/admin?kelas=${targetClass}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menambah mahasiswa.");
  }
});

// TAMBAH MAHASISWA DARI KELAS LAIN
app.post("/admin/add-to-class", requireRole("admin"), async (req, res) => {
  const { student_id, current_class } = req.body;
  try {
    const clsRows = await sbSelect("classes", { class_name: current_class });
    if (clsRows.length === 0) return res.send("Kelas tidak ditemukan");
    const classId = clsRows[0].class_id;
    const existing = await sbSelect("class_students", {
      student_id: toInt(student_id),
      class_id: classId,
    });
    if (existing.length === 0) {
      await sbInsert("class_students", {
        student_id: toInt(student_id),
        class_id: classId,
      });
    }
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menambahkan mahasiswa ke kelas.");
  }
});

// HAPUS MAHASISWA
app.post("/admin/delete", requireRole("admin"), async (req, res) => {
  const { id, current_class } = req.body;
  try {
    await sbDelete("students", { student_id: toInt(id) });
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menghapus mahasiswa.");
  }
});

// EDIT MAHASISWA
app.post("/admin/edit", requireRole("admin"), async (req, res) => {
  const { id, name, nim, rfid, current_class } = req.body;
  try {
    await sbUpdate(
      "students",
      { student_id: toInt(id) },
      { name, nim, rfid_uid: rfid },
    );
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal mengupdate mahasiswa.");
  }
});

// TAMBAH USER
app.post("/admin/add-user", requireRole("admin"), async (req, res) => {
  const { username, password, role, current_class } = req.body;
  try {
    const hashedPassword = await bcrypt.hash(password, SALT_ROUNDS);
    await sbInsert("users", { username, password: hashedPassword, role });
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menambah user.");
  }
});

// HAPUS USER
app.post("/admin/delete-user", requireRole("admin"), async (req, res) => {
  const { id, current_class } = req.body;
  console.log("🗑️ [delete-user] req.body:", req.body);
  const userId = toInt(id);
  console.log("🗑️ [delete-user] userId setelah toInt:", userId);
  if (userId && userId !== 1) {
    try {
      await sbDelete("users", { user_id: userId });
    } catch (err) {
      console.error("❌ [delete-user] Error:", err);
    }
  }
  const target = current_class
    ? `/admin?kelas=${current_class}`
    : `/pilih-kelas`;
  res.redirect(target);
});

// ================= DOSEN ROUTE =================
app.get("/dosen", requireRole("dosen", "admin"), async (req, res) => {
  const { username } = req.session.user;
  const currentClass = req.query.kelas;
  const currentDeviceId = req.query.device;

  if (!currentClass) return res.redirect("/pilih-kelas");

  try {
    const classRec = await sbSelect("classes", { class_name: currentClass });
    const classId = classRec.length > 0 ? classRec[0].class_id : null;

    let studentsInClass = [];
    if (classId) {
      const { data: csRows } = await supabase
        .from("class_students")
        .select("student_id, students(*)")
        .eq("class_id", classId);
      studentsInClass = (csRows || []).map((r) => ({
        ...r.students,
        kelas: currentClass,
      }));
    }

    const devicesRaw = await sbSelect(
      "devices",
      {},
      "device_id,name,status,battery_level",
    );
    const devices = devicesRaw.map((d) => ({
      id: d.device_id,
      name: d.name,
      status: d.status,
      battery: d.battery_level,
    }));

    if (currentDeviceId) {
      req.session.lastDeviceId = String(currentDeviceId);
    }
    if (!currentDeviceId && devices.length > 0) {
      const fallback = req.session.lastDeviceId || String(devices[0].id);
      return res.redirect(`/dosen?kelas=${currentClass}&device=${fallback}`);
    }

    const currentDevice = devices.find(
      (d) => d.id === parseInt(currentDeviceId),
    ) ||
      devices[0] || {
        id: "N/A",
        name: "Belum Ada Perangkat",
        status: "offline",
        battery: null,
      };

    const allStudents = await sbSelect("students");

    const mappedSessionList = sessionData.scannedList.map((item) => {
      const student = allStudents.find((s) => s.rfid_uid === item.uid);
      const isWrongClass = student
        ? !studentsInClass.find((x) => x.student_id === student.student_id)
        : false;
      return {
        id: item.id,
        uid: item.uid,
        name: student ? student.name : "Tidak Terdaftar",
        nim: student ? student.nim : "-",
        isRegistered: !!student,
        isWrongClass,
      };
    });

    let filteredLogs = [];
    const studentIds = studentsInClass.map((s) => s.student_id);
    if (classId) {
      const { data: questions } = await supabase
        .from("questions")
        .select(
          "question_id, transcript_text, created_at, audio_file_path, answers(answer_id, transcript_text, audio_file_path, duration_answer, nilai, student_id, students(name,nim))",
        )
        .eq("class_id", classId);

      for (const q of (questions || [])) {
        const relevantAnswers = (q.answers || []).filter((a) =>
          studentIds.includes(a.student_id),
        );
        if (relevantAnswers.length > 0) {
          for (const a of relevantAnswers) {
            filteredLogs.push({
              id: a.answer_id,
              question_id: q.question_id,
              date_obj: q.created_at,
              name: a.students?.name,
              nim: a.students?.nim,
              question: q.transcript_text,
              question_audio_url: q.audio_file_path || null,
              transcript: a.transcript_text,
              audio_url: a.audio_file_path,
              duration_answer: a.duration_answer,
              nilai: a.nilai ?? null,
            });
          }
        } else {
          filteredLogs.push({
            id: null,
            question_id: q.question_id,
            date_obj: q.created_at,
            name: null,
            nim: null,
            question: q.transcript_text,
            question_audio_url: q.audio_file_path || null,
            transcript: null,
            audio_url: null,
            duration_answer: null,
          });
        }
      }
    }

    let stats = studentsInClass.map((s) => {
      const sLogs = filteredLogs.filter(
        (l) => l.nim === s.nim && l.id !== null,
      );
      const graded = sLogs.filter(
        (l) => l.nilai !== null && l.nilai !== undefined,
      );
      const avgNilai =
        graded.length > 0
          ? Math.round(
              graded.reduce((sum, l) => sum + Number(l.nilai), 0) /
                graded.length,
            )
          : null;
      return {
        name: s.name,
        nim: s.nim,
        count: sLogs.length,
        gradedCount: graded.length,
        avgNilai,
      };
    });
    stats.sort((a, b) => b.count - a.count);

    filteredLogs.sort((a, b) => {
      const dateA = new Date(a.date_obj || 0).setHours(0, 0, 0, 0);
      const dateB = new Date(b.date_obj || 0).setHours(0, 0, 0, 0);
      if (dateA !== dateB) return dateB - dateA;
      const qA = a.question || "";
      const qB = b.question || "";
      if (qA < qB) return -1;
      if (qA > qB) return 1;
      return new Date(a.date_obj || 0) - new Date(b.date_obj || 0);
    });

    const rawDates = filteredLogs
      .filter((l) => l.date_obj)
      .map((l) => {
        const d = new Date(l.date_obj);
        d.setMinutes(d.getMinutes() - d.getTimezoneOffset());
        return d.toISOString().split("T")[0];
      });
    const uniqueDates = [...new Set(rawDates)].sort().reverse();

    res.render("dosen", {
      session: sessionData,
      devices,
      currentDevice,
      currentList: mappedSessionList,
      logs: filteredLogs.map((l) => ({ ...l, dateObj: l.date_obj })),
      stats,
      students: studentsInClass,
      availableDates: uniqueDates,
      currentClass,
      username,
    });
  } catch (err) {
    console.error(err);
    res.send("Gagal memuat halaman Dosen.");
  }
});

// DOSEN: DAFTARKAN MAHASISWA BARU
app.post(
  "/dosen/add-student",
  requireRole("dosen", "admin"),
  async (req, res) => {
    const { name, nim, rfid, kelas, current_class } = req.body;
    const targetClass = kelas || current_class;
    try {
      const clsRows = await sbSelect(
        "classes",
        { class_name: targetClass },
        "class_id",
      );
      if (clsRows.length === 0) return res.send("Kelas tidak ditemukan");
      const newStudent = await sbInsert("students", {
        name,
        nim,
        rfid_uid: rfid,
      });
      await sbInsert("class_students", {
        student_id: newStudent.student_id,
        class_id: clsRows[0].class_id,
      });
      res.redirect(`/dosen?kelas=${targetClass}`);
    } catch (err) {
      console.error(err);
      res.send("Gagal mendaftarkan mahasiswa.");
    }
  },
);

app.post(
  "/dosen/update-settings",
  requireRole("dosen", "admin"),
  (req, res) => {
    const { status, mode, timer, threshold, current_class } = req.body;
    if (status) sessionData.status = status;
    if (mode) sessionData.mode = mode;
    if (timer) {
      sessionData.maxTime = parseInt(timer);
      const payload = JSON.stringify({
        perintah: "set_timer",
        durasi_detik: sessionData.maxTime,
      });
      mqttClient.publish("kelas/alat/perintah", payload, (err) => {
        if (err) console.error("❌ Gagal mengirim timer ke MQTT:", err);
        else console.log("✅ Berhasil mengirim timer ke MQTT:", payload);
      });
    }
    if (threshold && THRESHOLD_VALUES[threshold] !== undefined) {
      sessionData.threshold = threshold;
      const payload = JSON.stringify({
        perintah: "set_threshold",
        nilai: THRESHOLD_VALUES[threshold],
      });
      mqttClient.publish("kelas/alat/perintah", payload, (err) => {
        if (err) console.error("❌ Gagal mengirim threshold ke MQTT:", err);
        else console.log("✅ Berhasil mengirim threshold ke MQTT:", payload);
      });
    }
    res.redirect(`/dosen?kelas=${current_class}`);
  },
);

app.post("/dosen/sync-uid", requireRole("dosen", "admin"), async (req, res) => {
  const { current_class, mode } = req.body;
  let daftarUid = [];

  if (mode === "semua_kelas") {
    try {
      const classRec = await sbSelect(
        "classes",
        { class_name: current_class },
        "class_id",
      );
      if (classRec.length > 0) {
        const csRows = await sbSelect(
          "class_students",
          { class_id: classRec[0].class_id },
          "student_id",
        );
        const studentIds = csRows.map((r) => r.student_id);
        if (studentIds.length > 0) {
          const students = await sbSelect(
            "students",
            { student_id: studentIds },
            "rfid_uid",
          );
          daftarUid = students.map((s) => s.rfid_uid).filter(Boolean);
        }
      }
    } catch (err) {
      console.error("❌ Gagal ambil UID semua kelas:", err.message);
      return res
        .status(500)
        .json({ success: false, message: "Gagal mengambil data mahasiswa." });
    }
    const payload = JSON.stringify({
      perintah: "sync_uid_kelas",
      label: "Daftar Mahasiswa Kelas",
      uids: daftarUid,
    });
    mqttClient.publish("kelas/alat/perintah", payload, (err) => {
      if (err) {
        console.error("❌ Gagal kirim sync_uid_kelas:", err);
        return res
          .status(500)
          .json({ success: false, message: "Gagal mengirim ke alat." });
      }
      console.log(`✅ sync_uid_kelas: ${daftarUid.length} UID`);
      res.json({
        success: true,
        message: `✅ Daftar Kelas: ${daftarUid.length} UID dikirim ke alat.`,
      });
    });
  } else {
    daftarUid = sessionData.scannedList.map((item) => item.uid);
    const payload = JSON.stringify({
      perintah: "sync_uid_aktif",
      label: "Peserta Aktif Sesi",
      uids: daftarUid,
    });
    mqttClient.publish("kelas/alat/perintah", payload, (err) => {
      if (err) {
        console.error("❌ Gagal kirim sync_uid_aktif:", err);
        return res
          .status(500)
          .json({ success: false, message: "Gagal mengirim ke alat." });
      }
      console.log(`✅ sync_uid_aktif: ${daftarUid.length} UID`);
      res.json({
        success: true,
        message: `✅ Peserta Aktif: ${daftarUid.length} UID dikirim ke alat.`,
      });
    });
  }
});

app.post("/dosen/edit-log", requireRole("dosen", "admin"), async (req, res) => {
  const {
    id,
    student_id,
    edit_date,
    edit_time,
    transcript,
    question,
    current_class,
  } = req.body;
  try {
    const newDateObj = new Date(`${edit_date}T${edit_time}`).toISOString();
    const ansInfo = await sbSelect(
      "answers",
      { answer_id: toInt(id) },
      "question_id",
    );
    if (ansInfo.length > 0) {
      const qId = ansInfo[0].question_id;
      await sbUpdate(
        "answers",
        { answer_id: toInt(id) },
        { transcript_text: transcript, created_at: newDateObj },
      );
      await sbUpdate(
        "questions",
        { question_id: qId },
        { transcript_text: question, created_at: newDateObj },
      );
    }
    res.redirect(`/dosen?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal mengedit log.");
  }
});

// ================= KELOLA RIWAYAT: EDIT/HAPUS JAWABAN & PERTANYAAN =================
app.patch("/dosen/answer/:id", requireRole("dosen", "admin"), async (req, res) => {
  try {
    const { id } = req.params;
    const { transcript, student_id, duration_answer } = req.body;
    const updates = { transcript_text: transcript };
    if (student_id !== undefined && student_id !== null && student_id !== '')
      updates.student_id = parseInt(student_id);
    if (duration_answer !== undefined && duration_answer !== '')
      updates.duration_answer = duration_answer === null ? null : parseFloat(duration_answer);
    await sbUpdate("answers", { answer_id: parseInt(id) }, updates);
    res.json({ ok: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "Gagal memperbarui jawaban." });
  }
});

app.delete("/dosen/answer/:id", requireRole("dosen", "admin"), async (req, res) => {
  try {
    const { id } = req.params;
    await sbDelete("answers", { answer_id: parseInt(id) });
    res.json({ ok: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "Gagal menghapus jawaban." });
  }
});

// Penilaian manual dosen (1-100), kosong = batalkan nilai
app.patch(
  "/dosen/answer/:id/nilai",
  requireRole("dosen", "admin"),
  async (req, res) => {
    try {
      const { id } = req.params;
      let { nilai } = req.body;
      if (nilai === "" || nilai === null || nilai === undefined) {
        nilai = null;
      } else {
        nilai = parseInt(nilai, 10);
        if (isNaN(nilai) || nilai < 1 || nilai > 100) {
          return res
            .status(400)
            .json({ error: "Nilai harus berupa angka antara 1 dan 100." });
        }
      }
      await sbUpdate("answers", { answer_id: parseInt(id) }, { nilai });
      res.json({ ok: true, nilai });
    } catch (err) {
      console.error(err);
      res.status(500).json({ error: "Gagal menyimpan nilai." });
    }
  },
);

app.patch("/dosen/question/:id", requireRole("dosen", "admin"), async (req, res) => {
  try {
    const { id } = req.params;
    const { transcript } = req.body;
    await sbUpdate("questions", { question_id: parseInt(id) }, { transcript_text: transcript });
    res.json({ ok: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "Gagal memperbarui pertanyaan." });
  }
});

app.delete("/dosen/question/:id", requireRole("dosen", "admin"), async (req, res) => {
  try {
    const { id } = req.params;
    await sbDelete("answers", { question_id: parseInt(id) });
    await sbDelete("questions", { question_id: parseInt(id) });
    res.json({ ok: true });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: "Gagal menghapus pertanyaan." });
  }
});

app.post(
  "/dosen/manage-uid",
  requireRole("dosen", "admin"),
  async (req, res) => {
    const { action, uid, entry_id, student_id, target_date, current_class } =
      req.body;

    if (action === "delete") {
      if (entry_id) {
        const idx = sessionData.scannedList.findIndex(
          (item) => item.id === parseInt(entry_id),
        );
        if (idx !== -1) sessionData.scannedList.splice(idx, 1);
      } else {
        const idx = sessionData.scannedList.findIndex(
          (item) => item.uid === uid,
        );
        if (idx !== -1) sessionData.scannedList.splice(idx, 1);
      }
    } else if (action === "add_single") {
      try {
        const rows = await sbSelect(
          "students",
          { student_id: toInt(student_id) },
          "rfid_uid",
        );
        if (rows.length > 0) {
          scanCounter++;
          sessionData.scannedList.unshift({
            id: scanCounter,
            uid: rows[0].rfid_uid,
          });
        }
      } catch (err) {}
    } else if (action === "add_date" && current_class) {
      try {
        const { data: logs } = await supabase
          .from("answers")
          .select("student_id, questions!inner(created_at)")
          .gte("questions.created_at", `${target_date}T00:00:00`)
          .lte("questions.created_at", `${target_date}T23:59:59`);

        const activeIds = [...new Set((logs || []).map((l) => l.student_id))];
        if (activeIds.length > 0) {
          const students = await sbSelect(
            "students",
            { student_id: activeIds },
            "rfid_uid",
          );
          students.forEach((s) => {
            scanCounter++;
            sessionData.scannedList.push({ id: scanCounter, uid: s.rfid_uid });
          });
        }
      } catch (err) {}
    }

    res.redirect(`/dosen?kelas=${current_class}`);
  },
);

app.post(
  "/dosen/request-audio",
  requireRole("dosen", "admin"),
  async (req, res) => {
    const { id, current_class } = req.body;
    try {
      await sbUpdate(
        "answers",
        { answer_id: toInt(id) },
        { audio_file_path: "#" },
      );
      res.redirect(`/dosen?kelas=${current_class}`);
    } catch (err) {
      console.error(err);
      res.send("Gagal request audio.");
    }
  },
);

app.post(
  "/dosen/request-audio-sync",
  requireRole("dosen", "admin"),
  (req, res) => {
    const { current_class } = req.body;
    const payloadTxt = JSON.stringify({
      perintah: "request_sync_audio",
      target_kelas: current_class,
    });
    const payloadWav = JSON.stringify({
      perintah: "request_sync_wav",
      target_kelas: current_class,
    });

    mqttClient.publish("kelas/alat/perintah", payloadTxt, (errTxt) => {
      if (errTxt) {
        console.error("❌ Gagal kirim request_sync_audio:", errTxt);
        return res
          .status(500)
          .json({ success: false, message: "Gagal menghubungi alat." });
      }
      console.log("✅ request_sync_audio terkirim:", payloadTxt);
      setTimeout(() => {
        mqttClient.publish("kelas/alat/perintah", payloadWav, (errWav) => {
          if (errWav) console.error("❌ Gagal kirim request_sync_wav:", errWav);
          else console.log("✅ request_sync_wav terkirim:", payloadWav);
        });
      }, 2000);
      res.json({
        success: true,
        message: "Permintaan sync metadata & audio dikirim ke alat.",
      });
    });
  },
);

app.post(
  "/dosen/request-wav-sync",
  requireRole("dosen", "admin"),
  (req, res) => {
    const { current_class } = req.body;
    const payload = JSON.stringify({
      perintah: "request_sync_wav",
      target_kelas: current_class,
    });
    mqttClient.publish("kelas/alat/perintah", payload, (err) => {
      if (err) {
        console.error("❌ Gagal kirim request_sync_wav:", err);
        return res
          .status(500)
          .json({ success: false, message: "Gagal menghubungi alat." });
      }
      console.log("✅ request_sync_wav terkirim:", payload);
      res.json({
        success: true,
        message: "Permintaan sync audio WAV dikirim ke alat.",
      });
    });
  },
);

// ================= API KONTROL SD SYNC =================
app.post("/api/sd-sync-keputusan", (req, res) => {
  const { keputusan } = req.body;
  if (keputusan === "ulangi") {
    const payload = JSON.stringify({ perintah: "ack_file", file: "" });
    mqttClient.publish("kelas/alat/perintah", payload, (err) => {
      if (err)
        return res
          .status(500)
          .json({ success: false, message: "Gagal kirim perintah." });
      console.log("🔄 [SD SYNC] Server memilih: ULANGI / LANJUT");
      res.json({ success: true, message: "ESP diperintahkan melanjutkan." });
    });
  } else if (keputusan === "batalkan") {
    const payload = JSON.stringify({ perintah: "batalkan_sync" });
    mqttClient.publish("kelas/alat/perintah", payload, (err) => {
      if (err)
        return res
          .status(500)
          .json({ success: false, message: "Gagal kirim perintah." });
      console.log("🛑 [SD SYNC] Server memilih: BATALKAN");
      res.json({ success: true, message: "Sinkronisasi dibatalkan." });
    });
  } else {
    res.status(400).json({ success: false, message: "Keputusan tidak valid." });
  }
});

app.post("/api/upload-audio", upload.single("audio"), async (req, res) => {
  try {
    const { log_id } = req.body;
    if (!req.file) return res.status(400).send("Tidak ada file yang diunggah.");
    const audioUrl = `/recordings/${req.file.filename}`;
    await sbUpdate(
      "answers",
      { answer_id: toInt(log_id) },
      { audio_file_path: audioUrl },
    );
    console.log(
      `✅ File audio diterima untuk Answer ID ${log_id}: ${audioUrl}`,
    );
    res.status(200).json({ status: "success", url: audioUrl });
  } catch (error) {
    console.error("❌ Gagal memproses upload audio:", error);
    res.status(500).send("Server Error");
  }
});

// ================= DEEPGRAM AUTO-SWEEP =================
// Jalankan sweep pertama 10 detik setelah server nyala,
// lalu setiap 5 menit sekali cek ulang.
setTimeout(() => {
  sweepUntranscribed(supabase, sbUpdate);
}, 10_000);

setInterval(
  () => {
    sweepUntranscribed(supabase, sbUpdate);
  },
  5 * 60 * 1000,
); // setiap 5 menit

// ================= START SERVER =================
const PORT = process.env.PORT || 3000;
app.listen(PORT, "0.0.0.0", () => {
  console.log("==================================================");
  console.log(`🚀 SERVER RUNNING!`);
  console.log(`💻 Akses Web via Laptop: http://localhost:${PORT}`);
  console.log("==================================================");
});
