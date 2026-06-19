#ifndef __BSP_LAN9253_H__
#define __BSP_LAN9253_H__

#include "bsp_gpio.h"
#include "bsp_spi.h"

/*
 * LAN9253 BSP 驱动说明
 *
 * 当前工程使用 STM32F407 + LAN9253，LAN9253 工作在 SPI 兼容模式。
 * STM32 通过 SPI 访问 LAN9253 主机侧寄存器，再由 LAN9253 完成对
 * EtherCAT ESC 内核寄存器和 DPRAM 的访问。
 *
 * 驱动按调试和验证顺序划分为四层：
 *   1. 直接寄存器访问：验证 SPI、CS、RST 以及芯片 ID。
 *   2. CSR 间接访问：访问 ESC 低地址寄存器，主要覆盖 0x0000..0x0FFF。
 *   3. PRAM FIFO 访问：访问 DPRAM/过程数据区，主要覆盖 0x1000 及以上。
 *   4. SSC 适配接口：为 EtherCAT 协议栈提供统一 ESC 读写入口。
 */

typedef struct LAN9253Device LAN9253Device_t;

/* ============================== LAN9253 操作函数表 ============================== */

typedef struct {
    /*
     * 第一阶段：最小硬件验证
     *
     * 这些接口直接操作 LAN9253 主机侧寄存器，不访问 ESC 地址空间。
     * BYTE_TEST 和 ID_REV 属于这类寄存器，适合确认 SPI 物理链路是否正常。
     */
    void (*Init)(LAN9253Device_t *this);
    void (*Reset)(LAN9253Device_t *this);
    uint8_t (*SelfTest)(LAN9253Device_t *this);
    uint32_t (*ReadDWord)(LAN9253Device_t *this, uint16_t addr);
    void (*WriteDWord)(LAN9253Device_t *this, uint16_t addr, uint32_t value);

    /*
     * 第二阶段：CSR 间接访问 ESC 寄存器
     *
     * CSR 是 LAN9253 提供的 ESC 间接访问窗口。CSR DATA 只有 32 位，
     * 因此驱动内部会把任意长度访问拆成最多 4 字节的小块。
     */
    uint8_t (*CsrWaitBusyClear)(LAN9253Device_t *this, uint32_t timeoutMs);
    void (*CsrReadBuffer)(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len);
    void (*CsrWriteBuffer)(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len);

    /*
     * 第三阶段：PRAM / DPRAM 过程数据访问
     *
     * PRAM 是访问 EtherCAT DPRAM/过程数据区的 FIFO 通道，一般用于
     * ESC 地址 0x1000 及以上的 mailbox、input process data、output process data 等区域。
     */
    uint8_t (*PramReadBusyWait)(LAN9253Device_t *this, uint32_t timeoutMs);
    uint8_t (*PramWriteBusyWait)(LAN9253Device_t *this, uint32_t timeoutMs);
    void (*PramRead)(LAN9253Device_t *this, uint16_t escAddr, uint8_t *buf, uint16_t len);
    void (*PramWrite)(LAN9253Device_t *this, uint16_t escAddr, const uint8_t *buf, uint16_t len);

    /*
     * 第四阶段：SSC / EtherCAT 协议栈适配接口
     *
     * 协议栈通常只关心“读写 ESC 地址空间”，不关心底层走 CSR 还是 PRAM。
     * 这组接口会按地址自动分流：
     *   - escAddr <  0x1000：走 CSR。
     *   - escAddr >= 0x1000：走 PRAM。
     */
    void (*EscRead)(LAN9253Device_t *this, uint8_t *buf, uint16_t escAddr, uint16_t len);
    void (*EscWrite)(LAN9253Device_t *this, const uint8_t *buf, uint16_t escAddr, uint16_t len);
    void (*EscReadIsr)(LAN9253Device_t *this, uint8_t *buf, uint16_t escAddr, uint16_t len);
    void (*EscWriteIsr)(LAN9253Device_t *this, const uint8_t *buf, uint16_t escAddr, uint16_t len);
} LAN9253Ops_t;

/*
 * LAN9253 设备对象
 *
 * pSpi   ：绑定到底层 SPI 设备，目前是 SPI3。
 * pReset ：绑定到 LAN9253 RST 引脚。
 * pOps   ：绑定操作函数表，便于分层调用和后续替换底层实现。
 */
struct LAN9253Device {
    SPIDevice_t *pSpi;
    GpioDevice_t *pReset;
    const LAN9253Ops_t *pOps;
};

extern LAN9253Device_t g_LAN9253Device;

/* ============================== 对外 API：第一阶段 ============================== */

/*
 * 初始化 LAN9253 BSP：初始化 CS 空闲电平、复位芯片并执行一次基础自检。
 * 调用前必须已经完成 MX_GPIO_Init() 和 MX_SPI3_Init()。
 */
void BSP_LAN9253_Init(void);

