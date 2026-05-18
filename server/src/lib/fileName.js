"use strict";

// Parse nama file dari ESP. Dipindahkan apa adanya dari server.js
// (behavior-preserving). Mengembalikan null bila format tak dikenal.
function parseNamaFile(namaFile) {
  const dsnMatch = namaFile.match(/^DSN_(\d+)_(\d+)\.(wav|txt)$/i);
  if (dsnMatch) {
    return {
      tipe: "dsn",
      device_id: parseInt(dsnMatch[1]),
      no_pertanyaan: parseInt(dsnMatch[2]),
    };
  }
  const mhsMatch = namaFile.match(/^MHS_(\d+)_(\d+)_(\d+)\.(wav|txt)$/i);
  if (mhsMatch) {
    return {
      tipe: "mhs",
      device_id: parseInt(mhsMatch[1]),
      no_pertanyaan: parseInt(mhsMatch[2]),
      no_jawaban: parseInt(mhsMatch[3]),
    };
  }
  return null;
}
const parseNamaAudio = parseNamaFile;

module.exports = { parseNamaFile, parseNamaAudio };
