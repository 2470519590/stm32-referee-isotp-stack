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
#include "can.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>

#include "app_can.h"
#include "app_log.h"
#include "protocol/app_ack.h"
#include "protocol/app_frame.h"
#include "protocol/tlv.h"
#include "service/dispatcher.h"
#include "service/retry_ack_scheduler.h"
#include "transport/isotp.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define APP_CAN1_TX_ID              0x501U
#define APP_CAN1_RX_ID              0x502U
#define APP_CAN2_TX_ID              0x502U
#define APP_CAN2_RX_ID              0x501U
#define APP_DEVICE_ID_CORE          0x01U
#define APP_DEVICE_ID_LAUNCHER      0x21U
#define APP_MODULE_UID              0x1234U
#define APP_CAN1_PERIOD_MS          1000U
#define APP_CAN2_PERIOD_MS          200U
#define APP_ISOTP_STMIN_MS          10U
#define APP_ISOTP_TIMEOUT_MS        100U
#define APP_RETRY_TIMEOUT_MS        100U
#define APP_RETRY_COUNT             2U
#define APP_LAUNCHER_PAYLOAD_LEN    100U
#define APP_ENABLE_FRAME_LOG        0U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static TransportIsotpContext g_can1_isotp;
static TransportIsotpContext g_can2_isotp;
static ServiceDispatcher g_dispatcher;
static ServiceRetryAckScheduler g_can1_scheduler;
static ServiceRetryAckScheduler g_can2_scheduler;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void App_Init(void);
static void App_Run(void);
static void App_OnCanRx(AppCanPort port, const AppCanFrame *frame);
static void App_OnIsotpMessage(void *user_arg, const uint8_t *payload, uint16_t length);
static void App_OnIsotpFrame(void *user_arg, AppCanPort port, const char *direction, const AppCanFrame *frame);
static void App_OnCan1TxComplete(void *user_arg, TransportIsotpStatus status);
static void App_OnCan2TxComplete(void *user_arg, TransportIsotpStatus status);
static void App_LogPayload(const char *prefix, const uint8_t *data, uint16_t length);
static void App_PrintCanHealth(void);
static bool App_DispatchDefault(void *user_arg, AppCanPort port, const ProtocolAppFrame *frame);
static bool App_DispatchAck(void *user_arg, AppCanPort port, const ProtocolAppFrame *frame);
static bool App_DispatchLauncherCmd(void *user_arg, AppCanPort port, const ProtocolAppFrame *frame);
static void App_OnSchedulerEvent(ServiceRetryAckScheduler *scheduler,
                                 ServiceRetryAckEvent event,
                                 ServiceRetryAckSlotId slot_id,
                                 const ProtocolAppFrame *frame,
                                 const ServiceRetryAckInfo *ack_info,
                                 void *user_arg);
static bool App_SendViaCan1(void *user_arg, const uint8_t *payload, uint16_t length);
static bool App_SendViaCan2(void *user_arg, const uint8_t *payload, uint16_t length);
static bool App_IsCan1Idle(void *user_arg);
static bool App_IsCan2Idle(void *user_arg);
static bool App_SendAppFrame(AppCanPort port, const ProtocolAppFrame *frame);
static bool App_SendAckFrame(AppCanPort port, const ProtocolAppFrame *request, uint8_t result_code, uint8_t reason_code, bool has_reason);
static void App_EnqueueLauncherMaxFrame(void);
static void App_EnqueueShortFrame(void);
static bool App_BuildLauncherCtrlMaxFrame(ProtocolAppFrame *frame);
static bool App_BuildShortStatusFrame(ProtocolAppFrame *frame);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void App_LogPayload(const char *prefix, const uint8_t *data, uint16_t length)
{
  char line[192];
  int offset = snprintf(line, sizeof(line), "%s len=%u data=", prefix, (unsigned int)length);

  for (uint16_t i = 0; (i < length) && (offset > 0) && (offset < (int)(sizeof(line) - 4)); ++i)
  {
    offset += snprintf(&line[offset], sizeof(line) - (size_t)offset, "%02X ", data[i]);
  }

  if (offset < 0)
  {
    return;
  }

  if (offset > (int)(sizeof(line) - 3))
  {
    offset = (int)(sizeof(line) - 3);
  }

  line[offset++] = '\r';
  line[offset++] = '\n';
  line[offset] = '\0';
  App_Log_Printf("%s", line);
}

