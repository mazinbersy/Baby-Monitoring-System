
#include "main.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "queue.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

// audio config
#define SAMPLE_RATE         8000U
#define FFT_SIZE            1024U
#define HALF_FFT            (FFT_SIZE / 2U)
#define PI                  3.14159265358979f
#define HZ_TO_BIN(hz)       ((int)((hz) * FFT_SIZE / SAMPLE_RATE))

#define ENERGY_MIN          60000.0f

#define PEAK_MIN_HZ         300U
#define PEAK_MAX_HZ         3500U
#define CENT_MIN_HZ         1200U
#define CENT_MAX_HZ         2400U
#define B3_MIN_PCT          20
#define MID_BAND_MIN        50
#define PROMINENCE_MIN      25
#define B0_SILENCE_THRESH   80
#define SCORE_THRESH        75

#define QUIET_WINDOW        60U

#define WINDOW_FRAMES       234U    // 30s / 128ms per frame
#define VOTE_THRESH         60U

// PIR wakeup detection (30 x 2.5s = 75s window)
#define SLEEP_SAMPLE_WINDOW  30U
#define SLEEP_MOTION_THRESH   8U   // hits needed to declare baby awake
#define SLEEP_SAMPLE_MS    2500U

// temp thresholds - LM35 on PA0, 10mV/°C
#define TEMP_HIGH_C         30.0f
#define TEMP_LOW_C          18.0f
#define TEMP_SAMPLE_MS    5000U

uint16_t adc_buf[FFT_SIZE * 2];
static float fft_re[FFT_SIZE];
static float fft_im[FFT_SIZE];
static float fft_mag[HALF_FFT];

ADC_HandleTypeDef  hadc1;
DMA_HandleTypeDef  hdma_adc1;
TIM_HandleTypeDef  htim1;
UART_HandleTypeDef huart1;   // DFPlayer @ 9600
UART_HandleTypeDef huart2;   // ESP32-CAM / debug @ 115200

volatile uint8_t baby_sleeping = 0; /* 1 = sleep/door-watch mode, 0 = awake/no-motion mode */

// USART2 interrupt RX
static uint8_t  uart2_rx_byte;
static char     uart2_rx_buf[16];
static uint8_t  uart2_rx_idx    = 0;
static char     uart2_cmd[16];
volatile uint8_t uart2_cmd_ready = 0;

// FreeRTOS tasks and queues
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
    .name = "defaultTask", .stack_size = 128 * 4,
    .priority = (osPriority_t) osPriorityIdle,
};
osThreadId_t AudioCaptureHandle;
const osThreadAttr_t AudioCapture_attributes = {
    .name = "AudioCapture", .stack_size = 256 * 4,
    .priority = (osPriority_t) osPriorityRealtime,
};
osThreadId_t FFTHandle;
const osThreadAttr_t FFT_attributes = {
    .name = "FFT", .stack_size = 512 * 4,
    .priority = (osPriority_t) osPriorityNormal,
};
osThreadId_t AlertHandle;
const osThreadAttr_t Alert_attributes = {
    .name = "Alert", .stack_size = 256 * 4,
    .priority = (osPriority_t) osPriorityAboveNormal,
};
osThreadId_t PIRHandle;
const osThreadAttr_t PIR_attributes = {
    .name = "PIR", .stack_size = 128 * 4,
    .priority = (osPriority_t) osPriorityLow,
};
osThreadId_t TempHandle;
const osThreadAttr_t Temp_attributes = {
    .name = "Temp", .stack_size = 128 * 4,
    .priority = (osPriority_t) osPriorityLow,
};

osMessageQueueId_t AudioQueueHandle;
const osMessageQueueAttr_t AudioQueue_attributes  = { .name = "AudioQueue"  };
osMessageQueueId_t ResultQueueHandle;
const osMessageQueueAttr_t ResultQueue_attributes = { .name = "ResultQueue" };

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
void StartDefaultTask  (void *argument);
void Start_AudioCapture(void *argument);
void Start_FFT         (void *argument);
void Start_Alert       (void *argument);
void Start_PIR_Motion  (void *argument);
void Start_TempMonitor (void *argument);

static void uart_print(const char *s)
{
    HAL_UART_Transmit(&huart2, (const uint8_t *)s, strlen(s), 2000);
}

