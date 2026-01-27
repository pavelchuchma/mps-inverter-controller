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
After flashing the firmware, `display_init()` initializes the LCD to 16×2 and prints “Starting...” on the first line. 
- The app calls `display_update_batt_soc(soc)`, which overwrites the first line with text in the form “Batt SOC: 62.3%”.
- The app calls `display_update_temperature(h, l)` every second, updating the second line with “Temp: 45.2/24.1C”.

### Step‑by‑step wiring
1. Connect GND (common ground) between the ESP32, the LCD module, and the thermistors.
2. Connect LCD VDD to +5 V.
3. Wire a 10 kΩ trimmer for contrast to VO (pin 3).
4. Connect the LCD signal pins: RS=GPIO21, E=GPIO22, D4=GPIO19, D5=GPIO18, D6=GPIO5, D7=GPIO23.
5. Tie LCD R/W permanently to GND.
6. Wire the backlight (pins 15–16).
7. Connect thermistor voltage dividers:
   - 3.3 V to 10 kΩ resistor, other end to GPIO 34 AND to one leg of NTC.
   - Other leg of NTC to GND.
   - Repeat for GPIO 35.
8. Optionally insert a level shifter for LCD inputs.
9. Flash the firmware with PlatformIO. Watch logs on the serial line (921600 baud).

### Where to change wiring in code
If you change GPIO pins, modify only `include/config.h` in the “LCD QC1602A (4-bit parallel mode)” section. There is no need to touch `display.cpp`.

### NTC Thermistors (Temperature Sensors)
The project supports two NTC thermistors (10k Ω, B3950) to monitor temperatures. These values are displayed on the second line of the LCD.

#### Pin Map (ESP32 Feather ↔ Thermistors)
- **GPIO 34 (ADC1_CH6)**: Sensor "L" (typically lower temperature/ambient)
- **GPIO 35 (ADC1_CH7)**: Sensor "H" (typically higher temperature/inverter)

#### Wiring (Voltage Divider)
Each thermistor must be connected using a voltage divider to the 3.3 V rail:
```
3.3 V --- [ 10k Ω Resistor ] ---+--- [ NTC 10k Ω ] --- GND
                                |
                             ADC Pin (34 or 35)
```
*Note: Use high-precision resistors (1%) for better accuracy. GPIO 34 and 35 are input-only pins on the ESP32, which is ideal for ADC.*

#### Configuration
Calibration constants (Beta, T0, R0) are defined in `src/thermistor.cpp`. Pin assignments are in `include/config.h`.

