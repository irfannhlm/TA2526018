"use strict";

// Error-middleware terpusat. Menggantikan ~40 blok try/catch per-route:
// error tak terduga diteruskan ke sini (via asyncHandler) dan dijawab
// dengan format seragam. Catatan: ini MENGUBAH perilaku response error
// lama yang sebelumnya bermacam-macam (disetujui saat refactor).

function errorHandler(err, req, res, next) {
  console.error(
    `❌ ${req.method} ${req.originalUrl}:`,
    err && err.message ? err.message : err,
  );
  if (res.headersSent) return next(err);

  const wantsJson =
    req.path.startsWith("/api/") ||
    (req.headers.accept || "").includes("application/json");

  res.status(500);
  if (wantsJson) {
    res.json({ error: "Terjadi kesalahan server." });
  } else {
    res.render(
      "error",
      {
        title: "Terjadi Kesalahan",
        message: "Maaf, terjadi kesalahan di server. Silakan coba lagi.",
      },
      (renderErr, html) => {
        // Fallback ke teks bila view gagal dirender, agar tidak crash.
        if (renderErr) return res.send("Terjadi kesalahan server.");
        res.send(html);
      },
    );
  }
}

module.exports = errorHandler;
