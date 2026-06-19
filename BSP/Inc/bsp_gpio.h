/**
 ******************************************************************************
 * @file    bsp_gpio.h
 * @brief   通用 GPIO 控制对象接口。
 *
 *          本模块把单个 GPIO 输出封装为 GpioDevice_t，供 LAN9253 CS、RST
 *          等控制引脚复用统一的 Init/SetState 操作。
 ******************************************************************************
 */
#ifndef __BSP_GPIO_H__
#define __BSP_GPIO_H__

#include "main.h"

/* GPIO 输出电平枚举。 */
typedef enum {
    GPIO_LOW = 0,
    GPIO_HIGH = 1,
} GPIO_STATE_T;

/* GPIO 设备前向声明。 */
typedef struct GpioDevice GpioDevice_t;

/* GPIO 操作函数表。 */
typedef struct {
    void (*Init)(GpioDevice_t *this);
    void (*SetState)(GpioDevice_t *this, GPIO_STATE_T state);
} GpioOps_t;

/* 单个 GPIO 输出设备实例描述。 */
struct GpioDevice {
    /** GPIO 端口。 */
    GPIO_TypeDef *pPort;
    /** GPIO 引脚。 */
    uint16_t pin;
    /** GPIO 操作函数表指针。 */
    const GpioOps_t *pOps;
};

/* LAN9253 SPI 片选控制引脚。 */
extern GpioDevice_t g_LAN9253CsCtrl;
/* LAN9253 硬件复位控制引脚。 */
extern GpioDevice_t g_LAN9253_RST;


#endif /* __BSP_GPIO_H__ */