// DFPlayer command format: 7E FF 06 [cmd] 00 [p1] [p2] [ckhi] [cklo] EF
static void DFPlayer_SendCmd(uint8_t cmd, uint8_t param1, uint8_t param2)
{
    uint8_t buf[10];
    buf[0] = 0x7E;
    buf[1] = 0xFF;
    buf[2] = 0x06;
    buf[3] = cmd;
    buf[4] = 0x00;
    buf[5] = param1;
    buf[6] = param2;
    int16_t chk = -(buf[1] + buf[2] + buf[3] + buf[4] + buf[5] + buf[6]);
    buf[7] = (chk >> 8) & 0xFF;
    buf[8] =  chk       & 0xFF;
    buf[9] = 0xEF;
    HAL_UART_Transmit(&huart1, buf, 10, 100);
}

static void DFPlayer_Reset(void)            { DFPlayer_SendCmd(0x0C, 0x00, 0x00); }
static void DFPlayer_SetVolume(uint8_t vol) { DFPlayer_SendCmd(0x06, 0x00, vol);  }
static void DFPlayer_PlayFile(uint8_t n)    { DFPlayer_SendCmd(0x03, 0x00, n);    }
static void DFPlayer_Stop(void)             { DFPlayer_SendCmd(0x16, 0x00, 0x00); }

// in-place radix-2 FFT
static void simple_fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j)
        {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        float ang = -2.0f * PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len)
        {
            float cr = 1.0f, ci = 0.0f;
            for (int j = 0; j < len / 2; j++)
            {
                float ur = re[i+j], ui = im[i+j];
                float vr = re[i+j+len/2]*cr - im[i+j+len/2]*ci;
                float vi = re[i+j+len/2]*ci + im[i+j+len/2]*cr;
                re[i+j]       = ur+vr; im[i+j]       = ui+vi;
                re[i+j+len/2] = ur-vr; im[i+j+len/2] = ui-vi;
                float ncr = cr*wr - ci*wi;
                ci = cr*wi + ci*wr; cr = ncr;
            }
        }
    }
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_ADC1_Init();
    MX_TIM1_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();

    TIM_MasterConfigTypeDef sMasterConfig = {0};
    sMasterConfig.MasterOutputTrigger  = TIM_TRGO_UPDATE;
    sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
    sMasterConfig.MasterSlaveMode      = TIM_MASTERSLAVEMODE_DISABLE;
    HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig);

    HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);

    // dfplayer needs HAL_Delay so init before RTOS starts
    HAL_Delay(3000);
    DFPlayer_Reset();
    HAL_Delay(3000);
    DFPlayer_SetVolume(40);
    HAL_Delay(500);

    HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1);  // start UART RX interrupt

    osKernelInitialize();
    AudioQueueHandle  = osMessageQueueNew(4, sizeof(uint16_t *), &AudioQueue_attributes);
    ResultQueueHandle = osMessageQueueNew(8, sizeof(uint8_t),    &ResultQueue_attributes);

    defaultTaskHandle  = osThreadNew(StartDefaultTask,   NULL, &defaultTask_attributes);
    AudioCaptureHandle = osThreadNew(Start_AudioCapture, NULL, &AudioCapture_attributes);
    FFTHandle          = osThreadNew(Start_FFT,          NULL, &FFT_attributes);
    AlertHandle        = osThreadNew(Start_Alert,        NULL, &Alert_attributes);
    PIRHandle          = osThreadNew(Start_PIR_Motion,   NULL, &PIR_attributes);
    TempHandle         = osThreadNew(Start_TempMonitor,  NULL, &Temp_attributes);

    osKernelStart();
    while (1) {}
}

void StartDefaultTask(void *argument) { for (;;) osDelay(1); }

void Start_AudioCapture(void *argument)
{
    HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, FFT_SIZE * 2);
    HAL_TIM_Base_Start(&htim1);
    for (;;) osDelay(5000);
}

