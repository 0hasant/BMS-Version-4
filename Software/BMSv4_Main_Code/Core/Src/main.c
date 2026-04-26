/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;

/* USER CODE BEGIN PV */

volatile uint16_t My_Device_Number = 0;
volatile uint16_t Battery_Status = 0;
volatile uint16_t REG18_Voltage_mV = 0; // Internal Reg Value
volatile uint16_t Cell_Voltages_mV[7] = {0};// 7 Hücrenin voltajını tutacak dizi (Index 0 = Cell 1, Index 6 = Cell 7)

// Diagnostic Variables
volatile uint8_t debug_Address_Echo = 0;
volatile uint8_t debug_Data_Byte = 0;
volatile uint8_t debug_CRC_Byte = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// 1. Calculate CRC (Mandatory for your specific BQ chip)
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

  HAL_Delay(1); // Give BQ time to apply the write
}

// 3. Reusable READ Function (TI Pipelined Architecture)
uint8_t BQ_SPI_ReadReg(uint8_t reg_addr) {
  uint8_t tx_buf[3];
  uint8_t rx_buf[3] = {0, 0, 0};
  uint8_t timeout = 10; // Maximum attempts

  // Step A: Prepare the Read Request
  // R/W bit is 0 for Read, so we just use the raw reg_addr
  tx_buf[0] = reg_addr;
  tx_buf[1] = 0xFF; // TI officially uses 0xFF for data during a Read request
  tx_buf[2] = BQ_CalculateCRC(tx_buf, 2);

  // Step B: Send the initial request (Tells the chip to fetch the data)
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
  HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

  HAL_Delay(2); // Give the BQ time to process

  // Step C: Send the EXACT SAME packet again to clock the data out.
  // Keep asking until the BQ echoes the register address in rx_buf[0]
  while (timeout > 0) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 100);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    // Check if the BQ replied with our address!
    if (rx_buf[0] == reg_addr) {
        break; // Success! The data is now in rx_buf[1]. Exit the loop.
    }

    HAL_Delay(1);
    timeout--;
  }

  // === DIAGNOSTIC TRAP ===
  // Save the raw frame so we can see what the BQ actually said!
  debug_Address_Echo = rx_buf[0];
  debug_Data_Byte = rx_buf[1];
  debug_CRC_Byte = rx_buf[2];

  return rx_buf[1];
}

void Read_BQ_Device_Number(void) {
  // 0. Wake up the SPI Oscillator
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
  HAL_Delay(1);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
  HAL_Delay(2);

  // 1. Write the subcommand 0x0001 to the Command Register (0x3E)
  BQ_SPI_WriteReg(0x3E, 0x01);
  BQ_SPI_WriteReg(0x3F, 0x00);

  // 2. Wait for the BQ chip to process the subcommand
  HAL_Delay(5);

  // 3. Read the result from the Transfer Buffer (0x40)
  uint8_t low_byte = BQ_SPI_ReadReg(0x40);
  uint8_t high_byte = BQ_SPI_ReadReg(0x41);

  // 4. Combine them into the final 16-bit number
  My_Device_Number = (high_byte << 8) | low_byte;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */

  Read_BQ_Device_Number();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  // 1. STM32'nin çalıştığını görmek için LED'i yakıp söndür
	  		HAL_GPIO_TogglePin(GPIOA, LED_Pin);

	  		// 2. Çipin uyku modunda olma ihtimaline karşı "Uyandırma Sinyali" ver
	  		HAL_GPIO_WritePin(GPIOA, CS_Pin, GPIO_PIN_RESET);
	  		HAL_Delay(1);
	  		HAL_GPIO_WritePin(GPIOA, CS_Pin, GPIO_PIN_SET);
	  		HAL_Delay(3); // Çipin iç saatinin uyanmasını bekle

	  		// =========================================================

	  				// TEST 1: REG18 DIAGNOSTIC ADC COUNTS (Subcommand 0x0075)
	  				// =========================================================
	  				// 1. Write the subcommand 0x0075 to the 0x3E Mailbox
	  				BQ_SPI_WriteReg(0x3E, 0x75); // Lower byte of 0x0075
	  				BQ_SPI_WriteReg(0x3F, 0x00); // Upper byte of 0x0075

	  				// 2. Wait for the BQ76952 to fetch the diagnostic data
	  				HAL_Delay(5);

	  				// 3. Read the answer from the Transfer Buffer (0x40 and 0x41)
	  				// We use the pipelined ReadReg function you just perfected!
	  				uint8_t reg18_low = BQ_SPI_ReadReg(0x40);
	  				uint8_t reg18_high = BQ_SPI_ReadReg(0x41);

	  				// 4. Combine them into a 16-bit number
	  				// NOTE: Expect a value near ~29137, NOT 1800!
	  				REG18_Voltage_mV = (reg18_high << 8) | reg18_low;


	  				// =========================================================
	  					  // 7 HÜCRENİN VOLTAJINI OTOMATİK OKUMA (DÖNGÜ İLE)
	  					  // =========================================================
	  				      // Hücre 1'in adresi 0x14'ten başlar, her hücre 2 byte yer kaplar.
	  					  for (int i = 0; i < 7; i++) {
	  						  // Matematiksel olarak adresi hesapla
	  						  uint8_t low_addr = 0x14 + (i * 2);
	  						  uint8_t high_addr = low_addr + 1;

	  						  // O hesaplanan adresten okuma yap
	  						  uint8_t cell_low = BQ_SPI_ReadReg(low_addr);
	  						  uint8_t cell_high = BQ_SPI_ReadReg(high_addr);

	  						  // Dizinin i. elemanına voltajı kaydet
	  						  Cell_Voltages_mV[i] = (cell_high << 8) | cell_low;
	  					  }



	  		// 4. Sistemi yormamak için yarım saniye bekle ve tekrarla
	  		HAL_Delay(500);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_128;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pins : LED_Pin CS_Pin */
  GPIO_InitStruct.Pin = LED_Pin|CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
