"use strict";

// Simpan URL audio (hasil upload) ke tabel questions/answers sesuai tipe
// file (DSN = pertanyaan dosen, MHS = jawaban mahasiswa).
// Dipindahkan apa adanya dari server.js (behavior-preserving).

const { sbSelect, sbInsert, sbUpdate } = require("../data/baseRepo");

async function simpanAudioKeDB(info, classId, audioUrl) {
  if (info.tipe === "dsn") {
    const qRows = await sbSelect("questions", {
      class_id: classId,
      number_q: info.no_pertanyaan,
    });
    if (qRows.length > 0) {
      const updateFields = { audio_file_path: audioUrl };
      if (info.device_id != null && qRows[0].device_id == null) {
        updateFields.device_id = info.device_id;
      }
      await sbUpdate(
        "questions",
        { question_id: qRows[0].question_id },
        updateFields,
      );
      console.log(
        `✅ [DSN WAV] audio_file_path disimpan di questions.id=${qRows[0].question_id}`,
      );
    } else {
      const newQ = await sbInsert("questions", {
        class_id: classId,
        device_id: info.device_id ?? null,
        number_q: info.no_pertanyaan,
        audio_file_path: audioUrl,
        transcript_text: "",
      });
      console.log(`✅ [DSN WAV] question baru dibuat id=${newQ.question_id}`);
    }
  } else if (info.tipe === "mhs") {
    let qRows = await sbSelect("questions", {
      class_id: classId,
      number_q: info.no_pertanyaan,
    });
    let qId;
    if (qRows.length > 0) {
      qId = qRows[0].question_id;
    } else {
      const newQ = await sbInsert("questions", {
        class_id: classId,
        device_id: info.device_id ?? null,
        number_q: info.no_pertanyaan,
        transcript_text: "",
      });
      qId = newQ.question_id;
      console.log(`✅ [MHS WAV] question baru dibuat id=${qId}`);
    }

    const aRows = await sbSelect("answers", {
      question_id: qId,
      number_a: info.no_jawaban,
    });
    if (aRows.length > 0) {
      await sbUpdate(
        "answers",
        { answer_id: aRows[0].answer_id },
        { audio_file_path: audioUrl },
      );
      console.log(
        `✅ [MHS WAV] audio_file_path disimpan di answers.id=${aRows[0].answer_id}`,
      );
    } else {
      const newA = await sbInsert("answers", {
        question_id: qId,
        transcript_text: "",
        audio_file_path: audioUrl,
        number_a: info.no_jawaban,
        class_id: classId,
      });
      console.log(`✅ [MHS WAV] answer baru dibuat id=${newA.answer_id}`);
    }
  }
}

module.exports = { simpanAudioKeDB };
