#include "display.h"
#include <LiquidCrystal.h>

// Create LiquidCrystal instance using 4-bit wiring (RS, EN, D4, D5, D6, D7)
static LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

void display_init() {
  lcd.begin(16, 2);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Starting...");
  lcd.setCursor(0, 1);
  lcd.print("                ");
}

void display_update_batt_soc(float soc) {
  char buf[17];
  // Format like: "Batt SOC: 62.3%"
  // Ensure we don't overflow 16 chars; then pad with spaces to clear old text.
  int n = snprintf(buf, sizeof(buf), "Batt SOC: %5.1f%%", soc);
  if (n < 0) return;
  if (n > 16) n = 16;
  // Pad remainder with spaces
  for (int i = n; i < 16; ++i) buf[i] = ' ';
  buf[16] = '\0';

  lcd.setCursor(0, 0);
  lcd.print(buf);
}
