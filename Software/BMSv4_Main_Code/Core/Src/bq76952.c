#include "bq76952.h"
#include <string.h>

// SPI handle is created by CubeMX in main.c
extern SPI_HandleTypeDef hspi1;

BQ_Health_t BQ_Health = {0};
uint16_t    BQ_Input_mV[16] = {0};
volatile uint8_t BQ_Balance_Enable = BALANCE_ENABLE;

// ---------------------------------------------------------------------------
// Timing (TI: >= 50 us between SPI transactions)
// ---------------------------------------------------------------------------
#define BQ_FRAME_GAP_US        100U   // pause after every SPI frame
#define BQ_READ_RETRIES        20U    // frames to wait for a valid read answer
#define BQ_WRITE_RETRIES       5U     // attempts for a write
#define BQ_SUBCMD_POLLS        50U    // completion polls (x ~0.8 ms)
#define BQ_CFG_TIMEOUT_MS      100U   // enter/exit CONFIG_UPDATE timeout
#define BQ_RESET_BOOT_MS       300U   // TRM: ~200-300 ms boot after reset
#define BQ_RESET_TIMEOUT_MS    1000U

// Registers whose write triggers an action. Writing them twice could run
// the action twice, so they are sent once and checked differently.
#define BQ_REG_SUBCMD_LO       0x3EU
#define BQ_REG_SUBCMD_HI       0x3FU
#define BQ_REG_BUFFER          0x40U
#define BQ_REG_CHECKSUM        0x60U
#define BQ_REG_LENGTH          0x61U
#define BQ_REG_BATTERY_STATUS  0x12U
#define BQ_REG_ALARM_STATUS    0x62U

// Subcommands
#define BQ_SUB_RESET           0x0012U
#define BQ_SUB_CB_ACTIVE_CELLS 0x0083U
#define BQ_SUB_DASTATUS5       0x0075U
#define BQ_SUB_SET_CFGUPDATE   0x0090U
#define BQ_SUB_EXIT_CFGUPDATE  0x0092U

// Data memory addresses (BQ76952 TRM, SLUUBY2B)
#define BQ_DM_VCELL_MODE       0x9304U   // H2
#define BQ_DM_DSG_CURR_THR     0x9310U   // I2 userA, followed by Chg Current Threshold
#define BQ_DM_CC_GAIN          0x91A8U   // F4, followed by Capacity Gain (F4)
#define BQ_DM_CB_CONFIG        0x9335U   // start of the 14-byte balancing block

// ---------------------------------------------------------------------------
// Microsecond delay using the Cortex-M3 cycle counter
// ---------------------------------------------------------------------------
static void bq_delay_us(uint32_t us) {
    if (!(DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk)) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < ticks) {
    }
}

static BQ_Status bq_fail(BQ_Status s) {
    BQ_Health.failures++;
    BQ_Health.last_error = s;
    return s;
}

// ---------------------------------------------------------------------------
// CRC-8, polynomial 0x07 (used on every SPI frame)
// ---------------------------------------------------------------------------
uint8_t BQ_CalculateCRC(uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// One 3-byte SPI frame. rx receives the answer to the PREVIOUS frame.
// ---------------------------------------------------------------------------
static BQ_Status bq_frame(uint8_t b0, uint8_t b1, uint8_t rx[3]) {
    uint8_t tx[3] = { b0, b1, 0 };
    tx[2] = BQ_CalculateCRC(tx, 2);

    HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef hs = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 3, 10);
    HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);

    BQ_Health.frames++;
    bq_delay_us(BQ_FRAME_GAP_US);
    return (hs == HAL_OK) ? BQ_OK : BQ_ERR_SPI;
}

