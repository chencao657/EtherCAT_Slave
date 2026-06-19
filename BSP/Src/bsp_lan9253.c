#include "bsp_lan9253.h"

#include <stddef.h>

/*
 * LAN9253 驱动实现说明
 *
 * 本文件把 LAN9253 的 SPI 访问拆成四个逐步验证过的阶段：
 *   1. 直接访问 LAN9253 主机侧寄存器，确认 SPI 链路和芯片 ID。
 *   2. 通过 CSR 窗口间接访问 ESC 内核寄存器。
 *   3. 通过 PRAM FIFO 访问 DPRAM/过程数据区，并使用连续 FIFO 读写优化。
 *   4. 为 EtherCAT 协议栈提供统一 ESC 读写入口，按地址自动选择 CSR 或 PRAM。
 *
 * 当前硬件处于 SPI LAN9252 兼容模式：
 *   - LAN9253 自身寄存器仍然直接通过 SPI 地址访问。
 *   - ESC 内核低地址寄存器通过 CSR 间接访问。
 *   - ESC DPRAM/过程数据区通过 PRAM FIFO 访问。
 */

/* ============================== LAN9253 直接寄存器 ============================== */
#define LAN9253_REG_ID_REV              0x0050U
#define LAN9253_REG_IRQ_CFG             0x0054U
#define LAN9253_REG_INT_STS             0x0058U
#define LAN9253_REG_INT_EN              0x005CU
#define LAN9253_REG_BYTE_TEST           0x0064U


#define LAN9253_IRQ_CFG_VALUE           0x00000101UL
#define LAN9253_INT_ECAT_EVENT          0x00000001UL
#define LAN9253_INT_EN_VALUE            LAN9253_INT_ECAT_EVENT
/* ============================== LAN9253 主机侧 CSR/PRAM 寄存器 ============================== */
#define LAN9253_ESC_CSR_DATA_REG        0x0300U
#define LAN9253_ESC_CSR_CMD_REG         0x0304U

#define LAN9253_PRAM_READ_ADDR_LEN_REG  0x0308U
#define LAN9253_PRAM_READ_CMD_REG       0x030CU
#define LAN9253_PRAM_WRITE_ADDR_LEN_REG 0x0310U
#define LAN9253_PRAM_WRITE_CMD_REG      0x0314U
#define LAN9253_PRAM_READ_FIFO_REG      0x0004U
#define LAN9253_PRAM_WRITE_FIFO_REG     0x0020U

/* ============================== ESC 内部寄存器地址 ============================== */
#define LAN9253_ESC_DPRAM_START_ADDR    0x1000U
#define LAN9253_ESC_ADDR_CHIP_ID        0x0E02U
#define LAN9253_ESC_ADDR_AL_EVENT       0x0220U

#define LAN9253_BYTE_TEST_VALUE         0x87654321UL
#define LAN9253_CHIP_ID_VALUE           0x9253U

/* ============================== CSR 命令格式 ==============================
 * CSR_CMD_REG(0x0304) 写入格式：
 * [7:0]   ESC 地址低字节
 * [15:8]  ESC 地址高字节
 * [23:16] 传输字节数，范围 1..4
 * [31:24] 读写命令，bit7 在命令执行期间为 BUSY
 */
#define LAN9253_ESC_CSR_BUSY_MASK       0x80000000UL
#define LAN9253_ESC_CSR_READ_CMD        0xC0U
#define LAN9253_ESC_CSR_WRITE_CMD       0x80U
#define LAN9253_ESC_CSR_MAX_LEN         4U
#define LAN9253_ESC_CSR_TIMEOUT_MS      100U

/*
 * PRAM 命令寄存器位定义
 *
 * BUSY：
 *   写 1 启动一次 PRAM 读/写事务，硬件完成后自动清 0。
 *
 * ABORT：
 *   写 1 终止上一次可能残留的 PRAM 事务。每次开始新事务前先 ABORT，
 *   可以避免上一次调试中断、超时或异常退出留下的状态影响本次访问。
 *
 * AVAIL / AVAIL_COUNT：
 *   表示 FIFO 当前是否可读/可写，以及可连续搬运多少个 DWORD。
 *   驱动会利用 AVAIL_COUNT 做 burst 读写，减少 CS 翻转和 SPI 命令开销。
 */
#define LAN9253_PRAM_BUSY_MASK          0x80000000UL
#define LAN9253_PRAM_ABORT_MASK         0x40000000UL
#define LAN9253_PRAM_AVAIL_MASK         0x00000001UL
#define LAN9253_PRAM_AVAIL_COUNT_MASK   0x00001F00UL
#define LAN9253_PRAM_AVAIL_COUNT_SHIFT  8U
#define LAN9253_PRAM_FIFO_DWORD_BYTES   4U
#define LAN9253_PRAM_BURST_MAX_DWORDS   16U
#define LAN9253_PRAM_BURST_MAX_BYTES    (LAN9253_PRAM_BURST_MAX_DWORDS * LAN9253_PRAM_FIFO_DWORD_BYTES)
#define LAN9253_PRAM_TIMEOUT_MS         2U

/* ============================== LAN9253 SPI 命令 ============================== */
#define LAN9253_SPI_CMD_FAST_READ       0x0BU
#define LAN9253_SPI_CMD_FAST_READ_DUMMY 0x01U
#define LAN9253_SPI_CMD_SERIAL_WRITE    0x02U
#define LAN9253_ISR_WAIT_LOOPS          1024U

/* ============================== 静态函数声明 ============================== */

static void LAN9253_Init(LAN9253Device_t *this);
static void LAN9253_Reset(LAN9253Device_t *this);
static uint8_t LAN9253_SelfTest(LAN9253Device_t *this);

static uint32_t LAN9253_ReadDWord(LAN9253Device_t *this, uint16_t addr);
static void LAN9253_WriteDWord(LAN9253Device_t *this, uint16_t addr, uint32_t value);

static uint8_t LAN9253_CsrWaitBusyClear(LAN9253Device_t *this, uint32_t timeoutMs);
static void LAN9253_CsrReadBuffer(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len);
static void LAN9253_CsrWriteBuffer(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len);

static uint8_t LAN9253_PramReadBusyWait(LAN9253Device_t *this, uint32_t timeoutMs);
static uint8_t LAN9253_PramWriteBusyWait(LAN9253Device_t *this, uint32_t timeoutMs);
static void LAN9253_PramRead(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len);
static void LAN9253_PramWrite(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len);

static void LAN9253_EscRead(LAN9253Device_t *this, uint8_t *buf, uint16_t escAddr, uint16_t len);
static void LAN9253_EscWrite(LAN9253Device_t *this, const uint8_t *buf, uint16_t escAddr, uint16_t len);
static void LAN9253_EscReadIsr(LAN9253Device_t *this, uint8_t *buf, uint16_t escAddr, uint16_t len);
static void LAN9253_EscWriteIsr(LAN9253Device_t *this, const uint8_t *buf, uint16_t escAddr, uint16_t len);

static uint8_t LAN9253_TransferByteIsr(LAN9253Device_t *this, uint8_t txData, uint8_t *rxData);
static uint8_t LAN9253_ReadDWordIsr(LAN9253Device_t *this, uint16_t addr, uint32_t *value);
static uint8_t LAN9253_WriteDWordIsr(LAN9253Device_t *this, uint16_t addr, uint32_t value);
static uint8_t LAN9253_CsrWaitBusyClearIsr(LAN9253Device_t *this);
static void LAN9253_CsrReadBufferIsr(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len);
static void LAN9253_CsrWriteBufferIsr(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len);

static uint8_t LAN9253_PramReadBusyWaitIsr(LAN9253Device_t *this);
static uint8_t LAN9253_PramWriteBusyWaitIsr(LAN9253Device_t *this);
static uint8_t LAN9253_PramReadAvailWaitIsr(LAN9253Device_t *this, uint8_t *pDwordCount);
static uint8_t LAN9253_PramWriteAvailWaitIsr(LAN9253Device_t *this, uint8_t *pDwordCount);
static uint8_t LAN9253_ReadFifoBurstIsr(LAN9253Device_t *this, uint16_t fifoAddr, uint8_t *buf, uint16_t len);
static uint8_t LAN9253_WriteFifoBurstIsr(LAN9253Device_t *this, uint16_t fifoAddr, const uint8_t *buf, uint16_t len);
static void LAN9253_PramReadIsr(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len);
static void LAN9253_PramWriteIsr(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len);

/* ============================== 操作函数表与设备对象 ============================== */

