const express = require("express");
const app = express();
const path = require("path");

app.set("view engine", "ejs");
app.set("views", path.join(__dirname, "views"));
app.use(express.urlencoded({ extended: true }));
app.use(express.static(path.join(__dirname, "public")));

// --- 1. DATA KELAS & USER ---
const classes = ["EL-401", "EL-402", "TK-305"]; // Daftar Kelas

const users = [
  { id: 1, username: "admin", password: "123", role: "admin" },
  { id: 2, username: "dosen", password: "123", role: "dosen" },
];

// --- 2. DATA MAHASISWA ---
let students = [
  {
    id: 1,
    name: "Ryan Reynolds",
    nim: "13222001",
    rfid: "A1-B2",
    kelas: "EL-401",
  },
  {
    id: 2,
    name: "Michael Cliffort",
    nim: "13222002",
    rfid: "X9-Y8",
    kelas: "EL-401",
  },
  {
    id: 3,
    name: "Emma Watson",
    nim: "13222003",
    rfid: "C3-D4",
    kelas: "EL-402",
  },
  {
    id: 4,
    name: "Tom Holland",
    nim: "13222004",
    rfid: "E5-F6",
    kelas: "EL-402",
  },
  {
    id: 5,
    name: "Robert Downey Jr",
    nim: "13222005",
    rfid: "G7-H8",
    kelas: "TK-305",
  },
  {
    id: 6,
    name: "Chris Evans",
    nim: "13222006",
    rfid: "I9-J0",
    kelas: "TK-305",
  },
  {
    id: 7,
    name: "Scarlett Johansson",
    nim: "13222007",
    rfid: "K1-L2",
    kelas: "EL-401",
  },
  {
    id: 8,
    name: "Chris Hemsworth",
    nim: "13222008",
    rfid: "M3-N4",
    kelas: "EL-402",
  },
];

// --- 3. DATA SESI ---
let sessionData = {
  status: "stopped",
  mode: "dosen_rec",
  battery: 85,
  maxTime: 5,
  scannedList: [],
};

// --- 3. DATA SESI & PERANGKAT ---
// (Ganti sessionData yang lama dengan struktur baru ini)

// Daftar Perangkat Keras yang tersedia
let devices = [
  { id: "DEV-MIC-3", name: "Mic Portable 3", status: "online", battery: 92 },
  { id: "DEV-MIC-2", name: "Mic Portable 2", status: "online", battery: 78 },
  { id: "DEV-MIC-1", name: "Mic Portable 1", status: "offline", battery: 0 }, // Simulasi offline
];

// --- 4. GENERATE LOG DUMMY ---
let activityLogs = [];

const questionsBank = [
  "Jelaskan prinsip kerja sensor LDR dalam rangkaian pembagi tegangan!",
  "Bagaimana cara mengatasi noise pada sinyal analog?",
  "Apa perbedaan utama antara Mikrokontroler dan Mikroprosesor?",
  "Mengapa kita perlu menggunakan resistor pull-up pada tombol?",
];

const answersBank = [
  "Sensor LDR merubah resistansi berdasarkan intensitas cahaya.",
  "Semakin terang cahaya, resistansi akan semakin kecil.",
  "Kita bisa menggunakan kapasitor sebagai filter.",
  "Mikrokontroler memiliki RAM/ROM internal.",
  "Mikroprosesor biasanya membutuhkan periferal eksternal.",
  "Agar pin tidak floating saat kondisi idle.",
  "Untuk menjaga logika HIGH saat tombol tidak ditekan.",
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

      activityLogs.push({
        id: Date.now() + Math.random(),
        dateObj: logTime,
        name: student.name,
        nim: student.nim,
        question: q,
        transcript: randomAnswer,
        audio_url: "#",
      });
    }
  });
}
generateGroupedLogs();

// ================= ROUTES =================

app.get("/", (req, res) => res.render("login", { error: null }));

app.post("/login", (req, res) => {
  const { username, password } = req.body;
  const user = users.find((u) => u.username === username);

  if (!user) return res.render("login", { error: "Username tidak ditemukan." });
  if (user.password !== password)
    return res.render("login", { error: "Password salah." });

  // Redirect ke pilih kelas dulu, bawa role sebagai query param
  res.redirect(`/pilih-kelas?role=${user.role}`);
});

