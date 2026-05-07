#include "bq76952.h"

// Borrow the SPI handler from main.c
extern SPI_HandleTypeDef hspi1;

// 1. Calculate CRC (Mandatory for BQ76952)
uint8_t BQ_CalculateCRC(uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc = (crc << 1);
        }
    }
    return crc;
}

// 2. Reusable WRITE Function
void BQ_SPI_WriteReg(uint8_t reg_addr, uint8_t data) {
    uint8_t tx_buf[3];
    uint8_t rx_buf[3];

    // R/W bit is 1 for Write (Add 0x80 to the address)
    tx_buf[0] = reg_addr | 0x80;
    tx_buf[1] = data;
    tx_buf[2] = BQ_CalculateCRC(tx_buf, 2);

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    HAL_Delay(1);
}

// 3. Reusable READ Function (TI Pipelined Architecture)
uint8_t BQ_SPI_ReadReg(uint8_t reg_addr) {
    uint8_t tx_buf[3];
    uint8_t rx_buf[3] = {0, 0, 0};
    uint8_t timeout = 10;

    tx_buf[0] = reg_addr;
    tx_buf[1] = 0xFF;
    tx_buf[2] = BQ_CalculateCRC(tx_buf, 2);

    // Step A: Send request
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    HAL_Delay(2);

    // Step B: Clock the data out
    while (timeout > 0) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
        HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

        if (rx_buf[0] == reg_addr) {
            break;
        }
        HAL_Delay(1);
        timeout--;
    }
    return rx_buf[1];
}

// 4. Configure 7S Vcell Mode
void BQ_Configure_Cell_Count(void) {
    // Step 1: Enter Config Update Mode (subcommand 0x0090)
    // BQ76952 SPI is little-endian: 0x3E = low byte, 0x3F = high byte
    BQ_SPI_WriteReg(0x3E, 0x90);  // Low byte of 0x0090
    BQ_SPI_WriteReg(0x3F, 0x00);  // High byte of 0x0090
    HAL_Delay(5);  // Allow IC time to enter Config Update mode (TRM: >=2 ms)

    // Step 2: Target subcommand 0x9304 (Settings:Configuration:Active Cells)
    BQ_SPI_WriteReg(0x3E, 0x04);  // Low byte of 0x9304
    BQ_SPI_WriteReg(0x3F, 0x93);  // High byte of 0x9304

    // Step 3: Write the 2-byte ActiveCells payload to Transfer Buffer
    // FIX: 0x7F enables VC1-VC7 (bits 0-6 = 7 cells).
    //      Old value 0x1F = 0b00011111 only enabled VC1-VC5 (bits 0-4).
    BQ_SPI_WriteReg(0x40, 0x7F);  // Byte 1: VC7(b6)..VC1(b0) = 0b01111111
    BQ_SPI_WriteReg(0x41, 0x00);  // Byte 2: VC8-VC16 all disabled

    // Step 4: Write Checksum and Length (required by subcommand data-write protocol)
    // BQ76952 uses a BYTE-SUM checksum (NOT XOR):
    //   Checksum = ~( (addr_low + addr_high + data[0] + data[1]) & 0xFF )
    //            = ~( (0x04 + 0x93 + 0x7F + 0x00) & 0xFF )
    //            = ~( (278) & 0xFF ) = ~(0x16) = 0xE9
    BQ_SPI_WriteReg(0x60, 0xE9);  // Correct checksum byte
    BQ_SPI_WriteReg(0x61, 0x06);  // Length: 2 (sub addr) + 2 (data) + 2 (chk+len) = 6

    HAL_Delay(5);

    // Step 5: Exit Config Update Mode (subcommand 0x0092)
    // The subcommand is 0x0092 (little-endian: low byte first):
    //   0x3E = 0x92 (low byte of 0x0092)
    //   0x3F = 0x00 (high byte of 0x0092)
    BQ_SPI_WriteReg(0x3E, 0x92);  // Low byte of 0x0092
    BQ_SPI_WriteReg(0x3F, 0x00);  // High byte of 0x0092
    HAL_Delay(5);
}

// 5. Read all 7 active cells and save to the provided array
void BQ_Read_All_Cell_Voltages(uint16_t *cell_array) {
    // BQ76952 Direct Command cell voltage register map (contiguous, little-endian):
    //   Cell 1: 0x14/0x15   Cell 2: 0x16/0x17   Cell 3: 0x18/0x19
    //   Cell 4: 0x1A/0x1B   Cell 5: 0x1C/0x1D   Cell 6: 0x1E/0x1F
    //   Cell 7: 0x20/0x21
    //
    // FIX: Loop extended from 5 to 7 iterations using the unified formula.
    //      Old code incorrectly read cells 6 & 7 from 0x30/0x31 and 0x32/0x33,
    //      which are the VC15 and VC16 registers - unconnected in a 7S pack.
    for (int i = 0; i < 7; i++) {
        uint8_t low_addr  = 0x14 + (i * 2);
        uint8_t high_addr = low_addr + 1;
        uint8_t cell_low  = BQ_SPI_ReadReg(low_addr);
        uint8_t cell_high = BQ_SPI_ReadReg(high_addr);
        cell_array[i] = (uint16_t)((cell_high << 8) | cell_low);
    }
}

