#include "ecat_io_app.h"

#include "bsp_key.h"
#include "bsp_led.h"

UINT8 EcatIo_ReadInputs(void)
{
    return BSP_Key_ReadStateByte();
}

void EcatIo_ApplyOutputs(UINT8 outputByte)
{
    BSP_LED_Set(LED1, ((outputByte & 0x01U) != 0U) ? LED_ON : LED_OFF);
    BSP_LED_Set(LED2, ((outputByte & 0x02U) != 0U) ? LED_ON : LED_OFF);
}

void EcatIo_Application(void)
{
}