static const LAN9253Ops_t s_LAN9253Ops = {
    .Init              = LAN9253_Init,
    .Reset             = LAN9253_Reset,
    .SelfTest          = LAN9253_SelfTest,
    .ReadDWord         = LAN9253_ReadDWord,
    .WriteDWord        = LAN9253_WriteDWord,
    .CsrWaitBusyClear  = LAN9253_CsrWaitBusyClear,
    .CsrReadBuffer     = LAN9253_CsrReadBuffer,
    .CsrWriteBuffer    = LAN9253_CsrWriteBuffer,
    .PramReadBusyWait  = LAN9253_PramReadBusyWait,
    .PramWriteBusyWait = LAN9253_PramWriteBusyWait,
    .PramRead          = LAN9253_PramRead,
    .PramWrite         = LAN9253_PramWrite,
    .EscRead           = LAN9253_EscRead,
    .EscWrite          = LAN9253_EscWrite,
    .EscReadIsr        = LAN9253_EscReadIsr,
    .EscWriteIsr       = LAN9253_EscWriteIsr,

};

LAN9253Device_t g_LAN9253Device = {
    .pSpi   = &g_SpiDevice,
    .pReset = &g_LAN9253_RST,
    .pOps   = &s_LAN9253Ops,
};

/* ============================== 第一阶段：最小硬件验证 ============================== */

/**
 * @brief 初始化 LAN9253 驱动。
 *
 * 当前第一阶段会完成：
 *   1. 初始化 SPI 片选空闲电平。
 *   2. 对 LAN9253 做硬件复位。
 *   3. 执行一次 BYTE_TEST/ID_REV 自检。
 */

// static void LAN9253_ConfigureInterrupt(LAN9253Device_t *this);

static void LAN9253_Init(LAN9253Device_t *this)
{
    this->pSpi->pOps->SPI_CS_Init(this->pSpi);
    this->pOps->Reset(this);
    this->pOps->SelfTest(this);
   
}

/**
 * @brief 复位 LAN9253 芯片。
 *
 * RST 拉低 100 ms 后释放，再等待 100 ms，让内部 ESC 逻辑稳定。
 */
static void LAN9253_Reset(LAN9253Device_t *this)
{
    this->pReset->pOps->SetState(this->pReset, GPIO_LOW);
    HAL_Delay(100);

    this->pReset->pOps->SetState(this->pReset, GPIO_HIGH);
    HAL_Delay(100);
}

/**
 * @brief 第一阶段自检。
 *
 * 检查直接寄存器 BYTE_TEST 是否为 0x87654321，
 * 并检查 ID_REV 高 16 位是否为 0x9253。
 */
static uint8_t LAN9253_SelfTest(LAN9253Device_t *this)
{
    uint32_t byteTest = this->pOps->ReadDWord(this, LAN9253_REG_BYTE_TEST);
    uint32_t idRev = this->pOps->ReadDWord(this, LAN9253_REG_ID_REV);
    uint16_t chipId = (uint16_t)((idRev >> 16) & 0xFFFFU);

    return ((byteTest == LAN9253_BYTE_TEST_VALUE) &&
            (chipId == LAN9253_CHIP_ID_VALUE)) ? 1U : 0U;
}
// static void LAN9253_ConfigureInterrupt(LAN9253Device_t *this)
// {
//     this->pOps->WriteDWord(this, LAN9253_REG_INT_EN, 0UL);
//     this->pOps->WriteDWord(this, LAN9253_REG_INT_STS, LAN9253_INT_ECAT_EVENT);
//     this->pOps->WriteDWord(this, LAN9253_REG_IRQ_CFG, LAN9253_IRQ_CFG_VALUE);
//     this->pOps->WriteDWord(this, LAN9253_REG_INT_EN, LAN9253_INT_EN_VALUE);
//     this->pOps->WriteDWord(this, LAN9253_REG_INT_STS, LAN9253_INT_ECAT_EVENT);
// }

/**
 * @brief 通过 SPI 快读命令读取 LAN9253 32 位直接寄存器。
 *
 * LAN9253 官方例程使用 FAST_READ(0x0B)，地址后跟 dummy 字节。
 * 32 位数据按小端返回。
 */
static uint32_t LAN9253_ReadDWord(LAN9253Device_t *this, uint16_t addr)
{
    uint8_t txBuf[9] = {
        LAN9253_SPI_CMD_FAST_READ,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFFU),
        LAN9253_SPI_CMD_FAST_READ_DUMMY,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
    };
    uint8_t rxBuf[9] = {0};
    HAL_StatusTypeDef status;

    this->pSpi->pOps->SPI_CS_Select(this->pSpi);
    status = this->pSpi->pOps->SPI_TransmitReceive(this->pSpi, txBuf, rxBuf, sizeof(txBuf), 100U);
    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);

    if (status != HAL_OK) {
        return 0U;
    }

    return ((uint32_t)rxBuf[5]) |
           ((uint32_t)rxBuf[6] << 8) |
           ((uint32_t)rxBuf[7] << 16) |
           ((uint32_t)rxBuf[8] << 24);
}

/**
 * @brief 通过 SPI 写入 LAN9253 32 位直接寄存器。
 *
 * 写寄存器使用 SERIAL_WRITE(0x02)，数据按小端发送。
 */
static void LAN9253_WriteDWord(LAN9253Device_t *this, uint16_t addr, uint32_t value)
{
    uint8_t txBuf[7] = {
        LAN9253_SPI_CMD_SERIAL_WRITE,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFFU),
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
        (uint8_t)((value >> 16) & 0xFFU),
        (uint8_t)((value >> 24) & 0xFFU),
    };

    this->pSpi->pOps->SPI_CS_Select(this->pSpi);
    (void)this->pSpi->pOps->SPI_Transmit(this->pSpi, txBuf, sizeof(txBuf), 100U);
    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);
}

/* ---------- 第一阶段 ISR 辅助：直接寄存器访问，不使用 HAL 阻塞收发 ---------- */

/**
 * @brief ISR 上下文使用的单字节 SPI 全双工收发封装。
 *
 * 这里不直接调用 HAL_SPI_TransmitReceive()，而是转到底层 bsp_spi 中的
 * SPI_TransferByteIsr()。底层函数只等待 TXE/RXNE/BSY 标志，等待次数有限，
 * 适合在 AL_EVENT 中断里完成很短的 LAN9253 寄存器访问。
 *
 * @return 1：传输成功；0：等待 SPI 标志超时。
 */
static uint8_t LAN9253_TransferByteIsr(LAN9253Device_t *this, uint8_t txData, uint8_t *rxData)
{
    return this->pSpi->pOps->SPI_TransferByteIsr(this->pSpi, txData, rxData);
}

/**
 * @brief ISR 上下文读取 LAN9253 32 位直接寄存器。
 *
 * 帧格式与普通 LAN9253_ReadDWord() 保持一致，只是每个字节通过
 * LAN9253_TransferByteIsr() 发送，避免进入 HAL 阻塞式超时等待。
 */
static uint8_t LAN9253_ReadDWordIsr(LAN9253Device_t *this, uint16_t addr, uint32_t *value)
{
    uint8_t rx[4] = {0};
    uint8_t dummy;

    if (value == NULL) {
        return 0U;
    }

    this->pSpi->pOps->SPI_CS_Select(this->pSpi);

    if (LAN9253_TransferByteIsr(this, LAN9253_SPI_CMD_FAST_READ, &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, (uint8_t)(addr >> 8), &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, (uint8_t)(addr & 0xFFU), &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, LAN9253_SPI_CMD_FAST_READ_DUMMY, &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, 0x00U, &dummy) == 0U) {
        goto fail;
    }

    for (uint8_t i = 0U; i < 4U; ++i) {
        if (LAN9253_TransferByteIsr(this, 0x00U, &rx[i]) == 0U) {
            goto fail;
        }
    }

    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);

    *value = ((uint32_t)rx[0]) |
             ((uint32_t)rx[1] << 8) |
             ((uint32_t)rx[2] << 16) |
             ((uint32_t)rx[3] << 24);
    return 1U;

fail:
    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);
    *value = 0U;
    return 0U;
}

/**
 * @brief ISR 上下文写入 LAN9253 32 位直接寄存器。
 *
 * 写命令、地址和 4 字节小端数据必须在同一个 CS 低电平窗口内完成。
 */
