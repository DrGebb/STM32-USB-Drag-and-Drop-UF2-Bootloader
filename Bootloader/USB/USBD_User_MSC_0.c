/*------------------------------------------------------------------------------
 * USBD_User_MSC_0.c (based on the MDK Middleware ::USB:Device:MSC template) 
* 
* Virtual disk 1 MiB, FAT12. Only service sectors are stored in RAM 
* (boot, FAT, root directory, README). Everything the host writes to the data area is 
* is passed to the uf2_process_block() bootloader kernel and is not saved on disk. 
* bootctl.c handles completion
 *----------------------------------------------------------------------------*/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rl_usb.h"
#include "cmsis_os2.h"
#include "uf2_loader.h"

/* MSC-level statistics and log (enabled by default, along with UF2_DIAG) */
#ifndef MSC_DIAG
#define MSC_DIAG        UF2_DIAG
#endif

/* ---------------- Disk geometry------------------------------------------
* LBA 0        boot sector
* LBA 1–6     FAT12 (6 sectors)
* LBA 7–8     root directory (32 entries)
* LBA 9...     data area; cluster 2 = LBA 9 (contains README.TXT)
* Cluster = 1 sector. 2039 clusters < 4085, which means it is FAT12.
 *----------------------------------------------------------------------------*/
#define SECTOR_SIZE     512U
#define TOTAL_SECTORS   2048U
#define FAT_LBA         1U
#define FAT_SECTORS     6U
#define ROOT_LBA        (FAT_LBA + FAT_SECTORS)        /* 7  */
#define ROOT_SECTORS    2U
#define FS_DATA_LBA     (ROOT_LBA + ROOT_SECTORS)      /* 9  */
#define SVC_SECTORS     (FS_DATA_LBA + 1U)             /* 10: service + README */

#if MSC_DIAG
typedef struct {
  uint32_t wr_calls, wr_blocks;                 /* Write calls and the sectors involved in them */
  uint32_t wr_boot, wr_fat, wr_root, wr_data;   /* sectors by regions            */
  uint32_t wr_oob;                              /* write beyond the disk limits */
  uint32_t cnt_hist[5];                         /* number of sectors per call: 1, 2, 3, 4, >=5 */
  uint32_t data_seq_breaks;                     /* Data LBAs did not arrive in sequential order */
} msc_stats_t;
typedef struct { uint32_t lba; uint32_t cnt; uint32_t tick; } msc_log_t;

volatile msc_stats_t msc_stats;
msc_log_t            msc_log[128];              /* ring buffer of recent Write calls */
volatile uint32_t    msc_log_head;
static uint32_t      data_next_lba;
#endif

static uint8_t  svc[SVC_SECTORS * SECTOR_SIZE] __attribute__((aligned(4)));
static uint32_t block_buf[SECTOR_SIZE / 4U];

/* ---------------- Building the disk image -------------------------------------*/
static void msc_build_image (void) {
  static const char readme[] = "Black Pill F401 USB MSC / UF2 loader\r\n";
  static const uint8_t bpb[62] = {
    0xEB, 0x3C, 0x90,
    'M','S','D','O','S','5','.','0',
    0x00, 0x02,                        /* bytes per sector = 512            */
    0x01,                               /* sectors per cluster = 1         */
    0x01, 0x00,                        /* reserved sectors = 1    */
    0x01,                              /* FAT number = 1 */
    0x20, 0x00,                        /* entries in the root = 32            */
    0x00, 0x08,                        /* total sectors = 2048           */
    0xF8,                               /* media descriptor                */
    0x06, 0x00,                        /* sectors per FAT = 6             */
    0x01, 0x00,                         /* sectors per track             */
    0x01, 0x00,                        /* heads */
    0x00, 0x00, 0x00, 0x00,             /* hidden sectors                 */
    0x00, 0x00, 0x00, 0x00,            /* total sectors (32-bit) = 0     */
    0x80, 0x00, 0x29,
    0x12, 0x34, 0x56, 0x78,             /* volume ID                       */
    'M','S','C',' ','D','I','S','K',' ',' ',' ',
    'F','A','T','1','2',' ',' ',' '
  };
  uint8_t *root;
  uint32_t len = (uint32_t)sizeof(readme) - 1U;

  memset (svc, 0, sizeof(svc));

  memcpy (svc, bpb, sizeof(bpb));
  svc[510] = 0x55; svc[511] = 0xAA;

/* FAT12: entries 0 and 1 are reserved; entry 2 (README) = end of chain */
  svc[FAT_LBA * SECTOR_SIZE + 0] = 0xF8;
  svc[FAT_LBA * SECTOR_SIZE + 1] = 0xFF;
  svc[FAT_LBA * SECTOR_SIZE + 2] = 0xFF;
  svc[FAT_LBA * SECTOR_SIZE + 3] = 0xFF;
  svc[FAT_LBA * SECTOR_SIZE + 4] = 0x0F;

  root = &svc[ROOT_LBA * SECTOR_SIZE];
  memcpy (&root[0], "MSC DISK   ", 11);
  root[11] = 0x08;                                      /* volume label */

  memcpy (&root[32], "README  TXT", 11);
  root[32 + 11] = 0x20;
  root[32 + 16] = 0x59; root[32 + 17] = 0x5D;
  root[32 + 18] = 0x59; root[32 + 19] = 0x5D;
  root[32 + 24] = 0x59; root[32 + 25] = 0x5D;
  root[32 + 26] = 0x02; root[32 + 27] = 0x00;           /* first cluster = 2 */
  root[32 + 28] = (uint8_t)len;

  memcpy (&svc[FS_DATA_LBA * SECTOR_SIZE], readme, len); /* cluster 2 */
}

