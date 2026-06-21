#include "LCD_Helper.h"
#include <LiquidCrystal_I2C.h>

// lcd aslinya tetap didefinisikan di Main.ino
extern LiquidCrystal_I2C lcd;

String animDots() {
  int dotCount = (millis() / 500) % 4; // 0,1,2,3 berbasis waktu (stateless)

  String dots = "";
  for (int i = 0; i < dotCount; i++) {
    dots += ".";
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

// ── Custom char panah bawah (CGRAM slot 0) ──
static byte arrowDown[8] = {
  B00100,
  B00100,
  B00100,
  B00100,
  B10101,
  B01110,
  B00100,
  B00000
};

void lcdInitCustomChars() {
  lcd.createChar(0, arrowDown);
}

// Cetak panah bawah di kolom 0, lalu teks setelahnya (tanpa alokasi String),
// dipadkan spasi sampai 16 kolom.
void lcdPrintArrow16(uint8_t row, const char* textAfterArrow) {
  lcd.setCursor(0, row);
  lcd.write(byte(0));

  uint8_t col = 1;
  while (*textAfterArrow && col < 16) {
    lcd.print(*textAfterArrow);
    textAfterArrow++;
    col++;
  }

  while (col < 16) {
    lcd.print(' ');
    col++;
  }
}

void lcdShowModeMahasiswa() {
  lcdPrint16(0, " MODE:MAHASISWA ");

  lcd.setCursor(0, 1);
  lcd.print("  ");        // kolom 0-1
  lcd.write(byte(0));     // kolom 2: panah bawah custom
  lcd.print(" SCAN KARTU  ");
}

void lcdShowModeOnline() {
  lcdPrint16(0, "  MODE: ONLINE  ");
  lcdPrintArrow16(1, " REGISTER KARTU");
}