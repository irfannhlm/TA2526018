const express = require("express");
const app = express();
const path = require("path");
const mysql = require("mysql2/promise");
const mqtt = require("mqtt");
const multer = require("multer");
const fs = require("fs");

app.set("view engine", "ejs");
app.set("views", path.join(__dirname, "views"));
app.use(express.urlencoded({ extended: true }));
app.use(express.static(path.join(__dirname, "public")));

// ================= KONFIGURASI DATABASE =================
const pool = mysql.createPool({
  host: "localhost",
  user: "root",
  password: "",
  database: "db_diskusi_kelas",
});

pool
  .query("UPDATE devices SET status = 'offline', battery = NULL")
  .then(() =>
    console.log(
      "System Startup: Status semua alat di-reset ke OFFLINE dan Baterai dikosongkan.",
    ),
  )
  .catch((err) => console.error("Gagal mereset status alat di database:", err));

// ================= DATA SESI (RAM / REAL-TIME STATE) =================
let sessionData = {
  status: "stopped",
  mode: "dosen_rec",
  battery: null,
  maxTime: 5,
  scannedList: [],
};

// ================= KONEKSI MQTT BROKER =================
const mqttClient = mqtt.connect("mqtt://172.20.10.2");

mqttClient.on("connect", () => {
  console.log("Backend API terhubung ke Broker Mosquitto di 10.46.103.253");
  // BERLANGGANAN 2 TOPIK: rfid (untuk tap kartu) & status (untuk online/offline & batre berkala)
  mqttClient.subscribe(["kelas/alat/rfid", "kelas/alat/status"], (err) => {
    if (!err)
      console.log(
        "Berlangganan topik MQTT: kelas/alat/rfid & kelas/alat/status",
      );
  });
});

mqttClient.on("message", async (topic, message) => {
  try {
    const payload = JSON.parse(message.toString());
    console.log(`Data masuk dari topik [${topic}]:`, payload);

    // 1. JIKA TOPIK ADALAH STATUS PERANGKAT (Online/Offline/Baterai)
    if (topic === "kelas/alat/status") {
      if (payload.device_id && payload.status) {
        if (payload.status === "offline") {
          // Jika broker mengabarkan alat putus, set status ke offline (biarkan info baterai terakhir)
          await pool.query(
            "UPDATE devices SET status = 'offline' WHERE id = ?",
            [payload.device_id],
          );
        } else {
          // Jika alat terhubung dan mengirim data baterai
          await pool.query(
            "UPDATE devices SET status = 'online', battery = ? WHERE id = ?",
            [payload.battery, payload.device_id],
          );
        }
      }
    }

    // 2. JIKA TOPIK ADALAH TAP KARTU RFID
    if (topic === "kelas/alat/rfid") {
      if (payload.action === "tap_rfid") {
        // Cek siapa pemilik kartu RFID ini di database
        const [students] = await pool.query(
          "SELECT * FROM students WHERE rfid = ?",
          [payload.uid],
        );
        const student = students[0];

        // PERBAIKAN: Masukkan ke antrean TANPA mengecek duplikat (bisa scan berkali-kali)
        sessionData.scannedList.unshift({ uid: payload.uid });

        // Otomatis buat baris riwayat diskusi kosong di database
        if (student) {
          await pool.query(
            "INSERT INTO activity_logs (id, date_obj, name, nim, question, transcript) VALUES (?, NOW(), ?, ?, ?, ?)",
            [
              Date.now(),
              student.name,
              student.nim,
              "Menunggu pertanyaan...",
              "",
            ],
          );
        }
      }
    }
  } catch (error) {
    console.error("Gagal memproses pesan MQTT:", error);
  }
});

// Pastikan folder untuk menyimpan audio ada
const uploadDir = path.join(__dirname, "public/recordings");
if (!fs.existsSync(uploadDir)) {
  fs.mkdirSync(uploadDir, { recursive: true });
}

