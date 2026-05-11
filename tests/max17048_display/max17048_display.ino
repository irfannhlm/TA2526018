#include <LiquidCrystal_I2C.h>
#include "Adafruit_MAX1704X.h"

Adafruit_MAX17048 maxlipo;
LiquidCrystal_I2C lcd(0x27,16,2);  // set the LCD address to 0x27 for a 16 chars and 2 line display

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();

  while (!maxlipo.begin()) {
    Serial.println(F("Couldnt find Adafruit MAX17048?\nMake sure a battery is plugged in!"));
    delay(2000);
  }
  Serial.print(F("Found MAX17048"));
  Serial.print(F(" with Chip ID: 0x")); 
  Serial.println(maxlipo.getChipID(), HEX);
}

void loop() {
  float cellVoltage = maxlipo.cellVoltage();
  float cellPercent = maxlipo.cellPercent();
  float chargeRate = maxlipo.chargeRate();
  if (isnan(cellVoltage)) {
    Serial.println("Failed to read cell voltage, check battery is connected!");
    delay(2000);
    return;
  }

  // Print readings to serial
  Serial.print(F("Batt Voltage: ")); Serial.print(cellVoltage, 3); Serial.println(" V");
  Serial.print(F("Batt Percent: ")); Serial.print(cellPercent, 1); Serial.println(" %");
  Serial.print(F("(Dis)Charge rate : ")); Serial.print(chargeRate, 1); Serial.println(" %/hr");

  // Print readings to LCD
  lcd.setCursor(0, 0);
  lcd.print(cellVoltage, 3); lcd.print("V");
  lcd.print(" ("); lcd.print(cellPercent, 1); lcd.print("%)");

  lcd.setCursor(0, 1);
  lcd.print("Rate:"); lcd.print(chargeRate, 1); lcd.print("%/hr");

  delay(2000);  // dont query too often!
}