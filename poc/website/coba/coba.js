const express = require("express");
const app = express();
const path = require("path");

app.set("view engine", "ejs");
app.set("views", path.join(__dirname, "views"));
app.use(express.urlencoded({ extended: true }));

// --- 1. DATA USER ---
const users = [
  { id: 1, username: "admin", password: "123", role: "admin" },
  { id: 2, username: "dosen", password: "123", role: "dosen" },
];

// --- 2. DATA MAHASISWA ---
let students = [
  { id: 1, name: "Ryan Reynolds", nim: "13222001", rfid: "A1-B2" },
  { id: 2, name: "Michael Cliffort", nim: "13222002", rfid: "X9-Y8" },
  { id: 3, name: "Emma Watson", nim: "13222003", rfid: "C3-D4" },
  { id: 4, name: "Tom Holland", nim: "13222004", rfid: "E5-F6" },
  { id: 5, name: "Robert Downey Jr", nim: "13222005", rfid: "G7-H8" },
  { id: 6, name: "Chris Evans", nim: "13222006", rfid: "I9-J0" },
  { id: 7, name: "Scarlett Johansson", nim: "13222007", rfid: "K1-L2" },
  { id: 8, name: "Chris Hemsworth", nim: "13222008", rfid: "M3-N4" },
];

// --- 3. DATA SESI ---
let sessionData = {
  status: "stopped",
  // mode: "dosen_rec", <--- FITUR DIHAPUS
  battery: 85,
  maxTime: 60,
  scannedList: [],
};

// --- 4. GENERATE LOG DUMMY ---
let activityLogs = [];

const questionsBank = [
  "Jelaskan prinsip kerja sensor LDR dalam rangkaian pembagi tegangan!",
  "Bagaimana cara mengatasi noise pada sinyal analog?",
  "Apa perbedaan utama antara Mikrokontroler dan Mikroprosesor?",
  "Mengapa kita perlu menggunakan resistor pull-up pada tombol?",
];

const answersBank = [
  "Sensor LDR merubah resistansi berdasarkan intensitas cahaya, Pak.",
  "Semakin terang cahaya, resistansi LDR semakin kecil.",
  "Kita bisa menggunakan kapasitor sebagai filter frekuensi tinggi.",
  "Mikrokontroler punya RAM dan ROM internal.",
  "Agar pin tidak floating saat tombol tidak ditekan.",
  "Untuk menjaga logika HIGH saat kondisi idle.",
];

function generateGroupedLogs() {
  const today = new Date();
  const yesterday = new Date();
  yesterday.setDate(yesterday.getDate() - 1);

  questionsBank.forEach((q, index) => {
    let targetDate = index < 2 ? today : yesterday;
    let numAnswers = Math.floor(Math.random() * 3) + 2;

    for (let i = 0; i < numAnswers; i++) {
      let student = students[Math.floor(Math.random() * students.length)];
      let randomAnswer =
        answersBank[Math.floor(Math.random() * answersBank.length)];
      let logTime = new Date(targetDate);
      logTime.setHours(9 + index, i * 5, 0);

      // LOGIKA AUDIO: 50% kemungkinan audio belum diupload (null)
      let hasAudio = Math.random() > 0.5;

      activityLogs.push({
        id: Date.now() + Math.random(),
        dateObj: logTime,
        name: student.name,
        nim: student.nim,
        question: q,
        transcript: randomAnswer,
        audio_url: hasAudio ? "#" : null, // Jika null berarti file belum ada
      });
    }
  });
  activityLogs.sort((a, b) => {
    const dateA = new Date(a.dateObj).setHours(0, 0, 0, 0);
    const dateB = new Date(b.dateObj).setHours(0, 0, 0, 0);
    if (dateA !== dateB) return dateB - dateA;
    if (a.question < b.question) return -1;
    if (a.question > b.question) return 1;
    return a.dateObj - b.dateObj;
  });
}
generateGroupedLogs();

// --- ROUTES ---

app.get("/", (req, res) => res.render("login", { error: null }));

app.post("/login", (req, res) => {
  const { username, password } = req.body;
  const user = users.find((u) => u.username === username);
  if (!user) return res.render("login", { error: "Username tidak ditemukan." });
  if (user.password !== password)
    return res.render("login", { error: "Password salah." });
  if (user.role === "admin") res.redirect("/admin");
  else res.redirect("/dosen");
});

app.get("/admin", (req, res) => {
  res.render("admin", { students: students, logs: activityLogs, users: users });
});

// Route Admin CRUD (Disingkat)
app.post("/admin/add", (req, res) => {
  students.push({ id: Date.now(), ...req.body });
  res.redirect("/admin");
});
app.post("/admin/delete", (req, res) => {
  students = students.filter((s) => s.id != req.body.id);
  res.redirect("/admin");
});
app.post("/admin/edit", (req, res) => {
  res.redirect("/admin");
});
app.post("/admin/add-user", (req, res) => {
  users.push({ id: Date.now(), ...req.body });
  res.redirect("/admin");
});
app.post("/admin/delete-user", (req, res) => {
  users = users.filter((u) => u.id != req.body.id);
  res.redirect("/admin");
});

