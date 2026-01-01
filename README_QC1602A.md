### LCD QC1602A — direct wiring (4-bit mode) according to `src/display.cpp`

This project uses a 16×2 character LCD QC1602A (HD44780-compatible controller) in 4‑bit parallel mode via the `LiquidCrystal` library.

### Pin map (ESP32 Feather ↔ QC1602A)
Wire the standard QC1602A pins (1–16) as follows:

- 1 (VSS) → GND
- 2 (VDD) → +5 V
- 3 (VO)  → wiper of a 10 kΩ trimmer for contrast; trimmer ends to +5 V and GND
- 4 (RS)  → ESP32 GPIO 21
- 5 (R/W) → GND
- 6 (E)   → ESP32 GPIO 22
- 7 (D0)  → not used (leave unconnected)
- 8 (D1)  → not used
- 9 (D2)  → not used
- 10 (D3) → not used
- 11 (D4) → ESP32 GPIO 19
- 12 (D5) → ESP32 GPIO 18
- 13 (D6) → ESP32 GPIO 5
- 14 (D7) → ESP32 GPIO 23
- 15 (LED+, A) → +5 V through a resistor if the module doesn’t have an onboard series resistor (many do — check the marking)
- 16 (LED−, K) → GND

Notes:
- ESP32 GND and LCD GND must be common.
- Tie the `R/W` pin to GND; the display is only written to.

### Power, logic levels, and recommendations
- Power the display with +5 V (VDD = 5 V) in most cases. Some modules work at 3.3 V, but contrast and backlight may be limited.
- ESP32 logic levels are 3.3 V. In practice they often work with HD44780 inputs even at VDD=5 V, but it’s not guaranteed by the manufacturer.
  - Recommendation: use a level shifter (e.g., 74HCT245/74HCT541, or MOSFET/buffer) between the ESP32 and the LCD inputs RS, E, D4–D7.
  - If you connect “directly” without a shifter, it’s at your own risk — it usually works, but may be less robust.

### Contrast and backlight
- Contrast: 10 kΩ trimmer between +5 V and GND; wiper to pin 3 (VO). On first power‑up, set the contrast so characters are visible (or dark blocks before init).
- Backlight: for pins 15/16, check whether the module already includes a series resistor. If not, add e.g. 100–220 Ω in series with LED+ (pin 15).

### Quick functionality check
After flashing the firmware, `display_init()` initializes the LCD to 16×2 and prints “Starting...” on the first line. Then the app calls `display_update_batt_soc(soc)`, which overwrites the first line with text in the form “Batt SOC: 62.3%”.

### Step‑by‑step wiring
1. Connect GND (common ground) between the ESP32 and the LCD module.
2. Connect LCD VDD to +5 V.
3. Wire a 10 kΩ trimmer for contrast to VO (pin 3).
4. Connect the signal pins per the map: RS=GPIO21, E=GPIO22, D4=GPIO19, D5=GPIO18, D6=GPIO5, D7=GPIO23.
5. Tie R/W permanently to GND.
6. Wire the backlight (pins 15–16) depending on whether the module has a built‑in resistor.
7. Optionally insert a level shifter between the ESP32 (3.3 V) and the LCD inputs (if the LCD is powered at 5 V).
8. Flash the firmware with PlatformIO. You can watch logs on the serial line (921600 baud).

### Where to change wiring in code
If you change GPIO pins, modify only `include/config.h` in the “LCD QC1602A (4-bit parallel mode)” section. There is no need to touch `display.cpp`.

### File references
- `include/config.h` — central pin definitions
- `src/display.h` — display API (`display_init`, `display_update_batt_soc`)
- `src/display.cpp` — implementation and `LiquidCrystal` initialization
- `platformio.ini` — `LiquidCrystal` library dependency