static uint8_t LAN9253_WriteDWordIsr(LAN9253Device_t *this, uint16_t addr, uint32_t value)
{
    uint8_t dummy;
    uint8_t txBuf[7] = {
        LAN9253_SPI_CMD_SERIAL_WRITE,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFFU),
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
        (uint8_t)((value >> 16) & 0xFFU),
        (uint8_t)((value >> 24) & 0xFFU),
    };

    this->pSpi->pOps->SPI_CS_Select(this->pSpi);

    for (uint8_t i = 0U; i < sizeof(txBuf); ++i) {
        if (LAN9253_TransferByteIsr(this, txBuf[i], &dummy) == 0U) {
            this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);
            return 0U;
        }
    }

    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);
    return 1U;
}

/* ============================== 第二阶段：CSR 间接访问 ESC 寄存器 ============================== */

/**
 * @brief 等待 CSR 命令寄存器 BUSY 位清零。
 * @return 1：空闲；0：超时。
 */
static uint8_t LAN9253_CsrWaitBusyClear(LAN9253Device_t *this, uint32_t timeoutMs)
{
    uint32_t startTick = HAL_GetTick();

    do {
        uint32_t csrCmd = this->pOps->ReadDWord(this, LAN9253_ESC_CSR_CMD_REG);

        if ((csrCmd & LAN9253_ESC_CSR_BUSY_MASK) == 0U) {
            return 1U;
        }
    } while ((HAL_GetTick() - startTick) < timeoutMs);

    return 0U;
}

/**
 * @brief 通过 CSR 从 ESC 地址空间读取任意长度数据。
 *
 * CSR DATA 寄存器只有 32 位，所以每次最多读 4 字节。
 * offset 表示已经完成的字节数，用于推算 ESC 地址和用户缓冲区位置。
 */
static void LAN9253_CsrReadBuffer(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    while (offset < len) {
        uint8_t chunkLen = (uint8_t)(len - offset);
        uint16_t currentAddr = (uint16_t)(escAddr + offset);
        uint32_t csrCmd;
        uint32_t csrData;

        if (chunkLen > LAN9253_ESC_CSR_MAX_LEN) {
            chunkLen = LAN9253_ESC_CSR_MAX_LEN;
        }

        if (this->pOps->CsrWaitBusyClear(this, LAN9253_ESC_CSR_TIMEOUT_MS) == 0U) {
            return;
        }

        csrCmd = ((uint32_t)(currentAddr & 0x00FFU)) |
                 ((uint32_t)((currentAddr >> 8) & 0x00FFU) << 8) |
                 ((uint32_t)chunkLen << 16) |
                 ((uint32_t)LAN9253_ESC_CSR_READ_CMD << 24);

        this->pOps->WriteDWord(this, LAN9253_ESC_CSR_CMD_REG, csrCmd);

        if (this->pOps->CsrWaitBusyClear(this, LAN9253_ESC_CSR_TIMEOUT_MS) == 0U) {
            return;
        }

        csrData = this->pOps->ReadDWord(this, LAN9253_ESC_CSR_DATA_REG);

        for (uint8_t i = 0U; i < chunkLen; ++i) {
            buf[offset + i] = (uint8_t)((csrData >> (8U * i)) & 0xFFU);
        }

        offset = (uint16_t)(offset + chunkLen);
    }
}

/**
 * @brief 通过 CSR 向 ESC 地址空间写入任意长度数据。
 *
 * 与读操作一样，写操作按最多 4 字节分块。
 * 每一块先写 CSR_DATA_REG，再写 CSR_CMD_REG 触发 ESC 写事务。
 */
static void LAN9253_CsrWriteBuffer(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    while (offset < len) {
        uint8_t chunkLen = (uint8_t)(len - offset);
        uint16_t currentAddr = (uint16_t)(escAddr + offset);
        uint32_t csrData = 0U;
        uint32_t csrCmd;

        if (chunkLen > LAN9253_ESC_CSR_MAX_LEN) {
            chunkLen = LAN9253_ESC_CSR_MAX_LEN;
        }

        if (this->pOps->CsrWaitBusyClear(this, LAN9253_ESC_CSR_TIMEOUT_MS) == 0U) {
            return;
        }

        for (uint8_t i = 0U; i < chunkLen; ++i) {
            csrData |= ((uint32_t)buf[offset + i] << (8U * i));
        }

        this->pOps->WriteDWord(this, LAN9253_ESC_CSR_DATA_REG, csrData);

        csrCmd = ((uint32_t)(currentAddr & 0x00FFU)) |
                 ((uint32_t)((currentAddr >> 8) & 0x00FFU) << 8) |
                 ((uint32_t)chunkLen << 16) |
                 ((uint32_t)LAN9253_ESC_CSR_WRITE_CMD << 24);

        this->pOps->WriteDWord(this, LAN9253_ESC_CSR_CMD_REG, csrCmd);

        if (this->pOps->CsrWaitBusyClear(this, LAN9253_ESC_CSR_TIMEOUT_MS) == 0U) {
            return;
        }

        offset = (uint16_t)(offset + chunkLen);
    }
}

/* ---------- 第二阶段 ISR 辅助：CSR 间接访问 ESC 低地址寄存器 ---------- */

/**
 * @brief ISR 上下文等待 CSR BUSY 清零。
 *
 * 与普通版本不同，这里不能使用 HAL_GetTick() 做毫秒级等待。
 * 因此使用 LAN9253_ISR_WAIT_LOOPS 做有限次数短轮询，避免中断内无限卡死。
 */
static uint8_t LAN9253_CsrWaitBusyClearIsr(LAN9253Device_t *this)
{
    for (uint32_t loops = LAN9253_ISR_WAIT_LOOPS; loops > 0U; --loops) {
        uint32_t csrCmd;

        if (LAN9253_ReadDWordIsr(this, LAN9253_ESC_CSR_CMD_REG, &csrCmd) == 0U) {
            return 0U;
        }

        if ((csrCmd & LAN9253_ESC_CSR_BUSY_MASK) == 0U) {
            return 1U;
        }
    }

    return 0U;
}

/**
 * @brief ISR 上下文通过 CSR 读取 ESC 低地址寄存器。
 *
 * CSR 每次最多搬运 4 字节；AL_EVENT、AL_STATUS、SM 状态等低地址寄存器
 * 都走这条路径。
 */
static void LAN9253_CsrReadBufferIsr(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    while (offset < len) {
        uint8_t chunkLen = (uint8_t)(len - offset);
        uint16_t currentAddr = (uint16_t)(escAddr + offset);
        uint32_t csrCmd;
        uint32_t csrData;

        if (chunkLen > LAN9253_ESC_CSR_MAX_LEN) {
            chunkLen = LAN9253_ESC_CSR_MAX_LEN;
        }

        if (LAN9253_CsrWaitBusyClearIsr(this) == 0U) {
            return;
        }

        csrCmd = ((uint32_t)(currentAddr & 0x00FFU)) |
                 ((uint32_t)((currentAddr >> 8) & 0x00FFU) << 8) |
                 ((uint32_t)chunkLen << 16) |
                 ((uint32_t)LAN9253_ESC_CSR_READ_CMD << 24);

        if (LAN9253_WriteDWordIsr(this, LAN9253_ESC_CSR_CMD_REG, csrCmd) == 0U) {
            return;
        }

        if (LAN9253_CsrWaitBusyClearIsr(this) == 0U) {
            return;
        }

        if (LAN9253_ReadDWordIsr(this, LAN9253_ESC_CSR_DATA_REG, &csrData) == 0U) {
            return;
        }

        for (uint8_t i = 0U; i < chunkLen; ++i) {
            buf[offset + i] = (uint8_t)((csrData >> (8U * i)) & 0xFFU);
        }

        offset = (uint16_t)(offset + chunkLen);
    }
}

/**
 * @brief ISR 上下文通过 CSR 写入 ESC 低地址寄存器。
 *
 * 每个分块先写 CSR_DATA_REG，再写 CSR_CMD_REG 触发 ESC 写事务。
 */
static void LAN9253_CsrWriteBufferIsr(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    while (offset < len) {
        uint8_t chunkLen = (uint8_t)(len - offset);
        uint16_t currentAddr = (uint16_t)(escAddr + offset);
        uint32_t csrData = 0U;
        uint32_t csrCmd;

        if (chunkLen > LAN9253_ESC_CSR_MAX_LEN) {
            chunkLen = LAN9253_ESC_CSR_MAX_LEN;
        }

        if (LAN9253_CsrWaitBusyClearIsr(this) == 0U) {
            return;
        }

        for (uint8_t i = 0U; i < chunkLen; ++i) {
            csrData |= ((uint32_t)buf[offset + i] << (8U * i));
        }

        if (LAN9253_WriteDWordIsr(this, LAN9253_ESC_CSR_DATA_REG, csrData) == 0U) {
            return;
        }

        csrCmd = ((uint32_t)(currentAddr & 0x00FFU)) |
                 ((uint32_t)((currentAddr >> 8) & 0x00FFU) << 8) |
                 ((uint32_t)chunkLen << 16) |
                 ((uint32_t)LAN9253_ESC_CSR_WRITE_CMD << 24);

        if (LAN9253_WriteDWordIsr(this, LAN9253_ESC_CSR_CMD_REG, csrCmd) == 0U) {
            return;
        }

        if (LAN9253_CsrWaitBusyClearIsr(this) == 0U) {
            return;
        }

        offset = (uint16_t)(offset + chunkLen);
    }
}

