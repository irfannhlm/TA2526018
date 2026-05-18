"use strict";

// Bungkus handler async agar error otomatis diteruskan ke error-middleware
// terpusat (tidak perlu try/catch di tiap route).
module.exports = function asyncHandler(fn) {
  return (req, res, next) => Promise.resolve(fn(req, res, next)).catch(next);
};
