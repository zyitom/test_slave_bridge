#include "firmware/ch32_board/app/src/board_app.hpp"

namespace librmcs::firmware::board {

// NOTE (bring-up): the GPIO pin choices below are placeholders (classic bxCAN /
// USART defaults) and the returned clock is SystemCoreClock. Confirm the actual
// CH32H417EVT transceiver/UART pins from the schematic and the CAN/USART kernel
// clock dividers here -- this is the single place that owns them.

uint32_t init_can(CAN_TypeDef* can) {
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO | RCC_HB2Periph_GPIOA, ENABLE);
    if (can == CAN1)
        RCC_HB1PeriphClockCmd(RCC_HB1Periph_CAN1, ENABLE);
    else if (can == CAN2)
        RCC_HB1PeriphClockCmd(RCC_HB1Periph_CAN2, ENABLE);

    GPIO_InitTypeDef gpio = {};
    gpio.GPIO_Pin = GPIO_Pin_11; // RX
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_12; // TX
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    return SystemCoreClock;
}

uint32_t init_uart(USART_TypeDef* usart) {
    RCC_HB2PeriphClockCmd(RCC_HB2Periph_AFIO | RCC_HB2Periph_GPIOA, ENABLE);
    if (usart == USART1)
        RCC_HB2PeriphClockCmd(RCC_HB2Periph_USART1, ENABLE);
    else if (usart == USART2)
        RCC_HB1PeriphClockCmd(RCC_HB1Periph_USART2, ENABLE);

    GPIO_InitTypeDef gpio = {};
    gpio.GPIO_Pin = (usart == USART1) ? GPIO_Pin_9 : GPIO_Pin_2; // TX
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_Very_High;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = (usart == USART1) ? GPIO_Pin_10 : GPIO_Pin_3; // RX
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &gpio);

    return SystemCoreClock;
}

} // namespace librmcs::firmware::board
