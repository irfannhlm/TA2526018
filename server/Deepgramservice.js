// ================= DEEPGRAM SERVICE =================
// Handles automatic speech-to-text transcription using Deepgram API.
// Supports both "on-demand" (called right after audio upload)
// and "background sweep" (checks DB periodically for untranscribed audio).

const { createClient } = require("@deepgram/sdk");
require("dotenv").config();

const deepgram = createClient(process.env.DEEPGRAM_KEY);

/**
 * Transkripsi satu URL audio menggunakan Deepgram.
 * @param {string} audioUrl - URL publik file audio (dari Supabase Storage)
 * @returns {string} - Teks hasil transkripsi, atau "" jika gagal
 */
async function transcribeAudio(audioUrl) {
  try {
    console.log(`🎙️  [Deepgram] Mulai transkripsi: ${audioUrl}`);

    const { result, error } = await deepgram.listen.prerecorded.transcribeUrl(
      { url: audioUrl },
      {
        model: "nova-2",
        language: "id", // Bahasa Indonesia
        smart_format: true,
        punctuate: true,
      },
    );

    if (error) {
      console.error("❌ [Deepgram] Error response:", error);
      return "";
    }

    const transcript =
      result?.results?.channels?.[0]?.alternatives?.[0]?.transcript ?? "";

    console.log(
      `✅ [Deepgram] Hasil: "${transcript.substring(0, 80)}${transcript.length > 80 ? "..." : ""}"`,
    );
    return transcript;
  } catch (err) {
    console.error("❌ [Deepgram] Exception:", err.message);
    return "";
  }
}

/**
 * Proses transkripsi untuk satu answer (jawaban mahasiswa).
 * Dipanggil langsung setelah audio tersimpan.
 *
 * @param {object} sbUpdate   - helper sbUpdate dari server.js
 * @param {number} answerId   - ID baris di tabel answers
 * @param {string} audioUrl   - URL audio publik
 */
async function transcribeAnswer(sbUpdate, answerId, audioUrl) {
  if (!audioUrl || audioUrl === "#") return;

  const transcript = await transcribeAudio(audioUrl);
  const textToSave = transcript || "[SILENT]";

  if (!transcript) {
    console.log(
      `🔇 [Deepgram] answers.id=${answerId} → tidak ada suara, ditandai [SILENT]`,
    );
  }

  try {
    await sbUpdate(
      "answers",
      { answer_id: answerId },
      { transcript_text: textToSave },
    );
    console.log(`✅ [Deepgram] answers.id=${answerId} → transcript disimpan`);
  } catch (err) {
    console.error(
      `❌ [Deepgram] Gagal simpan transcript answers.id=${answerId}:`,
      err.message,
    );
  }
}

/**
 * Proses transkripsi untuk satu question (pertanyaan dosen).
 * Dipanggil langsung setelah audio tersimpan.
 *
 * @param {object} sbUpdate     - helper sbUpdate dari server.js
 * @param {number} questionId   - ID baris di tabel questions
 * @param {string} audioUrl     - URL audio publik
 */
async function transcribeQuestion(sbUpdate, questionId, audioUrl) {
  if (!audioUrl || audioUrl === "#") return;

  const transcript = await transcribeAudio(audioUrl);
  const textToSave = transcript || "[SILENT]";

  if (!transcript) {
    console.log(
      `🔇 [Deepgram] questions.id=${questionId} → tidak ada suara, ditandai [SILENT]`,
    );
  }

  try {
    await sbUpdate(
      "questions",
      { question_id: questionId },
      { transcript_text: textToSave },
    );
    console.log(
      `✅ [Deepgram] questions.id=${questionId} → transcript disimpan`,
    );
  } catch (err) {
    console.error(
      `❌ [Deepgram] Gagal simpan transcript questions.id=${questionId}:`,
      err.message,
    );
  }
}

/**
 * Background sweep: cari semua baris answers/questions yang punya audio
 * tapi transcript_text masih kosong, lalu transkripsi satu per satu.
 *
 * Dipanggil oleh setInterval di server.js.
 *
 * @param {object} supabase   - supabase client dari server.js
 * @param {object} sbUpdate   - helper sbUpdate dari server.js
 */
async function sweepUntranscribed(supabase, sbUpdate) {
  console.log("🔍 [Deepgram Sweep] Mengecek audio yang belum ditranskripsi...");

  // ── Cek answers ──
  try {
    const { data: answers, error } = await supabase
      .from("answers")
      .select("answer_id, audio_file_path, transcript_text")
      .not("audio_file_path", "is", null)
      .neq("audio_file_path", "")
      .neq("audio_file_path", "#")
      .or("transcript_text.is.null,transcript_text.eq.");

    if (error) throw error;

    if (answers && answers.length > 0) {
      console.log(
        `📋 [Deepgram Sweep] Ditemukan ${answers.length} answers belum ada transcript`,
      );
      for (const row of answers) {
        await transcribeAnswer(sbUpdate, row.answer_id, row.audio_file_path);
        // Jeda kecil agar tidak membanjiri API
        await sleep(500);
      }
    } else {
      console.log("✅ [Deepgram Sweep] Semua answers sudah punya transcript");
    }
  } catch (err) {
    console.error("❌ [Deepgram Sweep] Gagal cek answers:", err.message);
  }

  // ── Cek questions ──
  try {
    const { data: questions, error } = await supabase
      .from("questions")
      .select("question_id, audio_file_path, transcript_text")
      .not("audio_file_path", "is", null)
      .neq("audio_file_path", "")
      .neq("audio_file_path", "#")
      .or("transcript_text.is.null,transcript_text.eq.");

    if (error) throw error;

    if (questions && questions.length > 0) {
      console.log(
        `📋 [Deepgram Sweep] Ditemukan ${questions.length} questions belum ada transcript`,
      );
      for (const row of questions) {
        await transcribeQuestion(
          sbUpdate,
          row.question_id,
          row.audio_file_path,
        );
        await sleep(500);
      }
    } else {
      console.log("✅ [Deepgram Sweep] Semua questions sudah punya transcript");
    }
  } catch (err) {
    console.error("❌ [Deepgram Sweep] Gagal cek questions:", err.message);
  }

  console.log("🏁 [Deepgram Sweep] Selesai\n");
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

module.exports = {
  transcribeAudio,
  transcribeAnswer,
  transcribeQuestion,
  sweepUntranscribed,
};