// True if rx is a valid echo of (b0, b1) with a correct CRC
static int bq_echo_ok(const uint8_t rx[3], uint8_t b0, uint8_t b1) {
    if (rx[0] != b0 || rx[1] != b1) return 0;
    uint8_t chk[2] = { rx[0], rx[1] };
    if (BQ_CalculateCRC(chk, 2) != rx[2]) {
        BQ_Health.crc_errors++;
        return 0;
    }
    return 1;
}

// 0xFFFFFF = oscillator was off, 0xFFFFAA = our frame had a CRC error:
// in both cases the previous frame was NOT accepted.
static int bq_frame_rejected(const uint8_t rx[3]) {
    return (rx[0] == 0xFF && rx[1] == 0xFF && (rx[2] == 0xFF || rx[2] == 0xAA));
}

// ---------------------------------------------------------------------------
// Register write
// ---------------------------------------------------------------------------
BQ_Status BQ_SPI_WriteReg(uint8_t reg_addr, uint8_t data) {
    uint8_t rx[3];
    uint8_t b0 = (uint8_t)(reg_addr | 0x80);   // R/W bit = 1 for write

    if (reg_addr == BQ_REG_SUBCMD_HI || reg_addr == BQ_REG_LENGTH) {
        // Trigger register: send once, then a harmless read frame to collect
        // the echo. Resend only if the IC clearly rejected the frame.
        for (uint8_t attempt = 0; attempt < BQ_WRITE_RETRIES; attempt++) {
            if (bq_frame(b0, data, rx) != BQ_OK) return bq_fail(BQ_ERR_SPI);
            if (bq_frame(BQ_REG_BATTERY_STATUS, 0xFF, rx) != BQ_OK) return bq_fail(BQ_ERR_SPI);
            if (bq_echo_ok(rx, b0, data)) return BQ_OK;
            if (!bq_frame_rejected(rx)) {
                // 0xFFFF00: IC still busy. Assume accepted; the caller
                // verifies the result (completion poll or read-back).
                BQ_Health.not_ready++;
                return BQ_OK;
            }
            BQ_Health.retries++;
            bq_delay_us(500);
        }
        return bq_fail(BQ_ERR_TIMEOUT);
    }

    // Normal register: repeat the same frame until its echo comes back
    // (TI reference method; repeating a plain register write is harmless).
    if (bq_frame(b0, data, rx) != BQ_OK) return bq_fail(BQ_ERR_SPI);
    for (uint8_t attempt = 0; attempt < BQ_WRITE_RETRIES; attempt++) {
        if (bq_frame(b0, data, rx) != BQ_OK) return bq_fail(BQ_ERR_SPI);
        if (bq_echo_ok(rx, b0, data)) return BQ_OK;
        BQ_Health.retries++;
        bq_delay_us(500);
    }
    return bq_fail(BQ_ERR_TIMEOUT);
}

// ---------------------------------------------------------------------------
// Register read (pipelined: the answer arrives on the next frame)
// ---------------------------------------------------------------------------
BQ_Status BQ_ReadReg8(uint8_t reg_addr, uint8_t *value) {
    uint8_t rx[3];
    uint8_t b0 = (uint8_t)(reg_addr & 0x7F);   // R/W bit = 0 for read

    if (bq_frame(b0, 0xFF, rx) != BQ_OK) return bq_fail(BQ_ERR_SPI);
    for (uint8_t i = 0; i < BQ_READ_RETRIES; i++) {
        if (bq_frame(b0, 0xFF, rx) != BQ_OK) return bq_fail(BQ_ERR_SPI);
        if (rx[0] == b0) {
            uint8_t chk[2] = { rx[0], rx[1] };
            if (BQ_CalculateCRC(chk, 2) == rx[2]) {
                *value = rx[1];
                return BQ_OK;
            }
            BQ_Health.crc_errors++;
        } else {
            BQ_Health.not_ready++;
        }
        BQ_Health.retries++;
    }
    return bq_fail(BQ_ERR_TIMEOUT);
}

