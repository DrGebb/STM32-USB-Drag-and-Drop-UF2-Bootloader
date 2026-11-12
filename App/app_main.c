
#include "app_main.h"
#include "stm32f4xx_hal.h"


#ifndef USER_VECT_TAB_ADDRESS
    #error "USER_VECT_TAB_ADDRESS is not defined!"
#endif

#ifndef VECT_TAB_OFFSET
    #error "VECT_TAB_OFFSET is not defined!"
#endif

#if (VECT_TAB_OFFSET != 0x00010000U)
    #error "VECT_TAB_OFFSET must be 0x00010000U!"
#endif


void LED13_BLINK_Init(void){

    //en clk for GPIOC
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOC);

    //cong PC13 -LED OUT
    LL_GPIO_InitTypeDef gpioInit = {
        .Mode = LL_GPIO_MODE_OUTPUT,
        .OutputType = LL_GPIO_OUTPUT_PUSHPULL,
        .Pin = LL_GPIO_PIN_13,
        .Pull = LL_GPIO_PULL_NO,
        .Speed = LL_GPIO_SPEED_FREQ_LOW
    };
    LL_GPIO_Init(GPIOC, &gpioInit);

    // PC13 LED pin - invert -- set 1 - to off
    LL_GPIO_SetOutputPin(GPIOC, LL_GPIO_PIN_13);

}

void app_main(){

    LED13_BLINK_Init();

    for (;;) {

        LL_GPIO_TogglePin(GPIOC, LL_GPIO_PIN_13);
    }

}