void Start_FFT(void *argument)
{

    const int band_lim[6] = {
        1,
        HZ_TO_BIN(300),
        HZ_TO_BIN(600),
        HZ_TO_BIN(1500),
        HZ_TO_BIN(3500),
        (int)HALF_FFT
    };

    uint8_t  quiet_hist[QUIET_WINDOW];
    uint32_t quiet_idx = 0;
    for (uint32_t i = 0; i < QUIET_WINDOW; i++) quiet_hist[i] = 0;

    uint16_t *audio_block = NULL;
    uint8_t   result      = 0;
    uint32_t  cry_frames  = 0;
    uint32_t  frame_cnt   = 0;
    uint32_t  window_num  = 0;

    for (;;)
    {
        if (osMessageQueueGet(AudioQueueHandle, &audio_block, NULL, osWaitForever) != osOK)
            continue;

        // DC removal
        uint32_t dc_acc = 0;
        for (int i = 0; i < (int)FFT_SIZE; i++) dc_acc += audio_block[i];
        float dc = (float)dc_acc / (float)FFT_SIZE;

        // energy
        float energy = 0.0f;
        float prev_s = (float)audio_block[0] - dc;
        for (int i = 0; i < (int)FFT_SIZE; i++)
        {
            float s = (float)audio_block[i] - dc;
            energy += s * s;
            prev_s = s;
        }

        // quiet gap tracker
        quiet_hist[quiet_idx] = (energy < ENERGY_MIN) ? 1u : 0u;
        quiet_idx = (quiet_idx + 1) % QUIET_WINDOW;
        uint8_t had_quiet = 0;
        for (uint32_t i = 0; i < QUIET_WINDOW; i++)
            if (quiet_hist[i]) { had_quiet = 1; break; }

        // score
        int score      = 0;
        int peak_hz    = 0;
        int prom_x10   = 0;
        int centroid   = 0;
        int band_pct[5]= {0,0,0,0,0};
        int mid_pct    = 0;

        if (energy >= ENERGY_MIN)
        {
            score += 20;

            for (int i = 0; i < (int)FFT_SIZE; i++)
            {
                float w   = 0.5f * (1.0f - cosf(2.0f * PI * (float)i / (float)(FFT_SIZE - 1)));
                fft_re[i] = ((float)audio_block[i] - dc) * w;
                fft_im[i] = 0.0f;
            }
            simple_fft(fft_re, fft_im, (int)FFT_SIZE);

            float total_mag = 0.0f, wsum_mag = 0.0f;
            float peak_mag  = 0.0f, total_e2 = 0.0f;
            int   peak_bin  = 1;
            float band_e2[5] = {0.0f,0.0f,0.0f,0.0f,0.0f};

            for (int i = 1; i < (int)HALF_FFT; i++)
            {
                float mag = sqrtf(fft_re[i]*fft_re[i] + fft_im[i]*fft_im[i]);
                fft_mag[i] = mag;
                total_mag += mag;
                wsum_mag  += (float)i * mag;
                if (mag > peak_mag) { peak_mag = mag; peak_bin = i; }
                float mag2 = mag * mag;
                total_e2  += mag2;
                for (int b = 0; b < 5; b++)
                    if (i >= band_lim[b] && i < band_lim[b+1])
                        band_e2[b] += mag2;
            }

            peak_hz  = peak_bin * (int)SAMPLE_RATE / (int)FFT_SIZE;
            float mean_mag = (total_mag > 0.0f) ? total_mag / (float)(HALF_FFT - 1) : 1.0f;
            prom_x10 = (int)(peak_mag / mean_mag * 10.0f);
            centroid = (total_mag > 0.0f)
                       ? (int)(wsum_mag / total_mag * (float)SAMPLE_RATE / (float)FFT_SIZE)
                       : 0;
            if (total_e2 > 0.0f)
                for (int b = 0; b < 5; b++)
                    band_pct[b] = (int)(band_e2[b] / total_e2 * 100.0f);
            mid_pct = band_pct[1] + band_pct[2] + band_pct[3];

            if (band_pct[0] > B0_SILENCE_THRESH) { score = 0; goto emit; }  // too much low-freq, not a cry

            if (peak_hz  >= (int)PEAK_MIN_HZ && peak_hz  <= (int)PEAK_MAX_HZ)  score += 5;
            if (centroid >= (int)CENT_MIN_HZ  && centroid <= (int)CENT_MAX_HZ)  score += 15;
            if (band_pct[3] >= B3_MIN_PCT)                                       score += 30;
            if (mid_pct     >= MID_BAND_MIN)                                     score += 15;
            if (prom_x10    >= PROMINENCE_MIN)                                   score += 15;
        }
        emit:;

        uint8_t vote = (score >= SCORE_THRESH && had_quiet) ? 1u : 0u;
        if (vote) cry_frames++;
        frame_cnt++;

        // 30s window decision
        if (frame_cnt >= WINDOW_FRAMES)
        {
            uint8_t crying = (cry_frames >= VOTE_THRESH) ? 1u : 0u;
            result = crying;

            char wdbg[64];
            snprintf(wdbg, sizeof(wdbg),
                     "[CRY] window=%lu cry_frames=%lu/%u thresh=%u => %s\r\n",
                     (unsigned long)window_num,
                     (unsigned long)cry_frames,
                     (unsigned)WINDOW_FRAMES,
                     (unsigned)VOTE_THRESH,
                     crying ? "CRY" : "QUIET");
            uart_print(wdbg);

            osMessageQueuePut(ResultQueueHandle, &result, 0, 100);

            cry_frames = 0;
            frame_cnt  = 0;
            window_num++;
            for (uint32_t i = 0; i < QUIET_WINDOW; i++) quiet_hist[i] = 0;
            quiet_idx = 0;
        }
    }
}

