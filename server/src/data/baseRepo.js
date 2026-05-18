"use strict";

// Helper akses data generik ke Supabase. Dipindahkan apa adanya dari
// server.js (behavior-preserving) — belum dipecah per-tabel (itu fase lanjut).

const { supabase } = require("../config/supabase");
const { TABLE_PK } = require("../lib/constants");

async function sbSelect(table, filters = {}, columns = "*", extra = {}) {
  let q = supabase.from(table).select(columns);
  for (const [col, val] of Object.entries(filters)) {
    if (Array.isArray(val)) q = q.in(col, val);
    else q = q.eq(col, val);
  }
  if (extra.order)
    q = q.order(extra.order.col, { ascending: extra.order.asc ?? true });
  if (extra.limit) q = q.limit(extra.limit);
  const { data, error } = await q;
  if (error) throw error;
  return data || [];
}

async function sbInsert(table, row) {
  const { data, error } = await supabase
    .from(table)
    .insert(row)
    .select()
    .single();
  if (error) throw error;
  return data;
}

async function sbUpdate(table, filters, updates) {
  let q = supabase.from(table).update(updates);
  const entries = Object.entries(filters);
  if (entries.length === 0) {
    q = q.neq(TABLE_PK[table] || "id", -1);
  } else {
    for (const [col, val] of entries) {
      if (Array.isArray(val)) q = q.in(col, val);
      else q = q.eq(col, val);
    }
  }
  const { data, error } = await q.select();
  if (error) throw error;
  return data || [];
}

async function sbDelete(table, filters) {
  let q = supabase.from(table).delete();
  for (const [col, val] of Object.entries(filters)) {
    if (Array.isArray(val)) q = q.in(col, val);
    else q = q.eq(col, val);
  }
  const { error } = await q;
  if (error) throw error;
}

module.exports = { sbSelect, sbInsert, sbUpdate, sbDelete };
