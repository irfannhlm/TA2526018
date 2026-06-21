"use strict";

// ================= HANDLER 404 =================
// Dipasang SETELAH semua router, SEBELUM error-handler terpusat. Bila tidak
// ada route yang cocok, request sampai ke sini. Request API/JSON dijawab
// dengan JSON; request web dirender sebagai halaman 404 bertema (404.ejs).

function notFound(req, res) {
  console.warn(`⚠️  404 ${req.method} ${req.originalUrl}`);

  const wantsJson =
    req.path.startsWith("/api/") ||
    (req.headers.accept || "").includes("application/json");

  res.status(404);
  if (wantsJson) {
    return res.json({ error: "Halaman atau sumber daya tidak ditemukan." });
  }
  res.render("404", { url: req.originalUrl });
}

module.exports = notFound;
