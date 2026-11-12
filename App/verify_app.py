
#!/usr/bin/env python3
"""Verify STM32F401 application BIN and UF2 after build."""

import struct
import sys
from pathlib import Path

APP_BASE = 0x08010000
FLASH_END = 0x08040000       # exclusive
RAM_START = 0x20000000
RAM_END = 0x20010000         # exclusive; initial SP may equal RAM_END
EXPECTED_FAMILY_ID = 0x57755A57

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
UF2_BLOCK_SIZE = 512


def fail(message):
    raise ValueError(message)


def check_bin(path):
    data = path.read_bytes()

    if len(data) < 8:
        fail(f"BIN is too small: {len(data)} bytes")

    sp, reset = struct.unpack_from("<II", data)

    print(f"BIN size:   {len(data)} bytes")
    print(f"Initial SP: 0x{sp:08X}")
    print(f"Reset:      0x{reset:08X}")

    if not (RAM_START < sp <= RAM_END):
        fail("Initial SP is outside STM32F401 SRAM")

    if sp & 7:
        fail("Initial SP is not 8-byte aligned")

    if not (reset & 1):
        fail("Reset Handler address must have Thumb bit set")

    reset_addr = reset & ~1

    if not (APP_BASE <= reset_addr < FLASH_END):
        fail("Reset Handler is outside application Flash region")

    print("BIN vectors: OK")


def check_uf2(path):
    data = path.read_bytes()

    if not data or len(data) % UF2_BLOCK_SIZE:
        fail("UF2 size is invalid (not a multiple of 512 bytes)")

    block_count = len(data) // UF2_BLOCK_SIZE
    seen = set()
    addresses = []
    family_ids = set()
    declared_counts = set()

    for offset in range(0, len(data), UF2_BLOCK_SIZE):
        block = data[offset:offset + UF2_BLOCK_SIZE]

        magic0, magic1, flags, target, payload_size, number, total, family = (
            struct.unpack_from("<IIIIIIII", block)
        )

        if magic0 != UF2_MAGIC0 or magic1 != UF2_MAGIC1:
            fail(f"Invalid UF2 magic in block {number}")

        if number in seen:
            fail(f"Duplicate UF2 block number: {number}")

        seen.add(number)
        declared_counts.add(total)
        addresses.append(target)

        if not (flags & UF2_FLAG_FAMILY_ID_PRESENT):
            fail(f"Family ID flag missing in block {number}")

        family_ids.add(family)

        if not (0 < payload_size <= 476):
            fail(f"Invalid payload size in block {number}")

    if seen != set(range(block_count)):
        fail("UF2 block numbering is incomplete")

    if declared_counts != {block_count}:
        fail("UF2 declared block count does not match file")

    if family_ids != {EXPECTED_FAMILY_ID}:
        fail(
            "Unexpected UF2 Family ID: "
            + ", ".join(f"0x{x:08X}" for x in sorted(family_ids))
        )

    if min(addresses) != APP_BASE:
        fail(
            f"UF2 start address is 0x{min(addresses):08X}, "
            f"expected 0x{APP_BASE:08X}"
        )

    if max(addresses) >= FLASH_END:
        fail("UF2 contains a target address outside application Flash")

    print(f"UF2 size:   {len(data)} bytes")
    print(f"UF2 blocks: {block_count}")
    print(f"Family ID:  0x{EXPECTED_FAMILY_ID:08X}")
    print(f"Start:      0x{min(addresses):08X}")
    print("UF2 structure: OK")


def main():
    if len(sys.argv) != 4:
        print(
            "Usage: verify_app.py <App.uf2> <App.bin> <report.txt>",
            file=sys.stderr,
        )
        return 2

    uf2_path = Path(sys.argv[1])
    bin_path = Path(sys.argv[2])
    report_path = Path(sys.argv[3])

    try:
        print("=== Checking application BIN ===")
        check_bin(bin_path)

        print("\n=== Checking application UF2 ===")
        check_uf2(uf2_path)

        report_path.write_text(
            "PASS: BIN vectors and UF2 structure verified\n",
            encoding="utf-8",
        )
        print("\nALL CHECKS PASSED")
        return 0

    except (OSError, ValueError, struct.error) as exc:
        report_path.unlink(missing_ok=True)
        print(f"\nERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