void Start_Alert(void *argument)
{
    uint8_t result = 0;
    for (;;)
    {
        osMessageQueueGet(ResultQueueHandle, &result, NULL, osWaitForever);

        if (result == 1)
        {
            uart_print("[CRY] >>> Sending CRY alert to ESP32\r\n");
            HAL_UART_Transmit(&huart2, (uint8_t *)"CRY\n", 4, 100);
            DFPlayer_PlayFile(1);
            for (int i = 0; i < 6; i++) { HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin); osDelay(100); }
            HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
        }
        else
        {
            uart_print("[CRY] Window QUIET — no alert\r\n");
            DFPlayer_Stop();
            HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
        }
    }
}

// ADC DMA ping-pong - half and full complete each fire a callback
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    uint16_t *ptr = &adc_buf[0];
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(AudioQueueHandle, &ptr, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    uint16_t *ptr = &adc_buf[FFT_SIZE];
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(AudioQueueHandle, &ptr, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) Error_Handler();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);
    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_LSE | RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.LSEState            = RCC_LSE_ON;
    RCC_OscInitStruct.MSIState            = RCC_MSI_ON;
    RCC_OscInitStruct.MSICalibrationValue = 0;
    RCC_OscInitStruct.MSIClockRange       = RCC_MSIRANGE_6;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_MSI;
    RCC_OscInitStruct.PLL.PLLM            = 1;
    RCC_OscInitStruct.PLL.PLLN            = 16;
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV7;
    RCC_OscInitStruct.PLL.PLLQ            = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR            = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();
    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK) Error_Handler();
    HAL_RCCEx_EnableMSIPLLMode();
}

static void MX_ADC1_Init(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_ASYNC_DIV1;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait      = DISABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIG_T1_TRGO;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    hadc1.Init.Overrun               = ADC_OVR_DATA_PRESERVED;
    hadc1.Init.OversamplingMode      = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();
    sConfig.Channel      = ADC_CHANNEL_11;  // PA6 - mic (moved from PA3 to free USART2)
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
    sConfig.SingleDiff   = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset       = 0;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) Error_Handler();

    // injected channel for LM35 on PA0
    ADC_InjectionConfTypeDef sInjConfig = {0};
    sInjConfig.InjectedChannel               = ADC_CHANNEL_5;
    sInjConfig.InjectedRank                  = ADC_INJECTED_RANK_1;
    sInjConfig.InjectedSamplingTime          = ADC_SAMPLETIME_247CYCLES_5;
    sInjConfig.InjectedSingleDiff            = ADC_SINGLE_ENDED;
    sInjConfig.InjectedOffsetNumber          = ADC_OFFSET_NONE;
    sInjConfig.InjectedOffset                = 0;
    sInjConfig.InjectedNbrOfConversion       = 1;
    sInjConfig.InjectedDiscontinuousConvMode = DISABLE;
    sInjConfig.AutoInjectedConv              = DISABLE;
    sInjConfig.QueueInjectedContext          = DISABLE;
    sInjConfig.ExternalTrigInjecConv         = ADC_INJECTED_SOFTWARE_START;
    sInjConfig.ExternalTrigInjecConvEdge     = ADC_EXTERNALTRIGINJECCONV_EDGE_NONE;
    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &sInjConfig) != HAL_OK) Error_Handler();
}

static void MX_TIM1_Init(void)
{
    TIM_ClockConfigTypeDef  sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig      = {0};
    htim1.Instance               = TIM1;
    htim1.Init.Prescaler         = 0;
    htim1.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim1.Init.Period            = 3999;
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim1) != HAL_OK) Error_Handler();
    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK) Error_Handler();
    sMasterConfig.MasterOutputTrigger  = TIM_TRGO_RESET;
    sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
    sMasterConfig.MasterSlaveMode      = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK) Error_Handler();
}

