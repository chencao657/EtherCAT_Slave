/**
 ******************************************************************************
 * @file    ring_buffer.c
 * @brief   通用环形缓冲区（Ring Buffer / Circular Buffer）实现
 *
 *          纯 C 实现，不依赖任何硬件或 OS。数据结构定义在 BSP/Inc/ring_buffer.h，
 *          本文件提供四个核心 API 的实现。
 *
 *          === 设计说明 ===
 *
 *          1. 单生产者 / 单消费者模型
 *             - 生产者（如 ISR）调用 WriteByte 写入数据
 *             - 消费者（如主循环）调用 Read 读出数据
 *             - head / tail 指针均为 volatile，编译器不会对其做激进优化，
 *               适合在中断与主循环之间共享。
 *
 *          2. 满 / 空判定策略（浪费一个槽位）
 *             - 空：head == tail
 *             - 满：(head + 1) % RING_BUF_SIZE == tail
 *             当 nextHead 追上 tail 时认为缓冲区已满，丢弃新数据。
 *             这种设计避免了使用额外计数器，以 1 字节的容量代价换取
 *             更简单的并发安全性。
 *
 *          3. 写满保护
 *             缓冲区满时丢弃新数据，保留旧数据。这是嵌入式通信（如 UART 接收）
 *             的常见策略：宁可丢新帧也不破坏已接收的完整帧。
 *
 *          4. 读取行为
 *             Read 一次最多读出 maxLen 字节。若缓冲区中数据不足 maxLen，
 *             则读出所有已有数据并返回实际字节数，不会阻塞等待。
 *
 *          === 典型用法 ===
 *
 *          // UART 接收中断（生产者）：
 *          void USART_IRQHandler(void) {
 *              if (USART_GetFlagStatus(USART1, USART_FLAG_RXNE)) {
 *                  uint8_t ch = USART_ReceiveData(USART1);
 *                  RingBuffer_WriteByte(&g_uartRxBuf, ch);
 *              }
 *          }
 *
 *          // 主循环（消费者）：
 *          uint8_t buf[64];
 *          uint16_t n = RingBuffer_Read(&g_uartRxBuf, buf, sizeof(buf));
 *          // 处理 buf[0..n-1] ...
 ******************************************************************************
 */

/*========================== 头文件包含 ====================================*/
#include "ring_buffer.h"

/*========================== API 实现 ======================================*/

/**
 * @brief   初始化环形缓冲区
 * @param   rb  :  指向 RingBuffer_t 实例的指针
 * @retval  无
 * @note    将读写指针复位到 0。不需要显式清空 buffer[] 数组——旧数据在
 *          写入指针覆盖前不会被读取，因此内容无效。
 *
 *          应在首次使用前调用一次（如系统初始化阶段）。重复调用会丢弃
 *          缓冲区中所有未读取的数据。
 */
void RingBuffer_Init(RingBuffer_t *rb)
{
    __disable_irq();
    rb->head = 0;
    rb->tail = 0;
    __enable_irq();
}

/**
 * @brief   向环形缓冲区写入一个字节
 * @param   rb   :  指向 RingBuffer_t 实例的指针
 * @param   byte :  待写入的字节
 * @retval  无（静默丢弃）
 * @note    写入前检查缓冲区是否已满。若满，则静默丢弃该字节，不覆盖旧数据。
 *
 *          该函数通常在中断上下文（如 UART RX ISR）中调用，执行路径短且
 *          无循环，适合 ISR 对时间确定性的要求。
 *
 *          算法：
 *            1. 计算 nextHead = (head + 1) % SIZE
 *            2. 若 nextHead == tail，缓冲区满 → 丢弃
 *            3. 否则：buffer[head] = byte; head = nextHead
 */
void RingBuffer_WriteByte(RingBuffer_t *rb, uint8_t byte)
{
    uint16_t nextHead = (rb->head + 1) % RING_BUF_SIZE;

    if (nextHead != rb->tail)
    {
        /* 缓冲区未满：写入当前 head 位置，前移 head */
        rb->buffer[rb->head] = byte;
        rb->head = nextHead;
    }
    /* else: 缓冲区满，静默丢弃（保护已有数据） */
}

/**
 * @brief   从环形缓冲区读取数据
 * @param   rb     :  指向 RingBuffer_t 实例的指针
 * @param   pDest  :  目标缓冲区指针（调用者提供）
 * @param   maxLen :  目标缓冲区最大容量（字节）
 * @retval  实际读取的字节数 [0, maxLen]
 * @note    从 tail 开始逐个读出，每读一个字节 tail 前移一位。
 *          读到 head == tail（缓冲区空）或达到 maxLen 时停止。
 *
 *          典型场景：
 *            - 主循环中批量读取 UART 接收数据后逐帧解析
 *            - 配合 IsEmpty() 先判断有无数据，再决定是否读取
 *
 *          @warning pDest 必须指向至少 maxLen 字节的有效内存空间。
 */
uint16_t RingBuffer_Read(RingBuffer_t *rb, uint8_t *pDest, uint16_t maxLen)
{
    uint16_t len = 0;

    while (rb->head != rb->tail && len < maxLen)
    {
        pDest[len] = rb->buffer[rb->tail];
        len++;
        rb->tail = (rb->tail + 1) % RING_BUF_SIZE;
    }

    return len;
}

/**
 * @brief   判断环形缓冲区是否为空
 * @param   rb  :  指向 RingBuffer_t 实例的指针（const，只读不修改）
 * @retval  1 — 缓冲区为空（无数据可读）
 * @retval  0 — 缓冲区非空（有数据待读取）
 * @note    时间复杂度 O(1)，仅比较 head 与 tail 两个字段。
 *          常用于轮询场景：先 IsEmpty()，若非空则 Read()。
 */
uint8_t RingBuffer_IsEmpty(const RingBuffer_t *rb)
{
    return (rb->head == rb->tail) ? 1 : 0;
}
