# UF2 USB Bootloader for STM32F401 (Black Pill)

A bootloader that shows up on the PC as a USB drive. A new firmware image is
installed by dragging `App.uf2` into the drive window. No programmer, no
drivers, no utilities on the user's side.

Built with: STM32F401xC (Black Pill), Arm MDK-Middleware USB (device, MSC class),
CMSIS-Driver_STM32 (USB), CMSIS-RTOS2 (RTX), registers and LL without HAL in the
bootloader logic.

> **Verification status.** Verified on a Black Pill F401 under Windows: reception
> of 1 KB and 192 KB test files (in order and with shuffled block order), flash
> programming with read-back, installing a real blinky application and starting
> it. Not verified: Linux and macOS, other boards, behaviour at the limit of the
> flash erase time (see "Limitations"). The section on porting to other STM32
> families is a guide to what has to change, not a verified result.


## Toolchain & Environment

* **Environment:** VS Code
* **Toolchain:** Arm Compiler 6.24 (Keil MDK v6)
* **Build System:** CMSIS-Toolbox (`csolution` / `cbuild`)
* **Target MCU:** STM32F401RCT6 (LQFP64, 256 KB Flash, 64 KB RAM)
* **USB:** Arm MDK-Middleware (USB Device, MSC)
* **USB Driver:** CMSIS-Driver_STM32
* **RTOS:** RTX5 (CMSIS-RTOS2)
* **File System:** Not required


---

## 1. Contents

| File | Purpose |
|---|---|
| `uf2_loader.c/.h` | Loader core. Knows nothing about USB. Receives one 512-byte block at a time, validates it, erases a sector on first touch, programs flash, reads it back, and keeps a map of received blocks. |
| `USBD_User_MSC_0.c` | Virtual disk (FAT12, 1 MiB) and MSC class callbacks. Everything the host writes to the data area is passed to `uf2_process_block()`. |
| `flash_ll.c/.h` | Register-level flash driver: unlock, sector erase, block write, cache flush, `flash_SectorOfAddr()`. **Specific to one MCU.** |
| `bootctl.c/.h` | Bootloader entry by the PA0 button, application check, hand-over of control, and the "pause -> USB detach -> reset" thread. |
| `make_test_uf2.py` | **Tests only.** Generator of test UF2 files. Not part of the production chain. |
| `uf2conv.py` | `.bin` to `.uf2` converter from the Microsoft repository (`microsoft/uf2`, `utils` folder). Used when building the application. |


---

## 2. Memory map

```
Flash (256 KB)
0x08000000 .. 0x0800FFFF   sectors 0..3 (4 x 16 KB)   bootloader (64 KB)
0x08010000 .. 0x0801FFFF   sector 4 (64 KB)            application
0x08020000 .. 0x0803FFFF   sector 5 (128 KB)           application
                           192 KB in total for the application
```

The bootloader takes about 25 KB of code in the test build (with `UF2_DIAG 1`,
size optimization and LTO), so the 64 KB reserve is generous.

Virtual disk (only the first 10 sectors, 5 KB, live in RAM):

| LBA | Contents |
|---|---|
| 0 | Boot sector (BPB) |
| 1..6 | FAT12 |
| 7..8 | Root directory (32 entries) |
| 9... | Data area, cluster = 1 sector. Cluster 2 (LBA 9) holds `README.TXT`. |

The disk has 2048 sectors (1 MiB) and 2039 clusters, so it is FAT12. Anything
written to the data area is **not stored**; it is parsed as a stream of UF2
blocks. Reading the data area returns zeros.

---

## 3. How it works

### 3.1. Start-up

```
Reset
  └─ main(): boot_early_check()   <-- the very first line, before clocks/USB/RTOS
        ├─ PA0 = 0 (button pressed)            -> stay in the bootloader (drive)
        ├─ application not valid               -> stay in the bootloader
        └─ PA0 = 1 and application valid       -> jump to the application
```