/* 硬件复位 LAN9253：RST 拉低后释放，并等待芯片内部逻辑稳定。 */
void BSP_LAN9253_Reset(void);

/* 读取 BYTE_TEST 和 ID_REV，判断最小硬件链路是否正常；返回 1 表示通过。 */
uint8_t BSP_LAN9253_SelfTest(void);

/* 直接读取/写入 LAN9253 主机侧 32 位寄存器，不用于 ESC 地址空间。 */
uint32_t BSP_LAN9253_ReadDWord(uint16_t addr);
void BSP_LAN9253_WriteDWord(uint16_t addr, uint32_t value);

/* 常用第一阶段诊断接口：读取 BYTE_TEST、ID_REV，以及从 ID_REV 提取出的 Chip ID。 */
uint32_t BSP_LAN9253_ReadByteTest(void);
uint32_t BSP_LAN9253_ReadIDRev(void);
uint16_t BSP_LAN9253_ReadChipID(void);
void BSP_LAN9253_ClearInterruptStatus(void);
void BSP_LAN9253_ClearInterruptStatusIsr(void);

/*
 * 一次性探测 LAN9253。
 * byteTest/idRev/chipId 可传 NULL；非 NULL 时函数会回填实际读数。
 * 返回 1 表示 BYTE_TEST 和 Chip ID 均符合 LAN9253 预期。
 */
uint8_t BSP_LAN9253_Probe(uint32_t *byteTest, uint32_t *idRev, uint16_t *chipId);

/* ============================== 对外 API：第二阶段 ============================== */

/* 等待 CSR 命令寄存器 BUSY 位清零；返回 1 表示空闲，返回 0 表示超时。 */
uint8_t BSP_LAN9253_CsrWaitBusyClear(uint32_t timeoutMs);

/* 通过 CSR 读取 ESC 内核寄存器，适合 0x0000..0x0FFF 范围。 */
uint8_t BSP_LAN9253_CsrRead8(uint16_t escAddr);
uint16_t BSP_LAN9253_CsrRead16(uint16_t escAddr);
uint32_t BSP_LAN9253_CsrRead32(uint16_t escAddr);
void BSP_LAN9253_CsrReadBuffer(uint16_t escAddr, uint8_t *buf, uint16_t len);

/* 通过 CSR 写入 ESC 内核寄存器；多字节数据按小端字节序发送。 */
void BSP_LAN9253_CsrWrite8(uint16_t escAddr, uint8_t value);
void BSP_LAN9253_CsrWrite16(uint16_t escAddr, uint16_t value);
void BSP_LAN9253_CsrWrite32(uint16_t escAddr, uint32_t value);
void BSP_LAN9253_CsrWriteBuffer(uint16_t escAddr, const uint8_t *buf, uint16_t len);

/* ============================== 对外 API：第三阶段 ============================== */

/* 等待 PRAM 读/写命令 BUSY 位清零；返回 1 表示空闲，返回 0 表示超时。 */
uint8_t BSP_LAN9253_PramReadBusyWait(uint32_t timeoutMs);
uint8_t BSP_LAN9253_PramWriteBusyWait(uint32_t timeoutMs);

/*
 * 通过 PRAM FIFO 访问 DPRAM/过程数据区。
 * escAddr 通常 >= 0x1000。驱动支持非 4 字节对齐地址，并使用 FIFO burst 优化。
 */
void BSP_LAN9253_PramRead(uint16_t escAddr, uint8_t *buf, uint16_t len);
void BSP_LAN9253_PramWrite(uint16_t escAddr, const uint8_t *buf, uint16_t len);

/* ============================== 对外 API：第四阶段 ============================== */

/*
 * 协议栈统一 ESC 地址空间读写接口。
 * 低地址 ESC 寄存器自动走 CSR，0x1000 及以上 DPRAM 自动走 PRAM。
 * 如果访问跨越 0x1000 边界，驱动内部会自动拆成两段处理。
 */
void BSP_LAN9253_EscRead(uint8_t *buf, uint16_t escAddr, uint16_t len);
void BSP_LAN9253_EscWrite(const uint8_t *buf, uint16_t escAddr, uint16_t len);

/*
 * ISR 版本接口。
 * 这些接口用于 AL_EVENT 等中断上下文，内部走有限轮询的 *Isr 底层路径，
 * 避免在中断中使用 HAL 的毫秒级阻塞等待。
 */
void BSP_LAN9253_EscReadIsr(uint8_t *buf, uint16_t escAddr, uint16_t len);
void BSP_LAN9253_EscWriteIsr(const uint8_t *buf, uint16_t escAddr, uint16_t len);

/* 读取 AL Event 寄存器 0x0220，EtherCAT 协议栈常用它判断 ESC 事件来源。 */
uint16_t BSP_LAN9253_ReadALEvent(void);
uint16_t BSP_LAN9253_ReadALEventIsr(void);

#endif /* __BSP_LAN9253_H__ */