// 6. Read the internal 1.8V LDO (REG18) Diagnostic ADC counts
uint16_t BQ_Read_REG18_ADC(void) {
    // 1. Write the subcommand 0x0075 to the 0x3E Mailbox
    BQ_SPI_WriteReg(0x3E, 0x75); // Lower byte of 0x0075
    BQ_SPI_WriteReg(0x3F, 0x00); // Upper byte of 0x0075

    // 2. Wait for the BQ76952 to fetch the diagnostic data
    HAL_Delay(5);

    // 3. Read the answer from the Transfer Buffer (0x40 and 0x41)
    uint8_t reg18_low = BQ_SPI_ReadReg(0x40);
    uint8_t reg18_high = BQ_SPI_ReadReg(0x41);

    // 4. Combine them into a 16-bit number and return
    return (reg18_high << 8) | reg18_low;
}

// 7. Read the Total Stack Voltage (VC16 pin)
uint16_t BQ_Read_Stack_Voltage(void) {
    uint8_t stack_low = BQ_SPI_ReadReg(0x34);
    uint8_t stack_high = BQ_SPI_ReadReg(0x35);

    // Combine the bytes. The raw value is in 10 mV units (Centivolts)
    uint16_t stack_cv = (stack_high << 8) | stack_low;

    // Multiply by 10 to return the value in standard millivolts (mV)
    // Note: A uint16_t can hold up to 65,535 mV (65.5V), which is safe for a 7S pack.
    return stack_cv * 10;
}
// 8. Read the Internal Die Temperature
float BQ_Read_Internal_Temp_C(void) {
    // Read the 16-bit temperature data
    uint8_t temp_low = BQ_SPI_ReadReg(0x68);
    uint8_t temp_high = BQ_SPI_ReadReg(0x69);

    // Combine the bytes into the raw 16-bit integer
    uint16_t raw_temp = (temp_high << 8) | temp_low;

    // The raw value is in units of 0.1 Kelvin.
    // Convert to Celsius: (Raw / 10.0) - 273.15
    float temp_celsius = (raw_temp / 10.0f) - 273.15f;

    return temp_celsius;
}

// Read the actual pack current in Amps using STM32 math (No BQ RAM writes required)
float BQ_Get_Pack_Current_Amps(void) {
    // Read direct commands 0x3A (CC2 Current Low Byte) and 0x3B (High Byte)
    uint8_t curr_low = BQ_SPI_ReadReg(0x3A);
    uint8_t curr_high = BQ_SPI_ReadReg(0x3B);

    // Combine into a signed 16-bit integer
    // 2's complement natively handles negative values for discharge currents
    int16_t current_raw = (int16_t)((curr_high << 8) | curr_low);

    // --- The Math Explanation ---
    // Out of the box, the BQ76952 expects a 1 mOhm sense resistor, where 1 LSB = 1 mA.
    // Because your hardware uses a 2 mOhm resistor, the voltage drop across it is twice as large.
    // This means the BQ76952 will report a raw number that is exactly DOUBLE your actual current.
    // To get the real current in mA, we divide by 2. To get Amperes, we divide by 1000.
    // Combined, we just divide the raw reading by 2000.0f.

    float actual_current_amps = (float)current_raw / 2000.0f;

    return actual_current_amps;
}


// Wake the BQ76952's internal oscillator before a burst of SPI reads
void BQ_Wake_SPI(void) {
    uint8_t tx_buf[3] = {0x00, 0xFF, 0xFF}; // Dummy read to Control Status (0x00)
    uint8_t rx_buf[3];

    // 1. Send the dummy frame to wake the High-Frequency Oscillator (HFO)
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    // 2. Wait for the HFO to stabilize (TRM: >=135 us; 1 ms is safe).
    HAL_Delay(1);
}

// ===========================================================================
// CELL BALANCING SUBSYSTEM - Autonomous Mode (BQ76952 does all the work)
// ===========================================================================

