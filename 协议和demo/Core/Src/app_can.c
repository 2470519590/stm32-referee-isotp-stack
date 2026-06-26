#include "app_can.h"

#include <string.h>

#include "can.h"
#include "app_log.h"

/* 说明：
 * 1. 这是当前工程中最主要的 HAL 相关文件之一。
 * 2. 若移植到 F1/F4/L431 或其他 MCU，优先重写本文件，而不是去改 transport/protocol/service。
 * 3. 上层协议栈只应通过 app_can.h 访问这里的能力。
 */

static AppCanRxCallback s_rx_callback = 0;
static uint32_t s_tx_ok_count[APP_CAN_PORT_COUNT] = {0};
static uint32_t s_tx_fail_count[APP_CAN_PORT_COUNT] = {0};
static uint32_t s_rx_irq_count[APP_CAN_PORT_COUNT] = {0};
static uint32_t s_rx_frame_count[APP_CAN_PORT_COUNT] = {0};

static uint32_t App_Can_GetTxCompleteFlag(uint32_t mailbox)
{
    if (mailbox == CAN_TX_MAILBOX0)
    {
        return CAN_TSR_RQCP0;
    }
    if (mailbox == CAN_TX_MAILBOX1)
    {
        return CAN_TSR_RQCP1;
    }

    return CAN_TSR_RQCP2;
}

static uint32_t App_Can_GetTxSuccessFlag(uint32_t mailbox)
{
    if (mailbox == CAN_TX_MAILBOX0)
    {
        return CAN_TSR_TXOK0;
    }
    if (mailbox == CAN_TX_MAILBOX1)
    {
        return CAN_TSR_TXOK1;
    }

    return CAN_TSR_TXOK2;
}

static uint32_t App_Can_GetTxArbitrationLostFlag(uint32_t mailbox)
{
    if (mailbox == CAN_TX_MAILBOX0)
    {
        return CAN_TSR_ALST0;
    }
    if (mailbox == CAN_TX_MAILBOX1)
    {
        return CAN_TSR_ALST1;
    }

    return CAN_TSR_ALST2;
}

static uint32_t App_Can_GetTxErrorFlag(uint32_t mailbox)
{
    if (mailbox == CAN_TX_MAILBOX0)
    {
        return CAN_TSR_TERR0;
    }
    if (mailbox == CAN_TX_MAILBOX1)
    {
        return CAN_TSR_TERR1;
    }

    return CAN_TSR_TERR2;
}

static CAN_HandleTypeDef *App_Can_GetHandle(AppCanPort port)
{
    /* 平台相关映射点：
     * - 单 CAN / 双 CAN 芯片
     * - 不同芯片的实例命名差异
     */
    if (port == APP_CAN_PORT_1)
    {
        return &hcan1;
    }

    return &hcan2;
}

static void App_Can_ConfigureFilter(CAN_HandleTypeDef *hcan, uint32_t bank, uint32_t fifo)
{
    CAN_FilterTypeDef filter = {0};

    /* 当前 demo 使用“全接收”滤波器，便于测试和日志分析。
     * 正式项目若 ID 规划明确，建议在这里收紧过滤规则。
     */
    filter.FilterActivation = ENABLE;
    filter.FilterBank = bank;
    filter.FilterFIFOAssignment = fifo;
    filter.FilterIdHigh = 0;
    filter.FilterIdLow = 0;
    filter.FilterMaskIdHigh = 0;
    filter.FilterMaskIdLow = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.SlaveStartFilterBank = 14;

    if (HAL_CAN_ConfigFilter(hcan, &filter) != HAL_OK)
    {
        Error_Handler();
    }
}

