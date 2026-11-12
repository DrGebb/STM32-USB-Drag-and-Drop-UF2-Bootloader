/*------------------------------------------------------------------------------
 * uf2_loader.c: ядро загрузчика UF2 для STM32F401xC.
 * Не зависит от USB: получает по одному 512-байтному блоку.
 * Прыжок в приложение здесь НЕ реализован: наружу даны только
 * uf2_is_complete() и uf2_app_vectors_ok().
 *----------------------------------------------------------------------------*/
#include <string.h>
#include "uf2_loader.h"
#include "flash_ll.h"

#define UF2_MAGIC0         0x0A324655UL
#define UF2_MAGIC1         0x9E5D5157UL
#define UF2_MAGIC_END      0x0AB16F30UL
#define UF2_FLAG_NOFLASH   0x00000001UL
#define UF2_FLAG_FAMILYID  0x00002000UL
#define UF2_FAMILY_STM32F4 0x57755A57UL
#define UF2_MAX_PAYLOAD    476U

/* prog_block error codes */
#define PB_OK              0U
#define PB_ERR_RANGE       1U
#define PB_ERR_ERASE       2U
#define PB_ERR_PROGRAM     3U
#define PB_ERR_VERIFY      4U

#if UF2_DIAG
volatile uf2_diag_t uf2_diag;
uint32_t            uf2_crc[UF2_MAX_BLOCKS];
#define DG_INC(f)       (uf2_diag.f++)
#define DG_ADD(f, v)    (uf2_diag.f += (v))
#define DG_SET(f, v)    (uf2_diag.f = (v))
#else
#define DG_INC(f)       ((void)0)
#define DG_ADD(f, v)    ((void)0)
#define DG_SET(f, v)    ((void)0)
#endif

static uint32_t g_seen[UF2_MAX_BLOCKS / 32U];   /* map of received blocks */
static uint32_t g_total;                       /* num_blocks of the current file (0 = not yet known) */
static uint32_t g_received;                     /* various blocks received */
static uint32_t g_erased_mask;                  /* erased sectors (bit = sector number) */
static bool     g_complete;
static uint32_t g_pbuf[UF2_MAX_PAYLOAD / 4U];  /* aligned copy of the block data */
#if UF2_VECTORS_LAST
static uint32_t g_hold[UF2_MAX_PAYLOAD / 4U];   /* held block with vectors */
static uint32_t g_hold_len;
static uint32_t g_hold_blkno;
static bool     g_hold_valid;
#endif

#if UF2_DIAG
/* CRC32, same as zlib.crc32 / Python (bit-by-bit, without a lookup table) */
static uint32_t crc32_calc (const uint8_t *p, uint32_t n) {
  uint32_t c = 0xFFFFFFFFUL;
  uint32_t k;
  while (n--) {
    c ^= *p++;
    for (k = 0; k < 8U; k++) {
      c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1UL)));
    }
  }
  return ~c;
}
#endif

void uf2_init (void) {
  memset (g_seen, 0, sizeof(g_seen));
  g_total       = 0U;
  g_received    = 0U;
  g_erased_mask = 0U;
  g_complete    = false;
#if UF2_VECTORS_LAST
  g_hold_valid  = false;
  g_hold_len    = 0U;
  g_hold_blkno  = 0U;
#endif
#if UF2_DIAG
  memset ((void *)&uf2_diag, 0, sizeof(uf2_diag));
  memset (uf2_crc, 0, sizeof(uf2_crc));
  uf2_diag.app_lo = 0xFFFFFFFFUL;
  flash_CycleInit ();
#endif
}

bool     uf2_is_complete (void)     { return g_complete; }
uint32_t uf2_blocks_received (void) { return g_received; }
uint32_t uf2_blocks_total (void)    { return g_total; }

bool uf2_app_vectors_ok (void) {
  uint32_t sp = FLASH_READ32 (UF2_APP_BASE);
  uint32_t rv = FLASH_READ32 (UF2_APP_BASE + 4U);
  if ((sp < 0x20000000UL) || (sp > 0x20010000UL)) { return false; }
  if (((rv & 1U) == 0U) || (rv < UF2_APP_BASE) || (rv >= UF2_APP_END)) { return false; }
  return true;
}

/* Writing a verified block: the sector is erased upon first access, then the block
* is programmed and read back. Access to the flash memory is enabled only for the duration of the call. */
static uint32_t prog_block (uint32_t addr, uint32_t len, const uint8_t *src) {
  int32_t  sec_first = flash_SectorOfAddr (addr);
  int32_t  sec_last  = flash_SectorOfAddr (addr + len - 1U);
  int32_t  sec;
  uint32_t words = len / 4U;
  uint32_t i;
  uint32_t rc = PB_OK;
#if UF2_DIAG
  uint32_t t0;
#endif

  if ((sec_first < 0) || (sec_last < 0)) { return PB_ERR_RANGE; }

  memcpy (g_pbuf, src, len);                        /* src может быть не выровнен */

  flash_KEY_EnableAccess ();

  for (sec = sec_first; sec <= sec_last; sec++) {
    if ((g_erased_mask & (1UL << (uint32_t)sec)) == 0U) {
#if UF2_DIAG
      t0 = flash_Cycles ();
#endif
      if (flash_EraseSector ((FLH_Sector_t)sec) != FLH_OK) {
        rc = PB_ERR_ERASE;
        goto done;
      }
      DG_ADD (erase_cycles, flash_Cycles () - t0);
      DG_INC (erase_cnt);
      g_erased_mask |= (1UL << (uint32_t)sec);
      DG_SET (sect_erased, g_erased_mask);
    }
  }

#if UF2_DIAG
  t0 = flash_Cycles ();
#endif
  if (flash_WriteBlock (addr, g_pbuf, words) != FLH_OK) {
    rc = PB_ERR_PROGRAM;
    goto done;
  }
  DG_ADD (prog_cycles, flash_Cycles () - t0);
  DG_ADD (prog_words, words);

  flash_FlushCaches ();                             /* читаем назад из флеша, не из кэша */
  for (i = 0; i < words; i++) {
    if (FLASH_READ32 (addr + 4U * i) != g_pbuf[i]) {
      DG_INC (verify_err);
      rc = PB_ERR_VERIFY;
      break;
    }
  }

done:
  flash_KEY_DisableAccess ();
  return rc;
}