app.get("/dosen", (req, res) => {
  const mappedSessionList = sessionData.scannedList.map((item) => {
    const student = students.find((s) => s.rfid === item.uid);
    return {
      uid: item.uid,
      name: student ? student.name : "Unknown Device",
      nim: student ? student.nim : "-",
      isRegistered: !!student,
    };
  });

  let stats = students.map((s) => {
    const count = activityLogs.filter((l) => l.nim === s.nim).length;
    return { name: s.name, nim: s.nim, count: count };
  });
  stats.sort((a, b) => b.count - a.count);

  const rawDates = activityLogs.map(
    (l) => new Date(l.dateObj).toISOString().split("T")[0]
  );
  const uniqueDates = [...new Set(rawDates)].sort().reverse();

  res.render("dosen", {
    session: sessionData,
    currentList: mappedSessionList,
    logs: activityLogs,
    stats: stats,
    students: students,
    availableDates: uniqueDates,
  });
});

app.post("/dosen/update-settings", (req, res) => {
  const { status, timer } = req.body; // Mode dihapus dari sini
  if (status) sessionData.status = status;
  if (timer) sessionData.maxTime = timer;
  res.redirect("/dosen");
});

app.post("/dosen/manage-uid", (req, res) => {
  const { action, uid, student_id, target_date } = req.body;
  if (action === "delete") {
    sessionData.scannedList = sessionData.scannedList.filter(
      (item) => item.uid !== uid
    );
  } else if (action === "add_single") {
    const student = students.find((s) => s.id == student_id);
    if (
      student &&
      !sessionData.scannedList.find((item) => item.uid === student.rfid)
    ) {
      sessionData.scannedList.push({ uid: student.rfid });
    }
  } else if (action === "add_date") {
    const targetLogs = activityLogs.filter((log) => {
      const logDateStr = new Date(log.dateObj).toISOString().split("T")[0];
      return logDateStr === target_date;
    });
    const activeNims = [...new Set(targetLogs.map((l) => l.nim))];
    activeNims.forEach((nim) => {
      const s = students.find((st) => st.nim === nim);
      if (s && !sessionData.scannedList.find((item) => item.uid === s.rfid)) {
        sessionData.scannedList.push({ uid: s.rfid });
      }
    });
  }
  res.redirect("/dosen");
});

app.post("/dosen/edit-log", (req, res) => {
  const { id, student_id, edit_date, edit_time, transcript, question } =
    req.body;
  const logIndex = activityLogs.findIndex((l) => l.id == id);

  if (logIndex !== -1) {
    const student = students.find((s) => s.id == student_id);
    if (student) {
      const oldQuestion = activityLogs[logIndex].question;
      const targetDateStr = new Date(activityLogs[logIndex].dateObj)
        .toISOString()
        .split("T")[0];

      activityLogs[logIndex].name = student.name;
      activityLogs[logIndex].nim = student.nim;
      activityLogs[logIndex].transcript = transcript;
      activityLogs[logIndex].question = question;
      activityLogs[logIndex].dateObj = new Date(`${edit_date}T${edit_time}`);

      activityLogs.forEach((l) => {
        const lDate = new Date(l.dateObj).toISOString().split("T")[0];
        if (lDate === targetDateStr && l.question === oldQuestion)
          l.question = question;
      });

      // Re-sort agar rapi
      activityLogs.sort((a, b) => {
        const dateA = new Date(a.dateObj).setHours(0, 0, 0, 0);
        const dateB = new Date(b.dateObj).setHours(0, 0, 0, 0);
        if (dateA !== dateB) return dateB - dateA;
        if (a.question < b.question) return -1;
        if (a.question > b.question) return 1;
        return a.dateObj - b.dateObj;
      });
    }
  }
  res.redirect("/dosen");
});

// --- RUTE BARU: REQUEST AUDIO FILE ---
app.post("/dosen/request-audio", (req, res) => {
  const { id } = req.body;
  const logIndex = activityLogs.findIndex((l) => l.id == id);

  // Simulasi: File ditemukan dan diupload
  if (logIndex !== -1) {
    activityLogs[logIndex].audio_url = "#"; // Set dummy URL agar player muncul
  }
  res.redirect("/dosen");
});

app.post("/api/simulate", (req, res) => {
  sessionData.battery = Math.max(0, sessionData.battery - 5);
  const student = students[Math.floor(Math.random() * students.length)];
  const randQ = questionsBank[Math.floor(Math.random() * questionsBank.length)];

  activityLogs.push({
    id: Date.now(),
    dateObj: new Date(),
    name: student.name,
    nim: student.nim,
    question: randQ,
    transcript: "Jawaban simulasi masuk...",
    audio_url: null, // Simulasi file belum masuk otomatis
  });
  res.redirect("/dosen");
});

app.listen(3000, () => {
  console.log("Server berjalan di http://localhost:3000");
});
