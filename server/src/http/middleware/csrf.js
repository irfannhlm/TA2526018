"use strict";

// Proteksi CSRF pola double-submit cookie (pustaka `csrf-csrf`).
// Token diset di cookie & di-expose ke view via res.locals.csrfToken.
// Endpoint ESP32 (`/api/upload-audio*`) di-skip karena dipanggil alat,
// bukan browser form. Saat NODE_ENV=test, proteksi dimatikan total
// (test harness tidak punya browser/cookie store).

const { doubleCsrf } = require("csrf-csrf");
const env = require("../../config/env");

const isProd = process.env.NODE_ENV === "production";
const isTest = process.env.NODE_ENV === "test";

const { generateCsrfToken, doubleCsrfProtection } = doubleCsrf({
  getSecret: () => env.SESSION_SECRET,
  getSessionIdentifier: (req) => req.sessionID || "anonymous",
  cookieName: isProd ? "__Host-x-csrf" : "x-csrf",
  cookieOptions: {
    httpOnly: true,
    sameSite: "lax",
    secure: isProd,
    path: "/",
  },
  size: 64,
  getCsrfTokenFromRequest: (req) =>
    (req.body && req.body._csrf) || req.headers["x-csrf-token"],
});

function isExempt(req) {
  // ESP32 mengirim multipart/form-data lewat HTTP biasa, tidak ada
  // konteks browser. Akan diamankan dengan API-key terpisah nanti.
  return req.path.startsWith("/api/upload-audio");
}

function csrfMiddleware(req, res, next) {
  if (isTest) {
    res.locals.csrfToken = "TEST";
    return next();
  }
  if (isExempt(req)) return next();

  // Generate token sekali per request lalu expose ke view.
  try {
    res.locals.csrfToken = generateCsrfToken(req, res);
  } catch (_) {
    res.locals.csrfToken = "";
  }

  // Validasi pada metode state-changing (GET/HEAD/OPTIONS dilewati lib).
  doubleCsrfProtection(req, res, (err) => {
    if (!err) return next();

    // Token bisa kedaluwarsa setelah server restart (sesi di MemoryStore
    // hilang, cookie x-csrf lama tidak cocok lagi). Hindari "layar putih":
    // - Navigasi form (HTML): redirect balik dgn pesan via flash, sehingga
    //   browser memuat halaman segar (token baru) lalu pengguna submit lagi.
    // - AJAX/API: balas JSON 403 supaya handler sisi-klien bisa menangani.
    const wantsHtml = (req.headers.accept || "").includes("text/html");
    const isApi = req.path.startsWith("/api/");

    if (wantsHtml && !isApi) {
      if (req.session) {
        req.session.flashError =
          "Sesi keamanan diperbarui. Silakan coba lagi.";
      }
      return res.redirect(req.get("Referer") || "/");
    }
    return res
      .status(403)
      .json({ error: "Token CSRF tidak valid atau hilang." });
  });
}

module.exports = { csrfMiddleware };