* PA0 is read at register level with the internal pull-up enabled, 16 samples in a
  row; the button counts as pressed only if all 16 are zero. After the read GPIOA
  is returned to its reset state and its clock is switched off.
* "Application valid" (`uf2_app_vectors_ok()`): the initial SP is inside RAM
  (`0x20000000..0x20010000`), the Reset vector is inside the application area and
  has bit 0 set (Thumb). This is not an integrity check (there is no CRC).
* The jump (`start_app`, a naked function): `BASEPRI`, `FAULTMASK` and `CONTROL`
  are zeroed, the application's `MSP` is loaded, `cpsie i`, then `bx` to the
  Reset_Handler. Beforehand IRQs are disabled, SysTick is stopped, NVIC
  (ICER/ICPR) and the SysTick/PendSV pending bits are cleared, and the
  application address is written to `VTOR`.
* `cpsie i` is required: without it the application starts with `PRIMASK = 1`
  and never receives an interrupt.

Because the check runs before any initialization, there is almost nothing to
undo when jumping.

### 3.2. Receiving a file

The host sees a drive and writes `App.uf2` to it. To the host this is an ordinary
sector write. In `USBD_MSC0_Write()` the data-area sectors are passed to
`uf2_process_block()`. The core does the following:

1. **Not UF2** (no magic numbers): ignored. This is host noise (Windows'
   `System Volume Information` and the like).
2. **Checks**: the family ID flag is set and equals `0x57755A57` (STM32F4); the
   "not main flash" flag is clear; the address is 4-byte aligned, the payload
   length is a multiple of 4 and at most 476; the address range lies entirely
   within `UF2_APP_BASE..UF2_APP_END`; the block number is below the total block
   count; the total block count does not change from block to block. Failing any
   of these: the block is rejected.
3. **Duplicate block** (by the bitmap): not written a second time.
4. **Write**: if the sector has not been erased in this session it is erased
   (on first touch), then the block is programmed word by word (x32), then read
   back from flash (after flushing the caches) and compared. Any error means the
   block is not counted as received and an error counter is incremented.
5. **The vector block** (address `0x08010000`) is not written immediately. It is
   held in RAM (476 bytes) and programmed **last**, once all other blocks are in
   place (`UF2_VECTORS_LAST`).
6. When as many distinct blocks have been received as the file declares, the core
   returns `UF2_COMPLETE` and `uf2_is_complete()` becomes true.

The order of blocks in the file does not matter: the address is stored in the
block itself.

### 3.3. After reception

The thread `boot_supervisor_thread()` waits for `uf2_is_complete()` and then:

1. pauses for `BOOT_POST_COMPLETE_MS` (3 s by default) so Windows can finish
   writing the directory;
2. if the vectors are valid: `USBD_Disconnect()`, a pause of
   `BOOT_USB_DETACH_MS` (1 s), then `NVIC_SystemReset()`;
3. after the reset `boot_early_check()` starts the application.

A software reset is used on purpose: it clears the peripherals, NVIC, SysTick,
RTX and USB, so there is no need to jump from an RTOS context.

### 3.4. Interrupted copy

The first block other than the vector block erases sector 4. Therefore:

* interrupted before that point: the old application is intact;
* interrupted later: the sector is erased, there are no vectors, after a reset
  `uf2_app_vectors_ok()` is false and the bootloader stays in drive mode on its
  own, without the button;
* an error while writing the vectors at the very end: `complete` does not
  happen; re-sending block 0 writes it and completes the file.

Nothing else needs to be done: just copy the file again (reset the board if
necessary).

---

## 4. Integrating into a project

### 4.1. main and threads (example)

