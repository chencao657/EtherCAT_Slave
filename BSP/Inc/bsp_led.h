/**
 ******************************************************************************
 * @file    bsp_led.h
 * @brief   板载 LED 驱动接口。
 *
 *          GPIO 映射来自 Core/Inc/main.h：
 *            LED1 -> PB6
 *            LED2 -> PB7
 ******************************************************************************
 */

#ifndef __BSP_LED_H__
#define __BSP_LED_H__

#include "main.h"

/* ============================== LED 编号枚举 ============================== */

/** @brief LED 编号。 */
typedef enum {
    LED1 = 0,
    LED2,
    LED_COUNT
} LED_NUM_T;

/* ============================== LED 状态枚举 ============================== */

/** @brief LED 亮灭状态。 */
typedef enum {
    LED_OFF = 0,
    LED_ON  = 1,
} LED_STATE_T;

/* ============================== LED 设备抽象 ============================== */

/** LED 设备前向声明。 */
typedef struct LedDevice LedDevice_t;

/**
 * @brief LED 操作函数表。
 */
typedef struct {
    void (*Init)(LedDevice_t *this);
    void (*SetState)(LedDevice_t *this, LED_NUM_T num, LED_STATE_T state);
    void (*Toggle)(LedDevice_t *this, LED_NUM_T num);
} LedOps_t;

/**
 * @brief LED 设备实例。
 */
struct LedDevice {
    /** 每个 LED 对应的 GPIO 端口。 */
    GPIO_TypeDef *port[LED_COUNT];
    /** 每个 LED 对应的 GPIO 引脚。 */
    uint16_t      pin[LED_COUNT];
    /** 每个 LED 点亮时的有效电平。 */
    GPIO_PinState activeLevel[LED_COUNT];
    /** LED 操作函数表指针。 */
    const LedOps_t *pOps;
};

/** 全局 LED 设备实例。 */
extern LedDevice_t g_LedDevice;

/** 初始化板载 LED。 */
void BSP_LED_Init(void);
/** 设置指定 LED 的亮灭状态。 */
void BSP_LED_Set(LED_NUM_T num, LED_STATE_T state);
/** 翻转指定 LED 的当前状态。 */
void BSP_LED_Toggle(LED_NUM_T num);

/** LED 心跳 1ms 节拍函数。 */
void LED_Heartbeat_1ms_Tick(void);

/** LED 心跳轮询函数。 */
void LED_Heartbeat_Poll(void);

#endif /* __BSP_LED_H__ */
