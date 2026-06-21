"use strict";

// ================= REQUEST LOGGER =================
// Logger HTTP ringan tanpa dependency (mirip morgan, tapi seadanya).
// Mencatat tiap request ke terminal saat response selesai: metode, URL,
// status (berwarna), dan durasi. Aset statis (/css, /js, /images, favicon)
// dilewati agar log tidak penuh oleh request remeh.

// Kode warna ANSI — dimatikan otomatis bila output bukan TTY (mis. di file
// log atau saat dijalankan oleh platform hosting).
const useColor = process.stdout.isTTY;
const c = (code, s) => (useColor ? `\x1b[${code}m${s}\x1b[0m` : String(s));

// Warnai status sesuai kelasnya: 2xx hijau, 3xx cyan, 4xx kuning, 5xx merah.
function colorStatus(status) {
  if (status >= 500) return c(31, status); // merah
  if (status >= 400) return c(33, status); // kuning
  if (status >= 300) return c(36, status); // cyan
  return c(32, status); // hijau
}

// Lewati request aset statis & favicon supaya terminal tetap terbaca.
function isAsset(url) {
  return (
    url === "/favicon.ico" ||
    /^\/(css|js|images|public|assets)\//.test(url) ||
    /\.(css|js|png|jpe?g|gif|svg|ico|woff2?|map)(\?|$)/.test(url)
  );
}

function requestLogger(req, res, next) {
  if (isAsset(req.originalUrl)) return next();

  const start = process.hrtime.bigint();
  res.on("finish", () => {
    const ms = Number(process.hrtime.bigint() - start) / 1e6;
    const stamp = new Date().toISOString();
    console.log(
      `${c(90, stamp)} ${c(1, req.method.padEnd(4))} ${req.originalUrl} ` +
        `${colorStatus(res.statusCode)} ${c(90, ms.toFixed(1) + "ms")}`,
    );
  });
  next();
}

module.exports = requestLogger;
