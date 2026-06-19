/**
 * @file    ring_buffer.h
 * @brief   通用环形缓冲区，纯 C 实现，不依赖任何硬件库
 * @note    可复用于中断接收、数据缓存等场景
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

/* ============================== 宏定义 ============================== */

#ifndef RING_BUF_SIZE
#define RING_BUF_SIZE   256     /**< 缓冲区大小（字节），可在编译时覆盖 */
#endif

/* ========================== 数据结构定义 ============================ */

/**
 * @brief 环形缓冲区结构体
 */
typedef struct {
    uint8_t           buffer[RING_BUF_SIZE];    /**< 数据存储区 */
    volatile uint16_t head;                     /**< 头指针（写入位置） */
    volatile uint16_t tail;                     /**< 尾指针（读取位置） */
} RingBuffer_t;

/* ============================ API 函数 ============================== */

void     RingBuffer_Init(RingBuffer_t *rb);                         /**< 初始化环形缓冲区 */
void     RingBuffer_WriteByte(RingBuffer_t *rb, uint8_t byte);      /**< 写入一个字节 */
uint16_t RingBuffer_Read(RingBuffer_t *rb, uint8_t *pDest, uint16_t maxLen); /**< 读取数据 */
uint8_t  RingBuffer_IsEmpty(const RingBuffer_t *rb);                /**< 判断是否为空 */

#endif /* RING_BUFFER_H */