static void App_OnIsotpFrame(void *user_arg, AppCanPort port, const char *direction, const AppCanFrame *frame)
{
#if APP_ENABLE_FRAME_LOG
  const char *name = (const char *)user_arg;

  App_Log_Printf("%s %s CAN%u ID=0x%03lX DLC=%u DATA=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                 name,
                 direction,
                 (unsigned int)(port + 1U),
                 (unsigned long)frame->can_id,
                 frame->dlc,
                 frame->data[0], frame->data[1], frame->data[2], frame->data[3],
                 frame->data[4], frame->data[5], frame->data[6], frame->data[7]);
#else
  (void)user_arg;
  (void)port;
  (void)direction;
  (void)frame;
#endif
}

static void App_OnIsotpMessage(void *user_arg, const uint8_t *payload, uint16_t length)
{
  const char *name = (const char *)user_arg;
  char prefix[48];

  snprintf(prefix, sizeof(prefix), "%s ISO-TP complete", name);
  App_LogPayload(prefix, payload, length);
  (void)Service_Dispatcher_DispatchRaw(&g_dispatcher,
                                       (strcmp(name, "CAN1") == 0) ? APP_CAN_PORT_1 : APP_CAN_PORT_2,
                                       payload,
                                       length);
}

static void App_OnCanRx(AppCanPort port, const AppCanFrame *frame)
{
  Transport_Isotp_OnCanFrame(&g_can1_isotp, port, frame);
  Transport_Isotp_OnCanFrame(&g_can2_isotp, port, frame);
}

static void App_OnCan1TxComplete(void *user_arg, TransportIsotpStatus status)
{
  (void)user_arg;
  Service_RetryAckScheduler_OnTransmitComplete(&g_can1_scheduler, status);
}

static void App_OnCan2TxComplete(void *user_arg, TransportIsotpStatus status)
{
  (void)user_arg;
  Service_RetryAckScheduler_OnTransmitComplete(&g_can2_scheduler, status);
}

static void App_PrintCanHealth(void)
{
  AppCanDiag can1 = {0};
  AppCanDiag can2 = {0};

  App_Can_GetDiag(APP_CAN_PORT_1, &can1);
  App_Can_GetDiag(APP_CAN_PORT_2, &can2);

  App_Log_Printf("CAN1 HEALTH TEC=%u REC=%u BOFF=%u EPVF=%u EWGF=%u LEC=%s HAL=0x%08lX ESR=0x%08lX TSR=0x%08lX RF0R=0x%08lX\r\n",
                 can1.tec, can1.rec, can1.bus_off, can1.error_passive, can1.error_warning,
                 App_Can_LastErrorToString(can1.last_error_code),
                 (unsigned long)can1.hal_error, (unsigned long)can1.esr,
                 (unsigned long)can1.tsr, (unsigned long)can1.rf0r);
  App_Log_Printf("CAN1 COUNT TX_OK=%lu TX_FAIL=%lu RX_IRQ=%lu RX_FRAME=%lu\r\n",
                 (unsigned long)can1.tx_ok_count,
                 (unsigned long)can1.tx_fail_count,
                 (unsigned long)can1.rx_irq_count,
                 (unsigned long)can1.rx_frame_count);

  App_Log_Printf("CAN2 HEALTH TEC=%u REC=%u BOFF=%u EPVF=%u EWGF=%u LEC=%s HAL=0x%08lX ESR=0x%08lX TSR=0x%08lX RF0R=0x%08lX\r\n",
                 can2.tec, can2.rec, can2.bus_off, can2.error_passive, can2.error_warning,
                 App_Can_LastErrorToString(can2.last_error_code),
                 (unsigned long)can2.hal_error, (unsigned long)can2.esr,
                 (unsigned long)can2.tsr, (unsigned long)can2.rf0r);
  App_Log_Printf("CAN2 COUNT TX_OK=%lu TX_FAIL=%lu RX_IRQ=%lu RX_FRAME=%lu\r\n",
                 (unsigned long)can2.tx_ok_count,
                 (unsigned long)can2.tx_fail_count,
                 (unsigned long)can2.rx_irq_count,
                 (unsigned long)can2.rx_frame_count);
}

