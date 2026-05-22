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
const { ROLES } = require("../../lib/constants");
const {
  requireOwnsAnswer,
  requireOwnsQuestion,
} = require("../middleware/ownership");
const {
  transcribeAnswer,
  transcribeQuestion,
} = require("../../../Deepgramservice");
const asyncHandler = require("../../lib/asyncHandler");
const { publishCommand } = require("../../mqtt/publisher");
const { validate } = require("../middleware/validate");
const {
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
} = require("../schemas/dosen.schemas");

const router = express.Router();

// ================= DOSEN ROUTE =================
router.get(
  "/dosen",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  asyncHandler(async (req, res) => {
    const { username } = req.session.user;
    const currentClass = req.query.kelas;
    const currentDeviceId = req.query.device;

    if (!currentClass) return res.redirect("/pilih-kelas");

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

    const mappedSessionList = state.sessionData.scannedList.map((item) => {
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

      for (const q of questions || []) {
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
      session: state.sessionData,
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
  }),
);

// DOSEN: DAFTARKAN MAHASISWA BARU
router.post(
  "/dosen/add-student",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  validate({ body: addStudentSchema }),
  asyncHandler(async (req, res) => {
    const { name, nim, rfid, kelas, current_class } = req.body;
    const targetClass = kelas || current_class;

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
  }),
);

router.post(
  "/dosen/update-settings",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  validate({ body: updateSettingsSchema }),
  asyncHandler((req, res) => {
    const { status, mode, timer, threshold, current_class } = req.body;
    if (status) state.sessionData.status = status;
    if (mode) state.sessionData.mode = mode;
    if (timer) {
      state.sessionData.maxTime = parseInt(timer);
      publishCommand({
        perintah: "set_timer",
        durasi_detik: state.sessionData.maxTime,
      });
    }
    if (threshold && THRESHOLD_VALUES[threshold] !== undefined) {
      state.sessionData.threshold = threshold;
      publishCommand({
        perintah: "set_threshold",
        nilai: THRESHOLD_VALUES[threshold],
      });
    }
    // Popup konfirmasi "terkirim" hanya untuk pengiriman ke alat (timer /
    // ambang kebisingan), bukan perubahan status/mode sesi.
    if (req.session && (timer || threshold)) {
      req.session.flashSuccess = timer
        ? "Durasi bicara berhasil dikirim ke alat."
        : "Ambang kebisingan ruangan berhasil dikirim ke alat.";
    }
    res.redirect(`/dosen?kelas=${current_class}`);
  }),
);

router.post(
  "/dosen/sync-uid",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  validate({ body: syncUidSchema }),
  asyncHandler(async (req, res) => {
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
      const ok = await publishCommand({
        perintah: "sync_uid_kelas",
        label: "Daftar Mahasiswa Kelas",
        uids: daftarUid,
      });
      if (!ok)
        return res
          .status(500)
          .json({ success: false, message: "Gagal mengirim ke alat." });
      console.log(`✅ sync_uid_kelas: ${daftarUid.length} UID`);
      res.json({
        success: true,
        message: `✅ Daftar Kelas: ${daftarUid.length} UID dikirim ke alat.`,
      });
    } else {
      daftarUid = state.sessionData.scannedList.map((item) => item.uid);
      const ok = await publishCommand({
        perintah: "sync_uid_aktif",
        label: "Peserta Aktif Sesi",
        uids: daftarUid,
      });
      if (!ok)
        return res
          .status(500)
          .json({ success: false, message: "Gagal mengirim ke alat." });
      console.log(`✅ sync_uid_aktif: ${daftarUid.length} UID`);
      res.json({
        success: true,
        message: `✅ Peserta Aktif: ${daftarUid.length} UID dikirim ke alat.`,
      });
    }
  }),
);

router.post(
  "/dosen/edit-log",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  validate({ body: editLogSchema }),
  requireOwnsAnswer("body"),
  asyncHandler(async (req, res) => {
    const {
      id,
      student_id,
      edit_date,
      edit_time,
      transcript,
      question,
      current_class,
    } = req.body;

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
  }),
);

// ================= KELOLA RIWAYAT: EDIT/HAPUS JAWABAN & PERTANYAAN =================
router.patch(
  "/dosen/answer/:id",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  requireOwnsAnswer(),
  validate({ body: answerPatchSchema }),
  asyncHandler(async (req, res) => {
    const { id } = req.params;
    const { transcript, student_id, duration_answer } = req.body;
    const updates = { transcript_text: transcript };
    if (student_id !== undefined && student_id !== null && student_id !== "")
      updates.student_id = parseInt(student_id);
    if (duration_answer !== undefined && duration_answer !== "")
      updates.duration_answer =
        duration_answer === null ? null : parseFloat(duration_answer);
    await sbUpdate("answers", { answer_id: parseInt(id) }, updates);
    res.json({ ok: true });
  }),
);

router.delete(
  "/dosen/answer/:id",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  requireOwnsAnswer(),
  asyncHandler(async (req, res) => {
    const { id } = req.params;
    await sbDelete("answers", { answer_id: parseInt(id) });
    res.json({ ok: true });
  }),
);

// Penilaian manual dosen (1-100), kosong = batalkan nilai
router.patch(
  "/dosen/answer/:id/nilai",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  requireOwnsAnswer(),
  validate({ body: nilaiSchema }),
  asyncHandler(async (req, res) => {
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
  }),
);

router.patch(
  "/dosen/question/:id",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  requireOwnsQuestion(),
  validate({ body: questionPatchSchema }),
  asyncHandler(async (req, res) => {
    const { id } = req.params;
    const { transcript } = req.body;
    await sbUpdate(
      "questions",
      { question_id: parseInt(id) },
      { transcript_text: transcript },
    );
    res.json({ ok: true });
  }),
);

router.delete(
  "/dosen/question/:id",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  requireOwnsQuestion(),
  asyncHandler(async (req, res) => {
    const { id } = req.params;
    await sbDelete("answers", { question_id: parseInt(id) });
    await sbDelete("questions", { question_id: parseInt(id) });
    res.json({ ok: true });
  }),
);

router.post(
  "/dosen/manage-uid",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  validate({ body: manageUidSchema }),
  asyncHandler(async (req, res) => {
    const { action, uid, entry_id, student_id, target_date, current_class } =
      req.body;

    if (action === "delete_all") {
      state.sessionData.scannedList = [];
    } else if (action === "delete") {
      if (entry_id) {
        const idx = state.sessionData.scannedList.findIndex(
          (item) => item.id === parseInt(entry_id),
        );
        if (idx !== -1) state.sessionData.scannedList.splice(idx, 1);
      } else {
        const idx = state.sessionData.scannedList.findIndex(
          (item) => item.uid === uid,
        );
        if (idx !== -1) state.sessionData.scannedList.splice(idx, 1);
      }
    } else if (action === "add_single") {
      try {
        const rows = await sbSelect(
          "students",
          { student_id: toInt(student_id) },
          "rfid_uid",
        );
        if (rows.length > 0) {
          state.scanCounter++;
          state.sessionData.scannedList.unshift({
            id: state.scanCounter,
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
            state.scanCounter++;
            state.sessionData.scannedList.push({
              id: state.scanCounter,
              uid: s.rfid_uid,
            });
          });
        }
      } catch (err) {}
    }

    res.redirect(`/dosen?kelas=${current_class}`);
  }),
);

router.post(
  "/dosen/request-audio",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  validate({ body: requestAudioSchema }),
  requireOwnsAnswer("body"),
  asyncHandler(async (req, res) => {
    const { id, current_class } = req.body;

    await sbUpdate(
      "answers",
      { answer_id: toInt(id) },
      { audio_file_path: "#" },
    );
    res.redirect(`/dosen?kelas=${current_class}`);
  }),
);

router.post(
  "/dosen/request-audio-sync",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  validate({ body: requestSyncSchema }),
  asyncHandler(async (req, res) => {
    const { current_class } = req.body;
    const ok = await publishCommand({
      perintah: "request_sync_audio",
      target_kelas: current_class,
    });
    if (!ok)
      return res
        .status(500)
        .json({ success: false, message: "Gagal menghubungi alat." });
    // Susul perintah sync WAV beberapa detik kemudian.
    setTimeout(() => {
      publishCommand({
        perintah: "request_sync_wav",
        target_kelas: current_class,
      });
    }, 2000);
    res.json({
      success: true,
      message: "Permintaan sync metadata & audio dikirim ke alat.",
    });
  }),
);

router.post(
  "/dosen/request-wav-sync",
  requireRole(ROLES.DOSEN, ROLES.ADMIN),
  validate({ body: requestSyncSchema }),
  asyncHandler(async (req, res) => {
    const { current_class } = req.body;
    const ok = await publishCommand({
      perintah: "request_sync_wav",
      target_kelas: current_class,
    });
    if (!ok)
      return res
        .status(500)
        .json({ success: false, message: "Gagal menghubungi alat." });
    res.json({
      success: true,
      message: "Permintaan sync audio WAV dikirim ke alat.",
    });
  }),
);

module.exports = router;