// Konfigurasi Penyimpanan Multer
const storage = multer.diskStorage({
  destination: function (req, file, cb) {
    cb(null, uploadDir);
  },
  filename: function (req, file, cb) {
    // Nama file: audio-1712345678.wav
    cb(null, `audio-${Date.now()}${path.extname(file.originalname)}`);
  },
});
const upload = multer({ storage: storage });

// ================= ROUTES =================
app.get("/", (req, res) => res.render("login", { error: null }));

app.post("/login", async (req, res) => {
  const { username, password } = req.body;
  try {
    const [rows] = await pool.query("SELECT * FROM users WHERE username = ?", [
      username,
    ]);
    const user = rows[0];

    if (!user)
      return res.render("login", { error: "Username tidak ditemukan." });
    if (user.password !== password)
      return res.render("login", { error: "Password salah." });

    res.redirect(`/pilih-kelas?role=${user.role}`);
  } catch (err) {
    console.error(err);
    res.render("login", { error: "Terjadi kesalahan server." });
  }
});

app.get("/pilih-kelas", async (req, res) => {
  const { role } = req.query;
  if (!role) return res.redirect("/");

  try {
    const [rows] = await pool.query("SELECT class_name FROM classes");
    const classes = rows.map((r) => r.class_name);
    res.render("pilih-kelas", { classes: classes, role: role });
  } catch (err) {
    console.error(err);
    res.send("Gagal memuat daftar kelas.");
  }
});

// ================= ADMIN ROUTE =================

app.get("/admin", async (req, res) => {
  const currentClass = req.query.kelas;
  const currentDeviceId = req.query.device;

  if (!currentClass) return res.redirect("/pilih-kelas?role=admin");

  try {
    const [students] = await pool.query(
      "SELECT * FROM students WHERE kelas = ?",
      [currentClass],
    );
    const [devices] = await pool.query("SELECT * FROM devices");
    const [users] = await pool.query("SELECT * FROM users");

    // PERBAIKAN: Ambil data kelas untuk tab Manajemen Kelas
    // Asumsi tabel kelas memiliki kolom: id, class_name, class_code
    const [classes] = await pool.query(
      "SELECT id, class_name AS name, class_code AS code FROM classes",
    );

    let classLogs = [];
    const classNims = students.map((s) => s.nim);
    if (classNims.length > 0) {
      const [logs] = await pool.query(
        "SELECT * FROM activity_logs WHERE nim IN (?)",
        [classNims],
      );
      classLogs = logs;
    }

    if (!currentDeviceId && devices.length > 0) {
      return res.redirect(
        `/admin?kelas=${currentClass}&device=${devices[0].id}`,
      );
    }
    const currentDevice = devices.find((d) => d.id === currentDeviceId) ||
      devices[0] || {
        id: "N/A",
        name: "Belum Ada Perangkat",
        status: "offline",
        battery: null,
      };

    const [allStudents] = await pool.query("SELECT * FROM students");
    const mappedSessionList = sessionData.scannedList.map((item) => {
      const student = allStudents.find((s) => s.rfid === item.uid);
      return {
        uid: item.uid,
        name: student ? student.name : "Unknown Device",
        nim: student ? student.nim : "-",
        kelas: student ? student.kelas : "-",
        isRegistered: !!student,
        isWrongClass: student && student.kelas !== currentClass,
      };
    });

    res.render("admin", {
      students: students,
      logs: classLogs,
      users: users,
      classes: classes, // <-- Dikirim ke frontend
      currentClass: currentClass,
      devices: devices,
      currentDevice: currentDevice,
      currentList: mappedSessionList,
      session: sessionData,
    });
  } catch (err) {
    console.error(err);
    res.send("Gagal memuat halaman Admin.");
  }
});

// ROUTE MANAJEMEN KELAS (DIPERBARUI)
app.post("/admin/add-class", async (req, res) => {
  const { name, code, current_class } = req.body;
  try {
    // Pastikan database kamu urutannya menerima class_name, lalu class_code
    await pool.query(
      "INSERT INTO classes (class_name, class_code) VALUES (?, ?)",
      [name, code],
    );
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menambah kelas.");
  }
});

