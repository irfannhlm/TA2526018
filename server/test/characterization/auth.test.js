"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { loadApp, hashPassword } = require("../helpers/loadApp");

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

test("GET / menampilkan halaman login (200)", async () => {
  const ctx = await loadApp();
  try {
    const res = await ctx.request("GET", "/");
    assert.equal(res.status, 200);
  } finally {
    await ctx.close();
  }
});

test("POST /login username tidak ada -> render login, tanpa sesi", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const res = await ctx.request("POST", "/login", {
      form: { username: "ghost", password: "x" },
    });
    assert.equal(res.status, 200);
    assert.equal(res.location, null);
  } finally {
    await ctx.close();
  }
});

test("POST /login password salah -> render login, tanpa redirect", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const res = await ctx.request("POST", "/login", {
      form: { username: "admin", password: "salah" },
    });
    assert.equal(res.status, 200);
    assert.equal(res.location, null);
  } finally {
    await ctx.close();
  }
});

test("POST /login admin benar -> 302 ke /admin + cookie sesi", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const res = await ctx.request("POST", "/login", {
      form: { username: "admin", password: "admin123" },
    });
    assert.equal(res.status, 302);
    assert.equal(res.location, "/admin");
    assert.ok(res.cookie, "harus set cookie sesi");
  } finally {
    await ctx.close();
  }
});

test("POST /login dosen benar -> 302 ke /pilih-kelas", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const res = await ctx.request("POST", "/login", {
      form: { username: "dosen1", password: "dosen123" },
    });
    assert.equal(res.status, 302);
    assert.equal(res.location, "/pilih-kelas");
  } finally {
    await ctx.close();
  }
});

test("requireLogin: /pilih-kelas tanpa sesi -> 302 ke /", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const res = await ctx.request("GET", "/pilih-kelas");
    assert.equal(res.status, 302);
    assert.equal(res.location, "/");
  } finally {
    await ctx.close();
  }
});

test("requireRole: /admin sebagai dosen -> 302 ke /", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");
    const res = await ctx.request("GET", "/admin", { cookie });
    assert.equal(res.status, 302);
    assert.equal(res.location, "/");
  } finally {
    await ctx.close();
  }
});

test("requireRole: /admin sebagai admin -> 200", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    const res = await ctx.request("GET", "/admin", { cookie });
    assert.equal(res.status, 200);
  } finally {
    await ctx.close();
  }
});

test("POST /logout -> 302 ke /", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const cookie = await ctx.loginAs("admin", "admin123");
    const res = await ctx.request("POST", "/logout", { cookie });
    assert.equal(res.status, 302);
    assert.equal(res.location, "/");
  } finally {
    await ctx.close();
  }
});

test("TC-AUTH-10 rate limit: percobaan ke-11 -> 429 + pesan tunggu", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    let last;
    for (let i = 0; i < 11; i++) {
      last = await ctx.request("POST", "/login", {
        form: { username: "admin", password: "salah" },
      });
    }
    assert.equal(last.status, 429);
    assert.match(last.text, /Coba lagi dalam/);
  } finally {
    await ctx.close();
  }
});

test("TC-AUTH-11 login sukses -> cookie sesi httpOnly", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const res = await ctx.request("POST", "/login", {
      form: { username: "admin", password: "admin123" },
    });
    const setCookie = res.headers.get("set-cookie");
    assert.ok(setCookie, "harus ada set-cookie");
    assert.match(setCookie, /HttpOnly/i);
  } finally {
    await ctx.close();
  }
});

test("TC-AUTH-12 /dosen tanpa ?kelas -> redirect /pilih-kelas", async () => {
  const ctx = await loadApp({ seed: seedUsers });
  try {
    const cookie = await ctx.loginAs("dosen1", "dosen123");
    const res = await ctx.request("GET", "/dosen", { cookie });
    assert.equal(res.status, 302);
    assert.equal(res.location, "/pilih-kelas");
  } finally {
    await ctx.close();
  }
});
