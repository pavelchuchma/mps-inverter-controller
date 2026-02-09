#include "display.h"
#include <LiquidCrystal.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LCD_COLS  16
#define LCD_LINES 2

// Create LiquidCrystal instance using 4-bit wiring (RS, EN, D4, D5, D6, D7)
static LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// --- Scrollable row system ---
static char display_rows[DISPLAY_MAX_ROWS][LCD_COLS + 1]; // logical row data
static uint8_t display_row_count = 0;
static uint8_t display_scroll_pos = 0;

// Buffer of what's currently shown on the LCD (for diff-based redraw)
static char lcd_buffer[LCD_LINES][LCD_COLS + 1];

// Write a single padded line to LCD hardware (no diff check).
static void lcd_write_line(uint8_t line, const char* text) {
  lcd.setCursor(0, line);
  lcd.print(text);
}

// Pad src into dst (LCD_COLS chars + null). dst must be at least LCD_COLS+1.
static void pad_to_lcd(char* dst, const char* src) {
  int len = strlen(src);
  if (len > LCD_COLS) len = LCD_COLS;
  memcpy(dst, src, len);
  for (int i = len; i < LCD_COLS; i++) dst[i] = ' ';
  dst[LCD_COLS] = '\0';
}

// --- Public API ---

void display_init() {
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
  GPIO_FAST_OUTPUT_ENABLE(LCD_BACKLIGHT_PIN);
  lcd.begin(LCD_COLS, LCD_LINES);
  lcd.clear();
  memset(display_rows, 0, sizeof(display_rows));
  memset(lcd_buffer, 0, sizeof(lcd_buffer));
}

void display_set_row_count(uint8_t count) {
  if (count > DISPLAY_MAX_ROWS) count = DISPLAY_MAX_ROWS;
  display_row_count = count;
  if (display_scroll_pos >= count) display_scroll_pos = 0;
}

void display_set_row(uint8_t index, const char* text) {
  if (index >= DISPLAY_MAX_ROWS) return;
  strncpy(display_rows[index], text, LCD_COLS);
  display_rows[index][LCD_COLS] = '\0';
}

void display_redraw() {
  if (display_row_count == 0) return;

  for (uint8_t i = 0; i < LCD_LINES; i++) {
    uint8_t row_idx = (display_scroll_pos + i) % display_row_count;
    char padded[LCD_COLS + 1];
    pad_to_lcd(padded, display_rows[row_idx]);

    if (strcmp(padded, lcd_buffer[i]) != 0) {
      memcpy(lcd_buffer[i], padded, LCD_COLS + 1);
      lcd_write_line(i, padded);
    }
  }
}

void display_scroll_up() {
  if (display_row_count == 0) return;
  display_scroll_pos = (display_scroll_pos + display_row_count - 1) % display_row_count;
  display_redraw();
}

void display_scroll_down() {
  if (display_row_count == 0) return;
  display_scroll_pos = (display_scroll_pos + 1) % display_row_count;
  display_redraw();
}

// Printf-style line write (bypasses row system, e.g. for WiFi connect screen).
// Also invalidates lcd_buffer for that line so next display_redraw() will refresh it.
void lcd_printf_line(uint8_t line, const char* fmt, ...) {
  char buf[LCD_COLS + 1];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n < 0) return;
  if (n > LCD_COLS) n = LCD_COLS;
  for (int i = n; i < LCD_COLS; ++i) buf[i] = ' ';
  buf[LCD_COLS] = '\0';

  if (line > 1) line = 1;
  lcd.setCursor(0, line);
  lcd.print(buf);

  // Invalidate buffer so display_redraw() will overwrite this transient message
  lcd_buffer[line][0] = '\0';
}

// --- Backlight control ---
static int16_t backlightOffCounter = 0;

inline boolean isBacklightOn() {
  return GPIO_FAST_GET_LEVEL(LCD_BACKLIGHT_PIN);
}

void displayBacklightOn() {
  backlightOffCounter = 0;
  GPIO_FAST_SET_1(LCD_BACKLIGHT_PIN);
}

void checkDisplayBacklightTimeout() {
  if (isBacklightOn()) {
    if (backlightOffCounter++ >= 10) {
      GPIO_FAST_SET_0(LCD_BACKLIGHT_PIN);
    }
  }
}
