#include "test_lan9253.h"

#include "bsp_lan9253.h"
#include "bsp_led.h"
#include "bsp_spi.h"
#include "main.h"
#include <stdio.h>

#define TEST_LAN9253_BYTE_TEST_EXPECTED 0x87654321UL
#define TEST_LAN9253_CHIP_ID_EXPECTED   0x9253U

#define TEST_LAN9253_REG_ID_REV         0x0050U
#define TEST_LAN9253_REG_BYTE_TEST      0x0064U
#define TEST_LAN9253_ESC_CHIP_ID        0x0E02U
#define TEST_LAN9253_PRAM_TEST_ADDR     0x1001U
#define TEST_LAN9253_PRAM_TEST_LEN      15U

#define TEST_LAN9253_CMD_SERIAL_READ    0x03U
#define TEST_LAN9253_CMD_FAST_READ      0x0BU

static void Test_LAN9253_PrintResult(const char *name, uint8_t pass);
static void Test_LAN9253_SetResultLed(uint8_t pass);
static uint8_t Test_LAN9253_BufferEqual(const uint8_t *buf1, const uint8_t *buf2, uint16_t len);
static void Test_LAN9253_PrintBuffer(const char *name, const uint8_t *buf, uint16_t len);
static void Test_LAN9253_RunReadDiagnostics(void);
static void Test_LAN9253_PrintGpioState(void);
static void Test_LAN9253_PrintReadLine(const char *name, HAL_StatusTypeDef status, uint32_t value);
static uint32_t Test_LAN9253_RawFastRead(uint16_t addr, uint8_t dummyByte, HAL_StatusTypeDef *pStatus);
static uint32_t Test_LAN9253_RawSerialRead(uint16_t addr, HAL_StatusTypeDef *pStatus);
static uint32_t Test_LAN9253_RawFastReadSplit(uint16_t addr, uint8_t dummyByte, HAL_StatusTypeDef *pStatus);

/**
 * @brief LAN9253 第一阶段测试。
 *
 * 第一阶段只验证最小硬件链路：
 *   1. 初始化 LAN9253 驱动对象，确保 CS 空闲为高电平，并复位 LAN9253。
 *   2. 读取 BYTE_TEST 寄存器，确认 SPI 命令、地址、dummy 字节和字节序正确。
 *   3. 读取 ID_REV 寄存器，并从高 16 位解析 Chip ID。
 *   4. 调用 BSP_LAN9253_SelfTest() 做驱动内部自检。
 *
 * 注意：
 *   当前测试不调用 BSP_LAN9253_WriteDWord() 写寄存器。
 *   第一阶段的重点是确认 SPI 读通、复位正常、芯片 ID 正确，避免测试代码修改芯片配置。
 */
