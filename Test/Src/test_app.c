#include "test_app.h"

#include "bsp_led.h"
#include "bsp_uart.h"
#include "main.h"
#include "test_lan9253.h"
#include <stdio.h>

static void TestApp_PrintMenu(void);
static void TestApp_RunLedPowerOnTest(void);
static void TestApp_ProcessUartCommand(void);
static void TestApp_SetAllLeds(LED_STATE_T state);

void TestApp_Init(void)
{
    printf("\r\nSystem boot OK\r\n");
    TestApp_RunLedPowerOnTest();
    Test_LAN9253_Stage1();
    TestApp_PrintMenu();
}

void TestApp_Poll(void)
{
    static uint32_t lastHeartbeatTick = 0;

    TestApp_ProcessUartCommand();

    if ((HAL_GetTick() - lastHeartbeatTick) >= 500U) {
        lastHeartbeatTick = HAL_GetTick();
        BSP_LED_Toggle(LED1);
        // printf("tick %lu ms\r\n", HAL_GetTick());
    }
}

static void TestApp_PrintMenu(void)
{
    printf("\r\n");
    printf("========================================\r\n");
    printf(" YSF4 LAN9253 UART/LED test\r\n");
    printf(" USART2: 115200, 8N1\r\n");
    printf("----------------------------------------\r\n");
    printf(" 1    : toggle LED1 heartbeat\r\n");
    printf(" 2    : toggle LED2 test result\r\n");
    printf(" a    : LED1/LED2 all ON\r\n");
    printf(" c    : LED1/LED2 all OFF\r\n");
    printf(" s    : LAN9253 stage1 test\r\n");
    printf(" p    : LAN9253 stage3 PRAM/DPRAM test\r\n");
    printf(" e    : LAN9253 stage4 ESC adapter test\r\n");
    printf(" ?    : print this menu\r\n");
    printf(" Other characters will be echoed.\r\n");
    printf("========================================\r\n");
}

static void TestApp_RunLedPowerOnTest(void)
{
    const LED_NUM_T leds[] = {
        LED1, LED2,
    };

    TestApp_SetAllLeds(LED_OFF);
    HAL_Delay(200);

    for (uint32_t i = 0; i < (sizeof(leds) / sizeof(leds[0])); ++i) {
        BSP_LED_Set(leds[i], LED_ON);
        HAL_Delay(120);
        BSP_LED_Set(leds[i], LED_OFF);
    }

    TestApp_SetAllLeds(LED_ON);
    HAL_Delay(300);

    TestApp_SetAllLeds(LED_OFF);
}

static void TestApp_SetAllLeds(LED_STATE_T state)
{
    for (LED_NUM_T led = LED1; led < LED_COUNT; led++) {
        BSP_LED_Set(led, state);
    }
}

static void TestApp_ProcessUartCommand(void)
{
    uint8_t rxBuf[16];
    uint16_t rxLen = g_DebugUart.pOps->Receive(&g_DebugUart, rxBuf, sizeof(rxBuf));

    for (uint16_t i = 0; i < rxLen; ++i) {
        uint8_t ch = rxBuf[i];

        if (ch == '1') {
            BSP_LED_Toggle(LED1);
            printf("\r\nLED1 toggled\r\n");
        } else if (ch == '2') {
            BSP_LED_Toggle(LED2);
            printf("\r\nLED2 toggled\r\n");
        } else if ((ch == 'a') || (ch == 'A')) {
            TestApp_SetAllLeds(LED_ON);
            printf("\r\nLED1/LED2 all ON\r\n");
        } else if ((ch == 'c') || (ch == 'C')) {
            TestApp_SetAllLeds(LED_OFF);
            printf("\r\nLED1/LED2 all OFF\r\n");
        } else if ((ch == 's') || (ch == 'S')) {
            Test_LAN9253_Stage1();
        } else if ((ch == 'p') || (ch == 'P')) {
            Test_LAN9253_Stage3();
        } else if ((ch == 'e') || (ch == 'E')) {
            Test_LAN9253_Stage4();
        } else if (ch == '?') {
            TestApp_PrintMenu();
        } else if ((ch != '\r') && (ch != '\n')) {
            printf("\r\nRX echo: '%c' 0x%02X\r\n", ch, ch);
        }
    }
}
