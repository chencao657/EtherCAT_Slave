/**
 ******************************************************************************
 * @file    bsp_gpio.c
 * @brief   通用 GPIO 控制对象实现。
 ******************************************************************************
 */
#include "bsp_gpio.h"

/**
 * @brief 初始化 GPIO 输出设备，默认置为高电平。
 */
static void GPIO_Init(GpioDevice_t *this)
{
    this->pOps->SetState(this, GPIO_HIGH);
}

/**
 * @brief 设置 GPIO 输出电平。
 */
static void GPIO_SetState(GpioDevice_t *this, GPIO_STATE_T state)
{
    GPIO_PinState pinState = (state == GPIO_HIGH) ? GPIO_PIN_SET : GPIO_PIN_RESET;

    HAL_GPIO_WritePin(this->pPort, this->pin, pinState);
}

/* GPIO 操作函数表实例。 */
static const GpioOps_t s_GpioOps = {
    .Init     = GPIO_Init,
    .SetState = GPIO_SetState,
};


/* LAN9253 SPI 片选控制设备。 */
GpioDevice_t g_LAN9253CsCtrl = {
    .pPort = LAN9253_CS_GPIO_Port,
    .pin   = LAN9253_CS_Pin,
    .pOps  = &s_GpioOps,
};

/* LAN9253 硬件复位控制设备。 */
GpioDevice_t g_LAN9253_RST = {
    .pPort = LAN9253_RST_GPIO_Port,
    .pin   = LAN9253_RST_Pin,
    .pOps  = &s_GpioOps,
};

