"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { loadApp, hashPassword } = require("../helpers/loadApp");

const PERINTAH = "kelas/alat/perintah";

function seedUsers(sb) {
  sb.seed("users", [
    {
      user_id: 1,
      username: "admin",
      password: hashPassword("admin123"),
      role: "admin",
    },
    {
      user_id: 2,
      username: "dosen1",
      password: hashPassword("dosen123"),
      role: "dosen",
    },
  ]);
}

function seedKelas(sb) {
  seedUsers(sb);
  sb.seed("classes", [
    {
      class_id: 10,
      class_name: "K1",
      class_code: "C1",
      lecturer_name: "Pak A",
      lecturer_user_id: 2,
    },
  ]);
  sb.seed("devices", [
    { device_id: 1, name: "Alat-1", status: "offline", battery_level: null },
  ]);
  sb.seed("students", [
    { student_id: 1, name: "Budi", nim: "111", rfid_uid: "U1" },
  ]);
  sb.seed("class_students", [
    { class_student_id: 1, student_id: 1, class_id: 10 },
  ]);
}

// ---------- Page smoke ----------
test("/pilih-kelas sebagai dosen -> 200", async () => {
  const ctx = await loadApp({ seed: seedKelas });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");
    const res = await ctx.request("GET", "/pilih-kelas", { cookie });
    assert.equal(res.status, 200);
  } finally {
    await ctx.close();
  }
});

test("/admin?kelas=K1 sebagai admin -> 200", async () => {
  const ctx = await loadApp({ seed: seedKelas });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    const res = await ctx.request("GET", "/admin?kelas=K1&device=1", {
      cookie,
    });
    assert.equal(res.status, 200);
  } finally {
    await ctx.close();
  }
});

test("/dosen?kelas=K1 sebagai dosen -> 200", async () => {
  const ctx = await loadApp({ seed: seedKelas });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");
    const res = await ctx.request("GET", "/dosen?kelas=K1&device=1", {
      cookie,
    });
    assert.equal(res.status, 200);
  } finally {
    await ctx.close();
  }
});

// ---------- Admin mutations ----------
test("/admin/add-class -> insert kelas + redirect", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    const res = await ctx.request("POST", "/admin/add-class", {
      cookie,
      form: {
        name: "KelasBaru",
        code: "KB",
        lecturer: "Bu X",
        lecturer_user_id: "2",
      },
    });
    assert.equal(res.status, 302);
    const cls = ctx.supabase
      .rows("classes")
      .find((c) => c.class_name === "KelasBaru");
    assert.ok(cls);
    assert.equal(cls.lecturer_user_id, 2);
  } finally {
    await ctx.close();
  }
});

test("/admin/add + /admin/delete mahasiswa", async () => {
  const ctx = await loadApp({ seed: seedKelas });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    await ctx.request("POST", "/admin/add", {
      cookie,
      form: { name: "Sari", nim: "222", rfid: "U2", current_class: "K1" },
    });
    const sari = ctx.supabase.rows("students").find((s) => s.nim === "222");
    assert.ok(sari);
    assert.equal(
      ctx.supabase
        .rows("class_students")
        .filter((cs) => cs.student_id === sari.student_id).length,
      1,
    );

    await ctx.request("POST", "/admin/delete", {
      cookie,
      form: { id: String(sari.student_id), current_class: "K1" },
    });
    assert.equal(
      ctx.supabase
        .rows("students")
        .some((s) => s.student_id === sari.student_id),
      false,
    );
  } finally {
    await ctx.close();
  }
});

test("/admin/add-user -> password ter-hash; /admin/delete-user guard user_id 1", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    await ctx.request("POST", "/admin/add-user", {
      cookie,
      form: { username: "dosen2", password: "rahasia8", role: "dosen" },
    });
    const u = ctx.supabase.rows("users").find((x) => x.username === "dosen2");
    assert.ok(u);
    assert.notEqual(u.password, "rahasia8", "password harus di-hash");

    // user_id 1 dilindungi (tidak terhapus)
    await ctx.request("POST", "/admin/delete-user", {
      cookie,
      form: { id: "1", current_class: "" },
    });
    assert.ok(ctx.supabase.rows("users").some((x) => x.user_id === 1));

    // user lain bisa dihapus
    await ctx.request("POST", "/admin/delete-user", {
      cookie,
      form: { id: String(u.user_id), current_class: "" },
    });
    assert.equal(
      ctx.supabase.rows("users").some((x) => x.user_id === u.user_id),
      false,
    );
  } finally {
    await ctx.close();
  }
});

// ---------- Dosen actions ----------
test("/dosen/update-settings: timer -> set_timer, threshold -> set_threshold", async () => {
  const ctx = await loadApp({ seed: seedKelas });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");
    await ctx.request("POST", "/dosen/update-settings", {
      cookie,
      form: { timer: "10", current_class: "K1" },
    });
    let p = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(p.payload.perintah, "set_timer");
    assert.equal(p.payload.durasi_detik, 10);

    await ctx.request("POST", "/dosen/update-settings", {
      cookie,
      form: { threshold: "bising", current_class: "K1" },
    });
    p = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(p.payload.perintah, "set_threshold");
    assert.equal(p.payload.nilai, 400);

    // Threshold custom: nilai diambil dari threshold_custom
    await ctx.request("POST", "/dosen/update-settings", {
      cookie,
      form: { threshold: "custom", threshold_custom: "555", current_class: "K1" },
    });
    p = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(p.payload.perintah, "set_threshold");
    assert.equal(p.payload.nilai, 555);
  } finally {
    await ctx.close();
  }
});

