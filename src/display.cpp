#include "display.h"
#include <LiquidCrystal.h>
#include <stdio.h>
#include <stdarg.h>

// Create LiquidCrystal instance using 4-bit wiring (RS, EN, D4, D5, D6, D7)
static LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// Helper that prints a formatted string to a specific LCD line (0 or 1),
// trims to 16 characters and pads the rest with spaces to clear leftovers.
static void lcd_printf_line(uint8_t line, const char* fmt, ...) {
  char buf[17];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n < 0) return;               // formatting error, do nothing
  if (n > 16) n = 16;              // limit to display width
  for (int i = n; i < 16; ++i) buf[i] = ' '; // pad with spaces
  buf[16] = '\0';

  if (line > 1) line = 1;          // clamp to available lines
  lcd.setCursor(0, line);
  lcd.print(buf);
}

void display_init() {
  lcd.begin(16, 2);
  lcd.clear();
  lcd_printf_line(0, "Starting...");
  lcd_printf_line(1, "");
}

void display_update_batt_soc(float soc) {
  if (isnan(soc)) {
    // No valid data
    lcd_printf_line(0, "Batt SOC:   --.-%%");
  } else {
    // Example: "Batt SOC: 62.3%"
    lcd_printf_line(0, "Batt SOC: %5.1f%%", soc);
  }
}

void display_update_temperature(float temp_c) {
  if (isnan(temp_c)) {
    // Show placeholder when reading is invalid
    lcd_printf_line(1, "Temp:   --.-C");
  } else {
    // Example: "Temp:   23.4C"
    lcd_printf_line(1, "Temp: %6.1fC", temp_c);
  }
}

void display_update_button0(uint16_t value) {
  // Example: "Button0:  1234"
  lcd_printf_line(1, "Button0: %5u", (unsigned)value);
}
