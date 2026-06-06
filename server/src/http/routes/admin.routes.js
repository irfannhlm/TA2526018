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
const { ROLES } = require("../../lib/constants");
const {
  transcribeAnswer,
  transcribeQuestion,
} = require("../../../Deepgramservice");
const asyncHandler = require("../../lib/asyncHandler");
const { validate } = require("../middleware/validate");
const {
  addClassSchema,
  editClassSchema,
  deleteClassSchema,
  deleteStudentSchema,
  editStudentSchema,
  addStudentSchema: adminAddStudentSchema,
  addToClassSchema,
  addUserSchema,
  deleteUserSchema,
  manageUidSchema: adminManageUidSchema,
} = require("../schemas/admin.schemas");

const router = express.Router();

// Pecah satu baris CSV (dukung pemisah "," / ";" & field berkutip).
function splitCsvLine(line) {
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
}

// CSV teks -> array 2D (baris x sel).
function rowsFromCsv(text) {
  return (text || "")
    .split(/\r?\n/)
    .filter((l) => l.trim().length > 0)
    .map((l) => splitCsvLine(l));
}

// Ubah nilai sel ExcelJS (bisa objek rich-text/formula/hyperlink) -> string.
function cellToStr(v) {
  if (v === null || v === undefined) return "";
  if (typeof v === "object") {
    if (Array.isArray(v.richText))
      return v.richText.map((t) => t.text).join("");
    if (v.text !== undefined && v.text !== null) return String(v.text);
    if (v.result !== undefined && v.result !== null) return String(v.result);
    if (v instanceof Date) return v.toISOString();
    return "";
  }
  return String(v);
}

// File XLSX (buffer) -> array 2D (baris x sel).
async function rowsFromXlsx(buffer) {
  const ExcelJS = require("exceljs");
  const wb = new ExcelJS.Workbook();
  await wb.xlsx.load(buffer);
  const ws = wb.worksheets[0];
  if (!ws) return [];
  const rows = [];
  ws.eachRow({ includeEmpty: true }, (row) => {
    const vals = row.values || []; // index 0 tidak dipakai (1-based)
    const arr = [];
    for (let i = 1; i < vals.length; i++) arr.push(cellToStr(vals[i]).trim());
    rows.push(arr);
  });
  return rows;
}

// Dari array 2D, cari baris header (ada sel mirip "nama" DAN "nim"),
// lewati baris preamble di atasnya, lalu ambil { nama, nim, rfid }.
// rfid opsional. Kalau tak ada header sama sekali -> anggap tanpa
// header (kolom 0=nama, 1=nim, 2=rfid) demi kompatibilitas CSV lama.
function extractStudents(rows) {
  if (!Array.isArray(rows) || rows.length === 0) return [];
  const isNama = (c) => c.includes("nama") || c === "name";
  const isNim = (c) => c.includes("nim");
  const isRfid = (c) => c.includes("rfid");

  let headerIdx = -1;
  for (let i = 0; i < rows.length; i++) {
    const cells = (rows[i] || []).map((c) =>
      String(c == null ? "" : c)
        .trim()
        .toLowerCase(),
    );
    if (cells.some(isNama) && cells.some(isNim)) {
      headerIdx = i;
      break;
    }
  }

  let namaIdx = 0;
  let nimIdx = 1;
  let rfidIdx = -1;
  let startIdx = 0;
  if (headerIdx !== -1) {
    const h = (rows[headerIdx] || []).map((c) =>
      String(c == null ? "" : c)
        .trim()
        .toLowerCase(),
    );
    namaIdx = h.findIndex(isNama);
    nimIdx = h.findIndex(isNim);
    rfidIdx = h.findIndex(isRfid);
    startIdx = headerIdx + 1;
  }

  const out = [];
  for (let i = startIdx; i < rows.length; i++) {
    const cells = rows[i] || [];
    const get = (idx) =>
      idx >= 0 && cells[idx] != null ? String(cells[idx]).trim() : "";
    const nama = get(namaIdx);
    const nim = get(nimIdx);
    const rfid = rfidIdx >= 0 ? get(rfidIdx) : "";
    if (nama || nim) out.push({ nama, nim, rfid });
  }
  return out;
}