AppCanStatus App_Can_Init(void)
{
    /* 移植提醒：
     * 1. F1/F4/L4 在滤波器银行、时钟树、双 CAN 结构上可能有差异。
     * 2. 需要保证底层波特率已经实际配置为 500 kbps。
     * 3. 若平台没有 HAL 风格通知接口，需要自己桥接接收和错误回调。
     */
    memset(s_tx_ok_count, 0, sizeof(s_tx_ok_count));
    memset(s_tx_fail_count, 0, sizeof(s_tx_fail_count));
    memset(s_rx_irq_count, 0, sizeof(s_rx_irq_count));
    memset(s_rx_frame_count, 0, sizeof(s_rx_frame_count));

    App_Can_ConfigureFilter(&hcan1, 0U, CAN_FILTER_FIFO0);
    App_Can_ConfigureFilter(&hcan2, 14U, CAN_FILTER_FIFO0);

    if (HAL_CAN_Start(&hcan1) != HAL_OK)
    {
        return APP_CAN_STATUS_ERROR;
    }

    if (HAL_CAN_Start(&hcan2) != HAL_OK)
    {
        return APP_CAN_STATUS_ERROR;
    }

    if (HAL_CAN_ActivateNotification(&hcan1,
                                     CAN_IT_RX_FIFO0_MSG_PENDING |
                                     CAN_IT_ERROR_WARNING |
                                     CAN_IT_ERROR_PASSIVE |
                                     CAN_IT_BUSOFF |
                                     CAN_IT_LAST_ERROR_CODE) != HAL_OK)
    {
        return APP_CAN_STATUS_ERROR;
    }

    if (HAL_CAN_ActivateNotification(&hcan2,
                                     CAN_IT_RX_FIFO0_MSG_PENDING |
                                     CAN_IT_ERROR_WARNING |
                                     CAN_IT_ERROR_PASSIVE |
                                     CAN_IT_BUSOFF |
                                     CAN_IT_LAST_ERROR_CODE) != HAL_OK)
    {
        return APP_CAN_STATUS_ERROR;
    }

    return APP_CAN_STATUS_OK;
}

AppCanStatus App_Can_Send(AppCanPort port, const AppCanFrame *frame, uint32_t timeout_ms)
{
    CAN_TxHeaderTypeDef header = {0};
    CAN_HandleTypeDef *hcan = App_Can_GetHandle(port);
    uint32_t mailbox = 0;
    uint32_t start_tick = HAL_GetTick();
    uint32_t complete_flag;
    uint32_t success_flag;
    uint32_t arbitration_lost_flag;
    uint32_t tx_error_flag;

    if ((frame == 0) || (frame->dlc > 8U))
    {
        return APP_CAN_STATUS_ERROR;
    }

    /* 先等邮箱空闲，避免无限阻塞。 */
    while (HAL_CAN_GetTxMailboxesFreeLevel(hcan) == 0U)
    {
        if ((HAL_GetTick() - start_tick) >= timeout_ms)
        {
            return APP_CAN_STATUS_TIMEOUT;
        }
    }

    header.StdId = (uint16_t)frame->can_id;
    header.ExtId = frame->can_id;
    header.IDE = frame->is_extended_id ? CAN_ID_EXT : CAN_ID_STD;
    header.RTR = CAN_RTR_DATA;
    header.DLC = frame->dlc;
    header.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(hcan, &header, (uint8_t *)frame->data, &mailbox) != HAL_OK)
    {
        return APP_CAN_STATUS_ERROR;
    }

    complete_flag = App_Can_GetTxCompleteFlag(mailbox);
    success_flag = App_Can_GetTxSuccessFlag(mailbox);
    arbitration_lost_flag = App_Can_GetTxArbitrationLostFlag(mailbox);
    tx_error_flag = App_Can_GetTxErrorFlag(mailbox);

    /* 再等本次发送真正完成。
     * transport/isotp 与 retry_ack_scheduler 都依赖“发送完成”这个语义。
     */
    while ((hcan->Instance->TSR & complete_flag) == 0U)
    {
        if ((HAL_GetTick() - start_tick) >= timeout_ms)
        {
            s_tx_fail_count[port]++;
            return APP_CAN_STATUS_TIMEOUT;
        }
    }

    if ((hcan->Instance->TSR & success_flag) == 0U)
    {
        uint32_t tsr_snapshot = hcan->Instance->TSR;
        s_tx_fail_count[port]++;
        App_Log_Printf("CAN%u TX FAIL TSR=0x%08lX ALST=%u TERR=%u HAL=0x%08lX\r\n",
                       (unsigned int)(port + 1U),
                       (unsigned long)tsr_snapshot,
                       ((tsr_snapshot & arbitration_lost_flag) != 0U) ? 1U : 0U,
                       ((tsr_snapshot & tx_error_flag) != 0U) ? 1U : 0U,
                       (unsigned long)HAL_CAN_GetError(hcan));
        __HAL_CAN_CLEAR_FLAG(hcan, complete_flag);
        return APP_CAN_STATUS_ERROR;
    }

    s_tx_ok_count[port]++;
    __HAL_CAN_CLEAR_FLAG(hcan, complete_flag);
    return APP_CAN_STATUS_OK;
}