app.post("/admin/edit-class", async (req, res) => {
  const { id, name, code, current_class } = req.body;
  try {
    // Mengubah class_name dan class_code berdasarkan ID
    await pool.query(
      "UPDATE classes SET class_name=?, class_code=? WHERE id=?",
      [name, code, id],
    );
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal mengupdate kelas.");
  }
});

app.post("/admin/delete-class", async (req, res) => {
  const { id, current_class } = req.body;
  try {
    await pool.query("DELETE FROM classes WHERE id=?", [id]);
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menghapus kelas.");
  }
});

// ROUTE HAPUS UID OLEH ADMIN AGAR TIDAK NYASAR KE DOSEN
app.post("/admin/manage-uid", async (req, res) => {
  const { action, uid, current_class } = req.body;
  if (action === "delete") {
    // Hapus uid dari antrean monitoring
    sessionData.scannedList = sessionData.scannedList.filter(
      (item) => item.uid !== uid,
    );
  }
  res.redirect(`/admin?kelas=${current_class}`);
});

app.post("/admin/add", async (req, res) => {
  const { name, nim, rfid, kelas, current_class } = req.body;
  const targetClass = kelas || current_class;
  try {
    await pool.query(
      "INSERT INTO students (name, nim, rfid, kelas) VALUES (?, ?, ?, ?)",
      [name, nim, rfid, targetClass],
    );
    res.redirect(`/admin?kelas=${targetClass}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menambah mahasiswa. Pastikan NIM unik.");
  }
});

app.post("/admin/delete", async (req, res) => {
  const { id, current_class } = req.body;
  try {
    await pool.query("DELETE FROM students WHERE id = ?", [id]);
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menghapus mahasiswa.");
  }
});

app.post("/admin/edit", async (req, res) => {
  const { id, name, nim, rfid, current_class } = req.body;
  try {
    await pool.query("UPDATE students SET name=?, nim=?, rfid=? WHERE id=?", [
      name,
      nim,
      rfid,
      id,
    ]);
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal mengupdate mahasiswa.");
  }
});

app.post("/admin/add-user", async (req, res) => {
  const { username, password, role, current_class } = req.body;
  try {
    await pool.query(
      "INSERT INTO users (username, password, role) VALUES (?, ?, ?)",
      [username, password, role],
    );
    res.redirect(`/admin?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal menambah user.");
  }
});

app.post("/admin/delete-user", async (req, res) => {
  const { id, current_class } = req.body;
  if (id != 1) {
    try {
      await pool.query("DELETE FROM users WHERE id = ?", [id]);
    } catch (err) {
      console.error(err);
    }
  }
  res.redirect(`/admin?kelas=${current_class}`);
});

// ================= DOSEN ROUTE =================

app.get("/dosen", async (req, res) => {
  const currentClass = req.query.kelas;
  const currentDeviceId = req.query.device;

  if (!currentClass) return res.redirect("/pilih-kelas?role=dosen");

  try {
    const [students] = await pool.query(
      "SELECT * FROM students WHERE kelas = ?",
      [currentClass],
    );
    const [devices] = await pool.query("SELECT * FROM devices");

    if (!currentDeviceId && devices.length > 0) {
      return res.redirect(
        `/dosen?kelas=${currentClass}&device=${devices[0].id}`,
      );
    }
    const currentDevice = devices.find((d) => d.id === currentDeviceId) ||
      devices[0] || {
        id: "N/A",
        name: "Belum Ada Perangkat",
        status: "offline",
        battery: null,
      };

    const mappedSessionList = sessionData.scannedList
      .map((item) => {
        const student = students.find((s) => s.rfid === item.uid);
        if (student) {
          return {
            uid: item.uid,
            name: student.name,
            nim: student.nim,
            isRegistered: true,
          };
        }
        return null;
      })
      .filter((item) => item !== null);

    let filteredLogs = [];
    const classNims = students.map((s) => s.nim);
    if (classNims.length > 0) {
      const [logs] = await pool.query(
        "SELECT * FROM activity_logs WHERE nim IN (?)",
        [classNims],
      );
      filteredLogs = logs;
    }

    let stats = students.map((s) => {
      const count = filteredLogs.filter((l) => l.nim === s.nim).length;
      return { name: s.name, nim: s.nim, count: count };
    });
    stats.sort((a, b) => b.count - a.count);

    filteredLogs.sort((a, b) => {
      const dateA = new Date(a.date_obj).setHours(0, 0, 0, 0);
      const dateB = new Date(b.date_obj).setHours(0, 0, 0, 0);
      if (dateA !== dateB) return dateB - dateA;
      if (a.question < b.question) return -1;
      if (a.question > b.question) return 1;
      return new Date(a.date_obj) - new Date(b.date_obj);
    });

    const rawDates = filteredLogs.map((l) => {
      const d = new Date(l.date_obj);
      d.setMinutes(d.getMinutes() - d.getTimezoneOffset());
      return d.toISOString().split("T")[0];
    });
    const uniqueDates = [...new Set(rawDates)].sort().reverse();

    res.render("dosen", {
      session: sessionData,
      devices: devices,
      currentDevice: currentDevice,
      currentList: mappedSessionList,
      logs: filteredLogs.map((l) => ({ ...l, dateObj: l.date_obj })),
      stats: stats,
      students: students,
      availableDates: uniqueDates,
      currentClass: currentClass,
    });
  } catch (err) {
    console.error(err);
    res.send("Gagal memuat halaman Dosen.");
  }
});

app.post("/dosen/update-settings", (req, res) => {
  const { status, mode, timer, current_class } = req.body;
  if (status) sessionData.status = status;
  if (mode) sessionData.mode = mode;
  
  if (timer) {
    sessionData.maxTime = parseInt(timer);
    
    // PUBLISH KE MQTT UNTUK DITERIMA ESP32
    // Mengirim payload JSON agar mudah di-parsing di ESP32 (menggunakan ArduinoJson)
    const payload = JSON.stringify({
      perintah: "set_timer",
      durasi_detik: sessionData.maxTime
    });

    mqttClient.publish("kelas/alat/perintah", payload, (err) => {
      if (err) console.error("Gagal mengirim timer ke MQTT:", err);
      else console.log("Berhasil mengirim timer ke MQTT:", payload);
    });
  }
  
  res.redirect(`/dosen?kelas=${current_class}`);
});

// ROUTE BARU: Untuk mengirimkan daftar UID ke ESP32
app.post("/dosen/sync-uid", (req, res) => {
  const { current_class } = req.body;
  
  // Ambil hanya array UID dari scannedList saat ini
  const daftarUid = sessionData.scannedList.map(item => item.uid);
  
  const payload = JSON.stringify({
    perintah: "sync_uid",
    uids: daftarUid
  });

  mqttClient.publish("kelas/alat/perintah", payload, (err) => {
    if (err) console.error("Gagal sinkronisasi UID ke MQTT:", err);
    else console.log("Berhasil sinkronisasi UID ke MQTT:", payload);
  });

  res.redirect(`/dosen?kelas=${current_class}`);
});

app.post("/dosen/edit-log", async (req, res) => {
  const {
    id,
    student_id,
    edit_date,
    edit_time,
    transcript,
    question,
    current_class,
  } = req.body;

  try {
    const [students] = await pool.query("SELECT * FROM students WHERE id = ?", [
      student_id,
    ]);
    const student = students[0];

    if (student) {
      const newDateObj = new Date(`${edit_date}T${edit_time}`);

      const [oldLog] = await pool.query(
        "SELECT question, date_obj FROM activity_logs WHERE id = ?",
        [id],
      );

      if (oldLog.length > 0) {
        const oldQuestion = oldLog[0].question;
        const oldDateStr = new Date(oldLog[0].date_obj)
          .toISOString()
          .split("T")[0];

        await pool.query(
          "UPDATE activity_logs SET name=?, nim=?, transcript=?, question=?, date_obj=? WHERE id=?",
          [student.name, student.nim, transcript, question, newDateObj, id],
        );

        if (oldQuestion !== question) {
          await pool.query(
            "UPDATE activity_logs SET question=? WHERE question=? AND DATE(date_obj) = ?",
            [question, oldQuestion, oldDateStr],
          );
        }
      }
    }
    res.redirect(`/dosen?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal mengedit log.");
  }
});

app.post("/dosen/request-audio", async (req, res) => {
  const { id, current_class } = req.body;
  try {
    await pool.query("UPDATE activity_logs SET audio_url = '#' WHERE id = ?", [
      id,
    ]);
    res.redirect(`/dosen?kelas=${current_class}`);
  } catch (err) {
    console.error(err);
    res.send("Gagal request audio.");
  }
});

app.post("/dosen/manage-uid", async (req, res) => {
  const { action, uid, student_id, target_date, current_class } = req.body;

  if (action === "delete") {
    sessionData.scannedList = sessionData.scannedList.filter(
      (item) => item.uid !== uid,
    );
  } else if (action === "add_single") {
    try {
      const [rows] = await pool.query(
        "SELECT rfid FROM students WHERE id = ?",
        [student_id],
      );
      if (rows.length > 0) {
        const rfid = rows[0].rfid;
        if (!sessionData.scannedList.find((item) => item.uid === rfid)) {
          sessionData.scannedList.unshift({ uid: rfid });
        }
      }
    } catch (err) {
      console.error(err);
    }
  } else if (action === "add_date" && current_class) {
    try {
      const [logs] = await pool.query(
        "SELECT nim FROM activity_logs WHERE DATE(date_obj) = ?",
        [target_date],
      );
      const activeNims = [...new Set(logs.map((l) => l.nim))];

      if (activeNims.length > 0) {
        const [students] = await pool.query(
          "SELECT rfid FROM students WHERE nim IN (?) AND kelas = ?",
          [activeNims, current_class],
        );
        students.forEach((s) => {
          if (!sessionData.scannedList.find((item) => item.uid === s.rfid)) {
            sessionData.scannedList.push({ uid: s.rfid });
          }
        });
      }
    } catch (err) {
      console.error(err);
    }
  }
  res.redirect(`/dosen?kelas=${current_class}`);
});

app.listen(3000, () => {
  console.log("Server berjalan di http://localhost:3000");
});

// ================= ROUTE BARU: REQUEST SYNC KE ESP32 =================

app.post("/dosen/request-audio-sync", (req, res) => {
  const { current_class } = req.body;

  // Kirim perintah ke ESP32 via MQTT untuk mulai mengunggah file yang ada di SD Card
  const payload = JSON.stringify({
    perintah: "request_sync_audio",
    target_kelas: current_class
  });

  mqttClient.publish("kelas/alat/perintah", payload, (err) => {
    if (err) {
      console.error("Gagal mengirim perintah sync ke MQTT:", err);
      return res.status(500).send("Gagal menghubungi alat.");
    }
    console.log("Perintah sync audio dikirim:", payload);
    res.redirect(`/dosen?kelas=${current_class}`);
  });
});

// ================= ROUTE BARU: TERIMA FILE DARI ESP32 =================

// Endpoint ini akan dipanggil oleh ESP32 menggunakan HTTP POST
app.post("/api/upload-audio", upload.single("audio"), async (req, res) => {
  try {
    const { log_id } = req.body; // ESP32 harus mengirim log_id agar server tahu file ini milik siapa
    
    if (!req.file) {
      return res.status(400).send("Tidak ada file yang diunggah.");
    }

    const audioUrl = `/recordings/${req.file.filename}`;

    // Update database dengan URL file yang baru diunggah
    await pool.query(
      "UPDATE activity_logs SET audio_url = ? WHERE id = ?",
      [audioUrl, log_id]
    );

    console.log(`File audio diterima untuk Log ID ${log_id}: ${audioUrl}`);
    res.status(200).json({ status: "success", url: audioUrl });
  } catch (error) {
    console.error("Gagal memproses upload audio:", error);
    res.status(500).send("Server Error");
  }
});