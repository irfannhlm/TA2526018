"use strict";

// Topik: kelas/alat/audio_data — data TXT dari SD Card ESP32 (DSN/MHS),
// termasuk cek duplikat (ditahan di antrian) & ACK ke alat.
// ACK dikirim via publishCommand (QoS 1) supaya tidak hilang.
// `return;` di sini setara `return` dari callback handler lama.
const { publishCommand } = require("../publisher");
const { pushPending } = require("../../data/duplicateQueue.repo");

module.exports = async function audioData(payload, ctx) {
  const { sbSelect, sbInsert, sbUpdate, parseNamaFile } = ctx;

  const {
    file,
    target_kelas,
    no_pertanyaan,
    no_jawaban,
    uid,
    waktu_diam_ms,
    tanggal,
  } = payload;

  console.log(`\n📂 [SD SYNC TXT] File diterima: ${file}`);
  console.log(`   Kelas        : ${target_kelas}`);
  console.log(`   No Pertanyaan: ${no_pertanyaan}`);

  const info = parseNamaFile(file);
  if (!info) {
    console.warn(`⚠️ Format nama file tidak dikenal: ${file}`);
    publishCommand({ perintah: "ack_file", file });
    return;
  }

  try {
    const classes = await sbSelect("classes", { class_name: target_kelas });
    const classId = classes.length > 0 ? classes[0].class_id : null;

    if (!classId) {
      console.warn(`⚠️ Kelas "${target_kelas}" tidak ditemukan di database.`);
      publishCommand({ perintah: "ack_file", file });
      return;
    }

    if (info.tipe === "dsn") {
      // DSN_{device_id}_{no_pertanyaan}.txt dibaca ESP (2 baris):
      //   Baris 1 → no_pertanyaan (integer)
      //   Baris 2 → tanggal       (string, format DD-MM-YYYY)
      //
      // Payload MQTT yang dikirim ESP (dari bacaFileSdKeJson):
      //   {
      //     "file": "DSN_2_1.txt",
      //     "target_kelas": "...",
      //     "device_id": 2,
      //     "no_pertanyaan": 1,
      //     "tanggal": "13-05-2026",
      //     "uid": "DOSEN"
      //   }
      //
      // device_id dan number_q di DB diambil dari parseNamaFile(file)

      const numberQ = parseInt(no_pertanyaan);
      // BARU
      const rawTanggal = (tanggal ?? "").trim();
      const parsedTanggal = (() => {
        const parts = rawTanggal.split("-");
        if (parts.length === 3 && parts[0].length <= 2 && parts[2].length === 4)
          return `${parts[2]}-${parts[1].padStart(2, "0")}-${parts[0].padStart(2, "0")}`;
        return rawTanggal;
      })();

      console.log(`   number_q (dari payload) : ${numberQ}`);
      console.log(`   number_q (dari namafile): ${info.no_pertanyaan}`);
      console.log(`   Tanggal                 : ${parsedTanggal}`);

      // Validasi: kedua sumber number_q harus konsisten
      if (isNaN(numberQ)) {
        console.warn(
          `⚠️ [DSN TXT] Field 'no_pertanyaan' tidak valid di payload: ${no_pertanyaan}`,
        );
        publishCommand({ perintah: "ack_file", file });
        return;
      }
      if (!parsedTanggal) {
        console.warn(
          `⚠️ [DSN TXT] Field 'tanggal' kosong atau tidak ada di payload`,
        );
        publishCommand({ perintah: "ack_file", file });
        return;
      }
      if (numberQ !== info.no_pertanyaan) {
        console.warn(
          `⚠️ [DSN TXT] Inkonsistensi: no_pertanyaan payload (${numberQ}) ≠ nama file (${info.no_pertanyaan}). Menggunakan payload.`,
        );
      }

      const existing = await sbSelect("questions", {
        class_id: classId,
        number_q: numberQ,
      });

      // CEK DUPLIKAT: hanya jika date_id sudah terisi sebelumnya
      if (existing.length > 0 && existing[0].date_id) {
        const qid = await pushPending({
          file,
          tipe: "txt_dsn",
          target_kelas,
          no_pertanyaan: numberQ,
          tanggal: parsedTanggal,
          existingId: existing[0].question_id,
          existingPreview: `Pertanyaan #${numberQ} | Tanggal lama: ${existing[0].date_id} → baru: ${parsedTanggal}`,
        });
        console.log(
          `⏸️ [TXT DSN] Duplikat ditahan — menunggu keputusan user. qid=${qid}`,
        );
        return; // Tahan, tidak ACK
      }

      // Tidak duplikat — proses normal
      if (existing.length > 0) {
        // Update date_id (dan device_id jika belum terisi) pada pertanyaan yang sudah ada
        const updateFields = { date_id: parsedTanggal };
        if (info.device_id != null && existing[0].device_id == null) {
          updateFields.device_id = info.device_id;
        }
        await sbUpdate(
          "questions",
          { question_id: existing[0].question_id },
          updateFields,
        );
        console.log(
          `✅ [DSN TXT] questions.id=${existing[0].question_id} → date_id="${parsedTanggal}" diperbarui.`,
        );
      } else {
        // Buat question baru
        await sbInsert("questions", {
          class_id: classId,
          device_id: info.device_id ?? null,
          number_q: numberQ,
          date_id: parsedTanggal,
          transcript_text: "",
        });
        console.log(
          `✅ [DSN TXT] question baru dibuat: number_q=${numberQ}, date_id="${parsedTanggal}".`,
        );
      }
    } else if (info.tipe === "mhs") {
      console.log(`   No Jawaban   : ${no_jawaban}`);
      console.log(`   UID          : ${uid}`);
      console.log(`   Waktu Diam   : ${waktu_diam_ms} ms`);

      const students = await sbSelect("students", { rfid_uid: uid });
      const student = students.length > 0 ? students[0] : null;
      if (!student) console.warn(`⚠️ UID ${uid} tidak ditemukan di database.`);

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
      }

      const aRows = await sbSelect("answers", {
        question_id: qId,
        number_a: info.no_jawaban,
      });

      // CEK DUPLIKAT: jika sudah ada data jawaban
      if (
        aRows.length > 0 &&
        (aRows[0].student_id || aRows[0].duration_answer)
      ) {
        const qid = await pushPending({
          file,
          tipe: "txt_mhs",
          target_kelas,
          no_pertanyaan: info.no_pertanyaan,
          no_jawaban: info.no_jawaban,
          uid,
          waktu_diam_ms,
          classId,
          studentId: student ? student.student_id : null,
          existingAnswerId: aRows[0].answer_id,
          existingPreview: `Q${info.no_pertanyaan} A${info.no_jawaban} | ${student?.name || uid}`,
        });
        console.log(
          `⏸️ [TXT MHS] Duplikat ditahan — menunggu keputusan user. qid=${qid}`,
        );
        return; // Tahan, tidak ACK
      }

      // Tidak duplikat — proses normal
      if (aRows.length > 0) {
        await sbUpdate(
          "answers",
          { answer_id: aRows[0].answer_id },
          {
            student_id: student ? student.student_id : null,
            duration_answer: waktu_diam_ms / 1000,
            class_id: classId,
          },
        );
        console.log(
          `✅ [MHS TXT] answers.id=${aRows[0].answer_id} diperbarui.`,
        );
      } else {
        await sbInsert("answers", {
          question_id: qId,
          student_id: student ? student.student_id : null,
          transcript_text: "",
          duration_answer: waktu_diam_ms / 1000,
          number_a: info.no_jawaban,
          class_id: classId,
        });
        console.log(
          `✅ [MHS TXT] answer baru | Q${info.no_pertanyaan} A${info.no_jawaban} | student=${student?.name || "unknown"}`,
        );
      }
    }

    // ACK ke ESP hanya jika tidak duplikat
    await publishCommand({ perintah: "ack_file", file });
  } catch (dbErr) {
    console.error("❌ [SD SYNC TXT] Gagal simpan ke database:", dbErr.message);
  }
};