BQ_Status BQ_ReadReg16(uint8_t reg_addr, uint16_t *value) {
    uint8_t lo, hi;
    BQ_Status s = BQ_ReadReg8(reg_addr, &lo);
    if (s != BQ_OK) return s;
    s = BQ_ReadReg8((uint8_t)(reg_addr + 1), &hi);
    if (s != BQ_OK) return s;
    *value = (uint16_t)((hi << 8) | lo);
    return BQ_OK;
}

// Kept for old code: returns 0 if the read failed (see BQ_Health)
uint8_t BQ_SPI_ReadReg(uint8_t reg_addr) {
    uint8_t v = 0;
    (void)BQ_ReadReg8(reg_addr, &v);
    return v;
}

// Wake the internal oscillator before a burst (TRM: wait >= 135 us)
void BQ_Wake_SPI(void) {
    uint8_t rx[3];
    (void)bq_frame(0x00, 0xFF, rx);
    bq_delay_us(200);
}

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------
BQ_Status BQ_Subcommand(uint16_t cmd) {
    BQ_Status s = BQ_SPI_WriteReg(BQ_REG_SUBCMD_LO, (uint8_t)(cmd & 0xFF));
    if (s != BQ_OK) return s;
    return BQ_SPI_WriteReg(BQ_REG_SUBCMD_HI, (uint8_t)(cmd >> 8));
}

// Wait until 0x3E/0x3F read back the command (0xFF = still busy)
static BQ_Status bq_wait_subcmd_done(uint16_t cmd) {
    uint8_t lo = (uint8_t)(cmd & 0xFF), hi = (uint8_t)(cmd >> 8);
    for (uint8_t i = 0; i < BQ_SUBCMD_POLLS; i++) {
        uint8_t r_lo, r_hi;
        if (BQ_ReadReg8(BQ_REG_SUBCMD_LO, &r_lo) == BQ_OK &&
            BQ_ReadReg8(BQ_REG_SUBCMD_HI, &r_hi) == BQ_OK &&
            r_lo == lo && r_hi == hi) {
            return BQ_OK;
        }
        bq_delay_us(200);
    }
    return BQ_ERR_TIMEOUT;
}

// Send a subcommand and wait for completion; resend if it never completes
// (the trigger frame may have been lost).
static BQ_Status bq_subcmd_and_wait(uint16_t cmd) {
    BQ_Status s = BQ_ERR_TIMEOUT;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if ((s = BQ_Subcommand(cmd)) != BQ_OK) continue;
        if ((s = bq_wait_subcmd_done(cmd)) == BQ_OK) return BQ_OK;
        BQ_Health.retries++;
    }
    return bq_fail(s);
}

// Send a subcommand that returns data and read up to len bytes of it.
// The whole response is read so the checksum at 0x60 can be verified.
BQ_Status BQ_SubcommandRead(uint16_t cmd, uint8_t *buf, uint8_t len) {
    uint8_t data[32];
    uint8_t resp_len, chk;

    BQ_Status s = bq_subcmd_and_wait(cmd);
    if (s != BQ_OK) return s;

    if ((s = BQ_ReadReg8(BQ_REG_LENGTH, &resp_len)) != BQ_OK) return s;
    if (resp_len < 5 || resp_len > 36) return bq_fail(BQ_ERR_CRC);
    uint8_t n = (uint8_t)(resp_len - 4);   // length includes 0x3E,0x3F,0x60,0x61

    uint8_t sum = (uint8_t)((cmd & 0xFF) + (cmd >> 8));
    for (uint8_t i = 0; i < n; i++) {
        if ((s = BQ_ReadReg8((uint8_t)(BQ_REG_BUFFER + i), &data[i])) != BQ_OK) return s;
        sum = (uint8_t)(sum + data[i]);
    }
    if ((s = BQ_ReadReg8(BQ_REG_CHECKSUM, &chk)) != BQ_OK) return s;
    if ((uint8_t)~sum != chk) {
        BQ_Health.crc_errors++;
        return bq_fail(BQ_ERR_CRC);
    }

    for (uint8_t i = 0; i < len && i < n; i++) buf[i] = data[i];
    return BQ_OK;
}

