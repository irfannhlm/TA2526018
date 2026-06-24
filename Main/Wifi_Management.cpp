// Wifi_Management.cpp

#include "Wifi_Management.h"
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LiquidCrystal_I2C.h>
#include "Config.h"
#include "Buzzer_Module.h"
#include "LCD_Helper.h"
#include "I2C_Handler.h"

extern LiquidCrystal_I2C lcd;
Preferences preferences;
WebServer   server(80);
DNSServer   dnsServer;
const byte  DNS_PORT = 53;

static bool portalActive = false;
static bool portalRoutesReady = false;
static String portalHtml = "";

char eap_nim[40]    = "";
char eap_pass[40]   = "";
char saved_ssid[40] = "";
char wifi_type[20]  = "biasa";
char wifi_pass[64]  = "";

bool isWifiPortalActive() {
  return portalActive;
}

//  bukaPortal
void bukaPortal() {

  WiFi.mode(WIFI_AP);
  WiFi.softAP("CatchNote1");
  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(DNS_PORT, "*", apIP);

  // HTML portal (dengan pre-fill dari Preferences) hanya dibangun sekali per boot.
  // Nilai pre-fill cuma berubah lewat /save, dan /save selalu diakhiri ESP.restart(),
  if (!portalRoutesReady) {
    // 1. BACA DATA TERAKHIR UNTUK PRE-FILL
    preferences.begin("catch_note", true);
    String saved_nim_str      = preferences.getString("nim", "");
    String saved_eap_pass_str = preferences.getString("eap_pass", "");
    String saved_ssid_str     = preferences.getString("ssid", ""); 
    String saved_type_str     = preferences.getString("wifi_type", "biasa");
    preferences.end();

    String chk_biasa   = (saved_type_str == "biasa")   ? "active" : "";
    String chk_eduroam = (saved_type_str == "eduroam") ? "active" : "";
    String sec_biasa   = (saved_type_str == "biasa")   ? "show"   : "";
    String sec_eduroam = (saved_type_str == "eduroam") ? "show"   : "";

  // 2. BANGUN HTML DENGAN PRE-FILL
  portalHtml = R"rawhtml(
<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Catch Note Setup</title>
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    :root {
      --pink: #e8638c; --pink-lt: #fde8ef; --pink-md: #f5b8cc; --text: #2d1a22;
      --muted: #9c7585; --bg: #fdf4f7; --card: #ffffff;
      --border: #f0d0dc; --radius: 14px;
    }
    body {
      font-family: Arial, sans-serif; background: var(--bg);
      min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 24px 16px;
      background-image: radial-gradient(circle at 20% 20%, #fce4ed 0%, transparent 50%), radial-gradient(circle at 80% 80%, #fde8ef 0%, transparent 50%);
    }
    .card { background: var(--card); border-radius: 24px; padding: 36px 32px; width: 100%; max-width: 420px; box-shadow: 0 4px 40px rgba(232,99,140,0.10); border: 1px solid var(--border); }
    .logo { text-align: center; margin-bottom: 28px; }
    .logo-icon { width: 56px; height: 56px; background: linear-gradient(135deg, #e8638c, #f598b4); border-radius: 16px; display: inline-flex; align-items: center; justify-content: center; font-size: 26px; margin-bottom: 12px; box-shadow: 0 6px 20px rgba(232,99,140,0.30); }
    .logo h1 { font-family: Georgia, serif; font-size: 22px; color: var(--text); }
    .logo p  { font-size: 13px; color: var(--muted); margin-top: 4px; }
    .section-title { font-size: 11px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: 1px; margin: 24px 0 14px; display: flex; align-items: center; gap: 8px; }
    .section-title::after { content: ''; flex: 1; height: 1px; background: var(--border); }
    .toggle-wrap { display: flex; background: var(--pink-lt); border-radius: 12px; padding: 4px; margin-bottom: 20px; gap: 4px; }
    .toggle-btn { flex: 1; padding: 10px; border: none; border-radius: 9px; font-family: Arial, sans-serif; font-size: 14px; font-weight: 500; cursor: pointer; transition: all 0.2s; background: transparent; color: var(--muted); }
    .toggle-btn.active { background: white; color: var(--pink); box-shadow: 0 2px 8px rgba(232,99,140,0.15); }
    .section { display: none; } .section.show { display: block; }
    .field { margin-bottom: 16px; }
    .field label { display: block; font-size: 12px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 6px; }
    .field input { width: 100%; padding: 12px 14px; border: 1.5px solid var(--border); border-radius: var(--radius); font-family: Arial, sans-serif; font-size: 15px; color: var(--text); background: #fff; outline: none; }
    .field input:focus { border-color: var(--pink); box-shadow: 0 0 0 3px rgba(232,99,140,0.10); }
    button[type=submit] { width: 100%; padding: 14px; background: linear-gradient(135deg, #e8638c, #f598b4); color: white; border: none; border-radius: var(--radius); font-family: Arial, sans-serif; font-size: 16px; font-weight: 600; cursor: pointer; margin-top: 12px; }
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

    server.on("/", HTTP_GET, []() {
      server.send(200, "text/html", portalHtml);
    });

    // 3. HANDLER SAVE WIFI
    server.on("/save", HTTP_POST, []() {
    String wifiType = server.arg("wifi_type");
    String ssid     = (wifiType == "eduroam") ? server.arg("ssid_eduroam") : server.arg("ssid_biasa");
    String eapPass  = server.arg("eap_pass");
    preferences.begin("catch_note", false);
    preferences.putString("ssid", ssid);
    preferences.putString("wifi_type", wifiType);
    preferences.putString("nim", server.arg("nim"));
    
    // Jangan overwrite password SSO jika field dikosongkan
    if (eapPass.length() > 0) preferences.putString("eap_pass", eapPass);
    if (server.arg("wifi_pass").length() > 0) preferences.putString("wifi_pass", server.arg("wifi_pass"));
    preferences.end();

    server.send(200, "text/html", "<!DOCTYPE html><html><head><meta charset='UTF-8'><style>body{font-family:Arial,sans-serif;background:#fdf4f7;display:flex;align-items:center;justify-content:center;min-height:100vh;}.box{background:white;border-radius:24px;padding:40px 32px;text-align:center;box-shadow:0 4px 40px rgba(232,99,140,.1);}h2{color:#e8638c;font-size:20px;}</style></head><body><div class='box'><h2>✅ Konfigurasi tersimpan!</h2><p>ESP32 akan restart...</p></div></body></html>");

    if (lockI2C(50)) {
      lcd.clear();
      lcdPrint16(0, "KONFIG TERSIMPAN");
      lcdPrint16(1, "RESTART SISTEM");
      unlockI2C();
    }

    unsigned long restartAt = millis() + 2000;
    while (millis() < restartAt) {
      updateBuzzer();
      dnsServer.processNextRequest();
      server.handleClient();
      delay(1);
    }

    ESP.restart();
  });

    auto servePortal = []() {
      server.send(200, "text/html", portalHtml);
    };

    server.on("/favicon.ico", HTTP_GET, []() {
      server.send(204, "image/x-icon", "");
    });
    server.on("/generate_204", HTTP_GET, servePortal);
    server.on("/fwlink", HTTP_GET, servePortal);
    server.on("/hotspot-detect.html", HTTP_GET, servePortal);
    server.on("/library/test/success.html", HTTP_GET, servePortal);
    server.on("/ncsi.txt", HTTP_GET, servePortal);
    server.on("/connecttest.txt", HTTP_GET, servePortal);
    server.onNotFound([]() {
      if (server.method() == HTTP_GET) {
        server.send(200, "text/html", portalHtml);
      } else {
        server.send(404, "text/plain", "");
      }
    });

    portalRoutesReady = true;
  }

  server.begin();
  portalActive = true;

}

void handleWifiPortal() {
  if (!portalActive) return;
  dnsServer.processNextRequest();
  server.handleClient();
  yield();
}

void stopWifiPortal() {
  if (!portalActive) return;

  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);

  portalActive = false;
}

//  initWifiPortal
void initWifiPortal() {
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

  if (cekSSID == "" || cekSSID == " " || cekSSID.length() < 2) {

    saved_ssid[0] = '\0';
    wifi_pass[0]  = '\0';

    lcd.clear();
    lcdPrint16(0, " MODE: OFFLINE ");
    lcdPrint16(1, "WIFI BELUM DISET");
    delay(1500);
  }
}