```c
int main(void) {
    boot_early_check();            /* FIRST line: either jumps or returns */

    HAL_Init();                    /* ordinary initialization follows */
    SystemClock_Config();          /* USB needs 48 MHz */
    MX_GPIO_Init();
    MX_USB_OTG_FS_PCD_Init();

    osKernelInitialize();
    osThreadNew(usb_thread, NULL, &usb_attr);
    osThreadNew(boot_supervisor_thread, NULL, &sup_attr);   /* stack of about 512 bytes */
    osKernelStart();
}

static void usb_thread(void *arg) {
    USBD_Initialize(0U);           /* calls USBD_MSC0_Initialize() -> uf2_init() inside */
    USBD_Connect(0U);
    for (;;) { osDelay(100U); }
}
```

### 4.2. USB and RTOS settings

| Parameter | Value | Why |
|---|---|---|
| `USBD0_MAX_PACKET0` | **64** | The ST driver and HAL cannot do EP0 transfers longer than the max packet size; with 8 enumeration may fail. |
| `USBD0_CORE_THREAD_STACK_SIZE` | at least 2048 | USB core thread. If enumeration is unstable, use 4096. |
| `USBD_MSC0_THREAD_STACK_SIZE` | 1024 | The `Read`/`Write` callbacks run in this thread. |
| VID/PID | **your own** | Do not keep the Keil `0xC251`. For experiments the pid.codes test pair `0x1209/0x0001` works (shared, not for a product). A product needs its own VID or a licensed PID. |
| Serial number | enabled | Windows binds an MSC drive by its serial number. |
| `OS_DYNAMIC_MEM_SIZE` (RTX) | trim to what is needed (about 6 KB instead of the default 32 KB) | Frees RAM. |
| Clocks | SYSCLK 84 MHz, USB 48 MHz | As in the working project. |

`USBD_User_MSC_0.c` from this repository **replaces** the template
`USBD_User_MSC_0.c` entirely (without the File System branches).

---

## 5. Configuration (defines)

Switches that can be overridden with `-D` (all wrapped in `#ifndef`):

| Define | File | Default | Meaning |
|---|---|---|---|
| `UF2_DIAG` | `uf2_loader.h` | 1 | 1: test layer (`uf2_diag`, 4 KB CRC table, DWT timing, digest). 0: production build, all of it removed. |
| `UF2_PATTERN_CHECK` | `uf2_loader.h` | 1 | Only with `UF2_DIAG 1`. Checks that each flash word equals its own address. Only suitable for `make_test_uf2.py` files; set to 0 for real firmware. |
| `UF2_VECTORS_LAST` | `uf2_loader.h` | 1 | 1: the vector block is written last (protection against an interrupted copy). |
| `MSC_DIAG` | `USBD_User_MSC_0.c` | = `UF2_DIAG` | `msc_stats` counters and the `msc_log` journal of the last 128 `Write` calls. |
| `BOOT_AUTO_RESET` | `bootctl.h` | 1 | 1: after reception, pause, USB detach and reset. 0: no reset (for tests, so the statistics are not lost). |
| `BOOT_POST_COMPLETE_MS` | `bootctl.h` | 3000 | Pause after `COMPLETE`, ms. |
| `BOOT_USB_DETACH_MS` | `bootctl.h` | 1000 | Pause after `USBD_Disconnect()` before the reset, ms. |

Constants tied to the hardware and the layout (plain `#define`s, `-D` does not
apply):

| Define | File | Value | Notes |
|---|---|---|---|
| `UF2_APP_BASE` / `UF2_APP_END` | `uf2_loader.h` | `0x08010000` / `0x08040000` | Application area. Change together with `flash_SectorOfAddr()` and the application linker script. |
| `UF2_MAX_BLOCKS` | `uf2_loader.h` | 1024 | Size of the block map and the CRC table. 192 KB at 256 bytes per block is 768 blocks. |
| `UF2_FAMILY_STM32F4` | `uf2_loader.c` | `0x57755A57` | Family ID. Must match `-f` of `uf2conv.py`. |
| `FLH_MAX_SECTOR_NUM` | `flash_ll.h` | `FLH_SECTOR_5` | Last sector of the F401xC. |
| `TOTAL_SECTORS`, `FAT_SECTORS`, etc. | `USBD_User_MSC_0.c` | 2048, 6 | **The disk size cannot be changed with one define:** the same numbers are also stored as bytes in the `bpb[]` array. Change them consistently. |

