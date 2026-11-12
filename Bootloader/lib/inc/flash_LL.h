#ifndef FLASH_LL_H
#define FLASH_LL_H

#include <stdint.h>
#include "stm32f4xx.h"
#include "stm32f4xx_ll_system.h" 

#define FLH_MAX_SECTOR_NUM      FLH_SECTOR_5      /* F401xC: сектора 0..5 */

typedef enum {
    FLH_SECTOR_0 = 0,
    FLH_SECTOR_1,
    FLH_SECTOR_2,
    FLH_SECTOR_3,
    FLH_SECTOR_4,
    FLH_SECTOR_5,
    FLH_SECTOR_6,
    FLH_SECTOR_7,
    FLH_SECTOR_8
} FLH_Sector_t;

typedef enum {
    FLH_OK = 0,
    FLH_ERR_PARAM,
    FLH_ERR_LOCKED,
    FLH_ERR_HW
} FLH_Status_t;

/* Flash reading and the cycle counter have been moved into macros so that the logic
* can be run on a PC with substitutions (no redefinition is required for the microcontroller project). */
#ifndef FLASH_READ32
#define FLASH_READ32(a)     (*(volatile const uint32_t *)(a))
#endif

void         flash_CycleInit(void);                       /* en DWT->CYCCNT */
uint32_t     flash_Cycles(void);

void         flash_KEY_EnableAccess(void);
void         flash_KEY_DisableAccess(void);
void         flash_FlushCaches(void);
int32_t      flash_SectorOfAddr(uint32_t addr);           /* sector of the application area, or -1 */
FLH_Status_t flash_EraseSector(FLH_Sector_t sector);
FLH_Status_t flash_WriteBlock(uint32_t addr, const uint32_t *src, uint32_t words);

#endif /* FLASH_LL_H */