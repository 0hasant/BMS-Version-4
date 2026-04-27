#ifndef BQ76952_H_
#define BQ76952_H_

#include "main.h"

// --- Public Function Prototypes ---

// Core SPI Functions
uint8_t BQ_CalculateCRC(uint8_t *data, uint8_t len);
void BQ_SPI_WriteReg(uint8_t reg_addr, uint8_t data);
uint8_t BQ_SPI_ReadReg(uint8_t reg_addr);

// BMS Setup and Configuration
void BQ_Configure_Cell_Count(void);

// BMS Data Reading
void BQ_Read_All_Cell_Voltages(uint16_t *cell_array);
uint16_t BQ_Read_Stack_Voltage(void);

// BMS Diagnostic Reading
uint16_t BQ_Read_REG18_ADC(void);

#endif /* BQ76952_H_ */