What is not configurable by a define: the button pin (PA0 is hard-coded in
`bootctl.c`).

---

## 6. Usage

### 6.1. Updating (for service staff)

1. Hold the button (PA0 to ground) and press reset, or power the board on with the
   button held. If the board has no application (or it is damaged), the button is
   not needed.
2. A drive with `README.TXT` appears. The button can be released.
3. Drag `App.uf2` into the drive window.
4. Wait: reception, erase and write take about 3.5 seconds for a 192 KB file,
   then the pause and the reset. The drive disappears and the application starts.
5. If something goes wrong (copy failure, power loss): copy the file again. If
   needed, reset the board or cycle the power.

If the button is still held at the moment of the automatic reset, the bootloader
stays in drive mode (this is by design).

### 6.2. Building the application

1. **Linker.** The application's flash region starts at `0x08010000` and is at
   most `0x00030000` (192 KB) in size.
2. **VTOR.** In the application's `system_stm32f4xx.c` the vector table must point
   to `0x08010000` (in the ST template this is `USER_VECT_TAB_ADDRESS` and
   `VECT_TAB_OFFSET 0x10000`; check the names against your version of the file).
   If it is left at `0x0`, the application will put `VTOR` back to the bootloader
   and the first interrupt will land in a foreign handler.
3. **BIN.** Get `App.bin` from the `.axf` (in a csolution project it is produced as
   an output, or `fromelf --bin --output App.bin App.axf`).
4. **UF2.** Convert with the **mandatory** `-f`:

```
python uf2conv.py -c -b 0x08010000 -f 0x57755A57 -o App.uf2 App.bin
```

Without `-f` the family in the block header is 0 and the family ID flag is not
set. The loader then rejects every block as `bad_family` (verified).

An example build step in `*.cproject.yml` (csolution):

```yaml
executes:
    - execute: ConvertToUF2
      run: python $input(1)$ -c -b 0x08010000 -f 0x57755A57 -o $output(0)$ $input(0)$
      input:
        - $OutDir()$/App.bin
        - $ProjectDir()$/uf2conv.py
      output:
        - $ProjectDir()$/App.uf2
```

### 6.3. Checking the finished file

```
python uf2conv.py -i App.uf2
```

Expected: `Family ID is STM32F4, hex value is 0x57755a57` and
`Target Address is 0x08010000`.

The first 8 bytes of `App.bin` are SP and Reset:

```
python -c "import struct;print([hex(x) for x in struct.unpack('<II',open('App.bin','rb').read(8))])"
```

SP of the form `0x2000xxxx` (at most `0x20010000`), Reset of the form
`0x0801xxxx` with the least significant bit set.

---

## 7. Python tools

Python 3 is required.

### 7.1. `uf2conv.py` (production chain)

From the Microsoft repository. The options we use:

| Option | Meaning |
|---|---|
| `-c` | convert mode (without it the script tries to find and flash a device itself) |
| `-b ADDR` | base address (mandatory for `.bin`, which has no addresses) |
| `-f FAMILY` | family ID as a number or a name (`STM32F4`). **Mandatory for our bootloader.** |
| `-o FILE` | output file |
| `-i` | show information about a `.uf2` |

Full list: `python uf2conv.py --help`. By default the data goes 256 bytes per block.

### 7.2. `make_test_uf2.py` (tests only)

Produces test files in which every 32-bit data word equals its own flash address.
Because of this, any word read back from flash shows at once whether it landed in
the right place. It is not used for real firmware.

