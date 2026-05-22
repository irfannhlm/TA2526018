-- Tabel antrian duplikat (persisten, menggantikan array RAM lama).
-- Jalankan di Supabase Studio (SQL Editor) SEBELUM deploy versi baru.
-- Aman di-rerun (IF NOT EXISTS).

create table if not exists duplicate_queue (
  qid          bigserial primary key,
  file         text not null,
  tipe         text not null check (tipe in ('txt_dsn', 'txt_mhs', 'wav')),
  target_kelas text,
  payload      jsonb not null default '{}'::jsonb,  -- field tambahan: no_pertanyaan, classId, studentId, tempPath, dst.
  resolved_at  timestamptz,
  created_at   timestamptz not null default now()
);

-- Percepat query "yang belum diputuskan".
create index if not exists duplicate_queue_pending_idx
  on duplicate_queue (qid)
  where resolved_at is null;