// ---------------------------------------------------------------------------
// Data memory
// ---------------------------------------------------------------------------
BQ_Status BQ_ReadDataMem(uint16_t addr, uint8_t *data, uint8_t len) {
    return BQ_SubcommandRead(addr, data, len);
}

// Write len bytes (max 32) to contiguous data memory.
// Must be called while in CONFIG_UPDATE mode.
BQ_Status BQ_WriteDataMem(uint16_t addr, const uint8_t *data, uint8_t len) {
    uint8_t lo = (uint8_t)(addr & 0xFF), hi = (uint8_t)(addr >> 8);
    BQ_Status s;

    if (len == 0 || len > 32) return bq_fail(BQ_ERR_VERIFY);

    // Writing the address makes the IC load the current contents into the
    // buffer; wait for that before overwriting the buffer with new data.
    if ((s = bq_subcmd_and_wait(addr)) != BQ_OK) return s;

    uint8_t sum = (uint8_t)(lo + hi);
    for (uint8_t i = 0; i < len; i++) {
        if ((s = BQ_SPI_WriteReg((uint8_t)(BQ_REG_BUFFER + i), data[i])) != BQ_OK) return s;
        sum = (uint8_t)(sum + data[i]);
    }
    if ((s = BQ_SPI_WriteReg(BQ_REG_CHECKSUM, (uint8_t)~sum)) != BQ_OK) return s;
    if ((s = BQ_SPI_WriteReg(BQ_REG_LENGTH, (uint8_t)(len + 4))) != BQ_OK) return s;

    HAL_Delay(2);
    return BQ_OK;
}

// Read back and compare
static BQ_Status bq_verify_dm(uint16_t addr, const uint8_t *expect, uint8_t len) {
    uint8_t buf[32];
    BQ_Status s = BQ_ReadDataMem(addr, buf, len);
    if (s != BQ_OK) return s;
    for (uint8_t i = 0; i < len; i++) {
        if (buf[i] != expect[i]) return bq_fail(BQ_ERR_VERIFY);
    }
    return BQ_OK;
}

// ---------------------------------------------------------------------------
// CONFIG_UPDATE mode
// ---------------------------------------------------------------------------
static BQ_Status bq_wait_cfgupdate(uint8_t want_set) {
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < BQ_CFG_TIMEOUT_MS) {
        uint16_t bs;
        if (BQ_ReadReg16(BQ_REG_BATTERY_STATUS, &bs) == BQ_OK) {
            uint8_t is_set = (bs & BQ_BATSTAT_CFGUPDATE) ? 1U : 0U;   // bit 0
            if (is_set == want_set) return BQ_OK;
        }
        HAL_Delay(1);
    }
    return BQ_ERR_TIMEOUT;
}

// Send SET_CFGUPDATE / EXIT_CFGUPDATE and confirm via Battery Status bit 0
static BQ_Status bq_cfgupdate(uint16_t cmd, uint8_t want_set) {
    BQ_Status s = BQ_ERR_TIMEOUT;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        if ((s = BQ_Subcommand(cmd)) != BQ_OK) continue;
        if ((s = bq_wait_cfgupdate(want_set)) == BQ_OK) return BQ_OK;
        BQ_Health.retries++;
    }
    return bq_fail(s);
}

BQ_Status BQ_EnterConfigUpdate(void) {
    return bq_cfgupdate(BQ_SUB_SET_CFGUPDATE, 1);
}

BQ_Status BQ_ExitConfigUpdate(void) {
    return bq_cfgupdate(BQ_SUB_EXIT_CFGUPDATE, 0);
}