static void MX_USART1_UART_Init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    // PA9=TX, PA10=RX
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin       = GPIO_PIN_9 | GPIO_PIN_10;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    huart1.Instance                    = USART1;
    huart1.Init.BaudRate               = 9600;
    huart1.Init.WordLength             = UART_WORDLENGTH_8B;
    huart1.Init.StopBits               = UART_STOPBITS_1;
    huart1.Init.Parity                 = UART_PARITY_NONE;
    huart1.Init.Mode                   = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart1.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

static void MX_USART2_UART_Init(void)
{
    huart2.Instance                    = USART2;
    huart2.Init.BaudRate               = 115200;
    huart2.Init.WordLength             = UART_WORDLENGTH_8B;
    huart2.Init.StopBits               = UART_STOPBITS_1;
    huart2.Init.Parity                 = UART_PARITY_NONE;
    huart2.Init.Mode                   = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart2.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

static void MX_DMA_Init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();
    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin   = LD3_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LD3_GPIO_Port, &GPIO_InitStruct);
    // PA0 = LM35 analog (ADC injected)
    GPIO_InitStruct.Pin  = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG_ADC_CONTROL;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // PA3 = USART2 RX, configured in HAL_UART_MspInit

    // PA4 = PIR sensor input
    GPIO_InitStruct.Pin  = GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2) HAL_IncTick();
}

// change to SENSOR_IR if using IR-05II (active low) instead of HC-SR501
/* #define SENSOR_IR */
#define SENSOR_PIR

#ifdef SENSOR_IR
    // IR-05II: LOW = detected
    #define SENSOR_DETECTED(pin)      ((pin) == GPIO_PIN_RESET)
    #define SENSOR_NOT_DETECTED(pin)  ((pin) == GPIO_PIN_SET)
    #define SENSOR_TRIGGER_EDGE(cur, last) \
        ((cur) == GPIO_PIN_RESET && (last) == GPIO_PIN_SET)
#else
    // HC-SR501: HIGH = detected
    #define SENSOR_DETECTED(pin)      ((pin) == GPIO_PIN_SET)
    #define SENSOR_NOT_DETECTED(pin)  ((pin) == GPIO_PIN_RESET)
    #define SENSOR_TRIGGER_EDGE(cur, last) \
        ((cur) == GPIO_PIN_SET && (last) == GPIO_PIN_RESET)
#endif

