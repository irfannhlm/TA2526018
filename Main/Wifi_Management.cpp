// Wifi_Management.cpp

#include "Wifi_Management.h"
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LiquidCrystal_I2C.h>
#include "Config.h"
#include "Buzzer_Module.h"

extern volatile bool buttonFlag;
extern LiquidCrystal_I2C lcd;
extern int active_threshold;

Preferences preferences;
WebServer   server(80);
DNSServer   dnsServer;
const byte  DNS_PORT = 53;

char eap_nim[40]    = "";
char eap_pass[40]   = "";
char saved_ssid[40] = "";
char wifi_type[20]  = "biasa";
char wifi_pass[64]  = "";

unsigned long buttonPressTime = 0;
bool          isButtonPressed = false;

static void handlePortalButton() {
  static unsigned long pressStart = 0;
  static bool wasPressed = false;

  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  if (pressed && !wasPressed) {
    wasPressed = true;
    pressStart = millis();
  }

  if (!pressed) {
    wasPressed = false;
    pressStart = 0;
  }

  // Tahan tombol 2 detik di portal = restart
  if (pressed && pressStart > 0 && millis() - pressStart > 2000) {
    playBuzzer(1, 300, 80);
    waitBuzzerDone();
    ESP.restart();
  }
}

// ════════════════════════════════════════════════
//  bukaPortal
// ════════════════════════════════════════════════
void bukaPortal() {
  Serial.println("🌐 Membuka portal konfigurasi...");

  // 1. BACA DATA TERAKHIR UNTUK PRE-FILL
  preferences.begin("catch_note", true); 
  String saved_nim_str      = preferences.getString("nim", "");
  String saved_eap_pass_str = preferences.getString("eap_pass", "");
  String saved_ssid_str     = preferences.getString("ssid", ""); // Meski tadi dihapus di NVS, kita bisa tetap pakai variabel global saved_ssid jika mau, tapi ini aman jika kosong.
  String saved_type_str     = preferences.getString("wifi_type", "biasa");
  int    saved_threshold    = preferences.getInt("threshold", 300);
  preferences.end();

  String chk_biasa   = (saved_type_str == "biasa")   ? "active" : "";
  String chk_eduroam = (saved_type_str == "eduroam") ? "active" : "";
  String sec_biasa   = (saved_type_str == "biasa")   ? "show"   : "";
  String sec_eduroam = (saved_type_str == "eduroam") ? "show"   : "";
  
  String chkT = (saved_threshold >= 400) ? "checked" : "";
  String chkS = (saved_threshold == 300) ? "checked" : "";
  String chkR = (saved_threshold <= 200) ? "checked" : "";

  WiFi.mode(WIFI_AP);
  WiFi.softAP("CatchNote-Setup");
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("📡 Portal aktif! IP: %s\n", apIP.toString().c_str());
  dnsServer.start(DNS_PORT, "*", apIP);

  // 2. BANGUN HTML DENGAN PRE-FILL
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Catch Note Setup</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=DM+Sans:wght@400;500;600&family=DM+Serif+Display&display=swap');
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    :root {
      --pink: #e8638c; --pink-lt: #fde8ef; --pink-md: #f5b8cc; --text: #2d1a22;
      --muted: #9c7585; --bg: #fdf4f7; --card: #ffffff;
      --border: #f0d0dc; --radius: 14px;
    }
    body {
      font-family: 'DM Sans', sans-serif; background: var(--bg);
      min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 24px 16px;
      background-image: radial-gradient(circle at 20% 20%, #fce4ed 0%, transparent 50%), radial-gradient(circle at 80% 80%, #fde8ef 0%, transparent 50%);
    }
    .card { background: var(--card); border-radius: 24px; padding: 36px 32px; width: 100%; max-width: 420px; box-shadow: 0 4px 40px rgba(232,99,140,0.10); border: 1px solid var(--border); }
    .logo { text-align: center; margin-bottom: 28px; }
    .logo-icon { width: 56px; height: 56px; background: linear-gradient(135deg, #e8638c, #f598b4); border-radius: 16px; display: inline-flex; align-items: center; justify-content: center; font-size: 26px; margin-bottom: 12px; box-shadow: 0 6px 20px rgba(232,99,140,0.30); }
    .logo h1 { font-family: 'DM Serif Display', serif; font-size: 22px; color: var(--text); }
    .logo p  { font-size: 13px; color: var(--muted); margin-top: 4px; }
    .section-title { font-size: 11px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: 1px; margin: 24px 0 14px; display: flex; align-items: center; gap: 8px; }
    .section-title::after { content: ''; flex: 1; height: 1px; background: var(--border); }
    .toggle-wrap { display: flex; background: var(--pink-lt); border-radius: 12px; padding: 4px; margin-bottom: 20px; gap: 4px; }
    .toggle-btn { flex: 1; padding: 10px; border: none; border-radius: 9px; font-family: 'DM Sans', sans-serif; font-size: 14px; font-weight: 500; cursor: pointer; transition: all 0.2s; background: transparent; color: var(--muted); }
    .toggle-btn.active { background: white; color: var(--pink); box-shadow: 0 2px 8px rgba(232,99,140,0.15); }
    .section { display: none; } .section.show { display: block; }
    .field { margin-bottom: 16px; }
    .field label { display: block; font-size: 12px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 6px; }
    .field input { width: 100%; padding: 12px 14px; border: 1.5px solid var(--border); border-radius: var(--radius); font-family: 'DM Sans', sans-serif; font-size: 15px; color: var(--text); background: #fff; outline: none; }
    .field input:focus { border-color: var(--pink); box-shadow: 0 0 0 3px rgba(232,99,140,0.10); }
    .threshold-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; margin-bottom: 8px; }
    .threshold-card { position: relative; cursor: pointer; }
    .threshold-card input[type=radio] { position: absolute; opacity: 0; width: 0; height: 0; }
    .threshold-label { display: flex; flex-direction: column; align-items: center; padding: 14px 8px 12px; border: 2px solid var(--border); border-radius: 14px; background: #fff; transition: all 0.18s; user-select: none; }
    .threshold-card input[type=radio]:checked + .threshold-label { border-color: var(--pink); background: var(--pink-lt); }
    .threshold-label .th-icon  { font-size: 22px; margin-bottom: 6px; }
    .threshold-label .th-name  { font-size: 13px; font-weight: 600; color: var(--text); }
    button[type=submit] { width: 100%; padding: 14px; background: linear-gradient(135deg, #e8638c, #f598b4); color: white; border: none; border-radius: var(--radius); font-family: 'DM Sans', sans-serif; font-size: 16px; font-weight: 600; cursor: pointer; margin-top: 12px; }
  </style>
</head>
<body>
<div class="card">
  <div class="logo">
    <div class="logo-icon">📋</div>
    <h1>CatchNote</h1>
    <p>Konfigurasi Perangkat</p>
  </div>
  <form method="POST" action="/save">
    <input type="hidden" name="wifi_type" id="wifi_type_val" value=")rawhtml" + saved_type_str + R"rawhtml(">
    
    <div class="section-title">📶 Koneksi WiFi</div>
    <div class="toggle-wrap">
      <button type="button" class="toggle-btn )rawhtml" + chk_biasa + R"rawhtml(" onclick="setTipe('biasa', this)">📶 Biasa</button>
      <button type="button" class="toggle-btn )rawhtml" + chk_eduroam + R"rawhtml(" onclick="setTipe('eduroam', this)">🏫 Eduroam</button>
    </div>

    <div class="section )rawhtml" + sec_biasa + R"rawhtml(" id="sec-biasa">
      <div class="field">
        <label>Nama WiFi (SSID)</label>
        <input name="ssid_biasa" placeholder="Nama jaringan" value=")rawhtml" + ((saved_type_str == "biasa") ? saved_ssid_str : "") + R"rawhtml(">
      </div>
      <div class="field">
        <label>Password WiFi</label>
        <input name="wifi_pass" type="password" placeholder="Password jaringan">
      </div>
    </div>

    <div class="section )rawhtml" + sec_eduroam + R"rawhtml(" id="sec-eduroam">
      <div class="field">
        <label>Nama WiFi (SSID)</label>
        <input name="ssid_eduroam" placeholder="eduroam" value=")rawhtml" + ((saved_type_str == "eduroam") ? saved_ssid_str : "") + R"rawhtml(">
      </div>
      <div class="field">
        <label>NIM ITB</label>
        <input name="nim" placeholder="13xxxxxxx" value=")rawhtml" + saved_nim_str + R"rawhtml(">
      </div>
      <div class="field">
        <label>Password SSO</label>
        <input name="eap_pass" type="password" placeholder=")rawhtml" + ((saved_eap_pass_str.length() > 0) ? "•••••••• (Tersimpan)" : "Password SSO ITB") + R"rawhtml(">
      </div>
    </div>

    <div class="section-title"> Sensitivitas Mic</div>
    <div class="threshold-grid">
      <label class="threshold-card"><input type="radio" name="threshold_cat" value="tinggi" )rawhtml" + chkT + R"rawhtml(><span class="threshold-label"><span class="th-icon">🔴</span><span class="th-name">Tinggi</span></span></label>
      <label class="threshold-card"><input type="radio" name="threshold_cat" value="sedang" )rawhtml" + chkS + R"rawhtml(><span class="threshold-label"><span class="th-icon">🟡</span><span class="th-name">Sedang</span></span></label>
      <label class="threshold-card"><input type="radio" name="threshold_cat" value="rendah" )rawhtml" + chkR + R"rawhtml(><span class="threshold-label"><span class="th-icon">🟢</span><span class="th-name">Rendah</span></span></label>
    </div>

    <button type="submit">Simpan &amp; Restart →</button>
  </form>
</div>
<script>
  function setTipe(tipe, btn) {
    document.getElementById('wifi_type_val').value = tipe;
    document.querySelectorAll('.toggle-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    document.querySelectorAll('.section').forEach(s => s.classList.remove('show'));
    document.getElementById('sec-' + tipe).classList.add('show');
  }
</script>
</body>
</html>
)rawhtml";

  server.on("/", HTTP_GET, [&, html]() { server.send(200, "text/html", html); });

  // 3. HANDLER SAVE DENGAN THRESHOLD
  server.on("/save", HTTP_POST, [&]() {
    String wifiType = server.arg("wifi_type");
    String ssid     = (wifiType == "eduroam") ? server.arg("ssid_eduroam") : server.arg("ssid_biasa");
    String eapPass  = server.arg("eap_pass");
    String thresholdCat = server.arg("threshold_cat");

    int thresholdVal = 300;
    if (thresholdCat == "tinggi") thresholdVal = 400;
    else if (thresholdCat == "rendah") thresholdVal = 200;

    preferences.begin("catch_note", false);
    preferences.putString("ssid", ssid);
    preferences.putString("wifi_type", wifiType);
    preferences.putString("nim", server.arg("nim"));
    preferences.putInt("threshold", thresholdVal);
    
    // Jangan overwrite password SSO jika field dikosongkan (artinya user pakai password lama)
    if (eapPass.length() > 0) preferences.putString("eap_pass", eapPass);
    if (server.arg("wifi_pass").length() > 0) preferences.putString("wifi_pass", server.arg("wifi_pass"));
    preferences.end();

    server.send(200, "text/html", "<!DOCTYPE html><html><head><meta charset='UTF-8'><style>body{font-family:'DM Sans',sans-serif;background:#fdf4f7;display:flex;align-items:center;justify-content:center;min-height:100vh;}.box{background:white;border-radius:24px;padding:40px 32px;text-align:center;box-shadow:0 4px 40px rgba(232,99,140,.1);}h2{color:#e8638c;font-size:20px;}</style></head><body><div class='box'><h2>✅ Konfigurasi tersimpan!</h2><p>ESP32 akan restart...</p></div></body></html>");

    unsigned long restartAt = millis() + 2000;
    while (millis() < restartAt) {
      updateBuzzer();
      dnsServer.processNextRequest();
      server.handleClient();
      delay(1);
    }

    ESP.restart();
  });

  // Captive portal detection boilerplate
  auto redirect = [&]() { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); };
  server.on("/generate_204", redirect); server.on("/fwlink", redirect); server.on("/hotspot-detect.html", redirect);
  server.on("/library/test/success.html", redirect); server.on("/ncsi.txt", redirect); server.on("/connecttest.txt", redirect); server.onNotFound(redirect);

  server.begin();
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();

    updateBuzzer();
    handlePortalButton();

    delay(1); // kasih napas ke WiFi stack
  }
}

//  initWifiPortal
void initWifiPortal() {
  pinMode(RESET_BUTTON, INPUT_PULLUP);

  preferences.begin("catch_note", false);

  // Ambil SSID
  String cekSSID = preferences.getString("ssid", "");
  cekSSID.toCharArray(saved_ssid, sizeof(saved_ssid));

  // Ambil konfigurasi WiFi
  preferences.getString("wifi_type", "biasa")
             .toCharArray(wifi_type, sizeof(wifi_type));

  preferences.getString("wifi_pass", "")
             .toCharArray(wifi_pass, sizeof(wifi_pass));

  // Ambil kredensial enterprise
  preferences.getString("nim", "")
             .toCharArray(eap_nim, sizeof(eap_nim));

  preferences.getString("eap_pass", "")
             .toCharArray(eap_pass, sizeof(eap_pass));

  preferences.end();

  Serial.println("\n=== KONFIGURASI ===");
  Serial.printf("SSID      : %s\n", saved_ssid);
  Serial.printf("WiFi Type : %s\n", wifi_type);
  Serial.println("===================\n");

  // Jika belum ada konfigurasi
  if (cekSSID == "" || cekSSID == " " || cekSSID.length() < 2) {
    Serial.println("⚠️ SSID kosong → Masuk Mode Setup...");
    // TAMPILAN LCD UNTUK MODE PORTAL
    lcd.clear();
    lcd.setCursor(0, 0); 
    lcd.print("CONNECT KE WIFI:"); // 16 Karakter
    lcd.setCursor(0, 1); 
    lcd.print("CatchNote Setup"); // 16 Karakter

    bukaPortal();



  } else {
    Serial.println("✅ Config ditemukan, lanjut boot.");
  }
}

// ════════════════════════════════════════════════
//  handleResetButton
// ════════════════════════════════════════════════
// void handleResetButton() {
//   if (digitalRead(RESET_BUTTON) == LOW) {
//     if (!isButtonPressed) {
//       buttonPressTime = millis();
//       isButtonPressed = true;
//       Serial.println("⚠️ Tahan 3 detik untuk reset konfigurasi...");
//     }
//     else if (millis() - buttonPressTime >= 3000) {
//       extern LiquidCrystal_I2C lcd;
//       lcd.clear();
//       lcd.setCursor(0, 0); lcd.print("  RESET CONFIG! ");
//       lcd.setCursor(0, 1); lcd.print("LEPAS TOMBOLNYA!");

//       preferences.begin("catch_note", false);
//       preferences.clear();
//       preferences.end();

//       Serial.println("✅ NVS terhapus. Lepaskan tombol!");
//       while (digitalRead(RESET_BUTTON) == LOW) delay(100);

//       lcd.clear();
//       lcd.setCursor(0, 0); lcd.print("  RESTARTING... ");
//       delay(500);
//       ESP.restart();
//     }
//   } else {
//     isButtonPressed = false;
//   }
// }