/* ---------------- MSC class callbacks ------------------------------------*/
void USBD_MSC0_Initialize (void) {
#if MSC_DIAG
  memset ((void *)&msc_stats, 0, sizeof(msc_stats));
  memset (msc_log, 0, sizeof(msc_log));
  msc_log_head  = 0U;
  data_next_lba = 0U;
#endif
  uf2_init ();
  msc_build_image ();
}

void USBD_MSC0_Uninitialize (void) {
}

bool USBD_MSC0_GetCacheInfo (uint32_t *buffer, uint32_t *size) {
  *buffer = (uint32_t)block_buf;
  *size   = sizeof(block_buf);
  return true;
}

bool USBD_MSC0_GetMediaCapacity (uint32_t *block_count, uint32_t *block_size) {
  *block_count = TOTAL_SECTORS;
  *block_size  = SECTOR_SIZE;
  return true;
}

bool USBD_MSC0_Read (uint32_t lba, uint32_t cnt, uint8_t *buf) {
  uint32_t i, l;

  if ((lba + cnt) > TOTAL_SECTORS) { return false; }

  for (i = 0; i < cnt; i++) {
    l = lba + i;
    if (l < SVC_SECTORS) {
      memcpy (&buf[i * SECTOR_SIZE], &svc[l * SECTOR_SIZE], SECTOR_SIZE);
    } else {
      memset (&buf[i * SECTOR_SIZE], 0, SECTOR_SIZE);  /* we do not store data */
    }
  }
  return true;
}

bool USBD_MSC0_Write (uint32_t lba, uint32_t cnt, const uint8_t *buf) {
  uint32_t i, l;
#if MSC_DIAG
  volatile msc_stats_t *s = &msc_stats;
  uint32_t h;
#endif

  if ((lba + cnt) > TOTAL_SECTORS) {
#if MSC_DIAG
    s->wr_oob++;
#endif
    return false;
  }

#if MSC_DIAG
  s->wr_calls++;
  s->wr_blocks += cnt;
  s->cnt_hist[(cnt > 5U) ? 4U : (cnt - 1U)]++;
  h = msc_log_head++ & 127U;
  msc_log[h].lba  = lba;
  msc_log[h].cnt  = cnt;
  msc_log[h].tick = osKernelGetTickCount();
#endif

  for (i = 0; i < cnt; i++) {
    l = lba + i;
    if (l < FS_DATA_LBA) {
#if MSC_DIAG
      if      (l == 0U)       { s->wr_boot++; }
      else if (l < ROOT_LBA)  { s->wr_fat++;  }
      else                    { s->wr_root++; }
#endif
      memcpy (&svc[l * SECTOR_SIZE], &buf[i * SECTOR_SIZE], SECTOR_SIZE);
    } else {
#if MSC_DIAG
      s->wr_data++;
      if ((s->wr_data > 1U) && (l != data_next_lba)) { s->data_seq_breaks++; }
      data_next_lba = l + 1U;
#endif
      if (l < SVC_SECTORS) {                      /* the README sector can also be overwritten */
        memcpy (&svc[l * SECTOR_SIZE], &buf[i * SECTOR_SIZE], SECTOR_SIZE);
      }
      (void)uf2_process_block (&buf[i * SECTOR_SIZE]);
    }
  }
  return true;
}

uint32_t USBD_MSC0_CheckMedia (void) {
  return USBD_MSC_MEDIA_READY;
}