// ---------------------------------------------------------------------------
// Private Helper: Write a 1-byte value into BQ76952 Data Memory.
// Must be called ONLY while the IC is already in CONFIG_UPDATE mode.
// ---------------------------------------------------------------------------
static void BQ_WriteDataMem1(uint16_t sub_addr, uint8_t value) {
    uint8_t lo_a = (uint8_t)(sub_addr & 0xFF);
    uint8_t hi_a = (uint8_t)(sub_addr >> 8);

    BQ_SPI_WriteReg(0x3E, lo_a);
    BQ_SPI_WriteReg(0x3F, hi_a);
    BQ_SPI_WriteReg(0x40, value);

    // Checksum = ~( (addr_low + addr_high + data) & 0xFF )
    uint8_t chk = (uint8_t)(~((lo_a + hi_a + value) & 0xFF));
    BQ_SPI_WriteReg(0x60, chk);
    BQ_SPI_WriteReg(0x61, 0x05);   // Length: 2 (addr) + 1 (data) + 2 (chk+len) = 5

    HAL_Delay(5);
}

// ---------------------------------------------------------------------------
// Private Helper: Write a 2-byte (16-bit) value into BQ76952 Data Memory.
// Must be called ONLY while the IC is already in CONFIG_UPDATE mode.
// ---------------------------------------------------------------------------
static void BQ_WriteDataMem2(uint16_t sub_addr, uint16_t value) {
    uint8_t lo_a = (uint8_t)(sub_addr & 0xFF);
    uint8_t hi_a = (uint8_t)(sub_addr >> 8);
    uint8_t lo_d = (uint8_t)(value & 0xFF);
    uint8_t hi_d = (uint8_t)(value >> 8);

    BQ_SPI_WriteReg(0x3E, lo_a);
    BQ_SPI_WriteReg(0x3F, hi_a);
    BQ_SPI_WriteReg(0x40, lo_d);
    BQ_SPI_WriteReg(0x41, hi_d);

    // Checksum = ~( (addr_low + addr_high + data_low + data_high) & 0xFF )
    uint8_t chk = (uint8_t)(~((lo_a + hi_a + lo_d + hi_d) & 0xFF));
    BQ_SPI_WriteReg(0x60, chk);
    BQ_SPI_WriteReg(0x61, 0x06);   // Length: 2 (addr) + 2 (data) + 2 (chk+len) = 6

    HAL_Delay(5);
}

