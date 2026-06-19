#ifndef __BSP_SPI_H__
#define __BSP_SPI_H__

#include "stm32f4xx_hal.h"

/**
 * @brief SPI 设备结构体前向声明。
 *
 * 当前工程的 BSP 驱动采用简单的“设备对象 + 操作函数表”风格：
 *   - SPIDevice_t 保存 SPI 硬件句柄和操作函数表；
 *   - SPIDeviceOps_t 保存上层驱动可以调用的 SPI 操作接口。
 *
 * 后续 LAN9253 驱动建议通过 g_SpiDevice 访问 SPI，
 * 不要直接操作 hspi3，这样 LAN9253 层不会强绑定具体的 STM32 SPI 外设。
 */
typedef struct SPIDevice SPIDevice_t;

/**
 * @brief SPI 操作函数表。
 *
 * 注意：
 *   1. SPI_CS_xxx 函数只控制片选 GPIO，不初始化、不重新配置 SPI 外设。
 *   2. SPI_Transmit / SPI_Receive / SPI_TransmitReceive 是 HAL_SPI_xxx 的薄封装。
 *   3. SPI 收发函数不会自动拉低/拉高 CS。
 *      调用者需要自己包住一次完整 SPI 事务，例如：
 *
 *          g_SpiDevice.pOps->SPI_CS_Select(&g_SpiDevice);
 *          g_SpiDevice.pOps->SPI_TransmitReceive(...);
 *          g_SpiDevice.pOps->SPI_CS_Deselect(&g_SpiDevice);
 *
 *      对 LAN9253 来说，CS 决定一帧 SPI 命令的边界，所以这一点很重要。
 */
typedef struct{
    /**
     * @brief 将 SPI 片选线置为空闲状态。
     *
     * LAN9253 的 CS 为低电平有效：
     *   - 空闲 / 未选中：高电平
     *   - 选中 / 事务进行中：低电平
     */
    void (*SPI_CS_Init)(SPIDevice_t *this);

    /**
     * @brief 选中 SPI 从设备。
     *
     * 该函数会将 SPI_CS 拉低，表示开始一次 SPI 事务。
     */
    void (*SPI_CS_Select)(SPIDevice_t *this);

    /**
     * @brief 释放 SPI 从设备。
     *
     * 该函数会将 SPI_CS 拉高，表示结束一次 SPI 事务。
     */
    void (*SPI_CS_Deselect)(SPIDevice_t *this);

    /**
     * @brief SPI 阻塞发送。
     *
     * @param this     SPI 设备对象。
     * @param pData    待发送数据缓冲区。
     * @param Size     待发送字节数。
     * @param Timeout  HAL 超时时间，单位 ms。
     * @return HAL_OK / HAL_ERROR / HAL_BUSY / HAL_TIMEOUT。
     */
    HAL_StatusTypeDef (*SPI_Transmit)(SPIDevice_t *this, uint8_t *pData, uint16_t Size,uint32_t Timeout);

    /**
     * @brief SPI 阻塞接收。
     *
     * @param this     SPI 设备对象。
     * @param pData    接收数据缓冲区。
     * @param Size     待接收字节数。
     * @param Timeout  HAL 超时时间，单位 ms。
     * @return HAL_OK / HAL_ERROR / HAL_BUSY / HAL_TIMEOUT。
     *
     * STM32 的 SPI 是全双工外设，接收数据时也需要产生 SCK，
     * HAL 内部会通过发送 dummy 数据来产生时钟。
     */
    HAL_StatusTypeDef (*SPI_Receive)(SPIDevice_t *this, uint8_t *pData, uint16_t Size,uint32_t Timeout);

    /**
     * @brief SPI 阻塞全双工收发。
     *
     * @param this     SPI 设备对象。
     * @param pTxData  发送数据缓冲区。
     * @param pRxData  接收数据缓冲区。
     * @param Size     收发字节数。
     * @param Timeout  HAL 超时时间，单位 ms。
     * @return HAL_OK / HAL_ERROR / HAL_BUSY / HAL_TIMEOUT。
     *
     * 这是后续访问 LAN9253 时最常用的底层函数。
     * 因为 LAN9253 读寄存器时通常需要一边发送命令字节、地址字节、
     * dummy 字节，一边从 MISO 读回有效数据。
     */
    HAL_StatusTypeDef (*SPI_TransmitReceive)(SPIDevice_t *this, uint8_t *pTxData, uint8_t *pRxData, uint16_t Size,uint32_t Timeout);
    /**
     * @brief ISR 上下文单字节 SPI 全双工传输，返回 1 表示成功。
     */
    uint8_t (*SPI_TransferByteIsr)(SPIDevice_t *this, uint8_t txData, uint8_t *rxData);
}SPIDeviceOps_t;

/**
 * @brief SPI 设备对象。
 */
struct SPIDevice{
    /**
     * @brief HAL SPI 句柄。
     *
     * 当前板子使用 CubeMX 生成的 hspi3。
     * 使用本 BSP 前，必须先调用 MX_SPI3_Init() 完成底层外设初始化。
     */
    SPI_HandleTypeDef *pInstance;

    /**
     * @brief 当前 SPI 设备绑定的操作函数表。
     */
    const SPIDeviceOps_t *pOps;
};

/**
 * @brief 全局 SPI 设备实例，供 LAN9253 等 SPI 设备驱动使用。
 *
 * 当前硬件映射：
 *   - SPI 外设：SPI3
 *   - SCK： PC10
 *   - MISO：PC11
 *   - MOSI：PC12
 *   - CS：  SPI_CS_GPIO_Port / SPI_CS_Pin
 */
extern SPIDevice_t g_SpiDevice;

#endif /* __BSP_SPI_H__ */