/* ============================== 第三阶段：PRAM / DPRAM 过程数据访问 ============================== */

/**
 * @brief 等待 PRAM 读事务结束。
 *
 * PRAM_READ_CMD_REG 的 BUSY 位为 1 时，表示 LAN9253 正在把 ESC DPRAM
 * 中的数据搬到 PRAM Read FIFO。只有 BUSY 清零后，主机才可以安全配置
 * 下一次 PRAM 读地址和长度。
 *
 * @param this LAN9253 设备对象。
 * @param timeoutMs 等待超时时间，单位 ms。
 * @return 1：BUSY 已清零；0：等待超时。
 */
static uint8_t LAN9253_PramReadBusyWait(LAN9253Device_t *this, uint32_t timeoutMs)
{
    uint32_t startTick = HAL_GetTick();

    do {
        uint32_t pramCmd = this->pOps->ReadDWord(this, LAN9253_PRAM_READ_CMD_REG);

        if ((pramCmd & LAN9253_PRAM_BUSY_MASK) == 0U) {
            return 1U;
        }
    } while ((HAL_GetTick() - startTick) < timeoutMs);

    return 0U;
}

/**
 * @brief 等待 PRAM 写事务结束。
 *
 * PRAM_WRITE_CMD_REG 的 BUSY 位为 1 时，表示 LAN9253 正在把 Write FIFO
 * 中的数据写入 ESC DPRAM。结束写事务或开始新事务前，都应该确认 BUSY 已清零。
 *
 * @param this LAN9253 设备对象。
 * @param timeoutMs 等待超时时间，单位 ms。
 * @return 1：BUSY 已清零；0：等待超时。
 */
static uint8_t LAN9253_PramWriteBusyWait(LAN9253Device_t *this, uint32_t timeoutMs)
{
    uint32_t startTick = HAL_GetTick();

    do {
        uint32_t pramCmd = this->pOps->ReadDWord(this, LAN9253_PRAM_WRITE_CMD_REG);

        if ((pramCmd & LAN9253_PRAM_BUSY_MASK) == 0U) {
            return 1U;
        }
    } while ((HAL_GetTick() - startTick) < timeoutMs);

    return 0U;
}

/**
 * @brief 等待 PRAM Read FIFO 中出现可读数据。
 *
 * 启动 PRAM 读事务后，LAN9253 会先从 ESC DPRAM 取数据并填入 Read FIFO。
 * AVAIL 置位后才可以读取 FIFO；AVAIL_COUNT 表示当前 FIFO 中可连续读取的 DWORD 数。
 *
 * @param this LAN9253 设备对象。
 * @param timeoutMs 等待超时时间，单位 ms。
 * @param pDwordCount 非 NULL 时返回当前可读 DWORD 数。
 * @return 1：FIFO 可读；0：等待超时。
 */
static uint8_t LAN9253_PramReadAvailWait(LAN9253Device_t *this, uint32_t timeoutMs, uint8_t *pDwordCount)
{
    uint32_t startTick = HAL_GetTick();

    do {
        uint32_t pramCmd = this->pOps->ReadDWord(this, LAN9253_PRAM_READ_CMD_REG);

        if ((pramCmd & LAN9253_PRAM_AVAIL_MASK) != 0U) {
            if (pDwordCount != NULL) {
                *pDwordCount = (uint8_t)((pramCmd & LAN9253_PRAM_AVAIL_COUNT_MASK) >>
                                         LAN9253_PRAM_AVAIL_COUNT_SHIFT);
            }
            return 1U;
        }
    } while ((HAL_GetTick() - startTick) < timeoutMs);

    return 0U;
}

/**
 * @brief 等待 PRAM Write FIFO 出现可写空间。
 *
 * 启动 PRAM 写事务后，主机需要等 Write FIFO 可写，再把数据写入 FIFO。
 * AVAIL_COUNT 表示当前 FIFO 可连续写入的 DWORD 数。
 *
 * @param this LAN9253 设备对象。
 * @param timeoutMs 等待超时时间，单位 ms。
 * @param pDwordCount 非 NULL 时返回当前可写 DWORD 数。
 * @return 1：FIFO 可写；0：等待超时。
 */
static uint8_t LAN9253_PramWriteAvailWait(LAN9253Device_t *this, uint32_t timeoutMs, uint8_t *pDwordCount)
{
    uint32_t startTick = HAL_GetTick();

    do {
        uint32_t pramCmd = this->pOps->ReadDWord(this, LAN9253_PRAM_WRITE_CMD_REG);

        if ((pramCmd & LAN9253_PRAM_AVAIL_MASK) != 0U) {
            if (pDwordCount != NULL) {
                *pDwordCount = (uint8_t)((pramCmd & LAN9253_PRAM_AVAIL_COUNT_MASK) >>
                                         LAN9253_PRAM_AVAIL_COUNT_SHIFT);
            }
            return 1U;
        }
    } while ((HAL_GetTick() - startTick) < timeoutMs);

    return 0U;
}

/**
 * @brief 在一个 CS 低电平窗口内连续读取 FIFO 数据。
 *
 * 这个函数用于 PRAM Read FIFO burst 优化。相比每 4 字节就重新发送一次
 * FAST_READ 命令，burst 模式只发送一次命令和 FIFO 地址，随后连续接收多个字节。
 *
 * 当前 FAST_READ 帧格式沿用已经验证通过的直接寄存器读格式：
 *   0x0B + addr_hi + addr_lo + dummy(0x01) + extra_dummy(0x00) + data...
 *
 * @param this LAN9253 设备对象。
 * @param fifoAddr FIFO 寄存器地址，读 PRAM 时为 0x0004。
 * @param buf 接收缓冲区。
 * @param len 要连续读取的字节数。
 * @return 1：SPI 传输成功；0：SPI 传输失败。
 */
static uint8_t LAN9253_ReadFifoBurst(LAN9253Device_t *this, uint16_t fifoAddr, uint8_t *buf, uint16_t len)
{
    uint8_t cmdBuf[5] = {
        LAN9253_SPI_CMD_FAST_READ,
        (uint8_t)(fifoAddr >> 8),
        (uint8_t)(fifoAddr & 0xFFU),
        LAN9253_SPI_CMD_FAST_READ_DUMMY,
        0x00U,
    };
    HAL_StatusTypeDef status;

    if ((buf == NULL) || (len == 0U)) {
        return 0U;
    }

    this->pSpi->pOps->SPI_CS_Select(this->pSpi);
    status = this->pSpi->pOps->SPI_Transmit(this->pSpi, cmdBuf, sizeof(cmdBuf), 100U);
    if (status == HAL_OK) {
        status = this->pSpi->pOps->SPI_Receive(this->pSpi, buf, len, 100U);
    }
    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);

    return (status == HAL_OK) ? 1U : 0U;
}

/**
 * @brief 在一个 CS 低电平窗口内连续写入 FIFO 数据。
 *
 * 这个函数用于 PRAM Write FIFO burst 优化。只发送一次 SERIAL_WRITE 命令和
 * FIFO 地址，然后连续发送多个字节，减少 SPI 命令开销。
 *
 * @param this LAN9253 设备对象。
 * @param fifoAddr FIFO 寄存器地址，写 PRAM 时为 0x0020。
 * @param buf 待发送缓冲区。
 * @param len 要连续写入的字节数。
 * @return 1：SPI 传输成功；0：SPI 传输失败。
 */
static uint8_t LAN9253_WriteFifoBurst(LAN9253Device_t *this, uint16_t fifoAddr, uint8_t *buf, uint16_t len)
{
    uint8_t cmdBuf[3] = {
        LAN9253_SPI_CMD_SERIAL_WRITE,
        (uint8_t)(fifoAddr >> 8),
        (uint8_t)(fifoAddr & 0xFFU),
    };
    HAL_StatusTypeDef status;

    if ((buf == NULL) || (len == 0U)) {
        return 0U;
    }

    this->pSpi->pOps->SPI_CS_Select(this->pSpi);
    status = this->pSpi->pOps->SPI_Transmit(this->pSpi, cmdBuf, sizeof(cmdBuf), 100U);
    if (status == HAL_OK) {
        status = this->pSpi->pOps->SPI_Transmit(this->pSpi, buf, len, 100U);
    }
    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);

    return (status == HAL_OK) ? 1U : 0U;
}