// ---------------------------------------------------------------------------
// Reset and configuration
// ---------------------------------------------------------------------------
// POR is already 1 after power-up, so first clear it (entering and leaving
// CONFIG_UPDATE clears POR). Then POR = 1 after RESET() proves the reset ran.
BQ_Status BQ_Reset(void) {
    if (BQ_EnterConfigUpdate() == BQ_OK) (void)BQ_ExitConfigUpdate();

    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        if (BQ_Subcommand(BQ_SUB_RESET) != BQ_OK) continue;
        HAL_Delay(BQ_RESET_BOOT_MS);

        uint32_t t0 = HAL_GetTick();
        while ((HAL_GetTick() - t0) < BQ_RESET_TIMEOUT_MS) {
            uint16_t bs;
            BQ_Wake_SPI();
            if (BQ_ReadReg16(BQ_REG_BATTERY_STATUS, &bs) == BQ_OK && (bs & BQ_BATSTAT_POR)) {
                return BQ_OK;
            }
            HAL_Delay(10);
        }
        BQ_Health.retries++;
    }
    return bq_fail(BQ_ERR_TIMEOUT);
}

BQ_Status BQ_Configure(void) {
    BQ_Health.configured = 0;

    // --- Values to write ---
    const uint8_t vcell[2] = { (uint8_t)(BQ_VCELL_MODE & 0xFF), (uint8_t)(BQ_VCELL_MODE >> 8) };

    // Dsg / Chg Current Threshold (userA). 100 / 50 are the TI defaults.
    // In mA (CC Gain is set for the real shunt below).
    const uint8_t curr_thr[4] = { 100, 0, 50, 0 };

    // CC Gain and Capacity Gain (two floats, little-endian like the STM32)
    const float gains[2] = { 7.4768f / BQ_SHUNT_MOHM, (7.4768f / BQ_SHUNT_MOHM) * 298261.6178f };
    uint8_t gain_bytes[8];
    memcpy(gain_bytes, gains, sizeof(gain_bytes));

    // Cell balancing block, 0x9335 .. 0x9342 (14 bytes, contiguous)
    const uint8_t cb[14] = {
        BQ_Balance_Enable ? 0x07 : 0x00,        // 0x9335 Balancing Configuration: CB_CHG | CB_RLX | CB_SLEEP, or off
        (uint8_t)(int8_t)-100,                  // 0x9336 Min Cell Temp  (bench test: wide limits)
        120,                                    // 0x9337 Max Cell Temp
        120,                                    // 0x9338 Max Internal Temp
        BALANCE_INTERVAL_S,                     // 0x9339 Cell Balance Interval
        BALANCE_MAX_CELLS,                      // 0x933A Cell Balance Max Cells
        (uint8_t)(BALANCE_MIN_VOLTAGE_MV & 0xFF),   // 0x933B Min Cell V (Charge), I2
        (uint8_t)(BALANCE_MIN_VOLTAGE_MV >> 8),
        BALANCE_THRESHOLD_MV,                   // 0x933D Min Delta (Charge)
        BALANCE_STOP_DELTA_MV,                  // 0x933E Stop Delta (Charge)
        (uint8_t)(BALANCE_MIN_VOLTAGE_MV & 0xFF),   // 0x933F Min Cell V (Relax), I2
        (uint8_t)(BALANCE_MIN_VOLTAGE_MV >> 8),
        BALANCE_THRESHOLD_MV,                   // 0x9341 Min Delta (Relax)
        BALANCE_STOP_DELTA_MV,                  // 0x9342 Stop Delta (Relax)
    };

    // --- Write everything in one CONFIG_UPDATE session ---
    BQ_Status s = BQ_EnterConfigUpdate();
    if (s != BQ_OK) return s;

    if (s == BQ_OK) s = BQ_WriteDataMem(BQ_DM_VCELL_MODE, vcell, sizeof(vcell));
    if (s == BQ_OK) s = BQ_WriteDataMem(BQ_DM_DSG_CURR_THR, curr_thr, sizeof(curr_thr));
    if (s == BQ_OK) s = BQ_WriteDataMem(BQ_DM_CB_CONFIG, cb, sizeof(cb));
    if (s == BQ_OK) s = BQ_WriteDataMem(BQ_DM_CC_GAIN, gain_bytes, sizeof(gain_bytes));

    // Always try to leave CONFIG_UPDATE, even after an error
    BQ_Status s_exit = BQ_ExitConfigUpdate();
    if (s != BQ_OK) return s;
    if (s_exit != BQ_OK) return s_exit;

    // --- Verify by reading back ---
    if ((s = bq_verify_dm(BQ_DM_VCELL_MODE, vcell, sizeof(vcell))) != BQ_OK) return s;
    if ((s = bq_verify_dm(BQ_DM_DSG_CURR_THR, curr_thr, sizeof(curr_thr))) != BQ_OK) return s;
    if ((s = bq_verify_dm(BQ_DM_CB_CONFIG, cb, sizeof(cb))) != BQ_OK) return s;
    if ((s = bq_verify_dm(BQ_DM_CC_GAIN, gain_bytes, sizeof(gain_bytes))) != BQ_OK) return s;

    BQ_Health.configured = 1;
    BQ_Health.configs++;
    return BQ_OK;
}

