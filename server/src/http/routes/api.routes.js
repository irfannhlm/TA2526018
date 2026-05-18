"use strict";

const express = require("express");
const fs = require("fs");
const path = require("path");
const bcrypt = require("bcrypt");
const {
  sbSelect,
  sbInsert,
  sbUpdate,
  sbDelete,
} = require("../../data/baseRepo");
const { supabase } = require("../../config/supabase");
const { state } = require("../../state");
const { mqttClient } = require("../../config/mqtt");
const { toInt } = require("../../lib/utils");
const {
  THRESHOLD_VALUES,
  STORAGE_BUCKET,
  SALT_ROUNDS,
} = require("../../lib/constants");
const { parseNamaFile } = require("../../lib/fileName");
const { simpanAudioKeDB } = require("../../services/audio.service");
const { uploadToSupabaseStorage } = require("../../services/storage.service");
const { upload, uploadTemp } = require("../middleware/upload");
const { requireLogin, requireRole } = require("../middleware/auth");
const {
  transcribeAnswer,
  transcribeQuestion,
} = require("../../../Deepgramservice");
const asyncHandler = require("../../lib/asyncHandler");

const router = express.Router();

// ================= API REALTIME =================
router.get(
  "/api/realtime-data",
  asyncHandler(async (req, res) => {
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

    const mappedList = state.sessionData.scannedList.map((item) => {
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
  }),
);

// ================= API REALTIME LOGS =================
router.get(
  "/api/realtime-logs",
  asyncHandler(async (req, res) => {
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
    for (const q of questions || []) {
      const relevantAnswers = (q.answers || []).filter((a) =>
        studentIds.includes(a.student_id),
      );
      if (relevantAnswers.length > 0) {
        for (const a of relevantAnswers) {
          formatted.push({
            id: a.answer_id,
            question_id: q.question_id,
            student_id: a.student_id,
            date_obj: q.created_at
              ? new Date(q.created_at).toISOString()
              : null,
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
  }),
);

// ================= API: STATUS SYNC SD CARD =================
router.get(
  "/api/sync-status",
  requireLogin,
  asyncHandler((req, res) => {
    // Auto-timeout jika loading lebih dari 60 detik tanpa update
    const elapsed = Date.now() - state.syncStatus.updatedAt;
    if (state.syncStatus.state === "loading" && elapsed > 60000) {
      state.syncStatus.state = "error";
      state.syncStatus.pesan = "Timeout — perangkat tidak merespons.";
    }
    res.json(state.syncStatus);
  }),
);

router.post(
  "/api/sync-status/reset",
  requireLogin,
  asyncHandler((req, res) => {
    state.syncStatus = {
      state: "loading",
      pesan: "Mengirim permintaan ke perangkat...",
      total: 0,
      berhasil: 0,
      updatedAt: Date.now(),
    };
    res.json({ ok: true });
  }),
);

// ================= API: DUPLIKAT QUEUE =================

// Ambil semua duplikat yang belum diputuskan
router.get(
  "/api/duplicate-queue",
  requireLogin,
  asyncHandler((req, res) => {
    const pending = state.duplicateQueue.filter((d) => !d.resolvedAt);
    res.json({ pending });
  }),
);

// User memutuskan: replace atau skip
router.post(
  "/api/duplicate-resolve",
  requireLogin,
  asyncHandler(async (req, res) => {
    const { qid, action } = req.body; // action: "replace" | "skip"
    const item = state.duplicateQueue.find(
      (d) => d.qid === parseInt(qid) && !d.resolvedAt,
    );

    if (!item)
      return res
        .status(404)
        .json({ ok: false, message: "Item tidak ditemukan." });

    item.resolvedAt = Date.now();

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
  }),
);

// ================= API KONTROL SD SYNC =================
router.post(
  "/api/sd-sync-keputusan",
  asyncHandler((req, res) => {
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
      res
        .status(400)
        .json({ success: false, message: "Keputusan tidak valid." });
    }
  }),
);

router.post(
  "/api/upload-audio",
  upload.single("audio"),
  asyncHandler(async (req, res) => {
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
  }),
);

// ================= ENDPOINT: Terima audio WAV dari ESP32 via HTTP =================
router.post(
  "/api/upload-audio-sd",
  uploadTemp.single("audio"),
  asyncHandler(async (req, res) => {
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

      state.duplicateQueueCounter++;
      const qid = state.duplicateQueueCounter;
      state.duplicateQueue.push({
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
            await transcribeQuestion(sbUpdate, qRows[0].question_id, audioUrl);
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
  }),
);

module.exports = router;