static bool App_DispatchDefault(void *user_arg, AppCanPort port, const ProtocolAppFrame *frame)
{
  (void)user_arg;
  App_Log_Printf("DISPATCH CAN%u FUNC=0x%02X SEQ=0x%02X TYPE=0x%02X LEN=%u DST=0x%02X SRC=0x%02X UID=0x%04X\r\n",
                 (unsigned int)(port + 1U),
                 frame->func_code,
                 frame->seq,
                 frame->data_type,
                 frame->data_len,
                 frame->dst_id,
                 frame->src_id,
                 frame->module_uid);
  return true;
}

static bool App_DispatchAck(void *user_arg, AppCanPort port, const ProtocolAppFrame *frame)
{
  (void)user_arg;
  (void)port;
  Service_RetryAckScheduler_OnReceivedFrame(&g_can1_scheduler, frame);
  Service_RetryAckScheduler_OnReceivedFrame(&g_can2_scheduler, frame);
  return App_DispatchDefault(0, port, frame);
}

static bool App_DispatchLauncherCmd(void *user_arg, AppCanPort port, const ProtocolAppFrame *frame)
{
  (void)user_arg;
  App_Log_Printf("LAUNCHER CMD RX CAN%u SEQ=0x%02X LEN=%u\r\n",
                 (unsigned int)(port + 1U),
                 frame->seq,
                 frame->data_len);

  if (Protocol_AppFrame_RequiresAck(frame))
  {
    return App_SendAckFrame(port, frame, 0x00U, 0U, false);
  }

  return true;
}

static void App_OnSchedulerEvent(ServiceRetryAckScheduler *scheduler,
                                 ServiceRetryAckEvent event,
                                 ServiceRetryAckSlotId slot_id,
                                 const ProtocolAppFrame *frame,
                                 const ServiceRetryAckInfo *ack_info,
                                 void *user_arg)
{
  const char *name = (const char *)user_arg;
  const char *event_name = "UNKNOWN";

  (void)scheduler;

  switch (event)
  {
    case SERVICE_RETRY_ACK_EVENT_ENQUEUED:
      event_name = "ENQUEUED";
      break;
    case SERVICE_RETRY_ACK_EVENT_SENT:
      event_name = "SENT";
      break;
    case SERVICE_RETRY_ACK_EVENT_ACK_OK:
      event_name = "ACK_OK";
      break;
    case SERVICE_RETRY_ACK_EVENT_ACK_REJECTED:
      event_name = "ACK_REJECTED";
      break;
    case SERVICE_RETRY_ACK_EVENT_RETRYING:
      event_name = "RETRYING";
      break;
    case SERVICE_RETRY_ACK_EVENT_TIMEOUT_DROPPED:
      event_name = "TIMEOUT_DROPPED";
      break;
    case SERVICE_RETRY_ACK_EVENT_SEND_FAILED:
      event_name = "SEND_FAILED";
      break;
    case SERVICE_RETRY_ACK_EVENT_CANCELLED:
      event_name = "CANCELLED";
      break;
    default:
      break;
  }

  if (ack_info != 0)
  {
    App_Log_Printf("%s SCHED %s SLOT=%u FUNC=0x%02X SEQ=0x%02X RESULT=0x%02X REASON=%s0x%02X\r\n",
                   name,
                   event_name,
                   slot_id,
                   frame->func_code,
                   frame->seq,
                   ack_info->result_code,
                   ack_info->has_reason ? "" : "N/A:",
                   ack_info->reason_code);
  }
  else
  {
    App_Log_Printf("%s SCHED %s SLOT=%u FUNC=0x%02X SEQ=0x%02X PENDING=%u\r\n",
                   name,
                   event_name,
                   slot_id,
                   frame->func_code,
                   frame->seq,
                   Service_RetryAckScheduler_PendingCount((const ServiceRetryAckScheduler *)scheduler));
  }
}

static bool App_SendViaCan1(void *user_arg, const uint8_t *payload, uint16_t length)
{
  (void)user_arg;
  return (Transport_Isotp_Send(&g_can1_isotp, payload, length) == TRANSPORT_ISOTP_OK);
}

static bool App_SendViaCan2(void *user_arg, const uint8_t *payload, uint16_t length)
{
  (void)user_arg;
  return (Transport_Isotp_Send(&g_can2_isotp, payload, length) == TRANSPORT_ISOTP_OK);
}

static bool App_IsCan1Idle(void *user_arg)
{
  (void)user_arg;
  return Transport_Isotp_IsIdle(&g_can1_isotp);
}

static bool App_IsCan2Idle(void *user_arg)
{
  (void)user_arg;
  return Transport_Isotp_IsIdle(&g_can2_isotp);
}