BQ_Status BQ_Init(void) {
    BQ_Wake_SPI();
#if BQ_RESET_ON_BOOT
    BQ_Status s = BQ_Reset();
    if (s != BQ_OK) return s;
#endif
    return BQ_Configure();
}

// The POR flag is set by any full reset of the IC and cleared when
// CONFIG_UPDATE is exited. If it is set, the RAM settings are gone.
BQ_Status BQ_CheckForReset(void) {
    uint16_t bs;
    BQ_Status s = BQ_ReadReg16(BQ_REG_BATTERY_STATUS, &bs);
    if (s != BQ_OK) return s;
    if (bs & BQ_BATSTAT_POR) {
        BQ_Health.resets_detected++;
        return BQ_Configure();
    }
    return BQ_OK;
}

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------

// Reads all 16 inputs into BQ_Input_mV (for wiring checks) and the cells
// enabled in BQ_VCELL_MODE into cell_array[0..NUM_CELLS-1], in pack order.
BQ_Status BQ_Read_All_Cell_Voltages(uint16_t *cell_array) {
    uint8_t n = 0;
    for (uint8_t input = 0; input < 16; input++) {
        uint16_t mv;
        BQ_Status s = BQ_ReadReg16((uint8_t)(0x14 + input * 2), &mv);   // Cell 1 = 0x14
        if (s != BQ_OK) return s;
        BQ_Input_mV[input] = mv;
        if ((BQ_VCELL_MODE & (1U << input)) && n < NUM_CELLS) cell_array[n++] = mv;
    }
    return BQ_OK;
}

// Stack voltage (0x34) in mV. Raw unit is 10 mV (default DA Configuration).
// uint32 because a full 16S pack is above 65.5 V.
BQ_Status BQ_Read_Stack_Voltage(uint32_t *stack_mV) {
    uint16_t raw;
    BQ_Status s = BQ_ReadReg16(0x34, &raw);
    if (s == BQ_OK) *stack_mV = (uint32_t)raw * 10U;
    return s;
}

// Internal die temperature (0x68), unit 0.1 K
BQ_Status BQ_Read_Internal_Temp_C(float *temp_c) {
    uint16_t raw;
    BQ_Status s = BQ_ReadReg16(0x68, &raw);
    if (s == BQ_OK) *temp_c = ((float)raw / 10.0f) - 273.15f;
    return s;
}

// CC2 current (0x3A), int16 in mA (CC Gain written for BQ_SHUNT_MOHM in BQ_Configure).
// Positive = charge, negative = discharge.
BQ_Status BQ_Get_Pack_Current_Amps(float *amps) {
    uint16_t raw;
    BQ_Status s = BQ_ReadReg16(0x3A, &raw);
    if (s == BQ_OK) *amps = (float)(int16_t)raw / 1000.0f;
    return s;
}

