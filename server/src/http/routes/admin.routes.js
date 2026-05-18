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
const { upload, uploadTemp, uploadMemory } = require("../middleware/upload");
const { requireLogin, requireRole } = require("../middleware/auth");
const {
  transcribeAnswer,
  transcribeQuestion,
} = require("../../../Deepgramservice");
const asyncHandler = require("../../lib/asyncHandler");

const router = express.Router();

// Parser CSV sederhana: dukung pemisah "," atau ";", field berkutip,
// dan deteksi baris header (kolom "nama"/"name", "nim", "rfid").
// Kolom rfid opsional. Kembalikan array { nama, nim, rfid }.
function parseStudentCsv(text) {
  const lines = (text || "")
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l.length > 0);
  if (lines.length === 0) return [];

  const splitLine = (line) => {
    const out = [];
    let cur = "";
    let inQuote = false;
    for (let i = 0; i < line.length; i++) {
      const c = line[i];
      if (inQuote) {
        if (c === '"') {
          if (line[i + 1] === '"') {
            cur += '"';
            i++;
          } else inQuote = false;
        } else cur += c;
      } else if (c === '"') {
        inQuote = true;
      } else if (c === "," || c === ";") {
        out.push(cur);
        cur = "";
      } else {
        cur += c;
      }
    }
    out.push(cur);
    return out.map((s) => s.trim());
  };

  let namaIdx = 0;
  let nimIdx = 1;
  let rfidIdx = 2;
  let startIdx = 0;
  const firstCells = splitLine(lines[0]).map((c) => c.toLowerCase());
  const looksHeader = firstCells.some(
    (c) =>
      c.includes("nama") ||
      c === "name" ||
      c.includes("nim") ||
      c.includes("rfid"),
  );
  if (looksHeader) {
    startIdx = 1;
    const ni = firstCells.findIndex(
      (c) => c.includes("nama") || c === "name",
    );
    const mi = firstCells.findIndex((c) => c.includes("nim"));
    const ri = firstCells.findIndex((c) => c.includes("rfid"));
    if (ni !== -1) namaIdx = ni;
    if (mi !== -1) nimIdx = mi;
    rfidIdx = ri; // -1 jika tidak ada kolom rfid
  }

  const rows = [];
  for (let i = startIdx; i < lines.length; i++) {
    const cells = splitLine(lines[i]);
    const nama = (cells[namaIdx] || "").trim();
    const nim = (cells[nimIdx] || "").trim();
    const rfid =
      rfidIdx >= 0 ? (cells[rfidIdx] || "").trim() : "";
    if (nama || nim) rows.push({ nama, nim, rfid });
  }
  return rows;
}

// ================= ADMIN ROUTE =================
router.get(
  "/admin",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { username } = req.session.user;
    const currentClass = req.query.kelas || null;
    const currentDeviceId = req.query.device;

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
        currentDevice: {
          id: "N/A",
          name: "Belum Ada Perangkat",
          status: "offline",
          battery: null,
        },
        classes: allClasses,
        currentClass: null,
        session: state.sessionData,
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
        session: state.sessionData,
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

    const mappedList = state.sessionData.scannedList.map((item) => {
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
      session: state.sessionData,
      currentList: mappedList,
      users,
      dosenUsers,
      logs: classLogs,
      username,
    });
  }),
);

// TAMBAH KELAS
router.post(
  "/admin/add-class",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { name, code, lecturer, lecturer_user_id, current_class } = req.body;

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
  }),
);

// EDIT KELAS
router.post(
  "/admin/edit-class",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { id, name, code, lecturer, lecturer_user_id, current_class } =
      req.body;

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
  }),
);

// HAPUS KELAS
router.post(
  "/admin/delete-class",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { id, current_class } = req.body;

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
  }),
);

// RESET ALL DATA
router.post(
  "/admin/reset-all",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    await supabase.from("answers").delete().neq("answer_id", 0);
    await supabase.from("questions").delete().neq("question_id", 0);
    await supabase.from("class_students").delete().neq("class_student_id", 0);
    await supabase.from("students").delete().neq("student_id", 0);
    await supabase.from("classes").delete().neq("class_id", 0);
    state.sessionData.scannedList = [];
    res.redirect(`/pilih-kelas`);
  }),
);

// HAPUS ENTRI RFID (admin)
router.post(
  "/admin/manage-uid",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { action, uid, entry_id, current_class } = req.body;
    if (action === "delete") {
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
    }
    const target = current_class
      ? `/admin?kelas=${current_class}`
      : `/pilih-kelas`;
    res.redirect(target);
  }),
);

// TAMBAH MAHASISWA BARU
router.post(
  "/admin/add",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { name, nim, rfid, kelas, current_class } = req.body;
    const targetClass = kelas || current_class;

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
  }),
);

// TAMBAH MAHASISWA DARI KELAS LAIN
router.post(
  "/admin/add-to-class",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { student_id, current_class } = req.body;

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
  }),
);

// HAPUS MAHASISWA
router.post(
  "/admin/delete",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { id, current_class } = req.body;

    await sbDelete("students", { student_id: toInt(id) });
    res.redirect(`/admin?kelas=${current_class}`);
  }),
);

// EDIT MAHASISWA
router.post(
  "/admin/edit",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { id, name, nim, rfid, current_class } = req.body;

    await sbUpdate(
      "students",
      { student_id: toInt(id) },
      { name, nim, rfid_uid: rfid },
    );
    res.redirect(`/admin?kelas=${current_class}`);
  }),
);

