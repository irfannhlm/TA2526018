"use strict";

// ============================================================================
// Fake Supabase client in-memory untuk characterization test.
//
// TUJUAN: meniru perilaku @supabase/supabase-js *secukupnya* untuk pola query
// yang benar-benar dipakai server.js — bukan emulator PostgREST lengkap.
//
// Didukung:
//   .from(t).select(cols).eq/.in/.neq/.not/.gte/.lte/.order/.limit/.single()
//   .from(t).insert(row).select().single()
//   .from(t).update(obj)<filters>.select()
//   .from(t).delete()<filters>
//   embedded select sederhana sesuai skema proyek (lihat resolveEmbedded)
//   .storage.from(bucket).upload()/getPublicUrl()/list()
// ============================================================================

const TABLE_PK = {
  answers: "answer_id",
  questions: "question_id",
  students: "student_id",
  classes: "class_id",
  devices: "device_id",
  users: "user_id",
  class_students: "class_student_id",
  duplicate_queue: "qid",
};

function clone(v) {
  return v == null ? v : JSON.parse(JSON.stringify(v));
}

class QueryBuilder {
  constructor(db, table) {
    this._db = db;
    this._table = table;
    this._op = "select";
    this._columns = "*";
    this._filters = []; // {type:'eq'|'in'|'neq'|'not'|'gte'|'lte', col, val, op?}
    this._order = null;
    this._limit = null;
    this._single = false;
    this._payload = null;
  }

  select(columns = "*") {
    if (this._op !== "insert" && this._op !== "update") this._op = "select";
    this._columns = columns;
    return this;
  }
  insert(row) {
    this._op = "insert";
    this._payload = row;
    return this;
  }
  update(obj) {
    this._op = "update";
    this._payload = obj;
    return this;
  }
  delete() {
    this._op = "delete";
    return this;
  }

  eq(col, val) {
    this._filters.push({ type: "eq", col, val });
    return this;
  }
  in(col, val) {
    this._filters.push({ type: "in", col, val });
    return this;
  }
  neq(col, val) {
    this._filters.push({ type: "neq", col, val });
    return this;
  }
  is(col, val) {
    this._filters.push({ type: "is", col, val });
    return this;
  }
  not(col, op, val) {
    this._filters.push({ type: "not", col, op, val });
    return this;
  }
  gte(col, val) {
    this._filters.push({ type: "gte", col, val });
    return this;
  }
  lte(col, val) {
    this._filters.push({ type: "lte", col, val });
    return this;
  }
  or() {
    // Hanya dipakai oleh Deepgram sweep yang di-mock — abaikan.
    return this;
  }
  order(col, opts = {}) {
    this._order = { col, asc: opts.ascending !== false };
    return this;
  }
  limit(n) {
    this._limit = n;
    return this;
  }
  single() {
    this._single = true;
    return this;
  }

  _rows() {
    return (
      this._db._tables[this._table] || (this._db._tables[this._table] = [])
    );
  }

  _matches(row) {
    return this._filters.every((f) => {
      // filter pada kolom embedded (mis. "classes.class_name") ditangani terpisah
      if (f.col && f.col.includes(".")) return true;
      const cell = row[f.col];
      switch (f.type) {
        case "eq":
          // Mirip PostgREST: .eq(col, null) TIDAK cocok dgn NULL
          // (harus pakai .is). Memaksa kode pakai filter yang benar.
          return f.val === null ? false : cell == f.val;
        case "neq":
          return cell != f.val;
        case "is":
          return f.val === null ? cell == null : cell === f.val;
        case "in":
          return Array.isArray(f.val) && f.val.some((v) => v == cell);
        case "gte":
          return cell >= f.val;
        case "lte":
          return cell <= f.val;
        case "not":
          if (f.op === "eq") return !(cell == f.val);
          if (f.op === "is" && f.val === null) return cell != null;
          return true;
        default:
          return true;
      }
    });
  }

  _embeddedColFilters() {
    return this._filters.filter((f) => f.col && f.col.includes("."));
  }

  async _run() {
    const db = this._db;
    if (this._op === "insert") {
      const pk = TABLE_PK[this._table] || "id";
      const insertOne = (r) => {
        const row = clone(r);
        if (row[pk] == null) row[pk] = db._nextId(this._table);
        if (
          (this._table === "questions" || this._table === "answers") &&
          row.created_at == null
        ) {
          row.created_at = new Date().toISOString();
        }
        this._rows().push(row);
        return clone(row);
      };
      const created = Array.isArray(this._payload)
        ? this._payload.map(insertOne)
        : insertOne(this._payload);
      const data = this._single
        ? Array.isArray(created)
          ? created[0]
          : created
        : created;
      return { data, error: null };
    }

    if (this._op === "update") {
      const updated = [];
      for (const row of this._rows()) {
        if (this._matches(row)) {
          Object.assign(row, clone(this._payload));
          updated.push(clone(row));
        }
      }
      return { data: updated, error: null };
    }

    if (this._op === "delete") {
      const rows = this._rows();
      const keep = rows.filter((r) => !this._matches(r));
      db._tables[this._table] = keep;
      return { data: null, error: null };
    }

    // select
    let rows = this._rows().filter((r) => this._matches(r));
    rows = rows.map((r) => resolveEmbedded(db, this._table, r, this._columns));

    // filter berdasarkan kolom embedded (mis. classes.class_name)
    for (const f of this._embeddedColFilters()) {
      const [rel, col] = f.col.split(".");
      rows = rows.filter((r) => {
        const emb = r[rel];
        const list = Array.isArray(emb) ? emb : emb ? [emb] : [];
        return list.some((e) => e && e[col] == f.val);
      });
    }

    if (this._order) {
      const { col, asc } = this._order;
      rows.sort((a, b) => {
        const av = a[col];
        const bv = b[col];
        if (av === bv) return 0;
        if (av == null) return asc ? -1 : 1;
        if (bv == null) return asc ? 1 : -1;
        return asc ? (av < bv ? -1 : 1) : av < bv ? 1 : -1;
      });
    }
    if (this._limit != null) rows = rows.slice(0, this._limit);

    if (this._single) {
      if (rows.length === 1) return { data: clone(rows[0]), error: null };
      if (rows.length === 0)
        return {
          data: null,
          error: { code: "PGRST116", message: "no rows" },
        };
      return { data: clone(rows[0]), error: null };
    }
    return { data: clone(rows), error: null };
  }

