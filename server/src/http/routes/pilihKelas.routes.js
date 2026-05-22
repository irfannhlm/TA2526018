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
const { ROLES } = require("../../lib/constants");
const {
  transcribeAnswer,
  transcribeQuestion,
} = require("../../../Deepgramservice");
const asyncHandler = require("../../lib/asyncHandler");

const router = express.Router();

// ================= PILIH KELAS =================
router.get(
  "/pilih-kelas",
  requireLogin,
  asyncHandler(async (req, res) => {
    const { username, role, user_id } = req.session.user;

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
  }),
);

module.exports = router;
