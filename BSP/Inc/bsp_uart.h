/**
 * @file    bsp_uart.h
 * @brief   串口驱动 BSP 层，基于环形缓冲区实现中断接收
 * @note    提供 USARTDevice 设备抽象，支持多串口实例和面向对象风格的操作接口
 */

#ifndef BSP_UART_H
#define BSP_UART_H

#include "stm32f4xx_hal.h"
#include "ring_buffer.h"
#include "stdio.h"

/* ============================ 前向声明 ============================== */

typedef struct USARTDevice USARTDevice_t;

/* ========================== 串口操作接口 ============================ */

/**
 * @brief 串口操作函数指针结构体（虚函数表）
 */
typedef struct {
    void     (*Send)(USARTDevice_t *this, const uint8_t *pData, uint16_t length);
    uint16_t (*Receive)(USARTDevice_t *this, uint8_t *pDestBuf, uint16_t maxLen);
} USARTOps_t;

/* ========================== 串口设备结构体 ========================== */

/**
 * @brief 串口设备结构体，封装硬件句柄、环形缓冲区和操作接口
 */
struct USARTDevice {
    UART_HandleTypeDef *pInstance;      /**< HAL 串口句柄指针 */
    RingBuffer_t        RxRing;         /**< 接收环形缓冲区 */
    volatile uint8_t    RxUpdateFlag;   /**< 数据更新标志 */
    uint8_t             RxCmdByte;      /**< 单字节接收缓存 */
    const USARTOps_t   *pOps;           /**< 操作函数表 */
};

/* ========================== 外部设备实例 ============================ */

// extern USARTDevice_t g_ModbusUart;  /**< Modbus 串口（USART3） */
extern USARTDevice_t g_DebugUart;   /**< 调试串口（USART2）   */

/* ============================ API 函数 ============================== */

void BSP_UART_Init(void);  /**< 初始化串口驱动，启动中断接收 */

#endif /* BSP_UART_H */