| Option | Default | Meaning |
|---|---|---|
| `--size` | 1024 | "firmware" size in bytes, multiple of 4 |
| `--base` | `0x08010000` | base address |
| `--family` | `0x57755A57` | family ID |
| `--payload` | 256 | data bytes per block (1..476) |
| `--shuffle N` | off | seed; shuffle the block order in the file |
| `--vectors` | off | the first two words become a valid SP and Reset (imitates an application) |
| `--out` | `test` | name prefix: `.uf2`, `.bin`, `_blocks.csv` |

It prints a `digest`, which must match `uf2_diag.uf2_digest` on the board.

```
python make_test_uf2.py --size 1024   --out t1k                           # 4 blocks
python make_test_uf2.py --size 196608 --out t192k                         # full size
python make_test_uf2.py --size 196608 --shuffle 1 --out t192k_shuf        # different block order
python make_test_uf2.py --size 8192   --vectors --out tvec                # "application" with valid vectors
python make_test_uf2.py --size 4096 --base 0x08008000 --out tboot         # bootloader area: everything is rejected (bad_range)
python make_test_uf2.py --size 1024 --base 0x08010002 --out tunal         # unaligned address (bad_range)
```

Files made with `--vectors` give `app_vec_ok = 1`, `pat_err = 2` (two vector words
are replaced, which is expected) and, with `BOOT_AUTO_RESET 1`, a reset after the
pause.

---

## 8. Testing and diagnostics

Build with `UF2_DIAG 1`. Look at the results in the debugger Watch window:

* `uf2_diag`: core counters (`uf2_ok`, `uf2_total`, `uf2_complete`, `uf2_dup`,
  `nonuf2`, `bad_family`, `bad_range`, `flash_err`, `flash_last_err`,
  `verify_err`, `sect_erased`, `erase_cnt`, `prog_words`, `app_lo`/`app_hi`,
  `app_vec_ok`, `pat_checked`/`pat_err`, `uf2_digest`) and timing in DWT cycles.
  `flash_last_err`: 1 range, 2 erase, 3 program, 4 read-back.
* `msc_stats` and `msc_log` (with `MSC_DIAG 1`): how many sectors arrived in which
  disk area, whether they came in order, and the last 128 `Write` calls with
  `lba`/`cnt`.
* Measure time with DWT, not RTX ticks: while flash is being erased or programmed
  the CPU is stalled and the ticks do not advance. For seconds, divide the cycles
  by the SYSCLK frequency.

Expected values for a 192 KB test file without `--vectors`:
`uf2_ok = uf2_total = 768`, `uf2_complete = 1`,
`flash_err = verify_err = pat_err = 0`, `sect_erased = 0x30`, `erase_cnt = 2`,
`prog_words = 49152`, `uf2_digest = 0xCEBC9A3A`, `app_lo = 0x08010000`,
`app_hi = 0x08040000`, `app_vec_ok = 0`.

Measurements on the Black Pill (SYSCLK 84 MHz, computed from DWT cycles): erasing
sector 4 (64 KB) takes about 0.68 s, sector 5 (128 KB) about 1.34 s, programming
about 14 us per word (0.69 s for 192 KB), and the whole 192 KB file takes about
3.4 s from the first block to the last. Windows survived both stalls without
copy errors.

---

## 9. Limitations and known issues

* **One session per power-up.** After `COMPLETE` a second file in the same session
  is not accepted (duplicate blocks are ignored, a different `num_blocks` is
  rejected). A reset follows reception, so in practice this does not get in the
  way.
* **The CPU stalls while flash is erased or programmed** (single-bank flash, code
  executes from the same flash). While an erase is in progress, interrupts are not
  serviced. The host tolerates this, but the datasheet's maximum erase times are
  higher than the typical ones, and the margin to the host's timeouts should be
  judged from those.
* **Integrity is not checked.** UF2 carries no CRC of the whole image; the loader
  only verifies that every block was programmed and read back correctly and that
  the vectors look reasonable. There are no signatures or encryption.
* **Internal flash only**, application area only.


---

## 10. Porting to another STM32 family

