#include "flash_LL.h"

/* error flag mask for FLASH->SR */
#define PROG_ERR_ALL (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                      FLASH_SR_PGPERR | FLASH_SR_PGSERR)

static inline void flash_WaitBusy(void) { while (FLASH->SR & FLASH_SR_BSY) { } }
static inline void flash_ClearFlags(void) { FLASH->SR = FLASH_SR_EOP | PROG_ERR_ALL; }

/* ---- cycle counter (measurements are independent of SysTick, which,
*      along with the CPU, is halted during flash erase/write operations) ---- */
void flash_CycleInit(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t flash_Cycles(void) {
    return DWT->CYCCNT;
}

/* ---- access ---- */
void flash_KEY_EnableAccess(void) {
    flash_WaitBusy();
    if (FLASH->CR & FLASH_CR_LOCK) {            /* ключи только если залочено */
        WRITE_REG(FLASH->KEYR, 0x45670123UL);
        WRITE_REG(FLASH->KEYR, 0xCDEF89ABUL);
    }
}

void flash_KEY_DisableAccess(void) {
    flash_WaitBusy();
    CLEAR_BIT(FLASH->CR, FLASH_CR_PG | FLASH_CR_SER | FLASH_CR_SNB);
    SET_BIT(FLASH->CR, FLASH_CR_LOCK);
}

/* ---- flash caches: invalidate after erase/write ---- */
void flash_FlushCaches(void) {
    LL_FLASH_DisableInstCache();
    LL_FLASH_EnableInstCacheReset();
    LL_FLASH_DisableInstCacheReset();

    LL_FLASH_DisableDataCache();
    LL_FLASH_EnableDataCacheReset();
    LL_FLASH_DisableDataCacheReset();

    LL_FLASH_EnableDataCache();
    LL_FLASH_EnableInstCache();
}

/* ---- application address -> sector number (application area only) ---- */
int32_t flash_SectorOfAddr(uint32_t addr) {
    if (addr >= 0x08010000UL && addr < 0x08020000UL) { return FLH_SECTOR_4; }
    if (addr >= 0x08020000UL && addr < 0x08040000UL) { return FLH_SECTOR_5; }
    return -1;
}

/* ---- sector erasure ---- */
FLH_Status_t flash_EraseSector(FLH_Sector_t sector) {
    uint32_t st;

    if ((uint32_t)sector > (uint32_t)FLH_MAX_SECTOR_NUM) { return FLH_ERR_PARAM; }
    if (FLASH->CR & FLASH_CR_LOCK)                       { return FLH_ERR_LOCKED; }

    flash_WaitBusy();
    flash_ClearFlags();                         /* убрать устаревшие флаги */

    /* PSIZE x32 (VDD 2.7..3.6 В), номер сектора, режим стирания */
    MODIFY_REG(FLASH->CR, FLASH_CR_PSIZE | FLASH_CR_SNB,
               FLASH_CR_PSIZE_1 | FLASH_CR_SER | ((uint32_t)sector << FLASH_CR_SNB_Pos));
    SET_BIT(FLASH->CR, FLASH_CR_STRT);
    __DSB();
    flash_WaitBusy();

    st = FLASH->SR;
    CLEAR_BIT(FLASH->CR, FLASH_CR_SER | FLASH_CR_SNB);
    flash_ClearFlags();
    flash_FlushCaches();

    return (st & PROG_ERR_ALL) ? FLH_ERR_HW : FLH_OK;
}

/* ---- block write: addr is a multiple of 4, src is 4-byte aligned, words number of words ---- */
FLH_Status_t flash_WriteBlock(uint32_t addr, const uint32_t *src, uint32_t words) {
    FLH_Status_t r = FLH_OK;

    if (addr & 3U)                 { return FLH_ERR_PARAM; }
    if (FLASH->CR & FLASH_CR_LOCK) { return FLH_ERR_LOCKED; }

    flash_WaitBusy();
    flash_ClearFlags();

    MODIFY_REG(FLASH->CR, FLASH_CR_PSIZE, FLASH_CR_PSIZE_1);   /* x32 */
    SET_BIT(FLASH->CR, FLASH_CR_PG);                           /* PG  once per block */

    while (words--) {
        *(__IO uint32_t *)addr = *src++;
        addr += 4U;
        flash_WaitBusy();
        if (FLASH->SR & PROG_ERR_ALL) { r = FLH_ERR_HW; break; }
    }

    CLEAR_BIT(FLASH->CR, FLASH_CR_PG);
    flash_ClearFlags();
    return r;
}