/**
 * @brief 通过 PRAM FIFO 从 ESC DPRAM 读取任意长度数据。
 *
 * PRAM 读流程：
 *   1. 写 ABORT，终止可能残留的上一次 PRAM 读事务。
 *   2. 等待 PRAM_READ_BUSY 清零。
 *   3. 向 PRAM_READ_ADDR_LEN_REG 写入 ESC 起始地址和读取长度。
 *   4. 向 PRAM_READ_CMD_REG 写 BUSY，启动硬件读事务。
 *   5. 等待 Read FIFO 可读，根据 AVAIL_COUNT 做连续 FIFO burst 读取。
 *
 * 非 4 字节对齐地址处理：
 *   PRAM FIFO 以 DWORD 为搬运单位。如果 escAddr 不是 4 字节对齐，
 *   第一组 FIFO 数据前面会有无效偏移字节。bytePos 用来跳过这些字节，
 *   只把用户真正请求的数据复制到 buf。
 *
 * @param this LAN9253 设备对象。
 * @param escAddr ESC DPRAM 地址，通常 >= 0x1000。
 * @param buf 接收缓冲区。
 * @param len 读取字节数。
 */
static void LAN9253_PramRead(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    /* 先终止可能残留的 PRAM 读命令，保证本次 ADDR/LEN 可以被安全写入。 */
    this->pOps->WriteDWord(this, LAN9253_PRAM_READ_CMD_REG, LAN9253_PRAM_ABORT_MASK);

    if (this->pOps->PramReadBusyWait(this, LAN9253_PRAM_TIMEOUT_MS) == 0U) {
        return;
    }

    this->pOps->WriteDWord(this,
                           LAN9253_PRAM_READ_ADDR_LEN_REG,
                           ((uint32_t)len << 16) | (uint32_t)escAddr);

    this->pOps->WriteDWord(this, LAN9253_PRAM_READ_CMD_REG, LAN9253_PRAM_BUSY_MASK);

    while (offset < len) {
        uint8_t fifoBytes[LAN9253_PRAM_BURST_MAX_BYTES];
        uint8_t availableDwords = 0U;
        uint16_t remainLen = (uint16_t)(len - offset);
        uint8_t bytePos = (offset == 0U) ? (uint8_t)(escAddr & 0x03U) : 0U;
        uint16_t fifoBytesNeeded = (uint16_t)(remainLen + bytePos);
        uint8_t dwordCount;
        uint16_t burstBytes;
        uint16_t copyLen;

        dwordCount = (uint8_t)((fifoBytesNeeded + LAN9253_PRAM_FIFO_DWORD_BYTES - 1U) /
                               LAN9253_PRAM_FIFO_DWORD_BYTES);

        if (dwordCount > LAN9253_PRAM_BURST_MAX_DWORDS) {
            dwordCount = LAN9253_PRAM_BURST_MAX_DWORDS;
        }

        if (LAN9253_PramReadAvailWait(this, LAN9253_PRAM_TIMEOUT_MS, &availableDwords) == 0U) {
            return;
        }

        if ((availableDwords != 0U) && (dwordCount > availableDwords)) {
            dwordCount = availableDwords;
        }

        burstBytes = (uint16_t)dwordCount * LAN9253_PRAM_FIFO_DWORD_BYTES;
        copyLen = (uint16_t)(burstBytes - bytePos);
        if (copyLen > remainLen) {
            copyLen = remainLen;
        }

        if (LAN9253_ReadFifoBurst(this, LAN9253_PRAM_READ_FIFO_REG, fifoBytes, burstBytes) == 0U) {
            return;
        }

        for (uint16_t i = 0U; i < copyLen; ++i) {
            buf[offset + i] = fifoBytes[bytePos + i];
        }

        offset = (uint16_t)(offset + copyLen);
    }
}

/**
 * @brief 通过 PRAM FIFO 向 ESC DPRAM 写入任意长度数据。
 *
 * PRAM 写流程：
 *   1. 写 ABORT，终止可能残留的上一次 PRAM 写事务。
 *   2. 等待 PRAM_WRITE_BUSY 清零。
 *   3. 向 PRAM_WRITE_ADDR_LEN_REG 写入 ESC 起始地址和写入长度。
 *   4. 向 PRAM_WRITE_CMD_REG 写 BUSY，启动硬件写事务。
 *   5. 等待 Write FIFO 可写，根据 AVAIL_COUNT 做连续 FIFO burst 写入。
 *
 * 非 4 字节对齐地址处理：
 *   写 FIFO 也以 DWORD 为搬运单位。如果 escAddr 不是 4 字节对齐，
 *   第一包写入时前面 bytePos 字节填 0，只在正确偏移位置放入用户数据。
 *
 * @param this LAN9253 设备对象。
 * @param escAddr ESC DPRAM 地址，通常 >= 0x1000。
 * @param buf 待写入数据缓冲区。
 * @param len 写入字节数。
 */
static void LAN9253_PramWrite(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    /* 先终止可能残留的 PRAM 写命令，保证本次 ADDR/LEN 可以被安全写入。 */
    this->pOps->WriteDWord(this, LAN9253_PRAM_WRITE_CMD_REG, LAN9253_PRAM_ABORT_MASK);

    if (this->pOps->PramWriteBusyWait(this, LAN9253_PRAM_TIMEOUT_MS) == 0U) {
        return;
    }

    this->pOps->WriteDWord(this,
                           LAN9253_PRAM_WRITE_ADDR_LEN_REG,
                           ((uint32_t)len << 16) | (uint32_t)escAddr);

    this->pOps->WriteDWord(this, LAN9253_PRAM_WRITE_CMD_REG, LAN9253_PRAM_BUSY_MASK);

    while (offset < len) {
        uint8_t fifoBytes[LAN9253_PRAM_BURST_MAX_BYTES] = {0};
        uint8_t availableDwords = 0U;
        uint16_t remainLen = (uint16_t)(len - offset);
        uint8_t bytePos = (offset == 0U) ? (uint8_t)(escAddr & 0x03U) : 0U;
        uint16_t fifoBytesNeeded = (uint16_t)(remainLen + bytePos);
        uint8_t dwordCount;
        uint16_t burstBytes;
        uint16_t copyLen;

        dwordCount = (uint8_t)((fifoBytesNeeded + LAN9253_PRAM_FIFO_DWORD_BYTES - 1U) /
                               LAN9253_PRAM_FIFO_DWORD_BYTES);

        if (dwordCount > LAN9253_PRAM_BURST_MAX_DWORDS) {
            dwordCount = LAN9253_PRAM_BURST_MAX_DWORDS;
        }

        if (LAN9253_PramWriteAvailWait(this, LAN9253_PRAM_TIMEOUT_MS, &availableDwords) == 0U) {
            return;
        }

        if ((availableDwords != 0U) && (dwordCount > availableDwords)) {
            dwordCount = availableDwords;
        }

        burstBytes = (uint16_t)dwordCount * LAN9253_PRAM_FIFO_DWORD_BYTES;
        copyLen = (uint16_t)(burstBytes - bytePos);
        if (copyLen > remainLen) {
            copyLen = remainLen;
        }

        for (uint16_t i = 0U; i < copyLen; ++i) {
            fifoBytes[bytePos + i] = buf[offset + i];
        }

        if (LAN9253_WriteFifoBurst(this, LAN9253_PRAM_WRITE_FIFO_REG, fifoBytes, burstBytes) == 0U) {
            return;
        }

        offset = (uint16_t)(offset + copyLen);
    }

    (void)this->pOps->PramWriteBusyWait(this, LAN9253_PRAM_TIMEOUT_MS);
}

/* ---------- 第三阶段 ISR 辅助：PRAM FIFO 访问 DPRAM/过程数据区 ---------- */

/**
 * @brief ISR 上下文等待 PRAM 读事务 BUSY 清零。
 *
 * 使用有限次数轮询替代毫秒级阻塞等待；返回 0 表示本次 ISR 访问失败，
 * 上层协议栈后续会再次进入中断处理。
 */