// --- ROUTE: PILIH KELAS ---
app.get("/pilih-kelas", (req, res) => {
  const { role } = req.query;
  if (!role) return res.redirect("/");
  res.render("pilih-kelas", { classes: classes, role: role });
});

// ================= ADMIN ROUTE =================

// --- ADMIN ROUTE (UPDATED) ---
app.get("/admin", (req, res) => {
  const currentClass = req.query.kelas;
  const currentDeviceId = req.query.device; 

  if (!currentClass) return res.redirect("/pilih-kelas?role=admin");

  // Default device selection logic (sama seperti dosen)
  if (!currentDeviceId && devices.length > 0) {
      return res.redirect(`/admin?kelas=${currentClass}&device=${devices[0].id}`);
  }
  const currentDevice = devices.find(d => d.id === currentDeviceId) || devices[0];

  // 1. Filter Data Database
  const classStudents = students.filter((s) => s.kelas === currentClass);
  const classNims = classStudents.map((s) => s.nim);
  const classLogs = activityLogs.filter((l) => classNims.includes(l.nim));

  // 2. Filter Live Scan List (PENTING UNTUK FITUR "PESERTA AKTIF")
  const mappedSessionList = sessionData.scannedList
    .map((item) => {
      const student = students.find((s) => s.rfid === item.uid);
      // Logika Admin: Tampilkan SEMUA scan. 
      // Jika terdaftar di kelas lain atau tidak terdaftar, tetap munculkan agar bisa di-manage.
      return {
        uid: item.uid,
        name: student ? student.name : "Unknown Device",
        nim: student ? student.nim : "-",
        kelas: student ? student.kelas : "-",
        isRegistered: !!student,
        isWrongClass: student && student.kelas !== currentClass
      };
    });

  res.render("admin", {
    students: classStudents,
    logs: classLogs,
    users: users,
    currentClass: currentClass,
    // Kirim Data Device & Live Scan ke Admin
    devices: devices,
    currentDevice: currentDevice,
    currentList: mappedSessionList,
    session: sessionData
  });
});

// Admin Add Student
app.post("/admin/add", (req, res) => {
  const { name, nim, rfid, kelas, current_class } = req.body;

  // Gunakan kelas dari input hidden jika user tidak memilih manual
  const targetClass = kelas || current_class;

  students.push({
    id: Date.now(),
    name,
    nim,
    rfid,
    kelas: targetClass,
  });

  res.redirect(`/admin?kelas=${targetClass}`);
});

// Admin Delete Student
app.post("/admin/delete", (req, res) => {
  const { id, current_class } = req.body;
  students = students.filter((s) => s.id != id);
  res.redirect(`/admin?kelas=${current_class}`);
});

// Admin Edit Student
app.post("/admin/edit", (req, res) => {
  const { id, name, nim, rfid, current_class } = req.body;
  const index = students.findIndex((s) => s.id == id);
  if (index !== -1) {
    students[index].name = name;
    students[index].nim = nim;
    students[index].rfid = rfid;
  }
  res.redirect(`/admin?kelas=${current_class}`);
});

// Admin User Management (Global, tapi redirect tetap ke kelas aktif agar UX nyaman)
app.post("/admin/add-user", (req, res) => {
  const { username, password, role, current_class } = req.body;
  users.push({ id: Date.now(), username, password, role });
  res.redirect(`/admin?kelas=${current_class}`);
});

app.post("/admin/delete-user", (req, res) => {
  const { id, current_class } = req.body;
  // Hindari menghapus user admin utama (opsional safety)
  if (id != 1) {
    // Logic hapus user... (kita filter array users)
    // Note: const users diatas adalah const, untuk fitur delete user yang proper
    // array users sebaiknya dideklarasikan dengan 'let'.
    // Namun untuk simulasi ini kita skip penghapusan array const atau anggap let.
    // users = users.filter(u => u.id != id);
  }
  res.redirect(`/admin?kelas=${current_class}`);
});

// ================= DOSEN ROUTE =================

