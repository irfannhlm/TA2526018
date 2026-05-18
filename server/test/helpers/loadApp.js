"use strict";

// Memuat server.js secara terisolasi untuk characterization test:
//  - set ENV dummy sebelum require (server.js butuh SUPABASE_URL dll.)
//  - inject mock @supabase/supabase-js, mqtt, ./Deepgramservice ke require.cache
//  - fresh-require server.js tiap pemanggilan (state RAM modul ter-reset)
//  - listen di port acak, kembalikan baseUrl + helper request

const path = require("path");
const bcrypt = require("bcrypt");
const { FakeSupabase } = require("./fakeSupabase");
const { makeFakeMqttModule } = require("./fakeMqtt");

const SERVER_DIR = path.join(__dirname, "..", "..");
const SERVER_PATH = path.join(SERVER_DIR, "server.js");
const DEEPGRAM_PATH = path.join(SERVER_DIR, "Deepgramservice.js");

function resolveFrom(spec) {
  return require.resolve(spec, { paths: [SERVER_DIR] });
}

const TEST_DIR = path.join(SERVER_DIR, "test");
function purgeServerCache() {
  for (const id of Object.keys(require.cache)) {
    if (
      id.startsWith(SERVER_DIR) &&
      !id.includes("node_modules") &&
      !id.startsWith(TEST_DIR)
    ) {
      delete require.cache[id];
    }
  }
}

function injectModule(id, exports) {
  require.cache[id] = {
    id,
    filename: id,
    loaded: true,
    exports,
    children: [],
    paths: [],
  };
}

// server.js sangat "cerewet" (console.log di mana-mana). Saat dijalankan
// sebagai child dari `node --test`, output itu mengganggu protokol stdout
// test runner ("Unable to deserialize cloned data"). Jadi console diredam
// selama test berjalan dan dipulihkan saat close().
function makeQuietConsole() {
  const orig = {};
  for (const m of ["log", "info", "debug", "warn", "error"]) {
    orig[m] = console[m];
    console[m] = () => {};
  }
  return () => Object.assign(console, orig);
}

/**
 * @param {object} [opts]
 * @param {(supabase: FakeSupabase) => void} [opts.seed] isi data awal
 * @param {boolean} [opts.quiet=true] redam console.log saat load
 */
async function loadApp(opts = {}) {
  const quiet = opts.quiet !== false;

  process.env.SUPABASE_URL = "https://example.supabase.co";
  process.env.SUPABASE_KEY = "test-key";
  process.env.SESSION_SECRET = "test-secret";
  process.env.DEEPGRAM_KEY = "test-deepgram";

  const supabase = new FakeSupabase();
  if (typeof opts.seed === "function") opts.seed(supabase);

  const { module: mqttModule, client: mqttClient } = makeFakeMqttModule();

  const deepgramCalls = [];
  const deepgramStub = {
    transcribeAudio: async () => "",
    transcribeAnswer: async (_sbUpdate, answerId, url) => {
      deepgramCalls.push({ kind: "answer", answerId, url });
    },
    transcribeQuestion: async (_sbUpdate, questionId, url) => {
      deepgramCalls.push({ kind: "question", questionId, url });
    },
    sweepUntranscribed: async () => {
      deepgramCalls.push({ kind: "sweep" });
    },
  };

  // Bersihkan cache SEMUA modul sumber server/ (server.js + src/*) agar
  // state modul (sessionData, duplicateQueue, client singleton, dll.) fresh
  // tiap test. node_modules & file test sengaja tidak disentuh.
  purgeServerCache();

  injectModule(resolveFrom("@supabase/supabase-js"), {
    createClient: () => supabase,
  });
  injectModule(resolveFrom("mqtt"), mqttModule);
  injectModule(require.resolve(DEEPGRAM_PATH), deepgramStub);

  const restoreConsole = quiet ? makeQuietConsole() : () => {};
  let serverModule;
  try {
    serverModule = require(SERVER_PATH);
  } catch (e) {
    restoreConsole();
    throw e;
  }

  const app = serverModule.app;
  const server = app.listen(0);
  await new Promise((resolve) => server.once("listening", resolve));
  const port = server.address().port;
  const baseUrl = `http://127.0.0.1:${port}`;

  async function request(
    method,
    urlPath,
    { body, headers, cookie, form } = {},
  ) {
    const h = Object.assign({}, headers);
    let payload;
    if (form) {
      h["content-type"] = "application/x-www-form-urlencoded";
      payload = new URLSearchParams(form).toString();
    } else if (body !== undefined) {
      h["content-type"] = "application/json";
      payload = JSON.stringify(body);
    }
    if (cookie) h["cookie"] = cookie;
    const res = await fetch(baseUrl + urlPath, {
      method,
      headers: h,
      body: payload,
      redirect: "manual",
    });
    const text = await res.text();
    const setCookie = res.headers.get("set-cookie");
    return {
      status: res.status,
      headers: res.headers,
      location: res.headers.get("location"),
      cookie: setCookie ? setCookie.split(";")[0] : null,
      text,
      json: () => JSON.parse(text),
    };
  }

  async function requestMultipart(
    method,
    urlPath,
    { fields = {}, file, cookie } = {},
  ) {
    const fd = new FormData();
    for (const [k, v] of Object.entries(fields)) fd.append(k, String(v));
    if (file) {
      const blob = new Blob([file.buffer || Buffer.from("RIFFfakeWAVdata")], {
        type: file.type || "audio/wav",
      });
      fd.append(file.field || "audio", blob, file.filename || "audio.wav");
    }
    const res = await fetch(baseUrl + urlPath, {
      method,
      headers: cookie ? { cookie } : {},
      body: fd,
      redirect: "manual",
    });
    const text = await res.text();
    return {
      status: res.status,
      location: res.headers.get("location"),
      text,
      json: () => JSON.parse(text),
    };
  }

  /** Login dan kembalikan cookie sesi. */
  async function loginAs(username, password) {
    const res = await request("POST", "/login", {
      form: { username, password },
    });
    return res.cookie;
  }

  async function close() {
    mqttClient.end();
    await new Promise((resolve) => server.close(resolve));
    purgeServerCache();
    restoreConsole();
  }

  return {
    app,
    server,
    baseUrl,
    supabase,
    mqtt: mqttClient,
    deepgramCalls,
    request,
    requestMultipart,
    loginAs,
    close,
  };
}

/** Hash password untuk seeding tabel users. */
function hashPassword(pw) {
  return bcrypt.hashSync(pw, 10);
}

module.exports = { loadApp, hashPassword };
