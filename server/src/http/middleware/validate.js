"use strict";

// Middleware factory untuk validasi input dengan zod.
//   validate({ body: schema, params: schema, query: schema })
// Pada sukses: req.body/params/query di-REPLACE dgn data hasil parse
// (sudah ter-coerce). Pada gagal: balasan 400 JSON utk /api/*, plain
// text utk page route, berisi pesan singkat.

function pickMessage(err) {
  const first = err.issues && err.issues[0];
  if (!first) return "Input tidak valid.";
  const path = first.path && first.path.length ? first.path.join(".") : "input";
  return `${path}: ${first.message}`;
}

function validate(schemas) {
  return (req, res, next) => {
    for (const key of ["body", "params", "query"]) {
      const schema = schemas && schemas[key];
      if (!schema) continue;
      const result = schema.safeParse(req[key]);
      if (!result.success) {
        const msg = pickMessage(result.error);
        if (req.path.startsWith("/api/")) {
          return res.status(400).json({ error: msg });
        }
        return res.status(400).send(msg);
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
