#include "LCD_Helper.h"
#include <LiquidCrystal_I2C.h>

// lcd aslinya tetap didefinisikan di Main.ino
extern LiquidCrystal_I2C lcd;

String animDots() {
  static unsigned long lastAnim = 0;
  static int dotCount = 0;
  static String dots = "";

  unsigned long now = millis();

  if (now - lastAnim >= 500) {
    lastAnim = now;

    dotCount = (dotCount + 1) % 4; // 0, 1, 2, 3, balik 0

    dots = "";
    for (int i = 0; i < dotCount; i++) {
      dots += ".";
    }
  }

  return dots;
}

String twoDigit(int value) {
  if (value < 10) return "0" + String(value);
  return String(value);
}

void lcdPrint16(uint8_t row, String text) {
  if (text.length() > 16) {
    text = text.substring(0, 16);
  }

  while (text.length() < 16) {
    text += " ";
  }

  lcd.setCursor(0, row);
  lcd.print(text);
}