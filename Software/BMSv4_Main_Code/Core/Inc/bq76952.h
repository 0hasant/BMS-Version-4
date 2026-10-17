#ifndef BQ76952_H_
#define BQ76952_H_

#include "main.h"

// ===========================================================================
// BQ76952 driver for STM32F1 (SPI with CRC)
//
// Design rules:
//  - Every SPI response is checked (echoed address + CRC) and retried.
//  - Every function that talks to the IC returns a BQ_Status.
//  - All RAM configuration is written in ONE CONFIG_UPDATE session and read
//    back to verify it. If the IC resets (POR flag set), the main loop calls
//    BQ_Configure() again, because RAM settings are lost on any reset.
// ===========================================================================

// ---------------------------------------------------------------------------
// Board / pack configuration
// ---------------------------------------------------------------------------

// Cell configuration. Pick the number of series cells actually wired to the board.
// BQ_VCELL_MODE says which VC inputs are real cells: bit n = input (n+1),
// the cell between VC(n) and VC(n+1). Unused inputs must be shorted to a
// neighbour so they read 0 V; the IC then ignores them (no protection, no
// balancing). An unused input that is left in BQ_VCELL_MODE reads 0 V and
// blocks balancing (it is below the "Min Cell V" limit).
//
//   7  : 7S bench test. Two wirings, select with BMS_7S_TOP_ON_VC16:
//        0 -> cells 1-7 on VC1-VC7, VC8..VC16 shorted to the top of cell 7 (0x007F)
//        1 -> cells 1-6 on VC1-VC6, cell 7 between VC15 and VC16,
//             VC7..VC15 shorted to VC6 (0x803F)
//   13 : 13S pack board. Cells 1-12 on VC1-VC12, cell 13 on VC16, VC13-VC15 shorted to VC12.
//   16 : all 16 inputs.
// Check the wiring in the debugger: BQ_Input_mV[0..15] shows all 16 inputs,
// Stack_Diff_mV (main.c) should be close to 0.
#define BMS_CELL_CONFIG          7
#define BMS_7S_TOP_ON_VC16       0

#if   BMS_CELL_CONFIG == 7
  #define NUM_CELLS              7U
  #if BMS_7S_TOP_ON_VC16
    #define BQ_VCELL_MODE        0x803FU
  #else
    #define BQ_VCELL_MODE        0x007FU
  #endif
#elif BMS_CELL_CONFIG == 13
  #define NUM_CELLS              13U
  #define BQ_VCELL_MODE          0x8FFFU
#elif BMS_CELL_CONFIG == 16
  #define NUM_CELLS              16U
  #define BQ_VCELL_MODE          0xFFFFU
#else
  #error "BMS_CELL_CONFIG must be 7, 13 or 16"
#endif

// Compile-time check that NUM_CELLS matches the number of bits in BQ_VCELL_MODE
#define BQ_BITS4(x)   (((x) & 1U) + (((x) >> 1) & 1U) + (((x) >> 2) & 1U) + (((x) >> 3) & 1U))
#define BQ_BITS16(x)  (BQ_BITS4(x) + BQ_BITS4((x) >> 4) + BQ_BITS4((x) >> 8) + BQ_BITS4((x) >> 12))
_Static_assert(BQ_BITS16(BQ_VCELL_MODE) == NUM_CELLS, "NUM_CELLS does not match BQ_VCELL_MODE");

// Sense resistor in milliohms. Written to the IC as CC Gain = 7.4768 / R
// (and Capacity Gain = CC Gain * 298261.6178), so Current (0x3A) is in real mA.
// Range of the int16 mA reading: +/-32.7 A.
// Current calibration: if the reading is X % high against a reference meter,
// increase this value by X % (e.g. 2.0 -> 2.04 for a 2 % high reading).
#define BQ_SHUNT_MOHM            2.0f

