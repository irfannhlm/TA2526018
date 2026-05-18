"use strict";

// Upload file audio ke Supabase Storage lalu kembalikan URL publiknya.
// Dipindahkan apa adanya dari server.js (behavior-preserving).

const fs = require("fs");
const { supabase } = require("../config/supabase");
const { STORAGE_BUCKET } = require("../lib/constants");

async function uploadToSupabaseStorage(localFilePath, storagePath) {
  const fileBuffer = fs.readFileSync(localFilePath);
  const { data, error } = await supabase.storage
    .from(STORAGE_BUCKET)
    .upload(storagePath, fileBuffer, {
      contentType: "audio/wav",
      upsert: true,
    });
  if (error) throw error;

  const { data: urlData } = supabase.storage
    .from(STORAGE_BUCKET)
    .getPublicUrl(storagePath);
  return urlData.publicUrl;
}

module.exports = { uploadToSupabaseStorage };
