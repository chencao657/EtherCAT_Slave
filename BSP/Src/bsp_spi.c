#include "bsp_spi.h"
#include "bsp_gpio.h"

/*
 * hspi3 由 CubeMX 在 Core/Src/spi.c 中生成。
 * 本 BSP 只保存并使用这个句柄，不负责创建或配置 SPI 外设。
 * 使用本模块前，必须先调用 MX_SPI3_Init()。
 */
extern SPI_HandleTypeDef hspi3;

/**
 * @brief 将 SPI 片选线置为空闲状态。
 *
 * LAN9253 的 CS 为低电平有效：
 *   - GPIO_HIGH：未选中 / 空闲
 *   - GPIO_LOW ：选中 / SPI 事务进行中
 *
 * 该函数只控制 CS GPIO，不会调用 HAL_SPI_Init()。
 */
static void SPI_CS_Init(SPIDevice_t *this)
{
    (void)this;

    g_LAN9253CsCtrl.pOps->SetState(&g_LAN9253CsCtrl, GPIO_HIGH);
}

/**
 * @brief 选中 SPI 从设备。
 *
 * 对 LAN9253 来说，CS 拉低表示开始一帧 SPI 命令。
 * 一次完整事务通常包含：命令字节 + 地址字节 + dummy 字节 + 数据字节。
 * 在这些字节全部传输完成前，CS 应保持低电平。
 */
static void SPI_CS_Select(SPIDevice_t *this)
{
    (void)this;

    g_LAN9253CsCtrl.pOps->SetState(&g_LAN9253CsCtrl, GPIO_LOW);
}

/**
 * @brief 释放 SPI 从设备。
 *
 * 对 LAN9253 来说，CS 拉高表示结束当前 SPI 命令帧。
 * 上层驱动在传输失败的错误路径里也应该释放 CS，
 * 这样下一次 SPI 访问可以从干净的帧边界重新开始。
 */
static void SPI_CS_Deselect(SPIDevice_t *this)
{
    (void)this;

    g_LAN9253CsCtrl.pOps->SetState(&g_LAN9253CsCtrl, GPIO_HIGH);
}

/**
 * @brief SPI 阻塞发送封装。
 *
 * 该函数不会自动控制 CS。
 * 调用者可以自己决定什么时候拉低/拉高 CS，
 * 这样多个 HAL_SPI_xxx 调用可以组合在同一次 CS 低电平窗口内。
 *
 * @param this    SPI 设备对象。
 * @param pData   待发送数据缓冲区。
 * @param Size    待发送字节数。
 * @param Timeout HAL 超时时间，单位 ms。
 * @return HAL_OK / HAL_ERROR / HAL_BUSY / HAL_TIMEOUT。
 */
static HAL_StatusTypeDef SPI_Transmit(SPIDevice_t *this, uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    return HAL_SPI_Transmit(this->pInstance, pData, Size, Timeout);
}

/**
 * @brief SPI 阻塞接收封装。
 *
 * STM32 的 SPI 是全双工外设。
 * 即使只接收数据，也需要通过 MOSI 移出 dummy 数据来产生 SCK 时钟。
 * 这部分由 HAL_SPI_Receive() 内部处理。
 *
 * @param this    SPI 设备对象。
 * @param pData   接收数据缓冲区。
 * @param Size    待接收字节数。
 * @param Timeout HAL 超时时间，单位 ms。
 * @return HAL_OK / HAL_ERROR / HAL_BUSY / HAL_TIMEOUT。
 */
static HAL_StatusTypeDef SPI_Receive(SPIDevice_t *this, uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    return HAL_SPI_Receive(this->pInstance, pData, Size, Timeout);
}

/**
 * @brief SPI 阻塞全双工收发封装。
 *
 * 后续访问 LAN9253 时，建议优先使用这个函数作为底层原语。
 * 因为 LAN9253 的读操作通常需要在同一次 CS 低电平窗口内：
 *   1. 发送读命令；
 *   2. 发送地址；
 *   3. 发送 dummy 字节；
 *   4. 同时读回 MISO 上的数据。
 *
 * @param this    SPI 设备对象。
 * @param pTxData 发送数据缓冲区。
 * @param pRxData 接收数据缓冲区。
 * @param Size    收发字节数。
 * @param Timeout HAL 超时时间，单位 ms。
 * @return HAL_OK / HAL_ERROR / HAL_BUSY / HAL_TIMEOUT。
 */
static HAL_StatusTypeDef SPI_TransmitReceive(SPIDevice_t *this, uint8_t *pTxData, uint8_t *pRxData, uint16_t Size, uint32_t Timeout)
{
    return HAL_SPI_TransmitReceive(this->pInstance, pTxData, pRxData, Size, Timeout);
}

/* ISR 轮询等待次数上限，防止中断内长时间阻塞。 */
#define SPI_ISR_FLAG_WAIT_LOOPS 10000U

/**
 * @brief ISR 上下文等待指定 SPI 标志达到目标状态。
 */
static uint8_t SPI_WaitFlagStateIsr(SPI_HandleTypeDef *hspi, uint32_t flag, FlagStatus state)
{
    uint32_t loops = SPI_ISR_FLAG_WAIT_LOOPS;

    while ((__HAL_SPI_GET_FLAG(hspi, flag) != state) && (loops > 0U)) {
        loops--;
    }

    return (loops > 0U) ? 1U : 0U;
}

/**
 * @brief ISR 上下文单字节 SPI 全双工传输。
 */
static uint8_t SPI_TransferByteIsr(SPIDevice_t *this, uint8_t txData, uint8_t *rxData)
{
    SPI_HandleTypeDef *hspi = this->pInstance;
    uint8_t localRx;

    if (SPI_WaitFlagStateIsr(hspi, SPI_FLAG_TXE, SET) == 0U) {
        return 0U;
    }

    *((__IO uint8_t *)&hspi->Instance->DR) = txData;

    if (SPI_WaitFlagStateIsr(hspi, SPI_FLAG_RXNE, SET) == 0U) {
        return 0U;
    }

    localRx = *((__IO uint8_t *)&hspi->Instance->DR);

    if (SPI_WaitFlagStateIsr(hspi, SPI_FLAG_TXE, SET) == 0U) {
        return 0U;
    }

    if (SPI_WaitFlagStateIsr(hspi, SPI_FLAG_BSY, RESET) == 0U) {
        return 0U;
    }

    if (rxData != NULL) {
        *rxData = localRx;
    }

    return 1U;
}

/**
 * @brief SPI3 设备的操作函数表。
 */

const SPIDeviceOps_t s_SpiOps = {
    .SPI_CS_Init         = SPI_CS_Init,
    .SPI_CS_Select       = SPI_CS_Select,
    .SPI_CS_Deselect     = SPI_CS_Deselect,
    .SPI_Transmit        = SPI_Transmit,
    .SPI_Receive         = SPI_Receive,
    .SPI_TransmitReceive = SPI_TransmitReceive,
    .SPI_TransferByteIsr = SPI_TransferByteIsr,
};

/**
 * @brief 全局 SPI 设备对象。
 *
 * 当前硬件映射：
 *   - SPI 外设：SPI3 / hspi3
 *   - CS 引脚：SPI_CS_GPIO_Port / SPI_CS_Pin
 */
SPIDevice_t g_SpiDevice = {
    .pInstance = &hspi3,
    .pOps      = &s_SpiOps,
};
