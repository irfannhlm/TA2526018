"use strict";

// Topik: kelas/alat/status — update status & level baterai device.
// Logika sama persis dengan blok lama di server.js (behavior-preserving).
module.exports = async function deviceStatus(payload, ctx) {
  const { sbUpdate } = ctx;

  const { device_id, status, battery } = payload;
  const updated = await sbUpdate(
    "devices",
    { device_id },
    { status, battery_level: battery },
  );
  if (!updated.length)
    console.log(`⚠️ GAGAL: Tidak ada device dengan ID ${device_id}`);
  else console.log(`✅ BERHASIL: Status ${device_id} sekarang ${status}`);
};
