#ifndef UF2_LOADER_H
#define UF2_LOADER_H

#include <stdint.h>
#include <stdbool.h>

/* 1: test layer (counters, CRC table, measurements, pattern verification). 0: production build. */
#ifndef UF2_DIAG
#define UF2_DIAG            0
#endif
/* check "word in flash == its address" for the test file (only when UF2_DIAG is enabled) */
#ifndef UF2_PATTERN_CHECK
#define UF2_PATTERN_CHECK   0
#endif

/* 1: The block containing the vector table (at address UF2_APP_BASE) is not written immediately;
* instead, it is held in RAM and programmed LAST. This ensures that an interrupted copy
* operation does not leave "valid" vectors pointing to an incompletely written application. */
#ifndef UF2_VECTORS_LAST
#define UF2_VECTORS_LAST    1
#endif

#define UF2_APP_BASE        0x08010000UL    /* start of application */
#define UF2_APP_END         0x08040000UL    /* end of flash (exclusive) */
#define UF2_MAX_BLOCKS      1024U           /* maximum number of blocks in a file */

typedef enum {
    UF2_NOT_UF2 = 0,    /* not a UF2 block (host-generated noise)          */
    UF2_REJECTED,       /* UF2, but it failed validation (family, address, number) */
    UF2_DUPLICATE,      /* this block has already been accepted; it is not written again */
    UF2_FLASH_ERROR,    /* erase/write/read-back error             */
    UF2_ACCEPTED,       /* block recorded                                   */
    UF2_COMPLETE        /* the block has been recorded, and it was the last missing one */
} uf2_result_t;

void         uf2_init(void);
uf2_result_t uf2_process_block(const uint8_t *blk);    /* blk: 512 bytes */

bool         uf2_is_complete(void);                    /* all file blocks have been accepted */
uint32_t     uf2_blocks_received(void);
uint32_t     uf2_blocks_total(void);
bool         uf2_app_vectors_ok(void);                /* SP in RAM, Thumb reset vector in the application */

#if UF2_DIAG
typedef struct {
    uint32_t nonuf2;                              /* data block does not resemble UF2 */
    uint32_t bad_family, bad_range, bad_blkno;    /* discarded during checks          */
    uint32_t total_mismatch;                      /* num_blocks differs from the first one */
    uint32_t uf2_ok;                              /* received and recorded from VARIOUS blocks */
    uint32_t uf2_dup, uf2_dup_diff;               /* repetitions / repetitions with a different CRC */
    uint32_t uf2_total;                           /* num_blocks from the first block */
    uint32_t uf2_complete;                        /* 1, when ok == total            */
    uint32_t uf2_digest;                          /* CRC32 of the table uf2_crc[0..total-1] */
    uint32_t flash_err;                           /* blocks rejected due to a flash error */
    uint32_t flash_last_err;                      /* 1 range, 2 erase, 3 write, 4 read back */
    uint32_t verify_err;                          /* read-back errors             */
    uint32_t sect_erased;                         /* mask of erased sectors          */
    uint32_t erase_cnt;                           /* number of erases */
    uint32_t erase_cycles, prog_cycles;           /* DWT clocks */
    uint32_t prog_words;                          /* words recorded                  */
    uint32_t cyc_first, cyc_last;                 /* DWT cycles for the first/last block */
    uint32_t app_lo, app_hi;                      /* boundaries of recorded data [lo, hi) */
    uint32_t app_vec_ok;
    uint32_t pat_checked, pat_err;                /* template check                */
} uf2_diag_t;

extern volatile uf2_diag_t uf2_diag;
extern uint32_t            uf2_crc[UF2_MAX_BLOCKS];   /* CRC32 of the payload by block number */
#endif

#endif /* UF2_LOADER_H */