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
#include "bq76952.h"
//V1.0.1 -Hasan Turan

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

// Live expressions for the debugger
uint16_t            Cell_Voltages_mV[NUM_CELLS] = {0};
uint16_t            REG18_Raw_ADC = 0;
uint32_t            Stack_Voltage_mV = 0;
uint32_t            Cell_Sum_mV = 0;      // sum of the NUM_CELLS cell voltages
int32_t             Stack_Diff_mV = 0;    // Stack - Cell_Sum: about 0 when the wiring matches BMS_CELL_CONFIG
float               Internal_Temp_C = 0.0f;
float               Battery_Current = 0.0f;
BQ_Balance_Status_t Balance_Status = {0};
BQ_Safety_t         BQ_Safety = {0};      // safety flags, Battery Status, Alarm Status
uint32_t            Loop_Count = 0;
uint32_t            Loop_Errors = 0;      // loops where at least one read failed
uint8_t             Consecutive_Errors = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// Independent watchdog: resets the MCU if the main loop hangs.
// Set to 0 while single-stepping if you prefer; the counter is frozen
// anyway while the debugger halts the core (DBG_IWDG_STOP).
#define BMS_USE_IWDG          1
// LSI ~40 kHz / 64 = 625 Hz; 1250 counts = ~2 s (1.3-4 s over LSI tolerance)
#define BMS_IWDG_RELOAD       1250U

// After this many loops in a row with failed reads, re-initialise the BQ76952
#define BMS_MAX_CONSEC_ERRORS 5U

static void BMS_IWDG_Start(void) {
#if BMS_USE_IWDG
    DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP;   // stop the watchdog while halted
    IWDG->KR  = 0x5555;                      // unlock PR/RLR
    IWDG->PR  = 4;                           // prescaler /64
    IWDG->RLR = BMS_IWDG_RELOAD;
    IWDG->KR  = 0xAAAA;                      // reload
    IWDG->KR  = 0xCCCC;                      // start
#endif
}

static inline void BMS_IWDG_Kick(void) {
#if BMS_USE_IWDG
    IWDG->KR = 0xAAAA;
#endif
}

// Retry BQ_Init a few times; returns BQ_OK when the IC is configured
static BQ_Status BMS_Init_BQ(void) {
    BQ_Status s = BQ_ERR_TIMEOUT;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        BMS_IWDG_Kick();
        s = BQ_Init();
        if (s == BQ_OK) break;
        HAL_Delay(100);
    }
    return s;
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

  // Give the BQ76952 time to boot if the whole board just powered up
  HAL_Delay(50);

  // Reset (BQ_RESET_ON_BOOT) + write and verify configuration.
  // If it fails, the main loop keeps retrying; check BQ_Health in the debugger.
  (void)BMS_Init_BQ();

  BMS_IWDG_Start();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

	  BMS_IWDG_Kick();
	  HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
	  Loop_Count++;

	  BQ_Wake_SPI();

	  // Not configured yet, or the IC was reset (POR flag) -> (re)configure.
	  BQ_Status s = BQ_Health.configured ? BQ_CheckForReset() : BQ_Configure();

	  // Measurements. A failed read keeps the previous value.
	  if (s == BQ_OK) s = BQ_Read_All_Cell_Voltages(Cell_Voltages_mV);
	  if (s == BQ_OK) s = BQ_Read_Balance_Status(Cell_Voltages_mV, &Balance_Status);
	  if (s == BQ_OK) s = BQ_Read_Stack_Voltage(&Stack_Voltage_mV);
	  if (s == BQ_OK) {
	      // Wiring check: a cell missing from BQ_VCELL_MODE shows up here
	      // (stack 10 mV resolution, so up to ~+/-50 mV is normal)
	      uint32_t sum = 0;
	      for (uint8_t i = 0; i < NUM_CELLS; i++) sum += Cell_Voltages_mV[i];
	      Cell_Sum_mV   = sum;
	      Stack_Diff_mV = (int32_t)Stack_Voltage_mV - (int32_t)sum;
	  }
	  if (s == BQ_OK) s = BQ_Read_Internal_Temp_C(&Internal_Temp_C);
	  if (s == BQ_OK) s = BQ_Get_Pack_Current_Amps(&Battery_Current);
	  if (s == BQ_OK) s = BQ_Read_REG18_ADC(&REG18_Raw_ADC);

	  // Safety flags: safety_status_a bits = BQ_SSA_* (CUV, COV, OCC, OCD, SCD),
	  // alarm_status bits = BQ_ALARM_* (CB = balancing active, SSA/SSBC = fault)
	  if (s == BQ_OK) s = BQ_Read_Safety(&BQ_Safety);

	  if (s != BQ_OK) {
	      Loop_Errors++;
	      if (++Consecutive_Errors >= BMS_MAX_CONSEC_ERRORS) {
	          Consecutive_Errors = 0;
	          (void)BMS_Init_BQ();    // lost contact: reset and reconfigure
	      }
	  } else {
	      Consecutive_Errors = 0;
	  }

	  // Loop period: ~100 ms plus the time spent on SPI reads.
	  HAL_Delay(100);

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