void Test_LAN9253_Stage1(void)
{
    uint32_t byteTest;
    uint32_t idRev;
    uint16_t chipId;
    uint16_t escChipId;
    uint8_t byteTestPass;
    uint8_t chipIdPass;
    uint8_t escChipIdPass;
    uint8_t selfTestPass;
    uint8_t totalPass;

    printf("\r\n");
    printf("========================================\r\n");
    printf(" LAN9253 stage1 hardware test\r\n");
    printf("----------------------------------------\r\n");

    printf("Init and reset LAN9253...\r\n");
    BSP_LAN9253_Init();

    byteTest = BSP_LAN9253_ReadByteTest();
    idRev = BSP_LAN9253_ReadIDRev();
    chipId = (uint16_t)((idRev >> 16) & 0xFFFFU);
    escChipId = BSP_LAN9253_CsrRead16(TEST_LAN9253_ESC_CHIP_ID);

    byteTestPass = (byteTest == TEST_LAN9253_BYTE_TEST_EXPECTED) ? 1U : 0U;
    chipIdPass = (chipId == TEST_LAN9253_CHIP_ID_EXPECTED) ? 1U : 0U;
    escChipIdPass = (escChipId == TEST_LAN9253_CHIP_ID_EXPECTED) ? 1U : 0U;
    selfTestPass = BSP_LAN9253_SelfTest();
    totalPass = (byteTestPass && chipIdPass && escChipIdPass && selfTestPass) ? 1U : 0U;

    printf("BYTE_TEST expect : 0x%08lX\r\n", (unsigned long)TEST_LAN9253_BYTE_TEST_EXPECTED);
    printf("BYTE_TEST read   : 0x%08lX  ", (unsigned long)byteTest);
    Test_LAN9253_PrintResult("BYTE_TEST", byteTestPass);

    printf("ID_REV read      : 0x%08lX\r\n", (unsigned long)idRev);

    printf("CHIP_ID expect   : 0x%04X\r\n", (unsigned int)TEST_LAN9253_CHIP_ID_EXPECTED);
    printf("CHIP_ID read     : 0x%04X      ", (unsigned int)chipId);
    Test_LAN9253_PrintResult("CHIP_ID", chipIdPass);

    printf("ESC CHIP_ID read : 0x%04X      ", (unsigned int)escChipId);
    Test_LAN9253_PrintResult("ESC_CHIP_ID", escChipIdPass);

    printf("SELF_TEST        : ");
    Test_LAN9253_PrintResult("SELF_TEST", selfTestPass);

    uint8_t pdi = BSP_LAN9253_CsrRead8(0x0140);
    printf("PDI_SELECT = 0x%02X\r\n", pdi);
    printf("----------------------------------------\r\n");
    printf("LAN9253 stage1   : %s\r\n", totalPass ? "PASS" : "FAIL");
    printf("========================================\r\n");

    if (totalPass == 0U) {
        Test_LAN9253_RunReadDiagnostics();
    }

    Test_LAN9253_SetResultLed(totalPass);
}

/**
 * @brief LAN9253 第三阶段测试。
 *
 * 第三阶段验证 PRAM FIFO 访问 DPRAM：
 *   1. 备份 0x1001 起始的一段过程 RAM。
 *   2. 写入固定测试花样。
 *   3. 再读回并逐字节比较。
 *   4. 最后恢复原内容，避免测试数据长期留在 DPRAM。
 *
 * 这里故意使用 0x1001 这个非 4 字节对齐地址，用来验证 PRAM FIFO 首包偏移处理。
 */
void Test_LAN9253_Stage3(void)
{
    uint8_t backup[TEST_LAN9253_PRAM_TEST_LEN] = {0};
    uint8_t pattern[TEST_LAN9253_PRAM_TEST_LEN] = {0};
    uint8_t readback[TEST_LAN9253_PRAM_TEST_LEN] = {0};
    uint8_t restored[TEST_LAN9253_PRAM_TEST_LEN] = {0};
    uint8_t readIdlePass;
    uint8_t writeIdlePass;
    uint8_t comparePass;
    uint8_t restorePass;
    uint8_t totalPass;

    for (uint8_t i = 0U; i < TEST_LAN9253_PRAM_TEST_LEN; ++i) {
        pattern[i] = (uint8_t)(0xA0U + (i * 3U));
    }

    printf("\r\n");
    printf("========================================\r\n");
    printf(" LAN9253 stage3 PRAM/DPRAM test\r\n");
    printf("----------------------------------------\r\n");
    printf("Init and reset LAN9253...\r\n");
    BSP_LAN9253_Init();

    readIdlePass = BSP_LAN9253_PramReadBusyWait(100U);
    writeIdlePass = BSP_LAN9253_PramWriteBusyWait(100U);

    printf("PRAM read idle   : ");
    Test_LAN9253_PrintResult("READ_BUSY", readIdlePass);

    printf("PRAM write idle  : ");
    Test_LAN9253_PrintResult("WRITE_BUSY", writeIdlePass);

    printf("DPRAM test addr  : 0x%04X\r\n", (unsigned int)TEST_LAN9253_PRAM_TEST_ADDR);
    printf("DPRAM test len   : %u\r\n", (unsigned int)TEST_LAN9253_PRAM_TEST_LEN);

    BSP_LAN9253_PramRead(TEST_LAN9253_PRAM_TEST_ADDR, backup, sizeof(backup));
    BSP_LAN9253_PramWrite(TEST_LAN9253_PRAM_TEST_ADDR, pattern, sizeof(pattern));
    BSP_LAN9253_PramRead(TEST_LAN9253_PRAM_TEST_ADDR, readback, sizeof(readback));

    comparePass = Test_LAN9253_BufferEqual(pattern, readback, sizeof(pattern));

    Test_LAN9253_PrintBuffer("WRITE expect", pattern, sizeof(pattern));
    Test_LAN9253_PrintBuffer("READ  actual", readback, sizeof(readback));
    printf("PRAM write/read  : ");
    Test_LAN9253_PrintResult("COMPARE", comparePass);

    BSP_LAN9253_PramWrite(TEST_LAN9253_PRAM_TEST_ADDR, backup, sizeof(backup));
    BSP_LAN9253_PramRead(TEST_LAN9253_PRAM_TEST_ADDR, restored, sizeof(restored));

    restorePass = Test_LAN9253_BufferEqual(backup, restored, sizeof(backup));
    printf("DPRAM restore    : ");
    Test_LAN9253_PrintResult("RESTORE", restorePass);

    totalPass = (readIdlePass && writeIdlePass && comparePass && restorePass) ? 1U : 0U;

    printf("----------------------------------------\r\n");
    printf("LAN9253 stage3   : %s\r\n", totalPass ? "PASS" : "FAIL");
    printf("========================================\r\n");

    Test_LAN9253_SetResultLed(totalPass);
}