// Send the RESET() subcommand at boot. Pressing the MCU reset button (S2)
// then also resets the BQ76952 into a known state.
#define BQ_RESET_ON_BOOT         1

// ---------------------------------------------------------------------------
// Cell balancing configuration (BQ76952 autonomous balancing)
// ---------------------------------------------------------------------------

// 1 = the IC balances on its own, 0 = balancing off.
// This is the value at boot. While debugging, change BQ_Balance_Enable
// (0 or 1) in the Expressions view: the main loop rewrites the config.
#define BALANCE_ENABLE           0

// Minimum spread between highest and lowest cell (mV) before balancing starts.
#define BALANCE_THRESHOLD_MV     40U

// Balancing stops when the spread drops below this value (mV).
#define BALANCE_STOP_DELTA_MV    20U

// Minimum cell voltage (mV) below which balancing is prohibited.
// TESTING: 2800 mV to observe balancing at all SOC levels.
// PRODUCTION: change to 3900U - only balance near-full cells.
#define BALANCE_MIN_VOLTAGE_MV   2800U

// Kept for reference; overvoltage is enforced by the BQ76952 COV protection.
#define BALANCE_MAX_VOLTAGE_MV   4250U

// The IC re-evaluates which cells to balance every N seconds.
#define BALANCE_INTERVAL_S       3U

// Maximum number of cells balanced at the same time.
#define BALANCE_MAX_CELLS        4U

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
typedef enum {
    BQ_OK = 0,
    BQ_ERR_SPI,        // HAL SPI call failed
    BQ_ERR_TIMEOUT,    // IC never returned a valid answer
    BQ_ERR_CRC,        // Response CRC or subcommand checksum wrong
    BQ_ERR_VERIFY,     // Read-back of written configuration did not match
} BQ_Status;

// Communication health counters (watch these in the debugger)
typedef struct {
    uint32_t frames;          // SPI frames sent
    uint32_t retries;         // Frames that had to be repeated
    uint32_t crc_errors;      // Responses with bad CRC
    uint32_t not_ready;       // 0xFFFF00 / 0xFFFFFF / 0xFFFFAA responses
    uint32_t failures;        // Operations that failed after all retries
    uint32_t configs;         // Successful BQ_Configure() runs
    uint32_t resets_detected; // Times the POR flag was found set
    BQ_Status last_error;
    uint8_t  configured;      // 1 = configuration written and verified
} BQ_Health_t;

// Balance status (read-only monitor; the IC decides what to balance)
typedef struct {
    uint16_t hw_balance_mask;   // as the IC reports it: bit n = VC input n+1
    uint16_t cell_balance_mask; // in pack order: bit 0 = cell 1 ... bit NUM_CELLS-1 = top cell
    uint8_t  cells_balancing;   // number of cells being balanced now
    uint16_t min_voltage_mV;    // lowest cell voltage (computed by STM32)
    uint16_t max_voltage_mV;    // highest cell voltage
    uint16_t delta_mV;          // max - min
    uint8_t  balancing_active;  // 1 = IC is balancing
} BQ_Balance_Status_t;

// Status and fault flags read from the IC
typedef struct {
    uint8_t  safety_alert_a;    // 0x02: SCD OCD2 OCD1 OCC COV CUV
    uint8_t  safety_status_a;   // 0x03
    uint8_t  safety_alert_b;    // 0x04: temperature alerts
    uint8_t  safety_status_b;   // 0x05
    uint8_t  safety_alert_c;    // 0x06
    uint8_t  safety_status_c;   // 0x07
    uint16_t battery_status;    // 0x12
    uint16_t alarm_status;      // 0x62 (latched bits, cleared after reading)
} BQ_Safety_t;

