#ifndef BOOTCTL_H
#define BOOTCTL_H

#include <stdint.h>
#include <stdbool.h>

/* Pause after receiving the last block: Windows has time to add the directory. */
#ifndef BOOT_POST_COMPLETE_MS
#define BOOT_POST_COMPLETE_MS   3000U
#endif

/* 1: perform a software reset after a pause (application startup will trigger boot_early_check). 
* 0: do nothing (for testing purposes: does not reset statistics in Watch). */
#ifndef BOOT_AUTO_RESET
#define BOOT_AUTO_RESET         1
#endif

/* Call this as the FIRST line in main(), before clock initialization, USB, or RTOS setup. 
* If PA0 is at a logic high level (button not pressed) and the application appears valid,
* the function does not return and instead transfers control to the application. 
* Otherwise, it returns, and the bootloader continues its execution. */
void boot_early_check(void);

/* RTOS thread body: waits for uf2_is_complete(), pauses, and resets the MCU. */
void boot_supervisor_thread(void *arg);

#endif