// REG18 voltage in ADC counts (first 2 bytes of DASTATUS5)
BQ_Status BQ_Read_REG18_ADC(uint16_t *counts) {
    uint8_t buf[2];
    BQ_Status s = BQ_SubcommandRead(BQ_SUB_DASTATUS5, buf, 2);
    if (s == BQ_OK) *counts = (uint16_t)(buf[0] | (buf[1] << 8));
    return s;
}

BQ_Status BQ_Read_Balance_Status(const uint16_t *cell_mV, BQ_Balance_Status_t *status) {
    uint8_t buf[2];
    BQ_Status s = BQ_SubcommandRead(BQ_SUB_CB_ACTIVE_CELLS, buf, 2);
    if (s != BQ_OK) return s;

    uint16_t mask = (uint16_t)(buf[0] | (buf[1] << 8));   // bit 0 = cell 1
    status->hw_balance_mask = mask;

    // Same mask in pack order (skip the unused inputs)
    uint16_t cmask = 0;
    uint8_t  n = 0;
    for (uint8_t input = 0; input < 16; input++) {
        if (!(BQ_VCELL_MODE & (1U << input))) continue;
        if (mask & (1U << input)) cmask |= (uint16_t)(1U << n);
        n++;
    }
    status->cell_balance_mask = cmask;

    uint8_t count = 0;
    for (uint16_t tmp = mask; tmp; tmp >>= 1) count += (uint8_t)(tmp & 1U);
    status->cells_balancing  = count;
    status->balancing_active = (count > 0) ? 1U : 0U;

    uint16_t v_min = cell_mV[0], v_max = cell_mV[0];
    for (uint8_t i = 1; i < NUM_CELLS; i++) {
        if (cell_mV[i] < v_min) v_min = cell_mV[i];
        if (cell_mV[i] > v_max) v_max = cell_mV[i];
    }
    status->min_voltage_mV = v_min;
    status->max_voltage_mV = v_max;
    status->delta_mV       = (uint16_t)(v_max - v_min);
    return BQ_OK;
}

// Reads all safety flags. Alarm Status bits are latched: they are cleared
// here (write 1 to clear) so the next read shows only new events.
BQ_Status BQ_Read_Safety(BQ_Safety_t *sf) {
    BQ_Status s;
    if ((s = BQ_ReadReg8(0x02, &sf->safety_alert_a))  != BQ_OK) return s;
    if ((s = BQ_ReadReg8(0x03, &sf->safety_status_a)) != BQ_OK) return s;
    if ((s = BQ_ReadReg8(0x04, &sf->safety_alert_b))  != BQ_OK) return s;
    if ((s = BQ_ReadReg8(0x05, &sf->safety_status_b)) != BQ_OK) return s;
    if ((s = BQ_ReadReg8(0x06, &sf->safety_alert_c))  != BQ_OK) return s;
    if ((s = BQ_ReadReg8(0x07, &sf->safety_status_c)) != BQ_OK) return s;
    if ((s = BQ_ReadReg16(BQ_REG_BATTERY_STATUS, &sf->battery_status)) != BQ_OK) return s;
    if ((s = BQ_ReadReg16(BQ_REG_ALARM_STATUS, &sf->alarm_status)) != BQ_OK) return s;

    if (sf->alarm_status) {
        if ((s = BQ_SPI_WriteReg(BQ_REG_ALARM_STATUS, (uint8_t)(sf->alarm_status & 0xFF))) != BQ_OK) return s;
        if ((s = BQ_SPI_WriteReg(BQ_REG_ALARM_STATUS + 1, (uint8_t)(sf->alarm_status >> 8))) != BQ_OK) return s;
    }
    return BQ_OK;
}
