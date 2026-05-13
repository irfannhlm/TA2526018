// Wifi_Management.cpp

#include "Wifi_Management.h"
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LiquidCrystal_I2C.h>
#include "Config.h"

extern LiquidCrystal_I2C lcd;

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

// ════════════════════════════════════════════════
//  bukaPortal
// ════════════════════════════════════════════════
void bukaPortal() {
  Serial.println("🌐 Membuka portal konfigurasi...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CatchNote-Setup");
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("📡 Portal aktif! IP: %s\n", apIP.toString().c_str());
  dnsServer.start(DNS_PORT, "*", apIP);

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
      --pink: #e8638c; --pink-lt: #fde8ef; --text: #2d1a22;
      --muted: #9c7585; --bg: #fdf4f7; --card: #ffffff;
      --border: #f0d0dc; --radius: 14px;
    }
    body {
      font-family: 'DM Sans', sans-serif; background: var(--bg);
      min-height: 100vh; display: flex; align-items: center;
      justify-content: center; padding: 24px 16px;
      background-image:
        radial-gradient(circle at 20% 20%, #fce4ed 0%, transparent 50%),
        radial-gradient(circle at 80% 80%, #fde8ef 0%, transparent 50%);
    }
    .card {
      background: var(--card); border-radius: 24px; padding: 36px 32px;
      width: 100%; max-width: 420px;
      box-shadow: 0 4px 40px rgba(232,99,140,0.10); border: 1px solid var(--border);
    }
    .logo { text-align: center; margin-bottom: 28px; }
    .logo-icon {
      width: 56px; height: 56px;
      background: linear-gradient(135deg, #e8638c, #f598b4);
      border-radius: 16px; display: inline-flex; align-items: center;
      justify-content: center; font-size: 26px; margin-bottom: 12px;
      box-shadow: 0 6px 20px rgba(232,99,140,0.30);
    }
    .logo h1 { font-family: 'DM Serif Display', serif; font-size: 22px; color: var(--text); }
    .logo p  { font-size: 13px; color: var(--muted); margin-top: 4px; }
    .toggle-wrap {
      display: flex; background: var(--pink-lt); border-radius: 12px;
      padding: 4px; margin-bottom: 24px; gap: 4px;
    }
    .toggle-btn {
      flex: 1; padding: 10px; border: none; border-radius: 9px;
      font-family: 'DM Sans', sans-serif; font-size: 14px; font-weight: 500;
      cursor: pointer; transition: all 0.2s; background: transparent; color: var(--muted);
    }
    .toggle-btn.active { background: white; color: var(--pink); box-shadow: 0 2px 8px rgba(232,99,140,0.15); }
    .section { display: none; } .section.show { display: block; }
    .field { margin-bottom: 16px; }
    .field label {
      display: block; font-size: 12px; font-weight: 600; color: var(--muted);
      text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 6px;
    }
    .field input {
      width: 100%; padding: 12px 14px; border: 1.5px solid var(--border);
      border-radius: var(--radius); font-family: 'DM Sans', sans-serif;
      font-size: 15px; color: var(--text); background: #fff;
      transition: border-color 0.2s, box-shadow 0.2s; outline: none;
    }
    .field input:focus { border-color: var(--pink); box-shadow: 0 0 0 3px rgba(232,99,140,0.10); }
    .divider {
      display: flex; align-items: center; gap: 10px; margin: 20px 0;
      color: var(--muted); font-size: 12px;
    }
    .divider::before, .divider::after { content: ''; flex: 1; height: 1px; background: var(--border); }
    .hint {
      background: var(--pink-lt); border-radius: 10px; padding: 10px 14px;
      font-size: 12.5px; color: var(--pink); margin-bottom: 16px; line-height: 1.5;
    }
    button[type=submit] {
      width: 100%; padding: 14px;
      background: linear-gradient(135deg, #e8638c, #f598b4);
      color: white; border: none; border-radius: var(--radius);
      font-family: 'DM Sans', sans-serif; font-size: 16px; font-weight: 600;
      cursor: pointer; margin-top: 8px; box-shadow: 0 4px 16px rgba(232,99,140,0.35);
      transition: opacity 0.2s, transform 0.1s;
    }
    button[type=submit]:hover  { opacity: 0.92; }
    button[type=submit]:active { transform: scale(0.98); }
  </style>
</head>
<body>
<div class="card">
  <div class="logo">
    <div class="logo-icon">📋</div>
    <h1>Catch Note</h1>
    <p>Konfigurasi perangkat ESP32</p>
  </div>

  <form method="POST" action="/save">
    <input type="hidden" name="wifi_type" id="wifi_type_val" value="biasa">

    <div class="toggle-wrap">
      <button type="button" class="toggle-btn active" onclick="setTipe('biasa', this)">📶 WiFi Biasa</button>
      <button type="button" class="toggle-btn"        onclick="setTipe('eduroam', this)">🏫 Eduroam ITB</button>
    </div>

    <div class="section show" id="sec-biasa">
      <div class="field">
        <label>Nama WiFi (SSID)</label>
        <input name="ssid_biasa" placeholder="Nama jaringan WiFi">
      </div>
      <div class="field">
        <label>Password WiFi</label>
        <input name="wifi_pass" type="password" placeholder="Password jaringan">
      </div>
    </div>

    <div class="section" id="sec-eduroam">
      <div class="hint">Gunakan akun SSO ITB. SSID biasanya <b>eduroam</b>.</div>
      <div class="field">
        <label>Nama WiFi (SSID)</label>
        <input name="ssid_eduroam" placeholder="eduroam">
      </div>
      <div class="field">
        <label>NIM ITB</label>
        <input name="nim" placeholder="13xxxxxxx (tanpa @itb.ac.id)">
      </div>
      <div class="field">
        <label>Password SSO</label>
        <input name="eap_pass" type="password" placeholder="Password SSO ITB">
      </div>
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

  server.on("/", HTTP_GET, [&, html]() {
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, [&]() {
    String wifiType = server.arg("wifi_type");
    String ssid     = (wifiType == "eduroam")
                      ? server.arg("ssid_eduroam")
                      : server.arg("ssid_biasa");

    preferences.begin("catch_note", false);
    preferences.putString("ssid",        ssid);
    preferences.putString("wifi_type",   wifiType);
    preferences.putString("wifi_pass",   server.arg("wifi_pass"));
    preferences.putString("nim",         server.arg("nim"));
    preferences.putString("eap_pass",    server.arg("eap_pass"));
    preferences.end();

    Serial.println("✅ Config tersimpan:");
    Serial.println("   SSID        : " + ssid);
    Serial.println("   WiFi Type   : " + wifiType);

    server.send(200, "text/html",
      "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
      "<style>body{font-family:'DM Sans',sans-serif;background:#fdf4f7;"
      "display:flex;align-items:center;justify-content:center;min-height:100vh;}"
      ".box{background:white;border-radius:24px;padding:40px 32px;text-align:center;"
      "box-shadow:0 4px 40px rgba(232,99,140,.1);border:1px solid #f0d0dc;}"
      ".icon{font-size:48px;margin-bottom:16px;}"
      "h2{color:#e8638c;font-size:20px;margin-bottom:8px;}p{color:#9c7585;font-size:14px;}"
      "</style></head><body><div class='box'>"
      "<div class='icon'>✅</div>"
      "<h2>Konfigurasi tersimpan!</h2>"
      "<p>ESP32 akan restart dan connect ke HiveMQ...</p>"
      "</div></body></html>");
    delay(2000);
    ESP.restart();
  });

  // Captive portal detection — semua OS
  auto redirect = [&]() { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); };
  server.on("/generate_204",              HTTP_GET, redirect);
  server.on("/fwlink",                    HTTP_GET, redirect);
  server.on("/hotspot-detect.html",       HTTP_GET, redirect);
  server.on("/library/test/success.html", HTTP_GET, redirect);
  server.on("/ncsi.txt",                  HTTP_GET, redirect);
  server.on("/connecttest.txt",           HTTP_GET, redirect);
  server.on("/redirect",                  HTTP_GET, redirect);
  server.onNotFound(redirect);

  server.begin();
  Serial.println("✅ Portal aktif. Menunggu konfigurasi...");

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
  }
}

// ════════════════════════════════════════════════
//  initWifiPortal
// ════════════════════════════════════════════════
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
void handleResetButton() {
  if (digitalRead(RESET_BUTTON) == LOW) {
    if (!isButtonPressed) {
      buttonPressTime = millis();
      isButtonPressed = true;
      Serial.println("⚠️ Tahan 3 detik untuk reset konfigurasi...");
    }
    else if (millis() - buttonPressTime >= 3000) {
      extern LiquidCrystal_I2C lcd;
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("  RESET CONFIG! ");
      lcd.setCursor(0, 1); lcd.print("LEPAS TOMBOLNYA!");

      preferences.begin("catch_note", false);
      preferences.clear();
      preferences.end();

      Serial.println("✅ NVS terhapus. Lepaskan tombol!");
      while (digitalRead(RESET_BUTTON) == LOW) delay(100);

      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("  RESTARTING... ");
      delay(500);
      ESP.restart();
    }
  } else {
    isButtonPressed = false;
  }
}
