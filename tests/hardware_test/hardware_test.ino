#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_MAX1704X.h"

// --- Pin Definitions ---
#define BUTTON_PIN GPIO_NUM_4  // Must use GPIO_NUM_X for sleep functions
#define POWER_LED_PIN 16
#define WARN_LED_PIN 17
#define BUZZER_PIN 13

// --- Timing Constants ---
const unsigned long HOLD_TIME = 2000; 
const unsigned long debounceDelay = 50;

// --- Objects ---
LiquidCrystal_I2C lcd(0x27, 16, 2); 
Adafruit_MPU6050 mpu;
Adafruit_MAX17048 maxlipo;

// --- State Variables ---
// RTC_DATA_ATTR saves this variable in RTC memory so it persists after waking up
RTC_DATA_ATTR int displayMode = 0; 

bool lastButtonState = HIGH;
unsigned long buttonPressedTime = 0;
bool isHolding = false;
unsigned long lastDisplayUpdate = 0;

void enterDeepSleep() {
  lcd.clear();
  lcd.print("Sleeping...");
  delay(1000);
  lcd.noBacklight();
  lcd.noDisplay(); // Turn off LCD pixels

  // 1. Turn off peripherals
  digitalWrite(POWER_LED_PIN, LOW);
  digitalWrite(WARN_LED_PIN, LOW);
  noTone(BUZZER_PIN);
  
  // 2. Put MPU6050 to sleep
  mpu.enableSleep(true); 

  // 3. Configure Wakeup
  // Wake up when the button (GPIO 4) is pressed (LOW)
  esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0); 

  Serial.println("Going to sleep now");
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  
  pinMode(POWER_LED_PIN, OUTPUT);
  pinMode(WARN_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(POWER_LED_PIN, HIGH);
  Wire.begin(21, 22); 
  lcd.init();
  lcd.backlight();
  
  // Check why we woke up
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    lcd.print("Waking up...");
    delay(500);
  }

  if (!mpu.begin()) { while (1); }
  maxlipo.begin();
  lcd.clear();
}

void loop() {
  bool currentButtonState = digitalRead(BUTTON_PIN);

  // --- Button Logic ---
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    buttonPressedTime = millis();
    isHolding = true;
  } 
  else if (lastButtonState == LOW && currentButtonState == HIGH) {
    unsigned long holdDuration = millis() - buttonPressedTime;
    
    if (holdDuration < HOLD_TIME && holdDuration > debounceDelay) {
      // Short press toggles mode
      displayMode = !displayMode;
      lcd.clear();
    }
    isHolding = false;
  }

  // --- Long Press to Sleep ---
  if (isHolding && (millis() - buttonPressedTime > HOLD_TIME)) {
    enterDeepSleep();
  }

  lastButtonState = currentButtonState;

  // --- Standard Hardware Logic ---
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  
  if (abs(a.acceleration.x) > 5.0 || abs(a.acceleration.y) > 5.0) {
    digitalWrite(WARN_LED_PIN, HIGH);
    tone(BUZZER_PIN, 1000);
  } else {
    digitalWrite(WARN_LED_PIN, LOW);
    noTone(BUZZER_PIN);
  }

  if (millis() - lastDisplayUpdate > 250) {
    if (displayMode == 0) {
      lcd.setCursor(0, 0);
      lcd.print("GyroX: "); lcd.print(g.gyro.x, 2);
      lcd.setCursor(0, 1);
      lcd.print("GyroY: "); lcd.print(g.gyro.y, 2);
    } else {
      lcd.setCursor(0, 0);
      lcd.print("Bat: "); lcd.print(maxlipo.cellPercent(), 1); lcd.print("%");
      lcd.setCursor(0, 1);
      lcd.print("Vol: "); lcd.print(maxlipo.cellVoltage(), 2); lcd.print("V");
    }
    lastDisplayUpdate = millis();
  }
}