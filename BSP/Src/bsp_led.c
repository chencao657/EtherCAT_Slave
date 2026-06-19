/**
 ******************************************************************************
 * @file    bsp_led.c
 * @brief   板载 LED 驱动实现。
 *
 *          本模块通过 LedDevice_t 记录 LED 的 GPIO 端口、引脚和有效电平，
 *          对上提供初始化、亮灭控制、翻转和心跳闪烁接口。
 ******************************************************************************
 */

#include "bsp_led.h"

/* 私有函数声明。 */
static void LED_SetState(LedDevice_t *this, LED_NUM_T num, LED_STATE_T state);

/**
 * @brief 初始化 LED 设备，并将所有 LED 置为关闭状态。
 */
static void LED_Init(LedDevice_t *this)
{
    for (uint32_t i = 0; i < LED_COUNT; ++i) {
        LED_SetState(this, (LED_NUM_T)i, LED_OFF);
    }
}

/**
 * @brief 设置指定 LED 的输出状态。
 */
static void LED_SetState(LedDevice_t *this, LED_NUM_T num, LED_STATE_T state)
{
    if (num >= LED_COUNT) {
        return;
    }

    GPIO_PinState activeLevel = this->activeLevel[num];
    GPIO_PinState inactiveLevel = (activeLevel == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET;

    HAL_GPIO_WritePin(this->port[num], this->pin[num],
                      (state == LED_ON) ? activeLevel : inactiveLevel);
}

/**
 * @brief 翻转指定 LED 当前电平。
 */
static void LED_Toggle(LedDevice_t *this, LED_NUM_T num)
{
    if (num >= LED_COUNT) {
        return;
    }

    HAL_GPIO_TogglePin(this->port[num], this->pin[num]);
}

/* LED 设备操作函数表。 */
static const LedOps_t g_LedOps = {
    .Init     = LED_Init,
    .SetState = LED_SetState,
    .Toggle   = LED_Toggle,
};

/* 板载 LED 设备实例，GPIO 映射来自 Core/Inc/main.h。 */
LedDevice_t g_LedDevice = {
    .port = {
        LED1_GPIO_Port,
        LED2_GPIO_Port,
    },
    .pin = {
        LED1_Pin,
        LED2_Pin,
    },
    .activeLevel = {
        GPIO_PIN_SET,
        GPIO_PIN_SET,
    },
    .pOps = &g_LedOps,
};

/**
 * @brief 初始化板载 LED。
 */
void BSP_LED_Init(void)
{
    g_LedDevice.pOps->Init(&g_LedDevice);
}

/**
 * @brief 设置板载 LED 亮灭状态。
 */
void BSP_LED_Set(LED_NUM_T num, LED_STATE_T state)
{
    g_LedDevice.pOps->SetState(&g_LedDevice, num, state);
}

/**
 * @brief 翻转板载 LED 状态。
 */
void BSP_LED_Toggle(LED_NUM_T num)
{
    g_LedDevice.pOps->Toggle(&g_LedDevice, num);
}

/* 心跳计时器，由 1ms 周期函数累加。 */
static volatile uint32_t s_LedTick = 0;

#define HEARTBEAT_INTERVAL_MS 250U

/**
 * @brief LED 心跳 1ms 节拍累加函数，通常在定时器中断或系统节拍中调用。
 */
void LED_Heartbeat_1ms_Tick(void)
{
    s_LedTick++;
}

/**
 * @brief 轮询 LED 心跳状态，达到周期后翻转 LED1。
 */
void LED_Heartbeat_Poll(void)
{
    if (s_LedTick < HEARTBEAT_INTERVAL_MS) {
        return;
    }

    s_LedTick = 0;

    BSP_LED_Toggle(LED1);
}