// ---------------------------------------------------------------------------
// 9. Configure BQ76952 Autonomous Cell Balancing
//    Call ONCE at startup, after BQ_Configure_Cell_Count().
//
//    This configures the IC's INTERNAL balancer so it runs on its own.
//    The STM32 does NOT need to send balance commands every loop.
//
//    Data Memory registers written (BQ76952 TRM - Settings:Cell Balancing):
//      0x9330 - CB Min Cell Temp       (1 byte,  °C, signed) [SAFETY]
//      0x9331 - CB Max Cell Temp       (1 byte,  °C)         [SAFETY]
//      0x9332 - CB Max Internal Temp   (1 byte,  °C)         [SAFETY]
//      0x9333 - CB Interval            (1 byte,  seconds)
//      0x9335 - Balancing Configuration(1 byte,  enable bits)
//      0x9336 - CB Active Cells        (2 bytes, cell bitmask)
//      0x9338 - CB Max Cells           (1 byte,  max simultaneous)
//      0x9339 - CB Min Cell V          (2 bytes, mV)
//      0x933B - CB Min Delta           (1 byte,  mV)  start threshold
//      0x933C - CB Stop Delta          (1 byte,  mV)  stop hysteresis
// ---------------------------------------------------------------------------
void BQ_Configure_Balancing(void) {

    // -----------------------------------------------------------------------
    // Step 1: Enter CONFIG_UPDATE mode (subcommand 0x0090).
    // Poll the CFGUPDATE bit (bit 2 of BatteryStatus low byte, direct cmd 0x12)
    // instead of relying on a blind delay, so we know the IC is actually ready.
    // -----------------------------------------------------------------------
    BQ_SPI_WriteReg(0x3E, 0x90);
    BQ_SPI_WriteReg(0x3F, 0x00);

    uint32_t t_start = HAL_GetTick();
    while (!(BQ_SPI_ReadReg(0x12) & 0x04)) {   // Wait for CFGUPDATE bit
        if (HAL_GetTick() - t_start > 500U) {
            Error_Handler();  // IC did not enter CONFIG_UPDATE within 500 ms
        }
        HAL_Delay(1);
    }

    // -----------------------------------------------------------------------
    // Step 2: Write all cell-balancing configuration registers.
    // -----------------------------------------------------------------------

    // --- Temperature Guards (SAFETY CRITICAL — TI SLUAA81A Figure 3-2) ---
    // Balancing is automatically inhibited outside these temperature bounds.
    BQ_WriteDataMem1(0x9330, (uint8_t)(-20)); // CB Min Cell Temp  = -20 °C
    BQ_WriteDataMem1(0x9331, 60);             // CB Max Cell Temp  =  60 °C
    BQ_WriteDataMem1(0x9332, 70);             // CB Max Internal Temp = 70 °C

    // --- Balancing Interval = 20 s (TI recommended) ---
    // The IC re-evaluates which cell to balance every 20 seconds.
    BQ_WriteDataMem1(0x9333, 20);

    // --- Balancing Configuration = 0x03 ---
    //   Bit 0 = CB_RLX (balance during relaxation) = 1
    //   Bit 1 = CB_CHG (balance during charging)   = 1
    BQ_WriteDataMem1(0x9335, 0x03);

    // --- CB Active Cells = 0x007F ---
    //   Bit 0 = VC1 ... Bit 6 = VC7 -> all 7 cells eligible
    BQ_WriteDataMem2(0x9336, 0x007F);

    // --- CB Max Cells = 1 ---
    // Limit simultaneous balancing to 1 cell at a time for thermal safety.
    // Balancing all 7 cells at once dissipates ~0.7 W into the IC die.
    BQ_WriteDataMem1(0x9338, 1);

    // --- CB Min Cell Voltage = BALANCE_MIN_VOLTAGE_MV ---
    // A cell below this voltage is not eligible for balancing.
    // NOTE: Set to 2800 mV intentionally during testing to observe balancing
    //       at all SOC levels. Change to 3900 mV for production deployment.
    BQ_WriteDataMem2(0x9339, BALANCE_MIN_VOLTAGE_MV);

    // --- CB Min Delta = BALANCE_THRESHOLD_MV (start threshold) ---
    // Balancing begins when (max_cell - min_cell) exceeds this value.
    BQ_WriteDataMem1(0x933B, (uint8_t)BALANCE_THRESHOLD_MV);

    // --- CB Stop Delta = BALANCE_STOP_DELTA_MV (stop hysteresis) ---
    // Balancing stops when (max_cell - min_cell) falls below this value.
    // Must be lower than Min Delta to prevent rapid on/off cycling.
    BQ_WriteDataMem1(0x933C, (uint8_t)BALANCE_STOP_DELTA_MV);

    // -----------------------------------------------------------------------
    // Step 3: Exit CONFIG_UPDATE mode (subcommand 0x0092).
    // -----------------------------------------------------------------------
    BQ_SPI_WriteReg(0x3E, 0x92);
    BQ_SPI_WriteReg(0x3F, 0x00);
    HAL_Delay(10);   // Allow IC to commit and return to normal mode

    // -----------------------------------------------------------------------
    // Step 4: Read-back verification.
    // Read CB Active Cells (0x9336) low byte and confirm it equals 0x7F.
    // If this fails, the SPI write did not reach the IC correctly.
    // -----------------------------------------------------------------------
    BQ_SPI_WriteReg(0x3E, 0x36);
    BQ_SPI_WriteReg(0x3F, 0x93);
    HAL_Delay(3);
    uint8_t verify = BQ_SPI_ReadReg(0x40);
    if (verify != 0x7Fu) {
        Error_Handler();  // Balancing configuration write failed
    }
}

// ---------------------------------------------------------------------------
// 10. Read Balance Status - monitor-only, call every loop iteration.
//
//     Reads the BQ76952 CBSTATUS direct command register (0x82/0x83).
//     This is a READ-ONLY register that reports which cells the IC's
//     autonomous balancer is CURRENTLY draining.
//
//     The STM32 does NOT command balancing - just observes.
//     Also computes min/max/delta from the cell voltage array for diagnostics.
// ---------------------------------------------------------------------------
void BQ_Read_Balance_Status(uint16_t *cell_mV, BQ_Balance_Status_t *status) {

    // --- Read CBSTATUS from BQ76952 direct command 0x82 (low byte) ---
    // Each bit = 1 means that cell's internal balance FET is currently ON.
    // Bit 0 = Cell 1, Bit 1 = Cell 2, ..., Bit 6 = Cell 7
    uint8_t cb_low = BQ_SPI_ReadReg(0x82);

    status->hw_balance_mask = cb_low & 0x7F;  // Mask to 7 cells

    // --- Count how many cells are actively balancing ---
    uint8_t count = 0;
    uint8_t tmp = status->hw_balance_mask;
    while (tmp) {
        count += (tmp & 1U);
        tmp >>= 1;
    }
    status->cells_balancing = count;
    status->balancing_active = (count > 0) ? 1U : 0U;

    // --- Compute min/max/delta from cell voltages for diagnostics ---
    uint16_t v_min = cell_mV[0];
    uint16_t v_max = cell_mV[0];
    for (uint8_t i = 1; i < NUM_CELLS; i++) {
        if (cell_mV[i] < v_min) v_min = cell_mV[i];
        if (cell_mV[i] > v_max) v_max = cell_mV[i];
    }
    status->min_voltage_mV = v_min;
    status->max_voltage_mV = v_max;
    status->delta_mV       = v_max - v_min;
}