The logic (`uf2_loader.c`, the MSC callbacks) is almost independent of the family.
Everything tied to a particular MCU is gathered in a few places. Work plan:

### 10.1. What to change

| Where | What is tied to the F401 | What to do |
|---|---|---|
| `flash_ll.c`: `flash_KEY_*`, `flash_EraseSector`, `flash_WriteBlock` | The `FLASH->KEYR/CR/SR` registers, the `PG`, `SER`, `SNB`, `PSIZE`, `STRT`, `LOCK` bits, the error flags `OPERR/WRPERR/PGAERR/PGPERR/PGSERR/BSY` | **Rewrite from the FLASH chapter of the new MCU's reference manual**, keeping the API: `flash_KEY_EnableAccess`, `flash_KEY_DisableAccess`, `flash_EraseSector`, `flash_WriteBlock`, `flash_FlushCaches`, `flash_SectorOfAddr`. The core calls nothing else. |
| `flash_ll.c`: **`flash_SectorOfAddr()`** | The sector table of the F401xC: address range 4 maps to sector 4 (64 KB), range 5 to sector 5 (128 KB) | See 10.2. |
| `flash_ll.c`: the programming unit | The F4 writes a word (x32 at VDD 2.7..3.6 V). Other families program differently (half-word, 64-bit double word, the 256-bit flash word on H7). | The programming unit determines what the block address and length must be a multiple of. Replace the conditions `(len & 3U)` and `(addr & 3U)` in `uf2_process_block()` with multiples of the new unit, and `flash_WriteBlock()` must pad the tail up to the boundary. |
| `flash_ll.c`: `flash_FlushCaches()` | Resetting the F4 ART caches (`LL_FLASH_*`) | On M7 cores (F7, H7) these are the core caches (`SCB_Invalidate*Cache`); on families without a cache the function is empty. It is needed so the read-back comes from flash, not from a cache. |
| `flash_ll.c`: `flash_CycleInit` / `flash_Cycles` | DWT `CYCCNT` | Present on M3/M4/M7. M0/M0+ have no DWT: replace with a timer or drop the timing (`UF2_DIAG 0`). |
| `uf2_loader.c`: `UF2_FAMILY_STM32F4` | The F4 family ID | Put in the new family's ID (10.3). Give the same value to `-f` of `uf2conv.py`. |
| `uf2_loader.c`: `uf2_app_vectors_ok()` | RAM `0x20000000..0x20010000` | The SP range from the new MCU's RAM map (H7, for example, has RAM at several addresses). |
| `uf2_loader.c`: `g_erased_mask` | A 32-bit mask: the sector number must be below 32 | Families with hundreds of pages need a larger bitmap. |
| `uf2_loader.h` | `UF2_APP_BASE`, `UF2_APP_END` | The new flash layout. The bootloader occupies the start of flash in whole sectors. |
| `bootctl.c`: `boot_key_pressed`, `boot_key_release` | `RCC->AHB1ENR`, `GPIOA->MODER/PUPDR/IDR`, `RCC->AHB1RSTR` | Bus and clock-enable register names differ between families (AHBENR, AHB2ENR, IOPENR, etc.). On F1 GPIO is configured through `CRL/CRH`, and the pull direction is also selected through `ODR`. The button pin follows the board. |
| `bootctl.c`: `start_app` | `BASEPRI`, `FAULTMASK`, `VTOR` | ARMv6-M cores (M0/M0+) have no `BASEPRI`/`FAULTMASK`; remove them from the assembly. `VTOR` is not present on every core or MCU: on an M0 without `VTOR` the vector table has to be relocated to RAM. |
| USB | `Driver_USBD0` for the F4 OTG_FS, the 48 MHz clock | A CMSIS USBD driver for the new MCU's peripheral, a 48 MHz clock from the PLL or HSI48, and the USB config files. The MSC callbacks (`USBD_User_MSC_0.c`) do not depend on the MCU. |
| Application | Linker script, `VTOR` | The flash region and `VECT_TAB_OFFSET`, as in 6.2. |

