"use strict";

// Repository antrian duplikat — persisten di Supabase (tabel
// duplicate_queue) menggantikan array RAM. Kolom file/tipe/target_kelas
// dipromosikan; sisa field disimpan di jsonb `payload`. Lihat
// docs/sql/duplicate_queue.sql untuk skema tabelnya.

const { sbInsert, sbSelect, sbUpdate } = require("./baseRepo");

const TABLE = "duplicate_queue";

function rowToItem(row) {
  return {
    qid: row.qid,
    file: row.file,
    tipe: row.tipe,
    target_kelas: row.target_kelas,
    resolvedAt: row.resolved_at,
    ...(row.payload || {}),
  };
}

/** Simpan satu item pending; kembalikan qid (auto-increment DB).
 *  Idempoten: bila file yang sama masih pending (belum diputuskan),
 *  kembalikan qid yang ada — tidak menggandakan antrian saat ESP/broker
 *  mengirim ulang file yang sama berkali-kali. */
async function pushPending(item) {
  const { file, tipe, target_kelas, ...rest } = item;
  const existing = await sbSelect(TABLE, { file, resolved_at: null }, "qid", {
    limit: 1,
  });
  if (existing.length > 0) return existing[0].qid;
  const row = await sbInsert(TABLE, {
    file,
    tipe,
    target_kelas: target_kelas ?? null,
    payload: rest,
  });
  return row.qid;
}

/** Semua item yang belum diputuskan, urut qid menaik. */
async function listPending() {
  const rows = await sbSelect(TABLE, { resolved_at: null }, "*", {
    order: { col: "qid", asc: true },
  });
  return rows.map(rowToItem);
}

/**
 * Tandai item resolved secara atomik (hanya bila masih pending) dan
 * kembalikan item-nya. Bila sudah resolved / tidak ada -> null.
 */
async function resolve(qid) {
  const updated = await sbUpdate(
    TABLE,
    { qid, resolved_at: null },
    { resolved_at: new Date().toISOString() },
  );
  if (!updated.length) return null;
  return rowToItem(updated[0]);
}

module.exports = { pushPending, listPending, resolve, rowToItem };