static bool App_SendAppFrame(AppCanPort port, const ProtocolAppFrame *frame)
{
  uint8_t encoded[PROTOCOL_APP_FRAME_MAX_ENCODED_SIZE];
  uint16_t encoded_len = 0U;

  if ((frame == 0) || !Protocol_AppFrame_Encode(frame, encoded, &encoded_len, sizeof(encoded)))
  {
    return false;
  }

  if (port == APP_CAN_PORT_1)
  {
    return App_SendViaCan1(0, encoded, encoded_len);
  }

  return App_SendViaCan2(0, encoded, encoded_len);
}

static bool App_SendAckFrame(AppCanPort port, const ProtocolAppFrame *request, uint8_t result_code, uint8_t reason_code, bool has_reason)
{
  ProtocolAppFrame ack_frame;

  if ((request == 0) || !Protocol_AppAck_Build(request, result_code, has_reason, reason_code, &ack_frame))
  {
    return false;
  }

  App_Log_Printf("ACK TX CAN%u SEQ=0x%02X RESULT=0x%02X\r\n",
                 (unsigned int)(port + 1U),
                 ack_frame.seq,
                 result_code);
  return App_SendAppFrame(port, &ack_frame);
}

static bool App_BuildLauncherCtrlMaxFrame(ProtocolAppFrame *frame)
{
  uint8_t tlv_buffer[PROTOCOL_APP_FRAME_MAX_DATA_LENGTH];
  ProtocolTlvWriter writer;
  uint8_t payload[APP_LAUNCHER_PAYLOAD_LEN];

  for (uint16_t i = 0; i < sizeof(payload); ++i)
  {
    payload[i] = (uint8_t)(i & 0xFFU);
  }

  Protocol_AppFrame_Init(frame,
                         APP_DEVICE_ID_LAUNCHER,
                         APP_DEVICE_ID_CORE,
                         APP_MODULE_UID,
                         PROTOCOL_MSG_LAUNCHER_CTRL_CMD,
                         0U,
                         PROTOCOL_TYPE_TLV_STREAM);

  Protocol_TlvWriter_Init(&writer, tlv_buffer, sizeof(tlv_buffer));
  if (!Protocol_TlvWriter_AppendU16(&writer, PROTOCOL_TLV_CMD_INDEX, 0x0001U))
  {
    return false;
  }

  if (!Protocol_TlvWriter_AppendBytes(&writer, PROTOCOL_TLV_PAYLOAD, payload, (uint8_t)sizeof(payload)))
  {
    return false;
  }

  return Protocol_AppFrame_SetData(frame, tlv_buffer, (uint8_t)Protocol_TlvWriter_GetLength(&writer));
}

static bool App_BuildShortStatusFrame(ProtocolAppFrame *frame)
{
  uint8_t tlv_buffer[16];
  ProtocolTlvWriter writer;

  Protocol_AppFrame_Init(frame,
                         APP_DEVICE_ID_CORE,
                         APP_DEVICE_ID_LAUNCHER,
                         APP_MODULE_UID,
                         PROTOCOL_MSG_STATUS_REPORT,
                         0U,
                         PROTOCOL_TYPE_TLV_STREAM);

  Protocol_TlvWriter_Init(&writer, tlv_buffer, sizeof(tlv_buffer));
  if (!Protocol_TlvWriter_AppendBytes(&writer, PROTOCOL_TLV_STATE, (const uint8_t *)"ok", 2U))
  {
    return false;
  }

  return Protocol_AppFrame_SetData(frame, tlv_buffer, (uint8_t)Protocol_TlvWriter_GetLength(&writer));
}

static void App_EnqueueLauncherMaxFrame(void)
{
  ProtocolAppFrame frame;

  if (App_BuildLauncherCtrlMaxFrame(&frame))
  {
    (void)Service_RetryAckScheduler_Enqueue(&g_can1_scheduler,
                                            &frame,
                                            PROTOCOL_PRIORITY_HIGHEST,
                                            true,
                                            APP_RETRY_COUNT,
                                            APP_RETRY_TIMEOUT_MS,
                                            0);
  }
}

static void App_EnqueueShortFrame(void)
{
  ProtocolAppFrame frame;

  if (App_BuildShortStatusFrame(&frame))
  {
    (void)Service_RetryAckScheduler_Enqueue(&g_can2_scheduler,
                                            &frame,
                                            PROTOCOL_PRIORITY_MEDIUM,
                                            false,
                                            0U,
                                            APP_RETRY_TIMEOUT_MS,
                                            0);
  }
}