/**
 * @brief LAN9253 第四阶段测试。
 *
 * 第四阶段验证协议栈适配层统一入口：
 *   1. 通过 BSP_LAN9253_EscRead() 读取 ESC Chip ID，确认低地址 CSR 分流正确。
 *   2. 通过 BSP_LAN9253_EscWrite()/EscRead() 访问 DPRAM，确认高地址 PRAM 分流正确。
 *   3. 恢复 DPRAM 原内容。
 */
void Test_LAN9253_Stage4(void)
{
    uint8_t chipIdBuf[2] = {0};
    uint16_t escChipId;
    uint8_t backup[TEST_LAN9253_PRAM_TEST_LEN] = {0};
    uint8_t pattern[TEST_LAN9253_PRAM_TEST_LEN] = {0};
    uint8_t readback[TEST_LAN9253_PRAM_TEST_LEN] = {0};
    uint8_t restored[TEST_LAN9253_PRAM_TEST_LEN] = {0};
    uint8_t csrPathPass;
    uint8_t pramPathPass;
    uint8_t restorePass;
    uint8_t totalPass;

    for (uint8_t i = 0U; i < TEST_LAN9253_PRAM_TEST_LEN; ++i) {
        pattern[i] = (uint8_t)(0x5AU ^ (uint8_t)(i * 7U));
    }

    printf("\r\n");
    printf("========================================\r\n");
    printf(" LAN9253 stage4 ESC adapter test\r\n");
    printf("----------------------------------------\r\n");
    printf("Init and reset LAN9253...\r\n");
    BSP_LAN9253_Init();

    BSP_LAN9253_EscRead(chipIdBuf, TEST_LAN9253_ESC_CHIP_ID, sizeof(chipIdBuf));
    escChipId = ((uint16_t)chipIdBuf[0]) |
                ((uint16_t)chipIdBuf[1] << 8);
    csrPathPass = (escChipId == TEST_LAN9253_CHIP_ID_EXPECTED) ? 1U : 0U;

    printf("ESC CHIP_ID read : 0x%04X      ", (unsigned int)escChipId);
    Test_LAN9253_PrintResult("ESC_CSR_PATH", csrPathPass);

    BSP_LAN9253_EscRead(backup, TEST_LAN9253_PRAM_TEST_ADDR, sizeof(backup));
    BSP_LAN9253_EscWrite(pattern, TEST_LAN9253_PRAM_TEST_ADDR, sizeof(pattern));
    BSP_LAN9253_EscRead(readback, TEST_LAN9253_PRAM_TEST_ADDR, sizeof(readback));

    pramPathPass = Test_LAN9253_BufferEqual(pattern, readback, sizeof(pattern));

    Test_LAN9253_PrintBuffer("WRITE expect", pattern, sizeof(pattern));
    Test_LAN9253_PrintBuffer("READ  actual", readback, sizeof(readback));
    printf("ESC PRAM path    : ");
    Test_LAN9253_PrintResult("ESC_PRAM_PATH", pramPathPass);

    BSP_LAN9253_EscWrite(backup, TEST_LAN9253_PRAM_TEST_ADDR, sizeof(backup));
    BSP_LAN9253_EscRead(restored, TEST_LAN9253_PRAM_TEST_ADDR, sizeof(restored));

    restorePass = Test_LAN9253_BufferEqual(backup, restored, sizeof(backup));
    printf("DPRAM restore    : ");
    Test_LAN9253_PrintResult("RESTORE", restorePass);

    totalPass = (csrPathPass && pramPathPass && restorePass) ? 1U : 0U;

    printf("----------------------------------------\r\n");
    printf("LAN9253 stage4   : %s\r\n", totalPass ? "PASS" : "FAIL");
    printf("========================================\r\n");

    Test_LAN9253_SetResultLed(totalPass);
}

