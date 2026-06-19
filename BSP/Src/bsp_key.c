/**
 ******************************************************************************
 * @file    bsp_key.c
 * @brief   按键驱动 — 实现（非阻塞消抖扫描）
 *
 *          消抖原理：
 *            按键按下时 GPIO 读为低电平（RESET）。机械触点会抖动 5~20ms，
 *            如果每次读到低电平就上报，一次按键会触发几十次事件。
 *
 *            本驱动的消抖策略：
 *              1. 每 10ms 扫描一次（由主循环定时调用）
 *              2. 连续 3 次读到低电平 → 判定为有效按下 → 上报事件
 *              3. 上报后计数器锁死在 3，按住不放不再重复上报（单次触发模式）
 *              4. 读到高电平（松手）→ 计数器清零 → 解锁，准备检测下一次按下
 *
 *          硬件映射：
 *            按键 → GPIOG 引脚，按下 = 低电平（GPIO_PIN_RESET）
 *            KEY1 → PG15    KEY2 → PG12
 *            KEY3 → PG10    KEY4 → PG8
 ******************************************************************************
 */

#include "bsp_key.h"
#include "stm32f4xx_hal.h"

/* ========================= 硬件引脚映射 ================================== */

#define KEY_PORT    GPIOG         /**< 所有按键共用 GPIOG 端口          */
#define PIN_KEY1    GPIO_PIN_15   /**< 按键 1 引脚（按下低电平）        */
#define PIN_KEY2    GPIO_PIN_12   /**< 按键 2 引脚（按下低电平）        */
#define PIN_KEY3    GPIO_PIN_10   /**< 按键 3 引脚（按下低电平）        */
#define PIN_KEY4    GPIO_PIN_8    /**< 按键 4 引脚（按下低电平）        */

#define DEBOUNCE_CNT_MAX  3       /**< 消抖阈值：连续 3 次 = 30ms       */

/* ========================= 公 共 API ==================================== */

/**
 * @brief   非阻塞按键扫描消抖函数
 * @return  本周期的按键事件（有效按下时返回对应事件，否则返回 KEY_EVT_NONE）
 *
 * @note    调用频率必须稳定在 10ms/次，否则消抖时间会偏移。
 *          如果需要更改消抖时长，修改 DEBOUNCE_CNT_MAX 和调用间隔。
 *
 *          "锁死"机制意味着：按键按住不放只触发一次，松手后才可再次触发。
 *          如需连发功能（按住每 200ms 重复上报），需在应用层另行实现。
 */
KeyEvent_t Key_Poll_10ms(void)
{
    /* 静态计数器：跨调用保持消抖状态（非线程安全，仅主循环使用） */
    static uint8_t k1_cnt = 0;
    static uint8_t k2_cnt = 0;
    static uint8_t k3_cnt = 0;
    static uint8_t k4_cnt = 0;

    /* ==================== KEY 1 扫描（PG15） ======================= */
    if (HAL_GPIO_ReadPin(KEY_PORT, PIN_KEY1) == GPIO_PIN_RESET) {
        /* 按下中：计数器累加，到阈值时上报事件并锁死 */
        if (k1_cnt < DEBOUNCE_CNT_MAX) {
            k1_cnt++;
            if (k1_cnt == DEBOUNCE_CNT_MAX) {
                return KEY_EVT_1_PRESS;
            }
        }
    } else {
        k1_cnt = 0;  /* 松手：解锁，准备下次按下 */
    }

    /* ==================== KEY 2 扫描（PG12） ======================= */
    if (HAL_GPIO_ReadPin(KEY_PORT, PIN_KEY2) == GPIO_PIN_RESET) {
        if (k2_cnt < DEBOUNCE_CNT_MAX) {
            k2_cnt++;
            if (k2_cnt == DEBOUNCE_CNT_MAX) {
                return KEY_EVT_2_PRESS;
            }
        }
    } else {
        k2_cnt = 0;
    }

    /* ==================== KEY 3 扫描（PG10） ======================= */
    if (HAL_GPIO_ReadPin(KEY_PORT, PIN_KEY3) == GPIO_PIN_RESET) {
        if (k3_cnt < DEBOUNCE_CNT_MAX) {
            k3_cnt++;
            if (k3_cnt == DEBOUNCE_CNT_MAX) {
                return KEY_EVT_3_PRESS;
            }
        }
    } else {
        k3_cnt = 0;
    }

    /* ==================== KEY 4 扫描（PG8） ======================== */
    if (HAL_GPIO_ReadPin(KEY_PORT, PIN_KEY4) == GPIO_PIN_RESET) {
        if (k4_cnt < DEBOUNCE_CNT_MAX) {
            k4_cnt++;
            if (k4_cnt == DEBOUNCE_CNT_MAX) {
                return KEY_EVT_4_PRESS;
            }
        }
    } else {
        k4_cnt = 0;
    }

    /* 本周期无任何按键触发 */
    return KEY_EVT_NONE;
}

uint8_t BSP_Key_ReadStateByte(void)
{
    uint8_t state = 0U;

    if (HAL_GPIO_ReadPin(KEY_PORT, PIN_KEY1) == GPIO_PIN_RESET) {
        state |= 0x01U;
    }

    if (HAL_GPIO_ReadPin(KEY_PORT, PIN_KEY2) == GPIO_PIN_RESET) {
        state |= 0x02U;
    }

    if (HAL_GPIO_ReadPin(KEY_PORT, PIN_KEY3) == GPIO_PIN_RESET) {
        state |= 0x04U;
    }

    if (HAL_GPIO_ReadPin(KEY_PORT, PIN_KEY4) == GPIO_PIN_RESET) {
        state |= 0x08U;
    }

    return state;
}
