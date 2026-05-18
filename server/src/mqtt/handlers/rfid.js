"use strict";

// Topik: kelas/alat/rfid — tap kartu RFID: catat ke daftar scan sesi,
// lalu (bila terdaftar & punya kelas) buat log diskusi (question + answer).
// Logika sama persis dengan blok lama di server.js (behavior-preserving).
module.exports = async function rfid(payload, ctx) {
  const { sbSelect, sbInsert, state } = ctx;

  const { uid, action } = payload;
  if (action === "tap_rfid" && uid) {
    console.log(`📝 RFID Tap: ${uid}`);
    state.scanCounter++;
    state.sessionData.scannedList.unshift({
      id: state.scanCounter,
      uid,
      time: new Date().toLocaleTimeString(),
    });

    const students = await sbSelect("students", { rfid_uid: uid });
    if (students.length > 0) {
      const student = students[0];
      const classRel = await sbSelect("class_students", {
        student_id: student.student_id,
      });
      if (classRel.length > 0) {
        const classId = classRel[0].class_id;
        const questions = await sbSelect(
          "questions",
          { class_id: classId },
          "*",
          { order: { col: "created_at", asc: false }, limit: 1 },
        );
        let qId = questions.length > 0 ? questions[0].question_id : null;
        if (!qId) {
          const newQ = await sbInsert("questions", {
            class_id: classId,
            device_id: null,
            transcript_text: "Pertanyaan baru",
          });
          qId = newQ.question_id;
        }
        await sbInsert("answers", {
          question_id: qId,
          student_id: student.student_id,
          transcript_text: "",
          class_id: classId,
        });
        console.log(`✅ Log diskusi tersimpan untuk: ${student.name}`);
      }
    }
  }
};