### 10.2. `flash_SectorOfAddr()` in detail

This is the most important function when porting. It does two things:

1. translates an address into the sector (or page) number used for erasing;
2. returns `-1` for everything that **does not belong to the application area**.
   This is a second line of defence: even if `UF2_APP_BASE/END` are misconfigured,
   the bootloader area is never erased.

For the F401xC it currently is:

```c
int32_t flash_SectorOfAddr(uint32_t addr) {
    if (addr >= 0x08010000UL && addr < 0x08020000UL) { return FLH_SECTOR_4; }
    if (addr >= 0x08020000UL && addr < 0x08040000UL) { return FLH_SECTOR_5; }
    return -1;
}
```

On other MCUs, first look at the layout in the reference manual:

* **Sectors of different sizes (F2, F4, F7, H7):** a table of boundaries. For
  example, the F407VG (1 MB) has sectors 0..3 of 16 KB, sector 4 of 64 KB and
  sectors 5..11 of 128 KB. The application starts at sector 4, and the function
  must cover sectors 4..11 with their boundaries.
* **Uniform pages (F0, F1, F3, G0, G4, L0, L1, L4, L5):** the number is computed
  by division: `(addr - 0x08000000) / PAGE_SIZE`, with a correction for the bank
  on dual-bank devices (the page number is often counted within the bank). The
  bootloader occupies a whole number of pages, and `UF2_APP_BASE` must lie on a
  page boundary.
* The returned value must be what `flash_EraseSector()` accepts, and below 32
  (otherwise change `g_erased_mask`, see above).
* If a UF2 block reaches into the neighbouring erase unit, the core handles it:
  `prog_block()` erases every unit from the one containing the first address of
  the block to the one containing the last (the loop `sec_first..sec_last`). For
  this, `flash_SectorOfAddr()` must answer correctly for both addresses.

### 10.3. Family ID

Taken from `uf2families.json` of the `microsoft/uf2` repository (values at the
time of writing):

| Family | ID |
|---|---|
| STM32F0 | `0x647824b6` |
| STM32F1 (F103) | `0x5ee21072` |
| STM32F2 | `0x5d1a0a2e` |
| STM32F3 | `0x6b846188` |
| STM32F4 (used here) | `0x57755a57` |
| STM32F7 | `0x53b80f00` |
| STM32G0 | `0x300f5633` |
| STM32G4 | `0x4c71240a` |
| STM32H5 | `0x4e8f1c5d` |
| STM32H7 | `0x6db66082` |
| STM32L0 | `0x202e3a91` |
| STM32L1 | `0x1e1f432d` |
| STM32L4 | `0x00ff6919` |
| STM32L5 | `0x04240bdf` |
| STM32WB | `0x70d16653` |
| STM32WL | `0x21460ff0` |

The registry also has IDs for individual models (for example the F407 and the
F411). Which one you choose does not matter as long as the loader and `-f` use the
same value.

### 10.4. Porting order

1. Decide the flash layout: how many whole sectors (pages) the bootloader takes and
   where the application starts. Put this into `UF2_APP_BASE/END` and into the
   application's linker script.
2. Rewrite `flash_ll.c` for the new MCU, starting with `flash_SectorOfAddr()`, the
   programming unit and the erase unit.
3. Put in the family ID (`uf2_loader.c` and `-f`).
4. Fix the RAM range in `uf2_app_vectors_ok()`.
5. Rewrite `boot_key_pressed()`/`boot_key_release()` for the new MCU's GPIO and RCC
   registers and your board's pin.
6. Check `start_app()` against the new MCU's core (see the table).
7. Bring up USB (driver and clocking) until the drive appears on the PC.
8. Run the test files (`--size 1024`, `--size 196608` or the maximum for the new
   area, `--shuffle`) and make sure the digest matches, `flash==bin` holds and
   `pat_err = 0`. Only then try a real application.
