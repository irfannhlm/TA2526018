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
  ROLES,
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
const rateLimit = require("express-rate-limit");
const { z } = require("zod");

const router = express.Router();

// Batasi percobaan login (mitigasi brute-force).
const loginLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  max: 20,
  standardHeaders: true,
  legacyHeaders: false,
  message: "Terlalu banyak percobaan login. Coba lagi nanti.",
});

const loginSchema = z.object({
  username: z.string().min(1),
  password: z.string().min(1),
});

// ================= ROUTES =================
router.get(
  "/",
  asyncHandler((req, res) => res.render("login", { error: null })),
);

router.post(
  "/login",
  loginLimiter,
  asyncHandler(async (req, res) => {
    const parsed = loginSchema.safeParse(req.body);
    if (!parsed.success) {
      return res.status(400).render("login", { error: "Input tidak valid." });
    }
    const { username, password } = parsed.data;

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

    const redirectTo = user.role === ROLES.ADMIN ? "/admin" : "/pilih-kelas";
    return res.redirect(redirectTo);
  }),
);

router.post(
  "/logout",
  asyncHandler((req, res) => {
    req.session.destroy(() => res.redirect("/"));
  }),
);

module.exports = router;
