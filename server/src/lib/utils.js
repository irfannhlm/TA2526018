"use strict";

// Konversi nilai ke integer; kembalikan null bila kosong/invalid.
// Perilaku sama persis dengan helper toInt lama di server.js.
function toInt(val) {
  if (val === null || val === undefined || val === "") return null;
  const n = parseInt(val, 10);
  return isNaN(n) ? null : n;
}

module.exports = { toInt };