AppCanStatus App_Can_SendStd(AppCanPort port, uint16_t std_id, const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    AppCanFrame frame = {0};

    if ((data == 0) || (len > 8U))
    {
        return APP_CAN_STATUS_ERROR;
    }

    frame.can_id = std_id;
    frame.is_extended_id = 0U;
    frame.dlc = len;

    for (uint8_t i = 0; i < len; ++i)
    {
        frame.data[i] = data[i];
    }

    return App_Can_Send(port, &frame, timeout_ms);
}

void App_Can_SetRxCallback(AppCanRxCallback callback)
{
    s_rx_callback = callback;
}

void App_Can_GetDiag(AppCanPort port, AppCanDiag *diag)
{
    CAN_HandleTypeDef *hcan = App_Can_GetHandle(port);
    uint32_t esr;

    if (diag == 0)
    {
        return;
    }

    esr = hcan->Instance->ESR;

    diag->tec = (uint8_t)((esr >> 16) & 0xFFU);
    diag->rec = (uint8_t)((esr >> 24) & 0xFFU);
    diag->last_error_code = (uint8_t)(esr & CAN_ESR_LEC);
    diag->error_warning = ((esr & CAN_ESR_EWGF) != 0U) ? 1U : 0U;
    diag->error_passive = ((esr & CAN_ESR_EPVF) != 0U) ? 1U : 0U;
    diag->bus_off = ((esr & CAN_ESR_BOFF) != 0U) ? 1U : 0U;
    diag->hal_error = HAL_CAN_GetError(hcan);
    diag->esr = esr;
    diag->tsr = hcan->Instance->TSR;
    diag->rf0r = hcan->Instance->RF0R;
    diag->tx_ok_count = s_tx_ok_count[port];
    diag->tx_fail_count = s_tx_fail_count[port];
    diag->rx_irq_count = s_rx_irq_count[port];
    diag->rx_frame_count = s_rx_frame_count[port];
}

const char *App_Can_LastErrorToString(uint8_t lec)
{
    switch (lec & 0x07U)
    {
    case 0x00U:
        return "NoError";
    case 0x01U:
        return "StuffError";
    case 0x02U:
        return "FormError";
    case 0x03U:
        return "AckError";
    case 0x04U:
        return "BitRecessiveError";
    case 0x05U:
        return "BitDominantError";
    case 0x06U:
        return "CrcError";
    case 0x07U:
    default:
        return "SetBySoftware";
    }
}

static void App_Can_DispatchRx(CAN_HandleTypeDef *hcan, AppCanPort port)
{
    CAN_RxHeaderTypeDef header = {0};
    AppCanFrame frame = {0};

    s_rx_irq_count[port]++;

    /* 一次性清空 FIFO，减轻高负载时中断抖动。 */
    while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0U)
    {
        if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, frame.data) != HAL_OK)
        {
            return;
        }

        s_rx_frame_count[port]++;

        frame.can_id = (header.IDE == CAN_ID_STD) ? header.StdId : header.ExtId;
        frame.is_extended_id = (header.IDE == CAN_ID_EXT) ? 1U : 0U;
        frame.dlc = (uint8_t)header.DLC;

        if (s_rx_callback != 0)
        {
            /* 向上只交付统一的 AppCanFrame，不暴露 HAL 的头结构。 */
            s_rx_callback(port, &frame);
        }
    }
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    /* 这是 HAL 中断入口到协议栈通用抽象层的桥接点。 */
    if (hcan->Instance == CAN1)
    {
        App_Can_DispatchRx(hcan, APP_CAN_PORT_1);
    }
    else if (hcan->Instance == CAN2)
    {
        App_Can_DispatchRx(hcan, APP_CAN_PORT_2);
    }
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
    AppCanDiag diag = {0};
    AppCanPort port = (hcan->Instance == CAN1) ? APP_CAN_PORT_1 : APP_CAN_PORT_2;

    /* 若后续日志量较大影响实时性，可改为只记录标志，在主循环/任务中异步打印。 */
    App_Can_GetDiag(port, &diag);
    App_Log_Printf("CAN%u ERROR HAL=0x%08lX TEC=%u REC=%u BOFF=%u EPVF=%u EWGF=%u LEC=%s ESR=0x%08lX\r\n",
                   (unsigned int)(port + 1U),
                   (unsigned long)diag.hal_error,
                   diag.tec,
                   diag.rec,
                   diag.bus_off,
                   diag.error_passive,
                   diag.error_warning,
                   App_Can_LastErrorToString(diag.last_error_code),
                   (unsigned long)diag.esr);
}
