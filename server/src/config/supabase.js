"use strict";

// Klien Supabase singleton. Logika pembentukan URL/key sama persis dengan
// yang sebelumnya inline di server.js (behavior-preserving).
// dotenv dipanggil di sini juga (idempoten) agar aman dari urutan require.
require("dotenv").config();
const { createClient } = require("@supabase/supabase-js");

const supabaseUrl = process.env.SUPABASE_URL.replace(
  /\/rest\/v1\/?$/,
  "",
).replace(/\/$/, "");
const supabaseKey = process.env.SUPABASE_KEY;
const supabase = createClient(supabaseUrl, supabaseKey);
console.log("✅ Supabase client dikonfigurasi:", supabaseUrl);

module.exports = { supabase };