#if UF2_DIAG
/* test checks after receiving the last block */
static void diag_finish (void) {
  uf2_diag.uf2_digest = crc32_calc ((const uint8_t *)uf2_crc, 4U * g_total);
  flash_FlushCaches ();
  uf2_diag.app_vec_ok = uf2_app_vectors_ok () ? 1U : 0U;
#if UF2_PATTERN_CHECK
  {
    uint32_t a;
    uf2_diag.pat_checked = 1U;
    for (a = uf2_diag.app_lo; a < uf2_diag.app_hi; a += 4U) {
      if (FLASH_READ32 (a) != a) { uf2_diag.pat_err++; }
    }
  }
#endif
  uf2_diag.uf2_complete = 1U;
}
#endif

uf2_result_t uf2_process_block (const uint8_t *b) {
  uint32_t h[8];
  uint32_t end_magic, addr, len, blkno, total, rc;
#if UF2_DIAG
  uint32_t crc;
#endif

  memcpy (h, b, 32);
  memcpy (&end_magic, &b[508], 4);

  if ((h[0] != UF2_MAGIC0) || (h[1] != UF2_MAGIC1) || (end_magic != UF2_MAGIC_END)) {
    DG_INC (nonuf2);
    return UF2_NOT_UF2;
  }
  if (((h[2] & UF2_FLAG_FAMILYID) == 0U) || (h[7] != UF2_FAMILY_STM32F4) ||
      ((h[2] & UF2_FLAG_NOFLASH) != 0U)) {
    DG_INC (bad_family);
    return UF2_REJECTED;
  }

  addr  = h[3];
  len   = h[4];
  blkno = h[5];
  total = h[6];

  if ((len == 0U) || (len > UF2_MAX_PAYLOAD) || ((len & 3U) != 0U) || ((addr & 3U) != 0U) ||
      (addr < UF2_APP_BASE) || (addr >= UF2_APP_END) || (len > (UF2_APP_END - addr))) {
    DG_INC (bad_range);                           /* address outside the application scope */
    return UF2_REJECTED;
  }
  if ((total == 0U) || (total > UF2_MAX_BLOCKS) || (blkno >= total)) {
    DG_INC (bad_blkno);
    return UF2_REJECTED;
  }

  if (g_total == 0U) {
    g_total = total;                               /* first valid block */
    DG_SET (uf2_total, total);
#if UF2_DIAG
    uf2_diag.cyc_first = flash_Cycles ();
#endif
  } else if (g_total != total) {
    DG_INC (total_mismatch);
    return UF2_REJECTED;
  }

#if UF2_DIAG
  crc = crc32_calc (&b[32], len);
#endif

  if (g_seen[blkno >> 5] & (1UL << (blkno & 31U))) {
#if UF2_DIAG
    uf2_diag.uf2_dup++;                            /* do not write the repeated block a second time */
    if (uf2_crc[blkno] != crc) { uf2_diag.uf2_dup_diff++; }
#endif
    return UF2_DUPLICATE;
  }

#if UF2_VECTORS_LAST
  if (addr == UF2_APP_BASE) {                      /* вектора: запишем в самом конце */
    memcpy (g_hold, &b[32], len);
    g_hold_len   = len;
    g_hold_blkno = blkno;
    g_hold_valid = true;
  } else
#endif
  {
    rc = prog_block (addr, len, &b[32]);
    if (rc != PB_OK) {
      DG_INC (flash_err);                         /* vectors: we’ll write them down at the very end */
      DG_SET (flash_last_err, rc);
      return UF2_FLASH_ERROR;
    }
  }

  g_seen[blkno >> 5] |= (1UL << (blkno & 31U));
  g_received++;
#if UF2_DIAG
  uf2_crc[blkno] = crc;
  uf2_diag.uf2_ok++;
  uf2_diag.cyc_last = flash_Cycles ();
  if (addr < uf2_diag.app_lo)          { uf2_diag.app_lo = addr; }
  if ((addr + len) > uf2_diag.app_hi)  { uf2_diag.app_hi = addr + len; }
#endif

  if (g_received == g_total) {
#if UF2_VECTORS_LAST
    if (g_hold_valid) {                           /* all the others are in place: writing the vectors */
      rc = prog_block (UF2_APP_BASE, g_hold_len, (const uint8_t *)g_hold);
      if (rc != PB_OK) {
        g_seen[g_hold_blkno >> 5] &= ~(1UL << (g_hold_blkno & 31U));
        g_received--;                             /* file is NOT completed */
        DG_INC (flash_err);
        DG_SET (flash_last_err, rc);
        return UF2_FLASH_ERROR;
      }
      g_hold_valid = false;
#if UF2_DIAG
      uf2_diag.cyc_last = flash_Cycles ();
#endif
    }
#endif
    g_complete = true;
#if UF2_DIAG
    diag_finish ();
#endif
    return UF2_COMPLETE;
  }
  return UF2_ACCEPTED;
}