static uint8_t LAN9253_PramReadBusyWaitIsr(LAN9253Device_t *this)
{
    for (uint32_t loops = LAN9253_ISR_WAIT_LOOPS; loops > 0U; --loops) {
        uint32_t pramCmd;

        if (LAN9253_ReadDWordIsr(this, LAN9253_PRAM_READ_CMD_REG, &pramCmd) == 0U) {
            return 0U;
        }

        if ((pramCmd & LAN9253_PRAM_BUSY_MASK) == 0U) {
            return 1U;
        }
    }

    return 0U;
}

/**
 * @brief ISR 上下文等待 PRAM 写事务 BUSY 清零。
 */
static uint8_t LAN9253_PramWriteBusyWaitIsr(LAN9253Device_t *this)
{
    for (uint32_t loops = LAN9253_ISR_WAIT_LOOPS; loops > 0U; --loops) {
        uint32_t pramCmd;

        if (LAN9253_ReadDWordIsr(this, LAN9253_PRAM_WRITE_CMD_REG, &pramCmd) == 0U) {
            return 0U;
        }

        if ((pramCmd & LAN9253_PRAM_BUSY_MASK) == 0U) {
            return 1U;
        }
    }

    return 0U;
}

/**
 * @brief ISR 上下文等待 PRAM Read FIFO 可读。
 * @param pDwordCount 非 NULL 时返回当前 FIFO 可连续读取的 DWORD 数。
 */
static uint8_t LAN9253_PramReadAvailWaitIsr(LAN9253Device_t *this, uint8_t *pDwordCount)
{
    for (uint32_t loops = LAN9253_ISR_WAIT_LOOPS; loops > 0U; --loops) {
        uint32_t pramCmd;

        if (LAN9253_ReadDWordIsr(this, LAN9253_PRAM_READ_CMD_REG, &pramCmd) == 0U) {
            return 0U;
        }

        if ((pramCmd & LAN9253_PRAM_AVAIL_MASK) != 0U) {
            if (pDwordCount != NULL) {
                *pDwordCount = (uint8_t)((pramCmd & LAN9253_PRAM_AVAIL_COUNT_MASK) >>
                                         LAN9253_PRAM_AVAIL_COUNT_SHIFT);
            }
            return 1U;
        }
    }

    return 0U;
}

/**
 * @brief ISR 上下文等待 PRAM Write FIFO 可写。
 * @param pDwordCount 非 NULL 时返回当前 FIFO 可连续写入的 DWORD 数。
 */
static uint8_t LAN9253_PramWriteAvailWaitIsr(LAN9253Device_t *this, uint8_t *pDwordCount)
{
    for (uint32_t loops = LAN9253_ISR_WAIT_LOOPS; loops > 0U; --loops) {
        uint32_t pramCmd;

        if (LAN9253_ReadDWordIsr(this, LAN9253_PRAM_WRITE_CMD_REG, &pramCmd) == 0U) {
            return 0U;
        }

        if ((pramCmd & LAN9253_PRAM_AVAIL_MASK) != 0U) {
            if (pDwordCount != NULL) {
                *pDwordCount = (uint8_t)((pramCmd & LAN9253_PRAM_AVAIL_COUNT_MASK) >>
                                         LAN9253_PRAM_AVAIL_COUNT_SHIFT);
            }
            return 1U;
        }
    }

    return 0U;
}

/**
 * @brief ISR 上下文在一个 CS 低电平窗口内连续读取 PRAM FIFO。
 *
 * 和普通 LAN9253_ReadFifoBurst() 的 SPI 帧格式一致，但所有字节都通过
 * LAN9253_TransferByteIsr() 搬运，保证不会调用 HAL 阻塞收发。
 */
static uint8_t LAN9253_ReadFifoBurstIsr(LAN9253Device_t *this, uint16_t fifoAddr, uint8_t *buf, uint16_t len)
{
    uint8_t dummy;

    if ((buf == NULL) || (len == 0U)) {
        return 0U;
    }

    this->pSpi->pOps->SPI_CS_Select(this->pSpi);

    if (LAN9253_TransferByteIsr(this, LAN9253_SPI_CMD_FAST_READ, &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, (uint8_t)(fifoAddr >> 8), &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, (uint8_t)(fifoAddr & 0xFFU), &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, LAN9253_SPI_CMD_FAST_READ_DUMMY, &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, 0x00U, &dummy) == 0U) {
        goto fail;
    }

    for (uint16_t i = 0U; i < len; ++i) {
        if (LAN9253_TransferByteIsr(this, 0x00U, &buf[i]) == 0U) {
            goto fail;
        }
    }

    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);
    return 1U;

fail:
    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);
    return 0U;
}

/**
 * @brief ISR 上下文在一个 CS 低电平窗口内连续写入 PRAM FIFO。
 */
static uint8_t LAN9253_WriteFifoBurstIsr(LAN9253Device_t *this, uint16_t fifoAddr, const uint8_t *buf, uint16_t len)
{
    uint8_t dummy;

    if ((buf == NULL) || (len == 0U)) {
        return 0U;
    }

    this->pSpi->pOps->SPI_CS_Select(this->pSpi);

    if (LAN9253_TransferByteIsr(this, LAN9253_SPI_CMD_SERIAL_WRITE, &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, (uint8_t)(fifoAddr >> 8), &dummy) == 0U) {
        goto fail;
    }
    if (LAN9253_TransferByteIsr(this, (uint8_t)(fifoAddr & 0xFFU), &dummy) == 0U) {
        goto fail;
    }

    for (uint16_t i = 0U; i < len; ++i) {
        if (LAN9253_TransferByteIsr(this, buf[i], &dummy) == 0U) {
            goto fail;
        }
    }

    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);
    return 1U;

fail:
    this->pSpi->pOps->SPI_CS_Deselect(this->pSpi);
    return 0U;
}

/**
 * @brief ISR 上下文通过 PRAM FIFO 从 ESC DPRAM 读取数据。
 *
 * 这是 AL_EVENT_ENABLED 后，协议栈在中断里读取过程数据区时会走到的路径。
 * 关键点是整条调用链都使用 *Isr 版本，避免进入 HAL 阻塞 SPI 接口：
 *   1. LAN9253_WriteDWordIsr() 配置 PRAM 命令和地址长度。
 *   2. LAN9253_PramReadBusyWaitIsr() 等待硬件读事务空闲。
 *   3. LAN9253_PramReadAvailWaitIsr() 等待 Read FIFO 可读。
 *   4. LAN9253_ReadFifoBurstIsr() 连续读取 FIFO 数据。
 *
 * PRAM FIFO 以 DWORD 为搬运单位，所以非 4 字节对齐地址需要用 bytePos
 * 跳过首个 DWORD 前面的无效字节。
 */
static void LAN9253_PramReadIsr(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    if (LAN9253_WriteDWordIsr(this, LAN9253_PRAM_READ_CMD_REG, LAN9253_PRAM_ABORT_MASK) == 0U) {
        return;
    }

    if (LAN9253_PramReadBusyWaitIsr(this) == 0U) {
        return;
    }

    if (LAN9253_WriteDWordIsr(this,
                              LAN9253_PRAM_READ_ADDR_LEN_REG,
                              ((uint32_t)len << 16) | (uint32_t)escAddr) == 0U) {
        return;
    }

    if (LAN9253_WriteDWordIsr(this, LAN9253_PRAM_READ_CMD_REG, LAN9253_PRAM_BUSY_MASK) == 0U) {
        return;
    }

    while (offset < len) {
        uint8_t fifoBytes[LAN9253_PRAM_BURST_MAX_BYTES];
        uint8_t availableDwords = 0U;
        uint16_t remainLen = (uint16_t)(len - offset);
        uint8_t bytePos = (offset == 0U) ? (uint8_t)(escAddr & 0x03U) : 0U;
        uint16_t fifoBytesNeeded = (uint16_t)(remainLen + bytePos);
        uint8_t dwordCount;
        uint16_t burstBytes;
        uint16_t copyLen;

        dwordCount = (uint8_t)((fifoBytesNeeded + LAN9253_PRAM_FIFO_DWORD_BYTES - 1U) /
                               LAN9253_PRAM_FIFO_DWORD_BYTES);

        if (dwordCount > LAN9253_PRAM_BURST_MAX_DWORDS) {
            dwordCount = LAN9253_PRAM_BURST_MAX_DWORDS;
        }

        if (LAN9253_PramReadAvailWaitIsr(this, &availableDwords) == 0U) {
            return;
        }

        if ((availableDwords != 0U) && (dwordCount > availableDwords)) {
            dwordCount = availableDwords;
        }

        burstBytes = (uint16_t)dwordCount * LAN9253_PRAM_FIFO_DWORD_BYTES;
        copyLen = (uint16_t)(burstBytes - bytePos);
        if (copyLen > remainLen) {
            copyLen = remainLen;
        }

        if (LAN9253_ReadFifoBurstIsr(this, LAN9253_PRAM_READ_FIFO_REG, fifoBytes, burstBytes) == 0U) {
            return;
        }

        for (uint16_t i = 0U; i < copyLen; ++i) {
            buf[offset + i] = fifoBytes[bytePos + i];
        }

        offset = (uint16_t)(offset + copyLen);
    }
}

