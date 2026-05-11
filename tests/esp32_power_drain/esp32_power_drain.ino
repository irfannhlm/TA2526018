#include <WiFi.h>
#include <Wire.h> 
#include <LiquidCrystal_I2C.h>

// Set the LCD address to 0x27 or 0x3F for a 16 chars and 2 line display
LiquidCrystal_I2C lcd(0x27, 16, 2); 

void setup() {
  Serial.begin(115200);

  // 1. Initialize LCD and turn on Backlight (High Current Draw)
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("DRAIN ACTIVE");

  // 2. Maximize CPU and Radio
  setCpuFrequencyMhz(240);
  WiFi.mode(WIFI_STA);
  WiFi.begin("StressTest_SSID", "NoPassword");

  // 3. Launch Core 0 for heavy math
  xTaskCreatePinnedToCore(highLoadTask, "MathTask", 10000, NULL, 1, NULL, 0);
}

void highLoadTask(void * pvParameters) {
  while(true) {
    // Perform heavy math to keep power high
    volatile float dummy = 3.14159 * 2.71828;
    dummy = dummy / 1.234;

    // This tiny delay (1ms) tells the Watchdog "I'm still alive"
    // and lets Core 0 handle background system tasks.
    vTaskDelay(1 / portTICK_PERIOD_MS); 
  }
}
void loop() {
  // Constant LCD Updates (Uses I2C bus and keeps CPU busy)
  lcd.setCursor(0, 1);
  lcd.print("Uptime: ");
  lcd.print(millis() / 1000);
  lcd.print("s   ");

  // Toggle Internal LED
  digitalWrite(2, HIGH);
  
  // High-intensity Wi-Fi Scanning
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.scanNetworks(true); 
  }

  delay(10); // Short delay to prevent WDT issues while keeping activity high
  digitalWrite(2, LOW);
}