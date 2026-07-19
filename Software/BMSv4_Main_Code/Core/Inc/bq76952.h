#ifndef BQ76952_H_
#define BQ76952_H_

#include "main.h"

// ---------------------------------------------------------------------------
// Cell Balancing Configuration Constants
// ---------------------------------------------------------------------------

// Minimum voltage difference between the highest and lowest cell (mV).
// If the pack spread is smaller than this, no balancing starts.
// TI SLUAA81A recommends 40 mV; raised from 20 mV to reduce noise-triggered cycles.
#define BALANCE_THRESHOLD_MV     40U

// Stop hysteresis (mV). Balancing halts when spread drops below this value.
// Must be < BALANCE_THRESHOLD_MV to prevent rapid on/off oscillation.
#define BALANCE_STOP_DELTA_MV    20U

// Minimum cell voltage (mV) below which balancing is prohibited.
// TESTING: Set to 2800 mV to observe balancing at all SOC levels.
// PRODUCTION: Change to 3900U — only balance near-full cells.
#define BALANCE_MIN_VOLTAGE_MV   2800U

// Maximum cell voltage (mV). Kept for reference; overvoltage protection
// is enforced by the BQ76952 COV threshold, not by this constant.
#define BALANCE_MAX_VOLTAGE_MV   4250U

// Balancing interval in seconds. The IC re-evaluates cell selection every N seconds.
#define BALANCE_INTERVAL_S       3U

// Number of series cells in this pack.
#define NUM_CELLS                16U

// ---------------------------------------------------------------------------
// Balance Status Struct (monitor-only — visible in debugger live expressions)
// The BQ76952's autonomous balancer makes all decisions internally.
// This struct is populated by BQ_Read_Balance_Status() each loop iteration.
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t  hw_balance_mask;      // HW readback: bit 0 = cell 1 balancing ... bit 6 = cell 7
    uint8_t  cells_balancing;      // Count of cells currently being drained by the IC
    uint16_t min_voltage_mV;       // Lowest cell voltage this cycle (computed by STM32)
    uint16_t max_voltage_mV;       // Highest cell voltage this cycle
    uint16_t delta_mV;             // max - min spread
    uint8_t  balancing_active;     // 1 = IC is autonomously balancing, 0 = idle
} BQ_Balance_Status_t;

// ---------------------------------------------------------------------------
// Public Function Prototypes
// ---------------------------------------------------------------------------

// Core SPI Functions
uint8_t BQ_CalculateCRC(uint8_t *data, uint8_t len);
void    BQ_SPI_WriteReg(uint8_t reg_addr, uint8_t data);
uint8_t BQ_SPI_ReadReg(uint8_t reg_addr);

// BMS Setup and Configuration
void BQ_Configure_Cell_Count(void);
void BQ_Configure_Balancing(void);
void BQ_Wake_SPI(void);
void BQ_Configure_Power_Modes(void);

// BMS Data Reading
void     BQ_Read_All_Cell_Voltages(uint16_t *cell_array);
uint16_t BQ_Read_Stack_Voltage(void);
float    BQ_Read_Internal_Temp_C(void);
float    BQ_Get_Pack_Current_Amps(void);

// Cell Balancing Monitor (read-only — BQ76952 handles balancing autonomously)
void BQ_Read_Balance_Status(uint16_t *cell_mV, BQ_Balance_Status_t *status);

// BMS Diagnostic Reading
uint16_t BQ_Read_REG18_ADC(void);

#endif /* BQ76952_H_ */
