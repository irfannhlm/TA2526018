"use strict";

// Middleware factory untuk validasi input dengan zod.
//   validate({ body: schema, params: schema, query: schema })
// Pada sukses: req.body/params/query di-REPLACE dgn data hasil parse
// (sudah ter-coerce). Pada gagal: balasan 400 JSON utk /api/*, plain
// text utk page route, berisi pesan singkat.

// Label field dalam bahasa Indonesia untuk pesan yang ramah dibaca.
const FIELD_LABEL = {
  username: "Username",
  password: "Password",
  name: "Nama",
  nim: "NIM",
  rfid: "RFID",
  role: "Role",
  code: "Kode kelas",
  lecturer: "Nama dosen",
  current_class: "Kelas",
  kelas: "Kelas",
};

// Terjemahkan satu issue zod -> kalimat Indonesia yang ramah.
function friendlyIssue(issue) {
  const field = issue.path && issue.path.length ? issue.path[0] : null;
  const label = (field && FIELD_LABEL[field]) || "Input";
  switch (issue.code) {
    case "too_small":
      if (issue.origin === "string") {
        return issue.minimum <= 1
          ? `${label} wajib diisi.`
          : `${label} minimal ${issue.minimum} karakter.`;
      }
      return `${label} terlalu kecil.`;
    case "too_big":
      if (issue.origin === "string")
        return `${label} maksimal ${issue.maximum} karakter.`;
      return `${label} terlalu besar.`;
    case "invalid_type":
      return `${label} wajib diisi.`;
    case "invalid_value":
    case "invalid_enum_value":
      return `${label} tidak valid.`;
    default:
      return `${label} tidak valid.`;
  }
}

function pickMessage(err) {
  const first = err.issues && err.issues[0];
  if (!first) return "Input tidak valid.";
  return friendlyIssue(first);
}

function validate(schemas) {
  return (req, res, next) => {
    for (const key of ["body", "params", "query"]) {
      const schema = schemas && schemas[key];
      if (!schema) continue;
      const result = schema.safeParse(req[key]);
      if (!result.success) {
        const msg = pickMessage(result.error);
        // Hanya navigasi form browser sungguhan (Accept: text/html) yang
        // di-redirect balik + popup. Request AJAX/fetch & /api/* tetap JSON
        // supaya handler sisi-klien tidak ikut ter-redirect ke HTML.
        const isHtmlNav =
          !req.path.startsWith("/api/") &&
          (req.headers.accept || "").includes("text/html");
        if (!isHtmlNav) {
          return res.status(400).json({ error: msg });
        }
        // Simpan pesan ke session, lalu kembali ke halaman asal. Popup
        // SweetAlert akan muncul di sana (lihat res.locals.flashError).
        if (req.session) req.session.flashError = msg;
        // Fallback aman bila Referer tidak ada (mis. Referrer-Policy di
        // hosting). Jangan jatuh ke "/" karena route itu merender login —
        // pengguna admin/dosen akan terlihat seperti "ter-logout".
        const role = req.session && req.session.user && req.session.user.role;
        const fallback =
          role === "admin" ? "/admin" : role === "dosen" ? "/dosen" : "/";
        return res.redirect(req.get("Referer") || fallback);
      }
      // Merge hasil parse (coerced) di atas nilai asli — field yang
      // divalidasi ter-coerce, field lain (tidak di-skema) tetap utuh.
      try {
        req[key] = { ...req[key], ...result.data };
      } catch (_) {
        // req.query di Express 5 read-only; abaikan, parsed data tetap di result
      }
    }
    next();
  };
}

module.exports = { validate };