static void Test_LAN9253_PrintResult(const char *name, uint8_t pass)
{
    if (pass != 0U) {
        printf("%s PASS\r\n", name);
    } else {
        printf("%s FAIL\r\n", name);
    }
}

static uint8_t Test_LAN9253_BufferEqual(const uint8_t *buf1, const uint8_t *buf2, uint16_t len)
{
    for (uint16_t i = 0U; i < len; ++i) {
        if (buf1[i] != buf2[i]) {
            return 0U;
        }
    }

    return 1U;
}

static void Test_LAN9253_PrintBuffer(const char *name, const uint8_t *buf, uint16_t len)
{
    printf("%s :", name);

    for (uint16_t i = 0U; i < len; ++i) {
        printf(" %02X", buf[i]);
    }

    printf("\r\n");
}

static void Test_LAN9253_SetResultLed(uint8_t pass)
{
    if (pass != 0U) {
        BSP_LED_Set(LED2, LED_OFF);
    } else {
        BSP_LED_Set(LED2, LED_ON);
    }
}

static void Test_LAN9253_RunReadDiagnostics(void)
{
    HAL_StatusTypeDef status;
    uint32_t value;

    printf("\r\n");
    printf("========================================\r\n");
    printf(" LAN9253 low level SPI diagnostics\r\n");
    printf("----------------------------------------\r\n");
    Test_LAN9253_PrintGpioState();

    value = Test_LAN9253_RawFastRead(TEST_LAN9253_REG_BYTE_TEST, 0x00U, &status);
    Test_LAN9253_PrintReadLine("FAST  dummy 0x00 BYTE_TEST", status, value);

    value = Test_LAN9253_RawFastRead(TEST_LAN9253_REG_BYTE_TEST, 0x01U, &status);
    Test_LAN9253_PrintReadLine("FAST  dummy 0x01 BYTE_TEST", status, value);

    value = Test_LAN9253_RawSerialRead(TEST_LAN9253_REG_BYTE_TEST, &status);
    Test_LAN9253_PrintReadLine("SER   no dummy   BYTE_TEST", status, value);

    value = Test_LAN9253_RawSerialRead(TEST_LAN9253_REG_ID_REV, &status);
    Test_LAN9253_PrintReadLine("SER   no dummy   ID_REV   ", status, value);

    value = Test_LAN9253_RawFastReadSplit(TEST_LAN9253_REG_BYTE_TEST, 0x01U, &status);
    Test_LAN9253_PrintReadLine("FAST  split rx   BYTE_TEST", status, value);

    value = Test_LAN9253_RawFastRead(TEST_LAN9253_REG_ID_REV, 0x01U, &status);
    Test_LAN9253_PrintReadLine("FAST  dummy 0x01 ID_REV   ", status, value);

    printf("----------------------------------------\r\n");
    printf("status: 0=OK, 1=ERROR, 2=BUSY, 3=TIMEOUT\r\n");
    printf("If every value is 0x00000000 with status 0,\r\n");
    printf("check CS/RST/MISO/SCK/MOSI by logic analyzer.\r\n");
    printf("========================================\r\n");
}

