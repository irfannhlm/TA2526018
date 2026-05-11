// migrate-password.js
const bcrypt = require("bcrypt");
const { createClient } = require("@supabase/supabase-js");
require("dotenv").config();

const supabaseUrl = process.env.SUPABASE_URL.replace(/\/rest\/v1\/?$/, "").replace(/\/$/, "");
const supabase = createClient(supabaseUrl, process.env.SUPABASE_KEY);

const SALT_ROUNDS = 10;

async function migratePasswords() {
  console.log("🔄 Mengambil semua user dari Supabase...");
  const { data: users, error } = await supabase.from("users").select("user_id, password");
  if (error) { console.error("❌ Gagal ambil data:", error.message); process.exit(1); }

  console.log(`📋 Total user ditemukan: ${users.length}`);

  for (const user of users) {
    // Lewati yang sudah di-hash bcrypt
    if (user.password && user.password.startsWith("$2b$")) {
      console.log(`⏭️  User ${user.user_id} sudah di-hash, dilewati.`);
      continue;
    }

    const hashed = await bcrypt.hash(user.password, SALT_ROUNDS);
    const { error: updateError } = await supabase
      .from("users")
      .update({ password: hashed })
      .eq("user_id", user.user_id);

    if (updateError) console.error(`❌ Gagal update user ${user.user_id}:`, updateError.message);
    else console.log(`✅ User ${user.user_id} berhasil di-hash.`);
  }

  console.log("\n🎉 Migrasi selesai!");
  process.exit();
}

migratePasswords();