// PIR task - two modes: awake (NOMOV after 5min) and sleep (AWAKE after wakeup window)
void Start_PIR_Motion(void *argument)
{
    GPIO_PinState last_pir_state       = GPIO_PIN_RESET;
    uint32_t      last_motion_tick     = osKernelGetTickCount();
    uint8_t       no_motion_alert_sent = 0;

    /* Sleep-mode wakeup detection */
    uint8_t       sleep_sample_count = 0;
    uint8_t       sleep_motion_count = 0;
    uint32_t      sleep_sample_tick  = 0;
    uint8_t       sleep_edge_flag    = 0;

    baby_sleeping = 0;
    uart_print("[IR] Task started — waiting 60s for PIR to stabilise\r\n");
    osDelay(60000);
    uart_print("[IR] PIR ready — monitoring in AWAKE mode\r\n");

    for (;;)
    {
        /* Check if ISR has deposited a complete command */
        if (uart2_cmd_ready)
        {
            uart2_cmd_ready = 0;
            if (strcmp(uart2_cmd, "SLEEP_ON") == 0)
            {
                baby_sleeping = 1;
                uart_print("[IR] Command received: SLEEP_ON -> mode=SLEEP\r\n");
            }
            else if (strcmp(uart2_cmd, "SLEEP_OFF") == 0)
            {
                baby_sleeping = 0;
                uart_print("[IR] Command received: SLEEP_OFF -> mode=AWAKE\r\n");
            }
        }

        GPIO_PinState pir = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4);
        uint32_t      now = osKernelGetTickCount();

        if (baby_sleeping)
        {
            // count rising edges in the fast loop so we don't miss any
            if (SENSOR_DETECTED(pir) && !SENSOR_DETECTED(last_pir_state))
                sleep_edge_flag = 1;

            /* Every 1 second: count whether any edge occurred this second */
            if ((now - sleep_sample_tick) >= SLEEP_SAMPLE_MS)
            {
                sleep_sample_tick = now;
                if (sleep_edge_flag)
                    sleep_motion_count++;
                sleep_edge_flag = 0;
                sleep_sample_count++;

                if (sleep_sample_count >= SLEEP_SAMPLE_WINDOW)
                {
                    char sres[64];
                    snprintf(sres, sizeof(sres),
                             "[IR] Sleep window done: hits=%u thresh=%u\r\n",
                             (unsigned)sleep_motion_count,
                             (unsigned)SLEEP_MOTION_THRESH);
                    uart_print(sres);

                    if (sleep_motion_count > SLEEP_MOTION_THRESH)
                    {
                        uart_print("[IR] >>> Baby awake detected — sending AWAKE\r\n");
                        HAL_UART_Transmit(&huart2, (uint8_t *)"AWAKE\n", 6, 100);
                        baby_sleeping        = 0;
                        last_motion_tick     = now;
                        no_motion_alert_sent = 0;
                    }
                    else
                    {
                        uart_print("[IR] Sleep window: still asleep, resetting\r\n");
                    }
                    sleep_sample_count = 0;
                    sleep_motion_count = 0;
                }
            }
        }
        else
        {
            if (SENSOR_DETECTED(pir))
            {
                last_motion_tick     = now;
                no_motion_alert_sent = 0;
            }
            else if (!no_motion_alert_sent && (now - last_motion_tick) >= 10000U) /* TODO: change back to 300000U (5 min) for production */
            {
                uart_print("[IR] >>> No motion timeout — sending NOMOV\r\n");
                HAL_UART_Transmit(&huart2, (uint8_t *)"NOMOV\n", 6, 100);
                no_motion_alert_sent = 1;
                baby_sleeping        = 1;
                sleep_sample_count   = 0;
                sleep_motion_count   = 0;
                sleep_sample_tick    = now;
                sleep_edge_flag      = 0;
            }
        }

        last_pir_state = pir;
        osDelay(10);
    }
}
// temp task - reads LM35 every 5s, T(C) = raw * 3300 / (4096 * 10)
void Start_TempMonitor(void *argument)
{
    uint8_t high_alert_sent = 0;
    uint8_t low_alert_sent  = 0;

    for (;;)
    {
        osDelay(TEMP_SAMPLE_MS);

        if (HAL_ADCEx_InjectedStart(&hadc1) != HAL_OK) continue;
        if (HAL_ADCEx_InjectedPollForConversion(&hadc1, 10) != HAL_OK)
        {
            HAL_ADCEx_InjectedStop(&hadc1);
            continue;
        }
        uint32_t raw  = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_1);
        HAL_ADCEx_InjectedStop(&hadc1);

        float temp_c = (float)raw * 3300.0f / (4096.0f * 10.0f);

        {
            int t10 = (int)(temp_c * 10.0f);
            char tdbg[40];
            snprintf(tdbg, sizeof(tdbg), "[TEMP] %d.%dC\r\n", t10 / 10, t10 % 10);
            uart_print(tdbg);
        }

        if (temp_c > TEMP_HIGH_C)
        {
            if (!high_alert_sent)
            {
                uart_print("[TEMP] >>> Above threshold — sending TEMP_HIGH\r\n");
                HAL_UART_Transmit(&huart2, (uint8_t *)"TEMP_HIGH\n", 10, 100);
                high_alert_sent = 1;
            }
            low_alert_sent = 0;
        }
        else if (temp_c < TEMP_LOW_C)
        {
            if (!low_alert_sent)
            {
                uart_print("[TEMP] >>> Below threshold — sending TEMP_LOW\r\n");
                HAL_UART_Transmit(&huart2, (uint8_t *)"TEMP_LOW\n", 9, 100);
                low_alert_sent = 1;
            }
            high_alert_sent = 0;
        }
        else
        {
            uart_print("[TEMP] OK — within range, alerts re-armed\r\n");
            high_alert_sent = 0;
            low_alert_sent  = 0;
        }
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        if (uart2_rx_byte == '\n')
        {
            uart2_rx_buf[uart2_rx_idx] = '\0';
            memcpy(uart2_cmd, uart2_rx_buf, uart2_rx_idx + 1);
            uart2_cmd_ready = 1;
            uart2_rx_idx    = 0;
        }
        else if (uart2_rx_idx < (uint8_t)(sizeof(uart2_rx_buf) - 1))
        {
            uart2_rx_buf[uart2_rx_idx++] = (char)uart2_rx_byte;
        }
        /* Re-arm for next byte */
        HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1);
    }
}

void Error_Handler(void)
{
    uart_print("ERROR: halted\r\n");
    __disable_irq();
    while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