/**
 * @brief ISR 上下文通过 PRAM FIFO 向 ESC DPRAM 写入数据。
 *
 * 写流程和普通 LAN9253_PramWrite() 一致，但寄存器访问、等待和 FIFO burst
 * 都使用 ISR 版本。最后再等待一次 PRAM_WRITE_BUSY 清零，确认 FIFO 数据已经
 * 被 LAN9253 搬进 ESC DPRAM。
 */
static void LAN9253_PramWriteIsr(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    if (LAN9253_WriteDWordIsr(this, LAN9253_PRAM_WRITE_CMD_REG, LAN9253_PRAM_ABORT_MASK) == 0U) {
        return;
    }

    if (LAN9253_PramWriteBusyWaitIsr(this) == 0U) {
        return;
    }

    if (LAN9253_WriteDWordIsr(this,
                              LAN9253_PRAM_WRITE_ADDR_LEN_REG,
                              ((uint32_t)len << 16) | (uint32_t)escAddr) == 0U) {
        return;
    }

    if (LAN9253_WriteDWordIsr(this, LAN9253_PRAM_WRITE_CMD_REG, LAN9253_PRAM_BUSY_MASK) == 0U) {
        return;
    }

    while (offset < len) {
        uint8_t fifoBytes[LAN9253_PRAM_BURST_MAX_BYTES] = {0};
        uint8_t availableDwords = 0U;
        uint16_t remainLen = (uint16_t)(len - offset);
        uint8_t bytePos = (offset == 0U) ? (uint8_t)(escAddr & 0x03U) : 0U;
        uint16_t fifoBytesNeeded = (uint16_t)(remainLen + bytePos);
        uint8_t dwordCount;
        uint16_t burstBytes;
        uint16_t copyLen;

        dwordCount = (uint8_t)((fifoBytesNeeded + LAN9253_PRAM_FIFO_DWORD_BYTES - 1U) /
                               LAN9253_PRAM_FIFO_DWORD_BYTES);

        if (dwordCount > LAN9253_PRAM_BURST_MAX_DWORDS) {
            dwordCount = LAN9253_PRAM_BURST_MAX_DWORDS;
        }

        if (LAN9253_PramWriteAvailWaitIsr(this, &availableDwords) == 0U) {
            return;
        }

        if ((availableDwords != 0U) && (dwordCount > availableDwords)) {
            dwordCount = availableDwords;
        }

        burstBytes = (uint16_t)dwordCount * LAN9253_PRAM_FIFO_DWORD_BYTES;
        copyLen = (uint16_t)(burstBytes - bytePos);
        if (copyLen > remainLen) {
            copyLen = remainLen;
        }

        for (uint16_t i = 0U; i < copyLen; ++i) {
            fifoBytes[bytePos + i] = buf[offset + i];
        }

        if (LAN9253_WriteFifoBurstIsr(this, LAN9253_PRAM_WRITE_FIFO_REG, fifoBytes, burstBytes) == 0U) {
            return;
        }

        offset = (uint16_t)(offset + copyLen);
    }

    (void)LAN9253_PramWriteBusyWaitIsr(this);
}

/* ============================== 第四阶段：ESC 协议栈适配接口 ============================== */

/**
 * @brief 协议栈统一 ESC 读接口。
 *
 * 协议栈传入的是 ESC 地址空间地址，而不是 LAN9253 主机侧寄存器地址。
 * 本函数按地址自动分流：
 *   - 0x0000..0x0FFF：ESC 控制/状态寄存器，走 CSR 间接访问。
 *   - 0x1000 及以上：ESC DPRAM/过程数据区，走 PRAM FIFO。
 *
 * 如果一次读取从低地址跨过 0x1000，本函数会先读 CSR 段，再读 PRAM 段。
 *
 * @param this LAN9253 设备对象。
 * @param buf 接收缓冲区。
 * @param escAddr ESC 地址空间起始地址。
 * @param len 读取字节数。
 */
static void LAN9253_EscRead(LAN9253Device_t *this, uint8_t *buf, uint16_t escAddr, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    while (offset < len) {
        uint16_t currentAddr = (uint16_t)(escAddr + offset);
        uint16_t remainLen = (uint16_t)(len - offset);

        if (currentAddr < LAN9253_ESC_DPRAM_START_ADDR) {
            uint16_t csrLen = remainLen;

            if ((uint32_t)currentAddr + csrLen > LAN9253_ESC_DPRAM_START_ADDR) {
                csrLen = (uint16_t)(LAN9253_ESC_DPRAM_START_ADDR - currentAddr);
            }

            this->pOps->CsrReadBuffer(this, currentAddr, &buf[offset], csrLen);
            offset = (uint16_t)(offset + csrLen);
        } else {
            this->pOps->PramRead(this, currentAddr, &buf[offset], remainLen);
            offset = (uint16_t)(offset + remainLen);
        }
    }
}

/**
 * @brief 协议栈统一 ESC 写接口。
 *
 * 分流规则与 LAN9253_EscRead() 相同：
 *   - escAddr < 0x1000 走 CSR。
 *   - escAddr >= 0x1000 走 PRAM。
 *
 * 注意：低地址 ESC 控制寄存器写入会影响 EtherCAT 状态机，调用者应确认写入地址
 * 和数据含义正确。
 *
 * @param this LAN9253 设备对象。
 * @param buf 待写入数据缓冲区。
 * @param escAddr ESC 地址空间起始地址。
 * @param len 写入字节数。
 */
static void LAN9253_EscWrite(LAN9253Device_t *this, const uint8_t *buf, uint16_t escAddr, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    while (offset < len) {
        uint16_t currentAddr = (uint16_t)(escAddr + offset);
        uint16_t remainLen = (uint16_t)(len - offset);

        if (currentAddr < LAN9253_ESC_DPRAM_START_ADDR) {
            uint16_t csrLen = remainLen;

            if ((uint32_t)currentAddr + csrLen > LAN9253_ESC_DPRAM_START_ADDR) {
                csrLen = (uint16_t)(LAN9253_ESC_DPRAM_START_ADDR - currentAddr);
            }

            this->pOps->CsrWriteBuffer(this, currentAddr, &buf[offset], csrLen);
            offset = (uint16_t)(offset + csrLen);
        } else {
            this->pOps->PramWrite(this, currentAddr, &buf[offset], remainLen);
            offset = (uint16_t)(offset + remainLen);
        }
    }
}

/**
 * @brief ISR 上下文统一 ESC 读接口。
 *
 * 分流规则与 LAN9253_EscRead() 相同。区别是低地址走 CSR ISR 版本，
 * DPRAM/过程数据区走 PRAM ISR 版本，避免在 AL_EVENT 中断里调用 HAL 阻塞接口。
 */
static void LAN9253_EscReadIsr(LAN9253Device_t *this, uint8_t *buf, uint16_t escAddr, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    while (offset < len) {
        uint16_t currentAddr = (uint16_t)(escAddr + offset);
        uint16_t remainLen = (uint16_t)(len - offset);

        if (currentAddr < LAN9253_ESC_DPRAM_START_ADDR) {
            uint16_t csrLen = remainLen;

            if ((uint32_t)currentAddr + csrLen > LAN9253_ESC_DPRAM_START_ADDR) {
                csrLen = (uint16_t)(LAN9253_ESC_DPRAM_START_ADDR - currentAddr);
            }

            LAN9253_CsrReadBufferIsr(this, currentAddr, &buf[offset], csrLen);
            offset = (uint16_t)(offset + csrLen);
        } else {
            LAN9253_PramReadIsr(this, currentAddr, &buf[offset], remainLen);
            offset = (uint16_t)(offset + remainLen);
        }
    }
}

/**
 * @brief ISR 上下文统一 ESC 写接口。
 *
 * 协议栈在中断中确认或更新 ESC 数据时会调用这里；所有底层访问都必须保持
 * 有限等待，不能使用毫秒级 HAL 阻塞等待。
 */
