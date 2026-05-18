"use strict";

// Middleware autentikasi/otorisasi berbasis sesi.
// Dipindahkan apa adanya dari server.js (behavior-preserving).

function requireLogin(req, res, next) {
  if (!req.session.user) return res.redirect("/");
  next();
}

function requireRole(...roles) {
  return (req, res, next) => {
    if (!req.session.user) return res.redirect("/");
    if (!roles.includes(req.session.user.role)) return res.redirect("/");
    next();
  };
}

module.exports = { requireLogin, requireRole };
