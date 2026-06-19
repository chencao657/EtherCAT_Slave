/**
 * @file    bsp_ssc_hw.c
 * @brief   EtherCAT SSC 硬件适配层。
 *
 * 本文件把 Beckhoff SSC 协议栈需要的 HW_xxx 接口，适配到当前板子的
 * STM32F407 + LAN9253 硬件平台。协议栈只感知 ESC 地址空间读写、
 * AL Event 中断开关和 1ms 定时器，具体 SPI/CSR/PRAM 访问由 BSP_LAN9253
 * 模块完成。
 */

#include "bsp_ssc_hw.h"

#include "bsp_lan9253.h"
#include "esc.h"
#include "main.h"
#include "stm32f4xx_hal.h"

#include <stdint.h>

/* SSC 计时基准，单位为 HAL_GetTick() 的毫秒计数。 */
static UINT32 s_timerStartMs;
static UINT32 s_timerMs;
static volatile uint32_t s_escIntDisableDepth;

#define LAN9253_ESC_CHIP_ID_ADDR 0x0E02U
#define LAN9253_ESC_CHIP_ID_VALUE 0x9253U
#define LAN9253_REG_IRQ_CFG 0x0054U
#define LAN9253_REG_INT_EN  0x005CU
#define LAN9253_IRQ_CFG_VALUE 0x00000101UL
#define LAN9253_INT_EN_ECAT_EVENT 0x00000001UL
#define LAN9253_AL_EVENT_PROCESS_OUTPUT 0x0400U
#define LAN9253_AL_EVENT_MASK_CHECK_VALUE \
    (LAN9253_AL_EVENT_PROCESS_OUTPUT | 0x0093U)

/* 初始化自检辅助函数声明。 */
static UINT8 HW_CheckEscChipIdAccess(void);
static UINT8 HW_CheckAlEventMaskAccess(void);
static void HW_ConfigureEscHostInterrupt(void);
static void HW_ClearEscInterruptPending(void);

/**
 * @brief 初始化 SSC 硬件适配层。
 * @return 0 表示初始化成功，非 0 表示硬件访问自检失败。
 */
UINT16 HW_Init(void)
{
    UINT16 initError = 1U;

    HW_DisableEscInterrupt();

    BSP_LAN9253_Init();

    do {
        if (BSP_LAN9253_SelfTest() == 0U) {
            break;
        }

        if (HW_CheckEscChipIdAccess() == 0U) {
            break;
        }

        if (HW_CheckAlEventMaskAccess() == 0U) {
            break;
        }

        HW_ConfigureEscHostInterrupt();

        s_timerStartMs = (UINT32)HAL_GetTick();
        initError = 0U;
    } while (0);

    HW_ClearEscInterruptPending();
    HW_EnableEscInterrupt();

    return initError;
}


/**
 * @brief 释放 SSC 硬件适配层资源。
 */
void HW_Release(void)
{
}

/**
 * @brief 检查能否通过 ESC 地址空间读取 LAN9253 芯片 ID。
 */
static UINT8 HW_CheckEscChipIdAccess(void)
{
    UINT16 chipId = 0U;

    HW_EscRead((MEM_ADDR *)&chipId, LAN9253_ESC_CHIP_ID_ADDR, sizeof(chipId));

    return (chipId == LAN9253_ESC_CHIP_ID_VALUE) ? 1U : 0U;
}

/**
 * @brief 检查 AL Event Mask 寄存器是否可正常读写。
 */
static UINT8 HW_CheckAlEventMaskAccess(void)
{
    UINT16 checkMask = LAN9253_AL_EVENT_MASK_CHECK_VALUE;
    UINT16 readBack = 0U;
    UINT16 clearMask = 0U;

    HW_EscWrite((MEM_ADDR *)&checkMask, ESC_AL_EVENTMASK_OFFSET, sizeof(checkMask));
    HW_EscRead((MEM_ADDR *)&readBack, ESC_AL_EVENTMASK_OFFSET, sizeof(readBack));
    HW_EscWrite((MEM_ADDR *)&clearMask, ESC_AL_EVENTMASK_OFFSET, sizeof(clearMask));

    return (readBack == checkMask) ? 1U : 0U;
}

/**
 * @brief 配置 LAN9253 主机侧 ECAT Event 中断输出。
 */
static void HW_ConfigureEscHostInterrupt(void)
{
    /* 先关闭主机中断输出，配置 IRQ_CFG 后再只打开 ECAT Event 中断源。 */
    BSP_LAN9253_WriteDWord(LAN9253_REG_INT_EN, 0UL);
    BSP_LAN9253_WriteDWord(LAN9253_REG_IRQ_CFG, LAN9253_IRQ_CFG_VALUE);
    BSP_LAN9253_WriteDWord(LAN9253_REG_INT_EN, LAN9253_INT_EN_ECAT_EVENT);
}

