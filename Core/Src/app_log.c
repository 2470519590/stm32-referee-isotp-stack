#include "app_log.h"

#include <stdio.h>
#include <string.h>

#include "usart.h"

void App_Log_Init(void)
{
}

void App_Log_VPrintf(const char *fmt, va_list args)
{
    char buffer[256];
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);

    if (len <= 0)
    {
        return;
    }

    if (len > (int)sizeof(buffer))
    {
        len = (int)sizeof(buffer);
    }

    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buffer, (uint16_t)len, 100U);
}

void App_Log_Printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    App_Log_VPrintf(fmt, args);
    va_end(args);
}
