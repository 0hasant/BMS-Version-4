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
    uint8_t timeout = 10;

    tx_buf[0] = reg_addr | 0x80; // W=1
    tx_buf[1] = data;
    tx_buf[2] = BQ_CalculateCRC(tx_buf, 2);

    while (timeout > 0) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
        HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

        // Verification: MISO must echo the command byte
        if (rx_buf[0] == tx_buf[0]) {
            break;
        }
        HAL_Delay(1);
        timeout--;
    }
}

// 3. Reusable READ Function (TI Pipelined Architecture)
uint8_t BQ_SPI_ReadReg(uint8_t reg_addr) {
    uint8_t tx_buf[3];
    uint8_t rx_buf[3] = {0, 0, 0};
    uint8_t timeout = 10;

    tx_buf[0] = reg_addr; // R=0
    tx_buf[1] = 0xFF;
    tx_buf[2] = BQ_CalculateCRC(tx_buf, 2);

    // Initial dummy clock to request data
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    while (timeout > 0) {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
        HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

        if (rx_buf[0] == reg_addr) {
            // Validate incoming CRC
            uint8_t expected_crc = BQ_CalculateCRC(rx_buf, 2);
            if (expected_crc == rx_buf[2]) {
                return rx_buf[1];
            } else {
                // Handle CRC error (e.g., retry or trigger fault)
            }
        }
        HAL_Delay(1); // Wait for IC to prepare data
        timeout--;
    }
    return 0; // Or a designated error code
}

// 4. Configure 7S Vcell Mode
void BQ_Configure_Cell_Count(void) {
    BQ_SPI_WriteReg(0x3E, 0x90);
    BQ_SPI_WriteReg(0x3F, 0x00);
    HAL_Delay(2);

    BQ_SPI_WriteReg(0x3E, 0x04);
    BQ_SPI_WriteReg(0x3F, 0x93);

    BQ_SPI_WriteReg(0x40, 0x1F);
    BQ_SPI_WriteReg(0x41, 0xC0);

    BQ_SPI_WriteReg(0x60, 0x89);
    BQ_SPI_WriteReg(0x61, 0x06);

    HAL_Delay(2);

    BQ_SPI_WriteReg(0x3E, 0x92);
    BQ_SPI_WriteReg(0x3F, 0x00);
    HAL_Delay(2);
}

// 5. Read all 7 active cells and save to the provided array
void BQ_Read_All_Cell_Voltages(uint16_t *cell_array) {
    // Cells 1-5 (Addresses 0x14 through 0x1D)
    for (int i = 0; i < 5; i++) {
        uint8_t low_addr = 0x14 + (i * 2);
        uint8_t high_addr = low_addr + 1;
        uint8_t cell_low = BQ_SPI_ReadReg(low_addr);
        uint8_t cell_high = BQ_SPI_ReadReg(high_addr);
        cell_array[i] = (cell_high << 8) | cell_low;
    }

    // Cell 6 (Physically connected to VC14-VC15 -> Address 0x30)
    uint8_t c6_low = BQ_SPI_ReadReg(0x30);
    uint8_t c6_high = BQ_SPI_ReadReg(0x31);
    cell_array[5] = (c6_high << 8) | c6_low;

    // Cell 7 (Physically connected to VC15-VC16 -> Address 0x32)
    uint8_t c7_low = BQ_SPI_ReadReg(0x32);
    uint8_t c7_high = BQ_SPI_ReadReg(0x33);
    cell_array[6] = (c7_high << 8) | c7_low;
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

// 9. Helper function to write 4 bytes to Data Memory
void BQ_Write_DataMemory_4Byte(uint16_t mem_addr, uint32_t data) {
    uint8_t buffer[4];
    // Convert 32-bit data to Little Endian
    buffer[0] = (uint8_t)(data & 0xFF);         // LSB
    buffer[1] = (uint8_t)((data >> 8) & 0xFF);
    buffer[2] = (uint8_t)((data >> 16) & 0xFF);
    buffer[3] = (uint8_t)((data >> 24) & 0xFF); // MSB

    // Write internal memory address to 0x3E and 0x3F (Mailbox)
    BQ_SPI_WriteReg(0x3E, (uint8_t)(mem_addr & 0xFF));
    BQ_SPI_WriteReg(0x3F, (uint8_t)((mem_addr >> 8) & 0xFF));

    // Write the 4 bytes of data to the transfer buffer (0x40 - 0x43)
    BQ_SPI_WriteReg(0x40, buffer[0]);
    BQ_SPI_WriteReg(0x41, buffer[1]);
    BQ_SPI_WriteReg(0x42, buffer[2]);
    BQ_SPI_WriteReg(0x43, buffer[3]);

    // Calculate checksum and write to 0x60
    // Checksum = ~(0x3E_byte + 0x3F_byte + data0 + data1 + data2 + data3)
    uint8_t sum = (uint8_t)(mem_addr & 0xFF) + (uint8_t)((mem_addr >> 8) & 0xFF) +
                  buffer[0] + buffer[1] + buffer[2] + buffer[3];
    uint8_t checksum = ~sum;

    BQ_SPI_WriteReg(0x60, checksum);

    // Write length to 0x61 (Length = 4 bytes data + 2 bytes addr + 2 bytes checksum/len = 8)
    BQ_SPI_WriteReg(0x61, 0x08);

    HAL_Delay(2); // Allow IC to process the RAM write
}

// 10. Initialize the Current Scaling
void BQ_Configure_Current_Sensor(void) {
    // Enter CONFIG_UPDATE mode
    BQ_SPI_WriteReg(0x3E, 0x90);
    BQ_SPI_WriteReg(0x3F, 0x00);
    HAL_Delay(2);

    // Write CC Gain (3.7842 -> 0x40723055)
    // Assuming CC Gain register address is 0x9180 (Verify exact address in BQ Studio Data Memory map)
    BQ_Write_DataMemory_4Byte(0x9180, 0x40723055);

    // Write Capacity Gain (1128681.6 -> 0x4989CA29)
    // Assuming Capacity Gain register address is 0x9184
    BQ_Write_DataMemory_4Byte(0x9184, 0x4989CA29);

    // Exit CONFIG_UPDATE mode
    BQ_SPI_WriteReg(0x3E, 0x92);
    BQ_SPI_WriteReg(0x3F, 0x00);
    HAL_Delay(2);
}

// 11. Read the actual current in Amps
float BQ_Get_Pack_Current_Amps(void) {
    // Read 0x3A (CC2 Current Low Byte) and 0x3B (CC2 Current High Byte)
    uint8_t curr_low = BQ_SPI_ReadReg(0x3A);
    uint8_t curr_high = BQ_SPI_ReadReg(0x3B);

    // Combine into signed 16-bit
    int16_t current_raw = (int16_t)((curr_high << 8) | curr_low);

    // The result is in userAmps. By default, 1 userA = 1 mA[cite: 4].
    // If you haven't changed DA Configuration[USER_AMPS], divide by 1000 to get Amperes.
    return (float)current_raw / 1000.0f;
}