// IMPORT MAHASISWA DARI CSV (nama, nim, rfid opsional)
router.post(
  "/admin/import-csv",
  requireRole("admin"),
  uploadMemory.single("file"),
  asyncHandler(async (req, res) => {
    const { current_class } = req.body;
    if (!req.file || !req.file.buffer) {
      return res.redirect(
        `/admin?kelas=${encodeURIComponent(current_class || "")}&csv=nofile`,
      );
    }

    const clsRows = await sbSelect("classes", { class_name: current_class });
    if (clsRows.length === 0) return res.send("Kelas tidak ditemukan");
    const classId = clsRows[0].class_id;

    const rows = parseStudentCsv(req.file.buffer.toString("utf8"));
    let added = 0;
    let linked = 0;
    let skipped = 0;
    const batchRfids = new Set();

    for (const r of rows) {
      const name = r.nama;
      const nim = r.nim;
      if (!name || !nim) {
        skipped++;
        continue;
      }

      // RFID opsional: hanya dipakai bila ada & belum dipakai siapa pun
      // (di DB maupun di baris CSV sebelumnya) supaya tidak bentrok.
      let rfidToUse = null;
      const rfidRaw = (r.rfid || "").trim();
      if (rfidRaw && !batchRfids.has(rfidRaw)) {
        const dup = await sbSelect("students", { rfid_uid: rfidRaw });
        if (dup.length === 0) {
          rfidToUse = rfidRaw;
          batchRfids.add(rfidRaw);
        }
      }

      const existing = await sbSelect("students", { nim });
      let studentId;
      if (existing.length > 0) {
        studentId = existing[0].student_id;
        // Isi RFID hanya bila mahasiswa belum punya & CSV menyediakan.
        const cur = existing[0].rfid_uid;
        if (rfidToUse && (!cur || !String(cur).trim())) {
          await sbUpdate(
            "students",
            { student_id: studentId },
            { rfid_uid: rfidToUse },
          );
        }
        linked++;
      } else {
        const created = await sbInsert("students", {
          name,
          nim,
          rfid_uid: rfidToUse,
        });
        studentId = created.student_id;
        added++;
      }
      const link = await sbSelect("class_students", {
        student_id: studentId,
        class_id: classId,
      });
      if (link.length === 0) {
        await sbInsert("class_students", {
          student_id: studentId,
          class_id: classId,
        });
      }
    }

    res.redirect(
      `/admin?kelas=${encodeURIComponent(
        current_class,
      )}&csv=ok&added=${added}&linked=${linked}&skipped=${skipped}`,
    );
  }),
);

// EXPORT MAHASISWA KELAS INI KE CSV (nama, nim, rfid)
router.get(
  "/admin/export-csv",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const currentClass = req.query.kelas || "";
    const clsRows = await sbSelect("classes", { class_name: currentClass });
    if (clsRows.length === 0) return res.send("Kelas tidak ditemukan");
    const classId = clsRows[0].class_id;

    const { data: csRows } = await supabase
      .from("class_students")
      .select("students(name,nim,rfid_uid)")
      .eq("class_id", classId);

    const csvField = (v) => {
      const s = v === null || v === undefined ? "" : String(v);
      return /[",\r\n;]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
    };

    const list = (csRows || [])
      .map((r) => r.students)
      .filter(Boolean)
      .sort((a, b) => (a.name || "").localeCompare(b.name || ""));

    let csv = "nama,nim,rfid\r\n";
    for (const s of list) {
      csv += `${csvField(s.name)},${csvField(s.nim)},${csvField(
        s.rfid_uid,
      )}\r\n`;
    }

    const safeName =
      (currentClass || "kelas").replace(/[^a-zA-Z0-9_-]+/g, "_") ||
      "kelas";
    res.setHeader("Content-Type", "text/csv; charset=utf-8");
    res.setHeader(
      "Content-Disposition",
      `attachment; filename="mahasiswa-${safeName}.csv"`,
    );
    res.send("﻿" + csv); // BOM agar Excel kenali UTF-8
  }),
);

// POLLING: scan kartu terbaru sejak id tertentu (untuk assign RFID)
router.get(
  "/admin/last-scan",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const since = parseInt(req.query.since || "0", 10) || 0;
    const baru = state.sessionData.scannedList.filter((s) => s.id > since);
    const scan =
      baru.length > 0
        ? baru.reduce((a, b) => (a.id > b.id ? a : b))
        : null;
    res.json({
      counter: state.scanCounter,
      scan: scan ? { id: scan.id, uid: scan.uid } : null,
    });
  }),
);

// ASSIGN UID HASIL SCAN KE MAHASISWA (RFID yang tadinya kosong)
router.post(
  "/admin/assign-rfid",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { student_id, uid } = req.body;
    const sid = toInt(student_id);
    if (!sid || !uid) {
      return res
        .status(400)
        .json({ ok: false, message: "Data tidak lengkap." });
    }
    const used = await sbSelect("students", { rfid_uid: uid });
    if (used.some((s) => s.student_id !== sid)) {
      return res.status(409).json({
        ok: false,
        message: "Kartu RFID ini sudah dipakai mahasiswa lain.",
      });
    }
    await sbUpdate("students", { student_id: sid }, { rfid_uid: uid });
    state.sessionData.scannedList = state.sessionData.scannedList.filter(
      (s) => s.uid !== uid,
    );
    res.json({ ok: true });
  }),
);

// TAMBAH USER
router.post(
  "/admin/add-user",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
    const { username, password, role, current_class } = req.body;

    const hashedPassword = await bcrypt.hash(password, SALT_ROUNDS);
    await sbInsert("users", { username, password: hashedPassword, role });
    res.redirect(`/admin?kelas=${current_class}`);
  }),
);

// HAPUS USER
router.post(
  "/admin/delete-user",
  requireRole("admin"),
  asyncHandler(async (req, res) => {
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
  }),
);

module.exports = router;