// 1ms 定时器中断服务函数
void HW_TimerTick1ms(void)
{
    s_timerMs++;
}


/**
 * @brief 获取从上次清零开始累计的毫秒数。
 */
UINT32 HW_GetTimer(void)
{
    // return (UINT32)HAL_GetTick() - s_timerStartMs;
    return s_timerMs - s_timerStartMs;
}

/**
 * @brief 清零 SSC 软件计时器。
 */
void HW_ClearTimer(void)
{
    // s_timerStartMs = (UINT32)HAL_GetTick();
    s_timerStartMs = s_timerMs;
}

/**
 * @brief 普通上下文读取 AL Event 寄存器。
 */
UINT16 HW_GetALEventRegister(void)
{
    UINT16 alEvent;

    HW_DisableEscInterrupt();
    alEvent = BSP_LAN9253_ReadALEvent();
    HW_EnableEscInterrupt();

    return alEvent;
}

/**
 * @brief 中断上下文读取 AL Event 寄存器。
 */
UINT16 HW_GetALEventRegister_Isr(void)
{
    return BSP_LAN9253_ReadALEventIsr();
}

/**
 * @brief 禁用 ESC 中断，并增加嵌套禁用深度。
 */
void HW_DisableEscInterrupt(void)
{
    HAL_NVIC_DisableIRQ(LAN9253_IRQ_EXTI_IRQn);

    if (s_escIntDisableDepth < UINT32_MAX) {
        s_escIntDisableDepth++;
    }
}

/**
 * @brief 解除一次 ESC 中断禁用，禁用深度归零后重新使能中断。
 */
void HW_EnableEscInterrupt(void)
{
    if (s_escIntDisableDepth > 0U) {
        s_escIntDisableDepth--;
    }

    if (s_escIntDisableDepth == 0U) {
        HAL_NVIC_EnableIRQ(LAN9253_IRQ_EXTI_IRQn);
    }
}

/**
 * @brief 清除 MCU 侧残留的 ESC 相关 EXTI 挂起标志。
 */
static void HW_ClearEscInterruptPending(void)
{
    /* 清掉 MCU EXTI 侧可能残留的挂起位，避免使能中断后立刻响应旧边沿。 */
    __HAL_GPIO_EXTI_CLEAR_IT(LAN9253_IRQ_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(LAN9253_SYNC0_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(LAN9253_SYNC1_Pin);

    HAL_NVIC_ClearPendingIRQ(LAN9253_IRQ_EXTI_IRQn);
    HAL_NVIC_ClearPendingIRQ(LAN9253_SYNC0_EXTI_IRQn);
    HAL_NVIC_ClearPendingIRQ(LAN9253_SYNC1_EXTI_IRQn);
}

/**
 * @brief 普通上下文从 ESC 地址空间读取数据。
 */
void HW_EscRead(MEM_ADDR *pData, UINT16 address, UINT16 len)
{
    HW_DisableEscInterrupt();
    BSP_LAN9253_EscRead((uint8_t *)pData, (uint16_t)address, (uint16_t)len);
    HW_EnableEscInterrupt();
}

/**
 * @brief 普通上下文向 ESC 地址空间写入数据。
 */
void HW_EscWrite(MEM_ADDR *pData, UINT16 address, UINT16 len)
{
    HW_DisableEscInterrupt();
    BSP_LAN9253_EscWrite((const uint8_t *)pData, (uint16_t)address, (uint16_t)len);
    HW_EnableEscInterrupt();
}

/**
 * @brief 中断上下文从 ESC 地址空间读取数据。
 */
void HW_EscReadIsr(MEM_ADDR *pData, UINT16 address, UINT16 len)
{
    BSP_LAN9253_EscReadIsr((uint8_t *)pData, (uint16_t)address, (uint16_t)len);
}

/**
 * @brief 中断上下文向 ESC 地址空间写入数据。
 */
void HW_EscWriteIsr(MEM_ADDR *pData, UINT16 address, UINT16 len)
{
    BSP_LAN9253_EscWriteIsr((const uint8_t *)pData, (uint16_t)address, (uint16_t)len);
}

/**
 * @brief 读取 ESC 邮箱内存。
 */
void HW_EscReadMbxMem(MEM_ADDR *pData, UINT16 address, UINT16 len)
{
    HW_EscRead(pData, address, len);
}

/**
 * @brief 写入 ESC 邮箱内存。
 */
void HW_EscWriteMbxMem(MEM_ADDR *pData, UINT16 address, UINT16 len)
{
    HW_EscWrite(pData, address, len);
}