app.get("/dosen", (req, res) => {
  const currentClass = req.query.kelas;
  const currentDeviceId = req.query.device;

  // Jika tidak ada kelas dipilih, kembali ke pemilihan
  if (!currentClass) return res.redirect("/pilih-kelas?role=dosen");

  if (!currentDeviceId && devices.length > 0) {
    return res.redirect(`/dosen?kelas=${currentClass}&device=${devices[0].id}`);
  }

  const currentDevice =
    devices.find((d) => d.id === currentDeviceId) || devices[0];

  // 1. Filter Siswa Sesuai Kelas
  const classStudents = students.filter((s) => s.kelas === currentClass);
  const classNims = classStudents.map((s) => s.nim);

  // 2. Filter Live List (Hanya tampilkan jika siswa ada di kelas ini)
  const mappedSessionList = sessionData.scannedList
    .map((item) => {
      const student = students.find((s) => s.rfid === item.uid);
      if (student && student.kelas === currentClass) {
        return {
          uid: item.uid,
          name: student.name,
          nim: student.nim,
          isRegistered: true,
        };
      } else if (!student) {
        // Option: Return null untuk hide, atau object unknown
        return null;
      }
      return null;
    })
    .filter((item) => item !== null);

  // 3. Filter Logs Sesuai Kelas
  let filteredLogs = activityLogs.filter((l) => classNims.includes(l.nim));

  // 4. Stats (Hanya untuk kelas ini)
  let stats = classStudents.map((s) => {
    const count = filteredLogs.filter((l) => l.nim === s.nim).length;
    return { name: s.name, nim: s.nim, count: count };
  });
  stats.sort((a, b) => b.count - a.count);

  // 5. Sorting Logs
  filteredLogs.sort((a, b) => {
    const dateA = new Date(a.dateObj).setHours(0, 0, 0, 0);
    const dateB = new Date(b.dateObj).setHours(0, 0, 0, 0);
    if (dateA !== dateB) return dateB - dateA; // Tanggal terbaru diatas
    if (a.question < b.question) return -1;
    if (a.question > b.question) return 1;
    return a.dateObj - b.dateObj; // Jam asc
  });

  // 6. Dropdown Dates
  const rawDates = filteredLogs.map(
    (l) => new Date(l.dateObj).toISOString().split("T")[0]
  );
  const uniqueDates = [...new Set(rawDates)].sort().reverse();

  res.render("dosen", {
    session: sessionData,
    devices: devices, // <--- KIRIM LIST SEMUA DEVICE
    currentDevice: currentDevice, // <--- KIRIM DEVICE YANG DIPILIH
    currentList: mappedSessionList,
    logs: filteredLogs,
    stats: stats,
    students: classStudents,
    availableDates: uniqueDates,
    currentClass: currentClass, // Kirim info kelas agar UI tahu
  });
});

app.post("/dosen/update-settings", (req, res) => {
  const { status, mode, timer, current_class } = req.body;
  if (status) sessionData.status = status;
  if (mode) sessionData.mode = mode;
  if (timer) sessionData.maxTime = timer;
  res.redirect(`/dosen?kelas=${current_class}`);
});

app.post("/dosen/edit-log", (req, res) => {
  const {
    id,
    student_id,
    edit_date,
    edit_time,
    transcript,
    question,
    current_class,
  } = req.body;

  const logIndex = activityLogs.findIndex((l) => l.id == id);

  if (logIndex !== -1) {
    const student = students.find((s) => s.id == student_id);
    if (student) {
      // Logic untuk update pertanyaan secara grup (agar tampilan tidak pecah)
      const oldQuestion = activityLogs[logIndex].question;
      const targetDateStr = new Date(activityLogs[logIndex].dateObj)
        .toISOString()
        .split("T")[0];
      const newDateStr = edit_date; // asumsi edit_date format YYYY-MM-DD

      // Update Log Target
      activityLogs[logIndex].name = student.name;
      activityLogs[logIndex].nim = student.nim;
      activityLogs[logIndex].transcript = transcript;
      activityLogs[logIndex].question = question;
      activityLogs[logIndex].dateObj = new Date(`${edit_date}T${edit_time}`);

      // Fitur Tambahan: Update pertanyaan log lain di tanggal yang sama jika pertanyaan awal sama
      // (Agar grouping pertanyaan di tabel tetap rapi)
      if (oldQuestion !== question) {
        activityLogs.forEach((l) => {
          const lDate = new Date(l.dateObj).toISOString().split("T")[0];
          // Hanya update jika tanggal sama (sebelum diedit) dan pertanyaan sama
          if (lDate === targetDateStr && l.question === oldQuestion) {
            // Note: ini simplifikasi, idealnya cek ID grup jika ada.
            // Di sini kita update semua yg pertanyaannya sama di hari itu.
            l.question = question;
          }
        });
      }
    }
  }
  res.redirect(`/dosen?kelas=${current_class}`);
});