// Battery Status (0x12) bits
#define BQ_BATSTAT_CFGUPDATE     0x0001U
#define BQ_BATSTAT_PCHG_MODE     0x0002U
#define BQ_BATSTAT_SLEEP_EN      0x0004U
#define BQ_BATSTAT_POR           0x0008U
#define BQ_BATSTAT_WD            0x0010U
#define BQ_BATSTAT_SS            0x0800U   // a safety fault is active
#define BQ_BATSTAT_PF            0x1000U
#define BQ_BATSTAT_SLEEP         0x8000U

// Alarm Status (0x62) bits
#define BQ_ALARM_WAKE            0x0001U
#define BQ_ALARM_ADSCAN          0x0002U
#define BQ_ALARM_CB              0x0004U   // cell balancing active
#define BQ_ALARM_FUSE            0x0008U
#define BQ_ALARM_SHUTV           0x0010U
#define BQ_ALARM_XDSG            0x0020U
#define BQ_ALARM_XCHG            0x0040U
#define BQ_ALARM_FULLSCAN        0x0080U
#define BQ_ALARM_INITCOMP        0x0200U
#define BQ_ALARM_INITSTART       0x0400U
#define BQ_ALARM_SSA             0x4000U   // something set in Safety Status A
#define BQ_ALARM_SSBC            0x8000U   // something set in Safety Status B/C

// Safety Status A (0x03) bits
#define BQ_SSA_CUV               0x04U
#define BQ_SSA_COV               0x08U
#define BQ_SSA_OCC               0x10U
#define BQ_SSA_OCD1              0x20U
#define BQ_SSA_OCD2              0x40U
#define BQ_SSA_SCD               0x80U

extern BQ_Health_t BQ_Health;
extern volatile uint8_t BQ_Balance_Enable;   // runtime copy of BALANCE_ENABLE
extern uint16_t    BQ_Input_mV[16];   // all 16 VC inputs, mV (updated with the cell voltages)

// ---------------------------------------------------------------------------
// Low-level access
// ---------------------------------------------------------------------------
uint8_t   BQ_CalculateCRC(uint8_t *data, uint8_t len);
BQ_Status BQ_SPI_WriteReg(uint8_t reg_addr, uint8_t data);
uint8_t   BQ_SPI_ReadReg(uint8_t reg_addr);              // legacy: returns 0 on error
BQ_Status BQ_ReadReg8(uint8_t reg_addr, uint8_t *value);
BQ_Status BQ_ReadReg16(uint8_t reg_addr, uint16_t *value);
BQ_Status BQ_Subcommand(uint16_t cmd);
BQ_Status BQ_SubcommandRead(uint16_t cmd, uint8_t *buf, uint8_t len);
BQ_Status BQ_WriteDataMem(uint16_t addr, const uint8_t *data, uint8_t len);
BQ_Status BQ_ReadDataMem(uint16_t addr, uint8_t *data, uint8_t len);
void      BQ_Wake_SPI(void);

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
BQ_Status BQ_Init(void);              // optional reset + configure
BQ_Status BQ_Reset(void);             // RESET() subcommand, wait for init
BQ_Status BQ_Configure(void);         // write + verify all RAM settings
BQ_Status BQ_EnterConfigUpdate(void);
BQ_Status BQ_ExitConfigUpdate(void);
BQ_Status BQ_CheckForReset(void);     // reconfigure if POR flag is set

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------
BQ_Status BQ_Read_All_Cell_Voltages(uint16_t *cell_array);   // NUM_CELLS values, mV; also fills BQ_Input_mV
BQ_Status BQ_Read_Stack_Voltage(uint32_t *stack_mV);
BQ_Status BQ_Read_Internal_Temp_C(float *temp_c);
BQ_Status BQ_Get_Pack_Current_Amps(float *amps);
BQ_Status BQ_Read_REG18_ADC(uint16_t *counts);
BQ_Status BQ_Read_Balance_Status(const uint16_t *cell_mV, BQ_Balance_Status_t *status);
BQ_Status BQ_Read_Safety(BQ_Safety_t *s);

#endif /* BQ76952_H_ */
