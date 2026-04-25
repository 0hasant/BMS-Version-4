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
uint16_t REG18_Voltage_mV = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// BQ76952 Haberleşme Durum Kodları
typedef enum {
	BQ_OK = 0x00,              // İşlem başarılı, veri geçerli
	BQ_ERR_SLEEP = 0x01,       // Çip uykuda veya osilatör hazır değil (0xFFFF)
	BQ_ERR_CRC_PREV = 0x02,    // Bir önceki gönderdiğimiz pakette CRC hatası yapmışız (0xAA)
	BQ_ERR_NOT_READY = 0x03,   // Çip veriyi buffer'a koymaya yetişemedi (0x00)
	BQ_ERR_CRC_RX = 0x04,      // Çipten gelen verinin CRC'si bozuk
	BQ_ERR_SPI = 0x05          // STM32 donanımsal SPI hatası (Kablo kopuk vb.)
} BQ_StatusTypeDef;

// Daha önce yazdığımız yazılım CRC fonksiyonumuz
uint8_t BQ_CalculateCRC(uint8_t *data, uint8_t length) {
	uint8_t crc = 0x00;
	for (uint8_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (uint8_t bit = 0; bit < 8; bit++) {
			if (crc & 0x80) crc = (crc << 1) ^ 0x07;
			else            crc = (crc << 1);
		}
	}
	return crc;
}

// Statüs döndüren güvenli okuma fonksiyonu
BQ_StatusTypeDef BQ_SPI_ReadReg(uint8_t reg_addr, uint8_t *data_out) {
	uint8_t tx_buf[3];
	uint8_t rx_buf[3];
	HAL_StatusTypeDef spi_status;

	// ==========================================
	// ADIM A: Çipe Adresi Söyle (R/W = 0)
	// ==========================================
	tx_buf[0] = reg_addr & 0x7F; // Okuma için MSB = 0
	tx_buf[1] = 0x00;            // Data baytı (Okumada önemsiz)

	// Eğer System Core altındaki bağımsız CRC'yi kullanacaksanız burayı açın:
	//tx_buf[2] = (uint8_t)HAL_CRC_Calculate(&hcrc, (uint32_t*)tx_buf, 2);
	tx_buf[2] = BQ_CalculateCRC(tx_buf, 2);
	HAL_GPIO_WritePin(GPIOA, CS_Pin, GPIO_PIN_RESET);
	HAL_Delay(2);
	spi_status = HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 50);
	HAL_Delay(2);
	HAL_GPIO_WritePin(GPIOA, CS_Pin, GPIO_PIN_SET);

	if (spi_status != HAL_OK) return BQ_ERR_SPI;

	// Çipin veriyi hazırlaması için bekle
	HAL_Delay(2);

	// ==========================================
	// ADIM B: Veriyi Çek (Dummy Transaction)
	// ==========================================
	tx_buf[0] = 0x00;
	tx_buf[1] = 0x00;
	tx_buf[2] = BQ_CalculateCRC(tx_buf, 2);

	HAL_GPIO_WritePin(GPIOA, CS_Pin, GPIO_PIN_RESET);
	spi_status = HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, 50);
	HAL_GPIO_WritePin(GPIOA, CS_Pin, GPIO_PIN_SET);

	if (spi_status != HAL_OK) return BQ_ERR_SPI;

	// ==========================================
	// HATA KONTROLLERİ (Datasheet'e göre)
	// ==========================================
	// Eğer ilk iki bayt 0xFFFF ise özel hata durumları incelenir
	if (rx_buf[0] == 0xFF && rx_buf[1] == 0xFF) {
		if (rx_buf[2] == 0xFF) return BQ_ERR_SLEEP;    // Çip uykuda
		if (rx_buf[2] == 0xAA) return BQ_ERR_CRC_PREV; // Önceki işlemde CRC hatası
		if (rx_buf[2] == 0x00) return BQ_ERR_NOT_READY;// Veri yetişmedi
	}

	// CRC Doğrulaması: Gelen verinin CRC'si, son baytla eşleşiyor mu?
	uint8_t expected_crc = BQ_CalculateCRC(rx_buf, 2);
	if (expected_crc != rx_buf[2]) {
		return BQ_ERR_CRC_RX; // İletişimde veri bozuldu
	}

	// Her şey mükemmelse veriyi dışarı aktar ve OK dön
	*data_out = rx_buf[1];
	return BQ_OK;
}




// Çipin hayatta ve sağlıklı olup olmadığını kontrol eden "Ping" fonksiyonu
// Dönüş: 1 -> Çalışıyor ve sağlıklı, 0 -> İletişim yok veya çip uykuda
uint8_t BQ_IsAlive(void) {
	uint8_t reg18_low = 0, reg18_high = 0;
	BQ_StatusTypeDef status_low, status_high;
	uint16_t voltage_mV = 0;

	// Alt ve üst baytları oku
	status_low  = BQ_SPI_ReadReg(0x6C, &reg18_low);
	status_high = BQ_SPI_ReadReg(0x6D, &reg18_high);

	// İletişim başarılıysa voltajı hesapla
	if (status_low == BQ_OK && status_high == BQ_OK) {
		voltage_mV = (reg18_high << 8) | reg18_low;

		// REG18 pini 1.8V (1800 mV) üretir.
		// 1700mV ile 1900mV arasındaysa çip kesinlikle kusursuz çalışıyordur.
		if (voltage_mV > 1700 && voltage_mV < 1900) {
			return 1; // Başarılı!
		}
	}

	return 0; // Hata: İletişim koptu, CRC bozuk veya çip uykuda
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

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
	while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
		if (BQ_IsAlive() == 1) {
			// Çip çalışıyor, haberleşme mükemmel
			HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);

			// İsterseniz Debug modunda görmek için bir değişkene atayabilirsiniz:
			// IC_Status = "OK";
		}
		else {
			// İletişim yok veya hata var
			HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

			// Eğer uyku modundaysa uyandırmak için CS pinini tetikleyebilirsiniz
			HAL_GPIO_WritePin(GPIOA, CS_Pin, GPIO_PIN_RESET);
			HAL_Delay(1);
			HAL_GPIO_WritePin(GPIOA, CS_Pin, GPIO_PIN_SET);
			HAL_Delay(5); // Uyanması için bekle
		}

		HAL_Delay(500); // Sistemi yormamak için bekle
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
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
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
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
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
  HAL_GPIO_WritePin(GPIOA, LED_Pin|CS_Pin, GPIO_PIN_RESET);

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
