"use strict";

// Otorisasi kepemilikan data: dosen hanya boleh mengubah/hapus baris
// (answer/question) yang berada di kelas yang dia ampu. Admin selalu
// di-bypass. Lookup dilakukan via tabel `classes.lecturer_user_id`.

const { sbSelect } = require("../../data/baseRepo");
const { toInt } = require("../../lib/utils");
const { ROLES } = require("../../lib/constants");
const asyncHandler = require("../../lib/asyncHandler");

async function userTeachesClass(userId, classId) {
  if (!userId || !classId) return false;
  const rows = await sbSelect(
    "classes",
    { class_id: classId, lecturer_user_id: userId },
    "class_id",
  );
  return rows.length > 0;
}

function getId(req, source) {
  if (source === "body") return toInt(req.body && req.body.id);
  return toInt(req.params && req.params.id);
}

function deny(res, code, msg) {
  // Jawab seragam: HTML kecil utk page route, JSON utk /api/*.
  if (res.req.path.startsWith("/api/")) {
    return res.status(code).json({ error: msg });
  }
  return res.status(code).send(msg);
}

function requireOwnsAnswer(idSource = "params") {
  return asyncHandler(async (req, res, next) => {
    if (req.session.user.role === ROLES.ADMIN) return next();
    const id = getId(req, idSource);
    if (!id) return deny(res, 400, "ID jawaban tidak valid.");
    const rows = await sbSelect(
      "answers",
      { answer_id: id },
      "class_id, question_id",
    );
    if (rows.length === 0) return deny(res, 404, "Jawaban tidak ditemukan.");
    let classId = rows[0].class_id;
    if (classId == null && rows[0].question_id) {
      const qr = await sbSelect(
        "questions",
        { question_id: rows[0].question_id },
        "class_id",
      );
      classId = qr[0] ? qr[0].class_id : null;
    }
    if (!classId) return deny(res, 403, "Akses ditolak.");
    if (await userTeachesClass(req.session.user.user_id, classId))
      return next();
    return deny(res, 403, "Akses ditolak.");
  });
}

function requireOwnsQuestion(idSource = "params") {
  return asyncHandler(async (req, res, next) => {
    if (req.session.user.role === ROLES.ADMIN) return next();
    const id = getId(req, idSource);
    if (!id) return deny(res, 400, "ID pertanyaan tidak valid.");
    const rows = await sbSelect("questions", { question_id: id }, "class_id");
    if (rows.length === 0) return deny(res, 404, "Pertanyaan tidak ditemukan.");
    if (await userTeachesClass(req.session.user.user_id, rows[0].class_id))
      return next();
    return deny(res, 403, "Akses ditolak.");
  });
}

module.exports = {
  requireOwnsAnswer,
  requireOwnsQuestion,
  userTeachesClass,
};