// ================= ADMIN ROUTE =================
router.get(
  "/admin",
  requireRole(ROLES.ADMIN),
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
  requireRole(ROLES.ADMIN),
  validate({ body: addClassSchema }),
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
  requireRole(ROLES.ADMIN),
  validate({ body: editClassSchema }),
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
  requireRole(ROLES.ADMIN),
  validate({ body: deleteClassSchema }),
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
  requireRole(ROLES.ADMIN),
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
  requireRole(ROLES.ADMIN),
  validate({ body: adminManageUidSchema }),
  asyncHandler(async (req, res) => {
    const { action, uid, entry_id, current_class } = req.body;
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
  requireRole(ROLES.ADMIN),
  validate({ body: adminAddStudentSchema }),
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
  requireRole(ROLES.ADMIN),
  validate({ body: addToClassSchema }),
  asyncHandler(async (req, res) => {
    const { student_id, current_class } = req.body;

    const clsRows = await sbSelect("classes", { class_name: current_class });
    if (clsRows.length === 0) return res.send("Kelas tidak ditemukan");
    const classId = clsRows[0].class_id;

    // Bisa banyak (checklist) atau satu — normalkan jadi array id.
    const ids = (Array.isArray(student_id) ? student_id : [student_id])
      .map((v) => toInt(v))
      .filter((v) => v !== null);

    for (const sid of ids) {
      const existing = await sbSelect("class_students", {
        student_id: sid,
        class_id: classId,
      });
      if (existing.length === 0) {
        await sbInsert("class_students", {
          student_id: sid,
          class_id: classId,
        });
      }
    }
    res.redirect(`/admin?kelas=${encodeURIComponent(current_class)}`);
  }),
);

// HAPUS MAHASISWA
// mode "class"    -> keluarkan dari kelas ini saja (record tetap ada)
// mode "database" -> hapus permanen record mahasiswa (default)
router.post(
  "/admin/delete",
  requireRole(ROLES.ADMIN),
  validate({ body: deleteStudentSchema }),
  asyncHandler(async (req, res) => {
    const { id, current_class, mode } = req.body;
    const studentId = toInt(id);

    if (mode === "class") {
      const clsRows = await sbSelect("classes", {
        class_name: current_class,
      });
      if (clsRows.length > 0) {
        await sbDelete("class_students", {
          student_id: studentId,
          class_id: clsRows[0].class_id,
        });
      }
    } else {
      await sbDelete("students", { student_id: studentId });
    }
    res.redirect(`/admin?kelas=${encodeURIComponent(current_class)}`);
  }),
);

// HAPUS SEMUA ANGGOTA KELAS
// mode "class"    -> hanya keluarkan dari kelas ini (data tetap ada)
// mode "database" -> hapus permanen record mahasiswa (default)
router.post(
  "/admin/delete-all-students",
  requireRole(ROLES.ADMIN),
  asyncHandler(async (req, res) => {
    const { current_class, mode } = req.body;

    const clsRows = await sbSelect("classes", { class_name: current_class });
    if (clsRows.length === 0) return res.send("Kelas tidak ditemukan");
    const classId = clsRows[0].class_id;

    const csRows = await sbSelect(
      "class_students",
      { class_id: classId },
      "student_id",
    );
    const studentIds = csRows.map((r) => r.student_id);

    await sbDelete("class_students", { class_id: classId });
    if (mode !== "class" && studentIds.length > 0) {
      await sbDelete("students", { student_id: studentIds });
    }
    res.redirect(`/admin?kelas=${encodeURIComponent(current_class)}`);
  }),
);

// EDIT MAHASISWA
router.post(
  "/admin/edit",
  requireRole(ROLES.ADMIN),
  validate({ body: editStudentSchema }),
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

// Baca file upload (CSV/XLSX) -> array { nama, nim, rfid } valid &
// unik per-NIM dalam file. Lempar error bila file tak terbaca.
async function bacaFileMahasiswa(file) {
  const fname = (file.originalname || "").toLowerCase();
  const mime = file.mimetype || "";
  const isXlsx =
    fname.endsWith(".xlsx") ||
    fname.endsWith(".xlsm") ||
    mime.includes("spreadsheetml") ||
    mime.includes("excel");
  const grid = isXlsx
    ? await rowsFromXlsx(file.buffer)
    : rowsFromCsv(file.buffer.toString("utf8"));

  const seen = new Set();
  const out = [];
  for (const r of extractStudents(grid)) {
    const nama = String(r.nama || "").trim();
    const nim = String(r.nim || "").trim();
    const rfid = String(r.rfid || "").trim();
    if (!nama || !nim) continue;
    if (seen.has(nim)) continue; // dobel di dalam file -> ambil yang pertama
    seen.add(nim);
    out.push({ nama, nim, rfid });
  }
  return out;
}

// RFID boleh dipakai bila tidak kosong & belum dipakai mahasiswa lain.
async function rfidUsable(rfidRaw, selfStudentId) {
  if (!rfidRaw) return null;
  const used = await sbSelect("students", { rfid_uid: rfidRaw });
  const bentrok = used.some((s) => s.student_id !== selfStudentId);
  return bentrok ? null : rfidRaw;
}

// LANGKAH 1: PREVIEW — deteksi mana yang baru & mana yang sudah ada
router.post(
  "/admin/import-preview",
  requireRole(ROLES.ADMIN),
  uploadMemory.single("file"),
  asyncHandler(async (req, res) => {
    const { current_class } = req.body;
    if (!req.file || !req.file.buffer) {
      return res.status(400).json({ ok: false, message: "File tidak ada." });
    }
    const clsRows = await sbSelect("classes", { class_name: current_class });
    if (clsRows.length === 0) {
      return res
        .status(404)
        .json({ ok: false, message: "Kelas tidak ditemukan." });
    }

    let rows;
    try {
      rows = await bacaFileMahasiswa(req.file);
    } catch (e) {
      console.error("❌ [import] Gagal baca file:", e.message);
      return res.status(400).json({
        ok: false,
        message: "File tidak bisa dibaca. Pastikan format CSV/XLSX benar.",
      });
    }

    const dups = [];
    for (const r of rows) {
      const existing = await sbSelect("students", { nim: r.nim });
      if (existing.length > 0) {
        dups.push({
          nama: r.nama,
          nim: r.nim,
          existingName: existing[0].name,
        });
      }
    }

    res.json({ ok: true, total: rows.length, rows, dups });
  }),
);

// LANGKAH 2: COMMIT — jalankan import dgn keputusan duplikat
router.post(
  "/admin/import-commit",
  requireRole(ROLES.ADMIN),
  asyncHandler(async (req, res) => {
    const { current_class, rows, dupMode } = req.body;
    const mode = dupMode === "replace" ? "replace" : "skip";

    const clsRows = await sbSelect("classes", { class_name: current_class });
    if (clsRows.length === 0) {
      return res
        .status(404)
        .json({ ok: false, message: "Kelas tidak ditemukan." });
    }
    const classId = clsRows[0].class_id;

    let added = 0;
    let replaced = 0;
    let skipped = 0;
    const seen = new Set();

    for (const r of Array.isArray(rows) ? rows : []) {
      const name = String(r.nama || "").trim();
      const nim = String(r.nim || "").trim();
      const rfidRaw = String(r.rfid || "").trim();
      if (!name || !nim || seen.has(nim)) {
        skipped++;
        continue;
      }
      seen.add(nim);

      let studentId = null;
      const existing = await sbSelect("students", { nim });

      if (existing.length > 0) {
        if (mode !== "replace") {
          skipped++;
          continue; // duplikat & user pilih SKIP
        }
        // REPLACE: timpa data mahasiswa yang sudah ada
        studentId = existing[0].student_id;
        const updates = { name };
        const rfidOk = await rfidUsable(rfidRaw, studentId);
        if (rfidOk) updates.rfid_uid = rfidOk;
        await sbUpdate("students", { student_id: studentId }, updates);
        replaced++;
      } else {
        const rfidOk = await rfidUsable(rfidRaw, null);
        try {
          const created = await sbInsert("students", {
            name,
            nim,
            rfid_uid: rfidOk,
          });
          studentId = created.student_id;
          added++;
        } catch (e) {
          const again = await sbSelect("students", { nim });
          if (again.length > 0 && mode === "replace") {
            studentId = again[0].student_id;
            await sbUpdate("students", { student_id: studentId }, { name });
            replaced++;
          } else {
            console.error(`❌ [import] Gagal simpan NIM ${nim}: ${e.message}`);
            skipped++;
            continue;
          }
        }
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

    res.json({ ok: true, added, replaced, skipped });
  }),
);

// EXPORT MAHASISWA KELAS INI KE CSV (nama, nim, rfid)
router.get(
  "/admin/export-csv",
  requireRole(ROLES.ADMIN),
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
      (currentClass || "kelas").replace(/[^a-zA-Z0-9_-]+/g, "_") || "kelas";
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
  requireRole(ROLES.ADMIN),
  asyncHandler(async (req, res) => {
    const since = parseInt(req.query.since || "0", 10) || 0;
    const baru = state.sessionData.scannedList.filter((s) => s.id > since);
    const scan =
      baru.length > 0 ? baru.reduce((a, b) => (a.id > b.id ? a : b)) : null;
    res.json({
      counter: state.scanCounter,
      scan: scan ? { id: scan.id, uid: scan.uid } : null,
    });
  }),
);

// ASSIGN UID HASIL SCAN KE MAHASISWA (RFID yang tadinya kosong)
router.post(
  "/admin/assign-rfid",
  requireRole(ROLES.ADMIN),
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
  requireRole(ROLES.ADMIN),
  validate({ body: addUserSchema }),
  asyncHandler(async (req, res) => {
    const { username, password, role, current_class } = req.body;
    const backTo = current_class ? `/admin?kelas=${current_class}` : `/admin`;

    // Pre-check duplikat: hindari error 500 dari unique constraint Postgres
    // (users_username_key) dan tampilkan pesan ramah lewat popup flashError.
    const existing = await sbSelect(
      "users",
      { username },
      "user_id",
      { limit: 1 },
    );
    if (existing.length > 0) {
      if (req.session) {
        req.session.flashError = `Username "${username}" sudah digunakan, pilih yang lain.`;
      }
      return res.redirect(backTo);
    }

    try {
      const hashedPassword = await bcrypt.hash(password, SALT_ROUNDS);
      await sbInsert("users", { username, password: hashedPassword, role });
    } catch (err) {
      // Race condition antara pre-check & insert: dua request paralel
      // dengan username sama. Tangkap dan beri pesan yang sama.
      const msg = err && err.message ? String(err.message) : "";
      const isDup =
        (err && err.code === "23505") || msg.includes("duplicate key");
      if (isDup) {
        if (req.session) {
          req.session.flashError = `Username "${username}" sudah digunakan, pilih yang lain.`;
        }
        return res.redirect(backTo);
      }
      throw err;
    }
    res.redirect(backTo);
  }),
);

// HAPUS USER
router.post(
  "/admin/delete-user",
  requireRole(ROLES.ADMIN),
  validate({ body: deleteUserSchema }),
  asyncHandler(async (req, res) => {
    const { id, current_class } = req.body;
    const userId = toInt(id);
    if (userId && userId !== 1) {
      try {
        await sbDelete("users", { user_id: userId });
      } catch (err) {
        console.error("❌ [delete-user] Error:", err);
      }
    }
    // Bila aksi dilakukan dari tab "Manajemen Akun" (tanpa kelas dipilih),
    // tetap di /admin — JANGAN redirect ke /pilih-kelas.
    res.redirect(current_class ? `/admin?kelas=${current_class}` : `/admin`);
  }),
);

module.exports = router;
