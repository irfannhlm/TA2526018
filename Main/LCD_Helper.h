#pragma once

#include <Arduino.h>

String animDots();
String twoDigit(int value);
void lcdPrint16(uint8_t row, String text);

// Custom char panah bawah + tampilan mode
void lcdInitCustomChars();
void lcdPrintArrow16(uint8_t row, const char* textAfterArrow);
void lcdShowModeMahasiswa();
void lcdShowModeOnline();