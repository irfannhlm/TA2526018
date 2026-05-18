"use strict";

// Topik: kelas/alat/sync_status — update state.syncStatus agar bisa
// dipolling frontend. Logika sama persis dengan blok lama di server.js
// (behavior-preserving).
module.exports = async function syncStatus(payload, ctx) {
  const { state } = ctx;

  const { status, pesan, total, berhasil } = payload;
  const timestamp = new Date().toLocaleTimeString("id-ID");

  // Update state.syncStatus agar bisa di-polling frontend
  state.syncStatus = {
    state:
      status === "selesai"
        ? "done"
        : status === "error"
          ? "error"
          : status === "dibatalkan"
            ? "cancelled"
            : status === "progress"
              ? "loading"
              : "idle",
    pesan: pesan || "",
    total: total ?? 0,
    berhasil: berhasil ?? 0,
    updatedAt: Date.now(),
  };

  if (status === "selesai") {
    console.log(`\n🎉 [SD SYNC] SELESAI [${timestamp}]`);
    console.log(`   Total : ${total} file`);
    console.log(`   Sukses: ${berhasil} file`);
    console.log(`   Pesan : ${pesan}`);
  } else if (status === "error") {
    console.warn(`\n⚠️ [SD SYNC] ERROR [${timestamp}]: ${pesan}`);
    console.warn(`   Progress: ${berhasil}/${total}`);
  } else if (status === "dibatalkan") {
    console.log(`\n🛑 [SD SYNC] DIBATALKAN [${timestamp}]: ${pesan}`);
  } else if (status === "progress") {
    console.log(`\n🔄 [SD SYNC] Progress: ${berhasil}/${total} — ${pesan}`);
  }
};
