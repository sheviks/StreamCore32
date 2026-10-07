// The Print methods Adafruit_GFX.cpp needs.  Linkers without --gc-sections
// (MinGW on Windows) keep code the sim never runs (Adafruit_GFX_Button) and
// want these; Print.cpp itself needs the Arduino core.
#include "Print.h"

size_t Print::write(const uint8_t* buffer, size_t size) {
  size_t n = 0;
  while (size--) {
    if (!write(*buffer++))
      break;
    n++;
  }
  return n;
}

size_t Print::print(const char str[]) { return write(str); }
