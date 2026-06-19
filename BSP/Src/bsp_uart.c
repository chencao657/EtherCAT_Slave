/**
 * @file    bsp_uart.c
 * @brief   串口驱动 BSP 层实现
 */

#include "bsp_uart.h"


/* ============================== 私有包含 ============================ */

/* ============================ 外部变量 ============================== */

extern UART_HandleTypeDef huart2;   /**< CubeMX 生成的 USART2 句柄 */

/* ======================= 串口操作函数（虚函数实现）================== */

/**
 * @brief  阻塞式串口发送
 */
static void UART_Send(USARTDevice_t *this, const uint8_t *pData, uint16_t length)
{
    HAL_UART_Transmit(this->pInstance, pData, length, HAL_MAX_DELAY);
}

/**
 * @brief  从环形缓冲区读取数据
 * @return 实际读取的字节数
 */
static uint16_t UART_Receive(USARTDevice_t *this, uint8_t *pDestBuf, uint16_t maxLen)
{
    return RingBuffer_Read(&this->RxRing, pDestBuf, maxLen);
}

/* ============================ 操作函数表 ============================ */

static const USARTOps_t s_UsartOps = {
    .Send    = UART_Send,
    .Receive = UART_Receive,
};

/* ========================== 设备实例初始化 ========================== */

// USARTDevice_t g_ModbusUart = {
//     .pInstance    = &huart3,
//     .pOps         = &s_UsartOps,
// };

USARTDevice_t g_DebugUart = {
    .pInstance    = &huart2,
    .pOps         = &s_UsartOps,
};

/* ============================ 公共 API ============================== */

/**
 * @brief  初始化串口驱动，初始化环形缓冲区并启动单字节中断接收
 */
void BSP_UART_Init(void)
{
   
    RingBuffer_Init(&g_DebugUart.RxRing);

    HAL_NVIC_SetPriority(USART2_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    HAL_UART_Receive_IT(&huart2, &g_DebugUart.RxCmdByte, 1);
}

/* ======================== HAL 回调函数重写 ========================== */

/**
 * @brief  串口接收完成回调，将数据写入对应设备的环形缓冲区并重新开启接收
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    // if (huart->Instance == USART3) {
    //     RingBuffer_WriteByte(&g_ModbusUart.RxRing, g_ModbusUart.RxCmdByte);
        
    //     g_ModbusTimer.pOps->Reset(&g_ModbusTimer);
        
    //     HAL_UART_Receive_IT(&huart3, &g_ModbusUart.RxCmdByte, 1);
    // }
    if (huart->Instance == USART2) {
        RingBuffer_WriteByte(&g_DebugUart.RxRing, g_DebugUart.RxCmdByte);
       
        HAL_UART_Receive_IT(&huart2, &g_DebugUart.RxCmdByte, 1);
    }
}

/* ======================== printf 重定向 ============================= */

/**
 * @brief  重定向 printf 到调试串口（USART2）
 * @note   覆盖 syscalls.c 中的 __weak __io_putchar，使 printf 输出到达 USART2
 */
int __io_putchar(int ch)
{
    g_DebugUart.pOps->Send(&g_DebugUart, (uint8_t *)&ch, 1);
    return ch;
}

/**
 * @brief USART2 中断服务入口，转交 HAL 串口中断处理。
 */
void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}
