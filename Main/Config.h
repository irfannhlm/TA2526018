// Config.h
#ifndef CONFIG_H
#define CONFIG_H

// I2C Pins (Shared: LCD, MPU6050, PN532)
#define I2C_SDA 21
#define I2C_SCL 22

// I2S Pins (INMP441)
#define I2S_WS 16
#define I2S_SD 4
#define I2S_SCK 17

// SPI Pins (SD Card Module)
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_CS 5

// Extra Pins
#define BUZZER_PIN 13 // Pin untuk indikator audio
#define BUZZER_TONE_HZ 2700
#define BUTTON_PIN 27  // Button untuk switching mode
#define LED_BATTERY 25
#define LED_WIFI    26
#define EN_POWER 33

// Audio & SD Config
#define SAMPLE_RATE       16000 
#define I2S_READ_LEN      1024 
#define BIT_SHIFT         14
// #define MAX_DURASI_DETIK  300
#define PRE_BUFFER_SEC    1.5     
#define PRE_BUFFER_SIZE   (SAMPLE_RATE * PRE_BUFFER_SEC)
// #define MAX_RECORD_SECONDS 300
// #define MAX_RECORD_MS (MAX_RECORD_SECONDS * 1000UL)
#define DOSEN_COUNTDOWN 6 // Jeda 6 detik
#define TIMEOUT_BICARA    60000

// Throw Detection Config
#define THROW_LOW_G_THRESHOLD 0.35f
#define THROW_EXIT_LOW_G 0.65f
#define THROW_MIN_LOW_G_MS 90UL
#define THROW_MIN_FLIGHT_MS 200UL
#define THROW_MIN_IMPACT_DELAY_MS 60UL
#define THROW_CLEAN_LOW_G_THRESHOLD 0.28f
#define THROW_SPIN_DPS 360.0f
#define THROW_MIN_SPIN_MS 80UL
#define THROW_SPIN_MAX_G 1.60f
#define THROW_SPIN_RELEASE_G 1.05f
#define THROW_SPIN_MIN_LOW_G_MS 40UL
#define THROW_STRONG_SPIN_DPS 700.0f
#define THROW_MIN_STRONG_SPIN_MS 60UL
#define THROW_STRONG_SPIN_MAX_G 2.20f
#define THROW_STRONG_SPIN_RELEASE_G 1.35f
#define THROW_IMPACT_G 2.25f
#define THROW_IMPACT_RISE_G 0.45f
#define THROW_HARD_IMPACT_G 3.20f
#define THROW_IMPACT_WINDOW_MS 800UL
#define THROW_SAMPLE_INTERVAL_MS 10UL

// MQTT CONFIG
#define MQTT_PORT 8883           
#define DEVICE_ID "1"
#define MQTT_SERVER "c4bbf4787735464dadc96ca13e4a9c6b.s1.eu.hivemq.cloud"
#define MQTT_USER "catchnote"
#define MQTT_PASS "Ta2526018"
#define MQTT_TOPIC_REG         "kelas/alat/rfid"
#define MQTT_TOPIC_STATUS      "kelas/alat/status"
#define MQTT_TOPIC_PERINTAH    "kelas/alat/perintah"
#define MQTT_TOPIC_AUDIO_DATA  "kelas/alat/audio_data"
#define MQTT_TOPIC_SYNC_STATUS "kelas/alat/sync_status"

// Wifi & MQTT Management
extern char eap_nim[40];
extern char eap_pass[40];
extern char saved_ssid[40];
extern char wifi_type[20];
extern char wifi_pass[64];

// Battery Status
extern float batteryPercent;
extern unsigned long maxRecordMs;
extern unsigned long lastWifiAttempt;
extern bool wifiDibatalkan;

#endif