static void App_Init(void)
{
  const TransportIsotpConfig can1_cfg = {
      .port = APP_CAN_PORT_1,
      .tx_id = APP_CAN1_TX_ID,
      .rx_id = APP_CAN1_RX_ID,
      .block_size = 0U,
      .st_min_ms = APP_ISOTP_STMIN_MS,
      .tx_timeout_ms = APP_ISOTP_TIMEOUT_MS,
      .tx_require_flow_control = true,
      .get_ms = HAL_GetTick,
      .on_message = App_OnIsotpMessage,
      .on_frame = App_OnIsotpFrame,
      .on_tx_complete = App_OnCan1TxComplete,
      .user_arg = "CAN1",
  };

  const TransportIsotpConfig can2_cfg = {
      .port = APP_CAN_PORT_2,
      .tx_id = APP_CAN2_TX_ID,
      .rx_id = APP_CAN2_RX_ID,
      .block_size = 0U,
      .st_min_ms = APP_ISOTP_STMIN_MS,
      .tx_timeout_ms = APP_ISOTP_TIMEOUT_MS,
      .tx_require_flow_control = true,
      .get_ms = HAL_GetTick,
      .on_message = App_OnIsotpMessage,
      .on_frame = App_OnIsotpFrame,
      .on_tx_complete = App_OnCan2TxComplete,
      .user_arg = "CAN2",
  };

  App_Log_Init();
  App_Can_SetRxCallback(App_OnCanRx);

  if (App_Can_Init() != APP_CAN_STATUS_OK)
  {
    Error_Handler();
  }

  Transport_Isotp_Init(&g_can1_isotp, &can1_cfg);
  Transport_Isotp_Init(&g_can2_isotp, &can2_cfg);
  Service_Dispatcher_Init(&g_dispatcher);
  Service_Dispatcher_Register(&g_dispatcher, PROTOCOL_MSG_ACK, App_DispatchAck, 0);
  Service_Dispatcher_Register(&g_dispatcher, PROTOCOL_MSG_LAUNCHER_CTRL_CMD, App_DispatchLauncherCmd, 0);
  Service_Dispatcher_SetDefault(&g_dispatcher, App_DispatchDefault, 0);

  Service_RetryAckScheduler_Init(&g_can1_scheduler, App_SendViaCan1, App_IsCan1Idle, HAL_GetTick, 0);
  Service_RetryAckScheduler_Init(&g_can2_scheduler, App_SendViaCan2, App_IsCan2Idle, HAL_GetTick, 0);
  Service_RetryAckScheduler_SetEventCallback(&g_can1_scheduler, App_OnSchedulerEvent, "CAN1");
  Service_RetryAckScheduler_SetEventCallback(&g_can2_scheduler, App_OnSchedulerEvent, "CAN2");

  App_Log_Printf("Dual CAN ISO-TP loop test start, CAN1=500K, CAN2=500K, max_app_frame=%u, max_isotp=%u\r\n",
                 PROTOCOL_APP_FRAME_MAX_ENCODED_SIZE,
                 TRANSPORT_ISOTP_MAX_MESSAGE_SIZE);
}

static void App_Run(void)
{
  static uint32_t last_can1_tick = 0U;
  static uint32_t last_can2_tick = 0U;
  static uint32_t last_diag_tick = 0U;
  uint32_t now = HAL_GetTick();

  Transport_Isotp_Poll(&g_can1_isotp);
  Transport_Isotp_Poll(&g_can2_isotp);
  Service_RetryAckScheduler_Poll(&g_can1_scheduler);
  Service_RetryAckScheduler_Poll(&g_can2_scheduler);

  if ((now - last_can1_tick) >= APP_CAN1_PERIOD_MS)
  {
    last_can1_tick += APP_CAN1_PERIOD_MS;
    App_EnqueueLauncherMaxFrame();
  }

  if ((now - last_can2_tick) >= APP_CAN2_PERIOD_MS)
  {
    last_can2_tick += APP_CAN2_PERIOD_MS;
    App_EnqueueShortFrame();
  }

  if ((now - last_diag_tick) >= 1000U)
  {
    last_diag_tick += 1000U;
    App_PrintCanHealth();
  }
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
  MX_CAN1_Init();
  MX_CAN2_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  App_Init();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    App_Run();
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
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
  while (1)
  {
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
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
