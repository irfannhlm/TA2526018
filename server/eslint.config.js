"use strict";

// ESLint flat config (ESLint 10). Fokus higiene, bukan gaya
// (formatting diserahkan ke Prettier via eslint-config-prettier).
// no-undef dimatikan agar tak butuh paket `globals` (Node sudah
// menangkap ReferenceError saat runtime/test).

const prettier = require("eslint-config-prettier");

module.exports = [
  {
    ignores: [
      "node_modules/**",
      "public/**",
      "temp_audio/**",
      "views/**",
      "package-lock.json",
    ],
  },
  {
    files: ["**/*.js"],
    languageOptions: {
      ecmaVersion: 2023,
      sourceType: "commonjs",
    },
    rules: {
      "no-undef": "off",
      "no-unused-vars": [
        "warn",
        { argsIgnorePattern: "^_", varsIgnorePattern: "^_" },
      ],
      "no-empty": ["warn", { allowEmptyCatch: true }],
      "no-var": "warn",
      "prefer-const": "warn",
      eqeqeq: ["warn", "smart"],
    },
  },
  prettier,
];