test("/dosen/sync-uid semua_kelas -> publish sync_uid_kelas", async () => {
  const ctx = await loadApp({ seed: seedKelas });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");
    const res = await ctx.request("POST", "/dosen/sync-uid", {
      cookie,
      form: { current_class: "K1", mode: "semua_kelas" },
    });
    assert.equal(res.json().success, true);
    const p = ctx.mqtt.lastPublished(PERINTAH);
    assert.equal(p.payload.perintah, "sync_uid_kelas");
    assert.deepEqual(p.payload.uids, ["U1"]);
  } finally {
    await ctx.close();
  }
});

test("/dosen/request-wav-sync -> publish request_sync_wav", async () => {
  const ctx = await loadApp({ seed: seedKelas });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");
    const res = await ctx.request("POST", "/dosen/request-wav-sync", {
      cookie,
      form: { current_class: "K1" },
    });
    assert.equal(res.json().success, true);
    assert.equal(
      ctx.mqtt.lastPublished(PERINTAH).payload.perintah,
      "request_sync_wav",
    );
  } finally {
    await ctx.close();
  }
});

test("/dosen/answer/:id/nilai validasi 1-100", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedUsers(sb);
      sb.seed("classes", [
        { class_id: 10, class_name: "K1", lecturer_user_id: 2 },
      ]);
      sb.seed("questions", [{ question_id: 1, class_id: 10 }]);
      sb.seed("answers", [
        {
          answer_id: 3,
          question_id: 1,
          class_id: 10,
          transcript_text: "",
        },
      ]);
    },
  });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");

    let res = await ctx.request("PATCH", "/dosen/answer/3/nilai", {
      cookie,
      body: { nilai: 80 },
    });
    assert.equal(res.json().ok, true);
    assert.equal(
      ctx.supabase.rows("answers").find((a) => a.answer_id === 3).nilai,
      80,
    );

    res = await ctx.request("PATCH", "/dosen/answer/3/nilai", {
      cookie,
      body: { nilai: 0 },
    });
    assert.equal(res.status, 400);

    res = await ctx.request("PATCH", "/dosen/answer/3/nilai", {
      cookie,
      body: { nilai: "" },
    });
    assert.equal(res.json().nilai, null);
  } finally {
    await ctx.close();
  }
});

test("/dosen/question/:id DELETE -> hapus question + answers terkait", async () => {
  const ctx = await loadApp({
    seed(sb) {
      seedUsers(sb);
      sb.seed("classes", [
        { class_id: 10, class_name: "K1", lecturer_user_id: 2 },
      ]);
      sb.seed("questions", [{ question_id: 5, class_id: 10 }]);
      sb.seed("answers", [
        { answer_id: 1, question_id: 5 },
        { answer_id: 2, question_id: 5 },
      ]);
    },
  });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");
    const res = await ctx.request("DELETE", "/dosen/question/5", { cookie });
    assert.equal(res.json().ok, true);
    assert.equal(ctx.supabase.rows("questions").length, 0);
    assert.equal(ctx.supabase.rows("answers").length, 0);
  } finally {
    await ctx.close();
  }
});

test("Validasi: /admin/add-user password < 8 -> 400", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    const res = await ctx.request("POST", "/admin/add-user", {
      cookie,
      form: { username: "x", password: "short", role: "dosen" },
    });
    assert.equal(res.status, 400);
    assert.equal(
      ctx.supabase.rows("users").some((u) => u.username === "x"),
      false,
    );
  } finally {
    await ctx.close();
  }
});

test("Ownership: dosen2 PATCH answer kelas dosen1 -> 403", async () => {
  const ctx = await loadApp({
    seed(sb) {
      // dosen1 = user 2 punya kelas K1 (class_id 10); dosen2 = user 3
      // tidak punya kelas. Coba PATCH nilai answer milik K1 dari dosen2.
      sb.seed("users", [
        {
          user_id: 1,
          username: "admin",
          password: hashPassword("admin123"),
          role: "admin",
        },
        {
          user_id: 2,
          username: "dosen1",
          password: hashPassword("dosen123"),
          role: "dosen",
        },
        {
          user_id: 3,
          username: "dosen2",
          password: hashPassword("dosen123"),
          role: "dosen",
        },
      ]);
      sb.seed("classes", [
        { class_id: 10, class_name: "K1", lecturer_user_id: 2 },
      ]);
      sb.seed("questions", [{ question_id: 1, class_id: 10 }]);
      sb.seed("answers", [
        { answer_id: 7, question_id: 1, class_id: 10, transcript_text: "" },
      ]);
    },
  });
  try {
    const cookie = await ctx.loginAs("dosen2", "dosen123");
    const res = await ctx.request("PATCH", "/dosen/answer/7/nilai", {
      cookie,
      body: { nilai: 80 },
    });
    assert.equal(res.status, 403);
    // Pastikan tidak ada nilai terupdate
    assert.equal(
      ctx.supabase.rows("answers").find((a) => a.answer_id === 7).nilai,
      undefined,
    );
  } finally {
    await ctx.close();
  }
});