static void Test_LAN9253_PrintGpioState(void)
{
    GPIO_PinState csState = HAL_GPIO_ReadPin(LAN9253_CS_GPIO_Port, LAN9253_CS_Pin);
    GPIO_PinState rstState = HAL_GPIO_ReadPin(LAN9253_RST_GPIO_Port, LAN9253_RST_Pin);
    GPIO_PinState misoState = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_11);

    printf("GPIO CS idle     : %s\r\n", (csState == GPIO_PIN_SET) ? "HIGH" : "LOW");
    printf("GPIO RST         : %s\r\n", (rstState == GPIO_PIN_SET) ? "HIGH" : "LOW");
    printf("GPIO MISO idle   : %s\r\n", (misoState == GPIO_PIN_SET) ? "HIGH" : "LOW");
}

static void Test_LAN9253_PrintReadLine(const char *name, HAL_StatusTypeDef status, uint32_t value)
{
    printf("%s : status=%d value=0x%08lX\r\n",
           name,
           (int)status,
           (unsigned long)value);
}

static uint32_t Test_LAN9253_RawFastRead(uint16_t addr, uint8_t dummyByte, HAL_StatusTypeDef *pStatus)
{
    uint8_t txBuf[9] = {
        TEST_LAN9253_CMD_FAST_READ,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFFU),
        dummyByte,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
    };
    uint8_t rxBuf[9] = {0};

    g_SpiDevice.pOps->SPI_CS_Select(&g_SpiDevice);
    *pStatus = g_SpiDevice.pOps->SPI_TransmitReceive(&g_SpiDevice, txBuf, rxBuf, sizeof(txBuf), 100U);
    g_SpiDevice.pOps->SPI_CS_Deselect(&g_SpiDevice);

    if (*pStatus != HAL_OK) {
        return 0;
    }

    return ((uint32_t)rxBuf[5]) |
           ((uint32_t)rxBuf[6] << 8) |
           ((uint32_t)rxBuf[7] << 16) |
           ((uint32_t)rxBuf[8] << 24);
}

static uint32_t Test_LAN9253_RawSerialRead(uint16_t addr, HAL_StatusTypeDef *pStatus)
{
    uint8_t txBuf[7] = {
        TEST_LAN9253_CMD_SERIAL_READ,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFFU),
        0x00U,
        0x00U,
        0x00U,
        0x00U,
    };
    uint8_t rxBuf[7] = {0};

    g_SpiDevice.pOps->SPI_CS_Select(&g_SpiDevice);
    *pStatus = g_SpiDevice.pOps->SPI_TransmitReceive(&g_SpiDevice, txBuf, rxBuf, sizeof(txBuf), 100U);
    g_SpiDevice.pOps->SPI_CS_Deselect(&g_SpiDevice);

    if (*pStatus != HAL_OK) {
        return 0;
    }

    return ((uint32_t)rxBuf[3]) |
           ((uint32_t)rxBuf[4] << 8) |
           ((uint32_t)rxBuf[5] << 16) |
           ((uint32_t)rxBuf[6] << 24);
}

static uint32_t Test_LAN9253_RawFastReadSplit(uint16_t addr, uint8_t dummyByte, HAL_StatusTypeDef *pStatus)
{
    uint8_t cmdBuf[5] = {
        TEST_LAN9253_CMD_FAST_READ,
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFFU),
        dummyByte,
        0x00U,
    };
    uint8_t rxBuf[4] = {0};

    g_SpiDevice.pOps->SPI_CS_Select(&g_SpiDevice);
    *pStatus = g_SpiDevice.pOps->SPI_Transmit(&g_SpiDevice, cmdBuf, sizeof(cmdBuf), 100U);
    if (*pStatus == HAL_OK) {
        *pStatus = g_SpiDevice.pOps->SPI_Receive(&g_SpiDevice, rxBuf, sizeof(rxBuf), 100U);
    }
    g_SpiDevice.pOps->SPI_CS_Deselect(&g_SpiDevice);

    if (*pStatus != HAL_OK) {
        return 0;
    }

    return ((uint32_t)rxBuf[0]) |
           ((uint32_t)rxBuf[1] << 8) |
           ((uint32_t)rxBuf[2] << 16) |
           ((uint32_t)rxBuf[3] << 24);
}