static void LAN9253_EscWriteIsr(LAN9253Device_t *this, const uint8_t *buf, uint16_t escAddr, uint16_t len)
{
    uint16_t offset = 0U;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    while (offset < len) {
        uint16_t currentAddr = (uint16_t)(escAddr + offset);
        uint16_t remainLen = (uint16_t)(len - offset);

        if (currentAddr < LAN9253_ESC_DPRAM_START_ADDR) {
            uint16_t csrLen = remainLen;

            if ((uint32_t)currentAddr + csrLen > LAN9253_ESC_DPRAM_START_ADDR) {
                csrLen = (uint16_t)(LAN9253_ESC_DPRAM_START_ADDR - currentAddr);
            }

            LAN9253_CsrWriteBufferIsr(this, currentAddr, &buf[offset], csrLen);
            offset = (uint16_t)(offset + csrLen);
        } else {
            LAN9253_PramWriteIsr(this, currentAddr, &buf[offset], remainLen);
            offset = (uint16_t)(offset + remainLen);
        }
    }
}

/* ============================== 对外 API：第一阶段 ============================== */

void BSP_LAN9253_Init(void)
{
    g_LAN9253Device.pOps->Init(&g_LAN9253Device);
}

void BSP_LAN9253_Reset(void)
{
    g_LAN9253Device.pOps->Reset(&g_LAN9253Device);
}

uint8_t BSP_LAN9253_SelfTest(void)
{
    return g_LAN9253Device.pOps->SelfTest(&g_LAN9253Device);
}

uint32_t BSP_LAN9253_ReadDWord(uint16_t addr)
{
    return g_LAN9253Device.pOps->ReadDWord(&g_LAN9253Device, addr);
}

void BSP_LAN9253_WriteDWord(uint16_t addr, uint32_t value)
{
    g_LAN9253Device.pOps->WriteDWord(&g_LAN9253Device, addr, value);
}

void BSP_LAN9253_ClearInterruptStatus(void)
{
    g_LAN9253Device.pOps->WriteDWord(&g_LAN9253Device,
                                     LAN9253_REG_INT_STS,
                                     LAN9253_INT_ECAT_EVENT);
}

void BSP_LAN9253_ClearInterruptStatusIsr(void)
{
    (void)LAN9253_WriteDWordIsr(&g_LAN9253Device,
                                LAN9253_REG_INT_STS,
                                LAN9253_INT_ECAT_EVENT);
}

uint32_t BSP_LAN9253_ReadByteTest(void)
{
    return BSP_LAN9253_ReadDWord(LAN9253_REG_BYTE_TEST);
}

uint32_t BSP_LAN9253_ReadIDRev(void)
{
    return BSP_LAN9253_ReadDWord(LAN9253_REG_ID_REV);
}

uint16_t BSP_LAN9253_ReadChipID(void)
{
    return (uint16_t)((BSP_LAN9253_ReadIDRev() >> 16) & 0xFFFFU);
}

uint8_t BSP_LAN9253_Probe(uint32_t *byteTest, uint32_t *idRev, uint16_t *chipId)
{
    uint32_t localByteTest = BSP_LAN9253_ReadByteTest();
    uint32_t localIdRev = BSP_LAN9253_ReadIDRev();
    uint16_t localChipId = (uint16_t)((localIdRev >> 16) & 0xFFFFU);

    if (byteTest != NULL) {
        *byteTest = localByteTest;
    }

    if (idRev != NULL) {
        *idRev = localIdRev;
    }

    if (chipId != NULL) {
        *chipId = localChipId;
    }

    return ((localByteTest == LAN9253_BYTE_TEST_VALUE) &&
            (localChipId == LAN9253_CHIP_ID_VALUE)) ? 1U : 0U;
}

/* ============================== 对外 API：第二阶段 ============================== */

uint8_t BSP_LAN9253_CsrWaitBusyClear(uint32_t timeoutMs)
{
    return g_LAN9253Device.pOps->CsrWaitBusyClear(&g_LAN9253Device, timeoutMs);
}

uint8_t BSP_LAN9253_CsrRead8(uint16_t escAddr)
{
    uint8_t buf[1] = {0};

    BSP_LAN9253_CsrReadBuffer(escAddr, buf, sizeof(buf));
    return buf[0];
}

uint16_t BSP_LAN9253_CsrRead16(uint16_t escAddr)
{
    uint8_t buf[2] = {0};

    BSP_LAN9253_CsrReadBuffer(escAddr, buf, sizeof(buf));
    return ((uint16_t)buf[0]) |
           ((uint16_t)buf[1] << 8);
}

uint32_t BSP_LAN9253_CsrRead32(uint16_t escAddr)
{
    uint8_t buf[4] = {0};

    BSP_LAN9253_CsrReadBuffer(escAddr, buf, sizeof(buf));
    return ((uint32_t)buf[0]) |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

void BSP_LAN9253_CsrReadBuffer(uint16_t escAddr, uint8_t *buf, uint16_t len)
{
    g_LAN9253Device.pOps->CsrReadBuffer(&g_LAN9253Device, escAddr, buf, len);
}

void BSP_LAN9253_CsrWrite8(uint16_t escAddr, uint8_t value)
{
    BSP_LAN9253_CsrWriteBuffer(escAddr, &value, 1U);
}

void BSP_LAN9253_CsrWrite16(uint16_t escAddr, uint16_t value)
{
    uint8_t buf[2] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
    };

    BSP_LAN9253_CsrWriteBuffer(escAddr, buf, sizeof(buf));
}

void BSP_LAN9253_CsrWrite32(uint16_t escAddr, uint32_t value)
{
    uint8_t buf[4] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
        (uint8_t)((value >> 16) & 0xFFU),
        (uint8_t)((value >> 24) & 0xFFU),
    };

    BSP_LAN9253_CsrWriteBuffer(escAddr, buf, sizeof(buf));
}

void BSP_LAN9253_CsrWriteBuffer(uint16_t escAddr, const uint8_t *buf, uint16_t len)
{
    g_LAN9253Device.pOps->CsrWriteBuffer(&g_LAN9253Device, escAddr, buf, len);
}

/* ============================== 对外 API：第三阶段 ============================== */

uint8_t BSP_LAN9253_PramReadBusyWait(uint32_t timeoutMs)
{
    return g_LAN9253Device.pOps->PramReadBusyWait(&g_LAN9253Device, timeoutMs);
}

uint8_t BSP_LAN9253_PramWriteBusyWait(uint32_t timeoutMs)
{
    return g_LAN9253Device.pOps->PramWriteBusyWait(&g_LAN9253Device, timeoutMs);
}

void BSP_LAN9253_PramRead(uint16_t escAddr, uint8_t *buf, uint16_t len)
{
    g_LAN9253Device.pOps->PramRead(&g_LAN9253Device, escAddr, buf, len);
}

void BSP_LAN9253_PramWrite(uint16_t escAddr, const uint8_t *buf, uint16_t len)
{
    g_LAN9253Device.pOps->PramWrite(&g_LAN9253Device, escAddr, buf, len);
}

/* ============================== 对外 API：第四阶段 ============================== */

void BSP_LAN9253_EscRead(uint8_t *buf, uint16_t escAddr, uint16_t len)
{
    g_LAN9253Device.pOps->EscRead(&g_LAN9253Device, buf, escAddr, len);
}

void BSP_LAN9253_EscWrite(const uint8_t *buf, uint16_t escAddr, uint16_t len)
{
    g_LAN9253Device.pOps->EscWrite(&g_LAN9253Device, buf, escAddr, len);
}

void BSP_LAN9253_EscReadIsr(uint8_t *buf, uint16_t escAddr, uint16_t len)
{
    g_LAN9253Device.pOps->EscReadIsr(&g_LAN9253Device, buf, escAddr, len);
}

void BSP_LAN9253_EscWriteIsr(const uint8_t *buf, uint16_t escAddr, uint16_t len)
{
    g_LAN9253Device.pOps->EscWriteIsr(&g_LAN9253Device, buf, escAddr, len);
}

uint16_t BSP_LAN9253_ReadALEvent(void)
{
    return BSP_LAN9253_CsrRead16(LAN9253_ESC_ADDR_AL_EVENT);
}

uint16_t BSP_LAN9253_ReadALEventIsr(void)
{
    uint8_t buf[2] = {0};

    g_LAN9253Device.pOps->EscReadIsr(&g_LAN9253Device,
                                     buf,
                                     LAN9253_ESC_ADDR_AL_EVENT,
                                     sizeof(buf));

    return ((uint16_t)buf[0]) |
           ((uint16_t)buf[1] << 8);
}