  then(resolve, reject) {
    return this._run().then(resolve, reject);
  }
  catch(cb) {
    return this._run().catch(cb);
  }
}

// Parser sederhana untuk daftar kolom select PostgREST: "a, b(c,d), e!inner(f)"
function parseSelect(columns) {
  if (!columns || columns === "*") return [];
  const parts = [];
  let depth = 0;
  let buf = "";
  for (const ch of columns) {
    if (ch === "(") depth++;
    if (ch === ")") depth--;
    if (ch === "," && depth === 0) {
      parts.push(buf.trim());
      buf = "";
    } else {
      buf += ch;
    }
  }
  if (buf.trim()) parts.push(buf.trim());
  return parts
    .map((p) => {
      const m = p.match(/^([a-zA-Z_]+)(!inner|!left)?\s*\((.*)\)$/s);
      if (m)
        return {
          name: m[1],
          embedded: true,
          inner: m[2] === "!inner",
          sub: m[3],
        };
      return { name: p, embedded: false };
    })
    .filter((x) => x.embedded);
}

// Resolusi relasi embedded sesuai skema proyek (hardcoded by convention).
function resolveEmbedded(db, table, row, columns) {
  const embeds = parseSelect(columns);
  if (embeds.length === 0) return clone(row);
  const out = clone(row);
  const t = (name) => db._tables[name] || [];

  for (const e of embeds) {
    if (table === "classes" && e.name === "class_students") {
      out.class_students = t("class_students")
        .filter((cs) => cs.class_id == row.class_id)
        .map((cs) => clone(cs));
    } else if (
      (table === "class_students" || table === "answers") &&
      e.name === "students"
    ) {
      const s = t("students").find((x) => x.student_id == row.student_id);
      out.students = s ? clone(s) : null;
    } else if (table === "class_students" && e.name === "classes") {
      const c = t("classes").find((x) => x.class_id == row.class_id);
      out.classes = c ? clone(c) : null;
    } else if (table === "answers" && e.name === "questions") {
      const q = t("questions").find((x) => x.question_id == row.question_id);
      out.questions = q ? clone(q) : null;
    } else if (table === "questions" && e.name === "answers") {
      out.answers = t("answers")
        .filter((a) => a.question_id == row.question_id)
        .map((a) => resolveEmbedded(db, "answers", a, e.sub));
    } else if (table === "students" && e.name === "class_students") {
      out.class_students = t("class_students")
        .filter((cs) => cs.student_id == row.student_id)
        .map((cs) => clone(cs));
    } else if (table === "students" && e.name === "classes") {
      const csList = t("class_students").filter(
        (cs) => cs.student_id == row.student_id,
      );
      const classes = csList
        .map((cs) => t("classes").find((c) => c.class_id == cs.class_id))
        .filter(Boolean);
      out.classes = classes.map((c) => clone(c));
    } else {
      out[e.name] = [];
    }
  }
  return out;
}

class StorageBucket {
  constructor(db, bucket) {
    this._db = db;
    this._bucket = bucket;
    this._store = db._storage[bucket] || (db._storage[bucket] = {});
  }
  async upload(p, _buf, _opts) {
    this._store[p] = true;
    return { data: { path: p }, error: null };
  }
  getPublicUrl(p) {
    return {
      data: { publicUrl: `https://fake.storage/${this._bucket}/${p}` },
    };
  }
  async list(folder, opts = {}) {
    const prefix = folder ? folder + "/" : "";
    const names = Object.keys(this._store)
      .filter((k) => k.startsWith(prefix))
      .map((k) => k.slice(prefix.length))
      .filter((n) => !opts.search || n.includes(opts.search));
    return { data: names.map((name) => ({ name })), error: null };
  }
}

class FakeSupabase {
  constructor() {
    this._tables = {};
    this._seq = {};
    this._storage = {};
    this.storage = {
      from: (bucket) => new StorageBucket(this, bucket),
    };
  }
  _nextId(table) {
    this._seq[table] = (this._seq[table] || 0) + 1;
    return this._seq[table];
  }
  from(table) {
    return new QueryBuilder(this, table);
  }

  // ---- util test ----
  seed(table, rows) {
    this._tables[table] = (this._tables[table] || []).concat(
      rows.map((r) => clone(r)),
    );
    const pk = TABLE_PK[table] || "id";
    for (const r of rows) {
      const id = Number(r[pk]);
      if (!Number.isNaN(id) && id > (this._seq[table] || 0)) {
        this._seq[table] = id;
      }
    }
    return this;
  }
  rows(table) {
    return this._tables[table] || [];
  }
  seedStorage(bucket, paths) {
    const store = this._storage[bucket] || (this._storage[bucket] = {});
    for (const p of paths) store[p] = true;
    return this;
  }
  reset() {
    this._tables = {};
    this._seq = {};
    this._storage = {};
  }
}

function createClient() {
  return new FakeSupabase();
}

module.exports = { FakeSupabase, createClient, TABLE_PK };
