#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_PN532.h>
#include <driver/i2s.h>

// ==========================================
// PIN DEFINITIONS (UPDATE THESE FOR CATCHNOTE)
// ==========================================
#define BUTTON_PIN 27
#define LED1_PIN 25
#define LED2_PIN 26
#define BUZZER_PIN 13

// I2S Microphone Pins (INMP441)
#define I2S_WS 16
#define I2S_SD 4
#define I2S_SCK 17

#define EN_POWER 33

// ==========================================
// OBJECT INSTANTIATION
// ==========================================
Adafruit_MPU6050 mpu;
LiquidCrystal_I2C lcd(0x27, 16, 2); // Check your LCD address (0x27 or 0x3F)
Adafruit_PN532 nfc(-1, -1, &Wire); 

// Variables
float maxGForce = 0.0;
const float SHAKE_THRESHOLD = 3.5; // G-force required to trigger the alarm
bool isShaking = false;

void setup() {
  Serial.begin(115200);

  // Initialize basic I/O
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(EN_POWER, OUTPUT);
  digitalWrite(EN_POWER, HIGH);

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Booting..");

  // Initialize MPU6050
  if (!mpu.begin()) {
    lcd.clear();
    lcd.print("MPU6050 ERR!");
    while (1) { yield(); } // Halt if failed
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G); // Max range for violent shaking

  // Initialize PN532
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    lcd.clear();
    lcd.print("PN532 ERR!");
    while (1) { yield(); }
  }

  // Initialize INMP441 (I2S)
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);

  lcd.clear();
}

void loop() {
  // 1. Read MPU6050
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // 2. Calculate Total Vector Magnitude (in Gs)
  float totalAccel = sqrt(pow(a.acceleration.x, 2) + 
                          pow(a.acceleration.y, 2) + 
                          pow(a.acceleration.z, 2));
  float currentG = totalAccel / 9.81;

  // Track the absolute maximum G-force seen
  if (currentG > maxGForce) {
    maxGForce = currentG;
  }

  // 3. Trigger Alarms on Violent Shake
  if (currentG > SHAKE_THRESHOLD) {
    digitalWrite(LED1_PIN, HIGH);
    digitalWrite(LED2_PIN, LOW);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(50); // Frenetic blinking
    digitalWrite(LED1_PIN, LOW);
    digitalWrite(LED2_PIN, HIGH);
    digitalWrite(BUZZER_PIN, LOW);
  } else {
    digitalWrite(LED1_PIN, LOW);
    digitalWrite(LED2_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // 4. Check INMP441 (Microphone) Integrity
  size_t bytesIn = 0;
  int32_t micData[32];
  esp_err_t micStatus = i2s_read(I2S_NUM_0, &micData, sizeof(micData), &bytesIn, portMAX_DELAY);

  // 5. Check PN532 (RFID) Integrity
  // A quick non-blocking check to ensure the I2C/SPI bus to the RFID hasn't locked up
  uint32_t rfidStatus = nfc.getFirmwareVersion();

  // 6. Update LCD Dashboard
  lcd.setCursor(0, 0);
  lcd.print("Max G: ");
  lcd.print(maxGForce, 1);
  lcd.print("    "); // Clear trailing characters

  lcd.setCursor(0, 1);
  if (!rfidStatus) {
    lcd.print("NFC:ERR ");
  } else {
    lcd.print("NFC:OK  ");
  }
  
  if (micStatus != ESP_OK || bytesIn == 0) {
    lcd.print("MIC:ERR ");
  } else {
    lcd.print("MIC:OK  ");
  }

  // 7. Check Reset Button
  if (digitalRead(BUTTON_PIN) == LOW) {
    maxGForce = 0.0;
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Max G Reset!");
    delay(500); // Debounce
  }

  delay(10); // Small delay for stability
}