app.post("/dosen/request-audio", (req, res) => {
  const { id, current_class } = req.body;
  const logIndex = activityLogs.findIndex((l) => l.id == id);
  if (logIndex !== -1) {
    activityLogs[logIndex].audio_url = "#"; // Dummy URL
  }
  res.redirect(`/dosen?kelas=${current_class}`);
});

// --- UPDATE ROUTE: MANAGE UID (DOSEN & ADMIN) ---
app.post("/dosen/manage-uid", (req, res) => {
  // Pastikan ambil current_class dari body
  const { action, uid, student_id, target_date, current_class } = req.body;

  if (action === "delete") {
    sessionData.scannedList = sessionData.scannedList.filter((item) => item.uid !== uid);
  } 
  else if (action === "add_single") {
    const student = students.find((s) => s.id == student_id);
    // Cek duplikasi
    if (student && !sessionData.scannedList.find((item) => item.uid === student.rfid)) {
      sessionData.scannedList.unshift({ uid: student.rfid }); // Tambah ke paling atas
    }
  } 
  else if (action === "add_date") {
    // FIX ERROR: Pastikan current_class tersedia
    if (!current_class) {
        console.error("Error: current_class undefined pada add_date");
        // Fallback atau return error page jika perlu
        return res.redirect('back');
    }

    const targetLogs = activityLogs.filter((log) => {
      const logDateStr = new Date(log.dateObj).toISOString().split("T")[0];
      return logDateStr === target_date;
    });
    
    const activeNims = [...new Set(targetLogs.map((l) => l.nim))];
    
    activeNims.forEach((nim) => {
      const s = students.find((st) => st.nim === nim);
      // GUNAKAN 'current_class' yang diambil dari req.body
      if (s && s.kelas === current_class && !sessionData.scannedList.find((item) => item.uid === s.rfid)) {
        sessionData.scannedList.push({ uid: s.rfid });
      }
    });
  }

  // Redirect kembali ke halaman yang benar
  res.redirect(`/dosen?kelas=${current_class}`);
});


// --- UPDATE ROUTE: SIMULASI TAP (UNKNOWN + RANDOM) ---
app.post("/api/simulate", (req, res) => {
  const { current_class, source } = req.body;
  
  // 1. Logika Random (Sama untuk Admin & Dosen)
  const isUnknown = Math.random() < 0.3; // 30% peluang Unknown
  let simulatedUid = "";

  // Filter siswa kelas ini agar simulasi 'Terdaftar' masuk akal
  let targetStudents = students;
  if(current_class) {
      targetStudents = students.filter(s => s.kelas === current_class);
  }
  
  if (isUnknown || targetStudents.length === 0) {
    // Generate UID Acak (Unknown)
    simulatedUid = "UNK-" + Math.floor(1000 + Math.random() * 9000);
  } else {
    // Ambil siswa random dari kelas yang aktif
    const randomStudent = targetStudents[Math.floor(Math.random() * targetStudents.length)];
    simulatedUid = randomStudent.rfid;
  }

  // 2. Masukkan ke List Scan (Cek Duplikasi)
  const isAlreadyScanned = sessionData.scannedList.find(item => item.uid === simulatedUid);
  
  if (!isAlreadyScanned) {
      sessionData.scannedList.unshift({ uid: simulatedUid });
      if(sessionData.battery > 0) sessionData.battery -= 2;
  }

  // 3. Tambah Log Chat (Opsional: Hanya jika Terdaftar)
  // Jika ingin chat muncul hanya saat mahasiswa terdaftar tap:
  const student = students.find(s => s.rfid === simulatedUid);
  if (student) {
      const randQ = questionsBank[Math.floor(Math.random() * questionsBank.length)];
      activityLogs.push({
        id: Date.now(),
        dateObj: new Date(),
        name: student.name,
        nim: student.nim,
        question: randQ,
        transcript: "Jawaban simulasi otomatis...",
        audio_url: null,
      });
  }

  // 4. Redirect
  if (source === 'admin') {
      res.redirect(`/admin?kelas=${current_class}`);
  } else {
      res.redirect(`/dosen?kelas=${current_class}`);
  }
});

app.listen(3000, () => {
  console.log("Server berjalan di http://localhost:3000");
});
