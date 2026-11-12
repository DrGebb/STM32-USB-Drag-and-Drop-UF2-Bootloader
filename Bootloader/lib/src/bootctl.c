/*------------------------------------------------------------------------------
 * bootctl.c: Bootloader entry (PA0) and transfer of control to the application. 
*
* Scheme:
*   1. main(): boot_early_check() runs before anything else. If PA0 = 0 or there is no
*      valid application, the system remains in the bootloader (UF2 drive). 
*   2. Once file reception is complete (uf2_is_complete()): a pause occurs, followed by a software reset. 
*      The reset clears peripherals, the NVIC, SysTick, and RTX, and disables USB;
*      subsequently, step 1 launches the application from a "clean" hardware state.
 *----------------------------------------------------------------------------*/
#include "bootctl.h"
#include "stm32f4xx.h"
#include "cmsis_os2.h"
#include "uf2_loader.h"

#define APP_BASE   UF2_APP_BASE

/* ---------------- Login button: PA0, pull-up to supply voltage ------------------- */
static bool boot_key_pressed (void) {
 __IO uint32_t i, j, low = 0U;

  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
  (void)RCC->AHB1ENR;                                        //delay before the start of a cycle

  GPIOA->MODER &= ~(3UL << (0U * 2U));                       
  GPIOA->PUPDR  = (GPIOA->PUPDR & ~(3UL << (0U * 2U)))
                | (1UL << (0U * 2U));                        

  for (i = 0U; i < 2000U; i++) { __NOP(); }                  //waiting for the resistor pull-up

  for (i = 0U; i < 16U; i++) {                               // 16 samples -- all need to be 0
    if ((GPIOA->IDR & 1UL) == 0U) { low++; }
    for (j = 0U; j < 200U; j++) { __NOP(); }
  }
  return (low == 16U);
}

//Return GPIOA to its post-reset state and turn off its clock
static void boot_key_release (void) {
  RCC->AHB1RSTR |=  RCC_AHB1RSTR_GPIOARST;
  RCC->AHB1RSTR &= ~RCC_AHB1RSTR_GPIOARST;
  RCC->AHB1ENR  &= ~RCC_AHB1ENR_GPIOAEN;
}

/* ---------------- Transition to the application ------------------------------------
* There is no return from C. The bootloader's stack is no longer used after the MSR MSP operation. */
__attribute__((naked, noreturn))
static void start_app (uint32_t app_msp __attribute__((unused)),
                       uint32_t app_reset __attribute__((unused))) {
  __asm volatile (
    "movs r2, #0        \n"
    "msr  BASEPRI, r2   \n"      //condition as if after a reset        
    "msr  FAULTMASK, r2 \n"
    "msr  CONTROL, r2   \n"      //privileged thread, MSP stack, without FPCA
    "isb                \n"
    "msr  MSP, r0       \n"      //application stack                            
    "cpsie i            \n"      //PRIMASK = 0, as it is after a reset (IRQs in the NVIC are disabled) 
    "bx   r1            \n"      //Application Reset_Handler                   
  );
}

__attribute__((noreturn))
static void boot_jump_to_app (void) {
  uint32_t app_msp   = *(volatile uint32_t *)(APP_BASE + 0x00U);
  uint32_t app_reset = *(volatile uint32_t *)(APP_BASE + 0x04U);
  uint32_t i;

  __disable_irq ();

  SysTick->CTRL = 0U;                                        //stop SysTick
  SCB->ICSR     = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

  for (i = 0U; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
    NVIC->ICER[i] = 0xFFFFFFFFU;                             // dis IRQ
    NVIC->ICPR[i] = 0xFFFFFFFFU;                             // clear pending
  }

  SCB->VTOR = APP_BASE;                                      // vector table
  __DSB ();
  __ISB ();

  start_app (app_msp, app_reset);
}

void boot_early_check (void) {
  bool key = boot_key_pressed ();
  boot_key_release ();

  if ((!key) && uf2_app_vectors_ok ()) {
    boot_jump_to_app ();
  }
}

/* ---------------- Stream: reception complete -> pause -> reset ------------------ */
void boot_supervisor_thread (void *arg) {
  (void)arg;

  while (!uf2_is_complete ()) {
    osDelay (100U);
  }

  osDelay (BOOT_POST_COMPLETE_MS);                           // wait Windows appends to the directory path

#if BOOT_AUTO_RESET
  if (uf2_app_vectors_ok ()) {
    NVIC_SystemReset ();                                     
  }
#endif

  for (;;) {                                                 //vectors are invalid or reset is disabled: waiting for a manual reset
    osDelay (1000U);
  }
}