# Solar Team Solaris BMS Version 4

BQ76952-based BMS (up to 16S per board) with an STM32F103C8 host, isolated CAN,
external cell balancing and contactor control.

**System:** the 26S battery pack uses **two BMS boards**. Contactor coil current
is about 0.4 A. Balancing is enabled by an external button (J4 opto input).

**Active branch:** `v4.02` (the ordered board, Gerbers in `Hardware/All_Gerber/Gerber v4.02.zip`).

**Present focus:** make the firmware stable on the board that already works
(v4.013) before the v4.02 board arrives.

---

## Firmware (Software/BMSv4_Main_Code)

### Done
- [x] SPI driver with CRC check and retries on every frame (`bq76952.c`)
- [x] CONFIG_UPDATE wait checks the correct bit (Battery Status bit 0, `CFGUPDATE`)
- [x] All RAM settings written in one CONFIG_UPDATE session and read back to verify
- [x] BQ76952 reset detected (POR flag) and configuration re-applied automatically
- [x] MCU reset button (S2) also resets the BQ76952 (`BQ_RESET_ON_BOOT`)
- [x] Independent watchdog (IWDG, ~2 s, frozen while debugging)
- [x] Safety Status A/B/C, Battery Status and Alarm Status read every loop
- [x] Stack voltage as `uint32_t` (a full 16S pack is above 65.5 V)
- [x] Communication health counters in `BQ_Health` (watch in debugger)

### To do (on the working board)
- [ ] Protection setup: COV/CUV for the DMEGC INR45E cells, enable CUV, OCC/OCD1/OCD2/SCD
      thresholds for the 2 mOhm shunt (default SCD = 10 mV = only 5 A!), OTC/OTD
- [ ] Current calibration: CC Gain / Capacity Gain for 2 mOhm and 10 mA user units
      (raw int16 mA saturates at about 16.4 A today)
- [ ] Disable the charge pump (CP1 is tied to BAT, saves ~200 uA)
- [ ] Balancing: production `BALANCE_MIN_VOLTAGE_MV = 3900`, real temperature limits,
      `BALANCE_MAX_CELLS` chosen for heat (see Hardware notes)

### To do (for the v4.02 board, after it arrives)
- [ ] Fix `.ioc` before regenerating code:
  - PB10 = BMS number input (pull-down R31). PB11 = **contactor output** (rework below)
  - Mux select lines: PB15 = S0, PB14 = S1, PB13 = S2, PB12 = S3 (the `.ioc` has them reversed)
  - Remove PB8 output and I2C1 (PB8/PB7 are tied to SPI SCLK/MISO through R1/R2)
  - HSE = crystal (Y1, 16 MHz), not external clock; then 72 MHz SYSCLK
  - PA8 = balance-enable opto input (pull-up R30), PA0 = ADC for the thermistor mux
  - CAN at the bitrate used by the car
- [ ] Contactor logic on PB11: ON only when configured, no safety fault, SPI healthy;
      OFF on any fault. Watchdog reset -> pin floats -> R32 pulls it low -> contactor OFF
- [ ] VCell Mode for 13S per board (`0x8FFF`, unused VC13-VC15 shorted to VC12),
      board number from PB10
- [ ] Cell temperature reading through the CD74HC4067 mux
- [ ] CAN messages
- [ ] Balance-enable button (J4) with debounce

---

## Hardware

### v4.02 (ordered) - fixes before / during assembly
- [ ] **PS1:** fit **1S4E_0505S1U** (5 V in). The BOM part 1S4E_1505S1U is the 15 V input version -> CAN would be dead
- [ ] **F1:** 250 mA fuse blows when the top cell balances (~0.42 A). Use 1 A, 2410 size
      (e.g. Reomax SET1100 1 A 125 V time-lag, in stock at Özdisan). The 2410 body (6.1 mm) is
      longer than the 2010 footprint, solder carefully
- [ ] **Contactor drive (DDSG can't drive the PhotoMOS LEDs from REG18):**
  1. Bottom side: cut the short trace from the via in R10 pad 2 (89.5, 83.25) to the via at (95.25, 87.03)
  2. Wire SW1 pin 2 (PB11) to R28 pad 1 (R28/R29 LED node)
  3. Do **not** close SW1 position 1-2 (it would force the contactor on). Better: leave SW1 out and
     bridge pads 3-4 for BMS #2 (PB10 high)
  4. R10 can stay unfitted. Keep R32 (pull-down = contactor off during reset)
- [ ] **R1/R2:** leave unfitted (reserved for future I2C work)
- [ ] **VDDA:** add 100 nF + 1 uF from TP5 to GND (no decoupling on the board)
- [ ] **Ground, lower board (carries pack current through the shunt):** wire GND to TP2 (-Batt_In,
      battery side of the shunt) and **remove R43**. Never fit both, or pack current flows in the cell-0 sense wire
- [ ] **Ground, upper board (no pack current):** keep R43 = 0 R, tie TP2 and TP3 to GND so SRP/SRN do not float
- [ ] Balancing heat: 10 R balance resistors = ~1.7 W each (2 W parts). Limit to 2 cells or change to 22 R
- [ ] C27 (CAN split termination) is 10 pF, should be ~4.7 nF
- [ ] Regenerate the BOM from the current schematic (`BMS_V4.csv` and `BMS_V4.013.xlsx` use old references)

### Next revision (v4.03) wish list
- [ ] Power REGIN from +5V_Main and enable REG1 = 3.3 V: MISO becomes 3.3 V logic (`MISO_REG1`),
      and DDSG/DCHG can drive real loads
- [ ] RST_SHUT: 10 k-100 k pull-down (R11 is 100 R) and drive it from an STM32 pin and/or a button to 3.3 V
- [ ] Connect S1 to an MCU pin (it is connected to nothing today)
- [ ] Direct GND star point at the battery side of the shunt, VC0 through R/C to it
- [ ] Separate VC16 sense wire from the fused power wire (fuse drop shows up in cell 16)
- [ ] Balance-enable input: R27 sized for the button supply voltage (5 V needs ~1 k)
- [ ] Contactor wiring for two boards in series (flyback diode only once, across the coil)
- [ ] MOSI bottom link is a graphic line with a net; convert to a real track

---

## Repository organisation

- `main` is far behind `v4.02`: merge `v4.02` into `main` once the board is verified
- Remove `Hardware/BMS_V4/untitled.kicad_sch` (not in the hierarchy, old flat schematic)
- Lock files (`*.lck`), `fp-info-cache` and auto-save files are now ignored; don't commit them
- `Hardware/Porduciton Outputs/` -> rename to `Production Outputs`; `report.txt` still points to v4.013
- Keep one BOM file generated from the current schematic, next to the Gerbers of the same version
- Tag each ordered board, e.g. `hw-v4.02` on the commit the Gerbers came from
