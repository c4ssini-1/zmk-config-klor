#!/usr/bin/env python3
"""
Build the read-only USB drive image from docs/klor-keymap.html.

Plugging the left half into a computer makes a small drive appear next to the
keyboard, holding an offline copy of the keymap guide. This script builds that
drive as a complete FAT12 filesystem image and emits it as a C array; the
firmware serves the bytes verbatim and never parses a filesystem itself. The
host does all of that, which is why no FAT code is needed on the device --
Zephyr's USB_MASS_STORAGE only wants a disk_access driver.

Run after changing the guide:

    python3 scripts/gen_usb_disk.py

WHY A PREBUILT IMAGE RATHER THAN A FILESYSTEM

The contents can only change by reflashing, which is exactly what was wanted.
It also means the whole layout is decided here, on a machine where it can be
verified, instead of on a microcontroller that cannot be debugged. This script
parses its own output back and checks the file round-trips byte for byte before
writing the header.

docs/klor-keymap.html is a complete, standalone document -- openable straight
from the repo -- so it is embedded verbatim with no rewriting in between. What
the browser shows from the repo is byte for byte what the drive serves.

TRAILING SECTORS ARE NOT STORED. The volume is 100 KB but the guide only fills
about 40 KB of it. Only the used prefix goes into flash; the driver returns
zeros for anything past the end, so the host still sees a full-size volume and
roughly 60 KB of flash is not wasted on blank sectors.
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_HTML = ROOT / "docs/klor-keymap.html"
OUT = ROOT / "src/usb_disk_image.h"

SECTOR = 512
TOTAL_SECTORS = 200          # 100 KB
RESERVED_SECTORS = 1         # the boot sector
NUM_FATS = 2
FAT_SECTORS = 1              # 512 B of FAT12 covers ~340 clusters; we need ~200
ROOT_ENTRIES = 16            # one sector of 32-byte directory entries
SECTORS_PER_CLUSTER = 1

VOLUME_LABEL = b"KLOR GUIDE "     # exactly 11 bytes
FILE_NAME = b"KEYMAP  HTM"        # 8.3, space padded. Plain 8.3 avoids needing
                                  # long-filename entries, which every host
                                  # would have to agree on.

ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
FIRST_DATA_SECTOR = RESERVED_SECTORS + NUM_FATS * FAT_SECTORS + ROOT_DIR_SECTORS
DATA_SECTORS = TOTAL_SECTORS - FIRST_DATA_SECTOR

ATTR_READ_ONLY = 0x01
ATTR_VOLUME_ID = 0x08


def boot_sector() -> bytes:
    b = bytearray(SECTOR)
    b[0:3] = b"\xeb\x3c\x90"              # jump, as every BPB starts
    b[3:11] = b"MSWIN4.1"                 # OEM name; this exact string is the
                                          # best understood by old drivers
    struct.pack_into("<H", b, 11, SECTOR)             # bytes per sector
    b[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", b, 14, RESERVED_SECTORS)
    b[16] = NUM_FATS
    struct.pack_into("<H", b, 17, ROOT_ENTRIES)
    struct.pack_into("<H", b, 19, TOTAL_SECTORS)      # fits in 16 bits here
    b[21] = 0xF8                                      # fixed disk
    struct.pack_into("<H", b, 22, FAT_SECTORS)
    struct.pack_into("<H", b, 24, 32)                 # sectors per track
    struct.pack_into("<H", b, 26, 2)                  # heads
    struct.pack_into("<I", b, 28, 0)                  # hidden sectors
    struct.pack_into("<I", b, 32, 0)                  # total sectors 32
    b[36] = 0x80                                      # drive number
    b[38] = 0x29                                      # extended boot signature
    struct.pack_into("<I", b, 39, 0x4B4C4F52)         # volume id, "KLOR"
    b[43:54] = VOLUME_LABEL
    b[54:62] = b"FAT12   "
    b[510:512] = b"\x55\xaa"
    return bytes(b)


def fat_sector(cluster_count: int) -> bytes:
    """FAT12: one chain from cluster 2, each entry 12 bits, little endian."""
    entries = [0xFF8, 0xFFF]              # reserved entries 0 and 1
    for i in range(cluster_count):
        last = i == cluster_count - 1
        entries.append(0xFFF if last else (2 + i + 1))

    b = bytearray(FAT_SECTORS * SECTOR)
    for idx, val in enumerate(entries):
        off = (idx * 3) // 2
        if idx % 2 == 0:
            b[off] = val & 0xFF
            b[off + 1] = (b[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
        else:
            b[off] = (b[off] & 0x0F) | ((val << 4) & 0xF0)
            b[off + 1] = (val >> 4) & 0xFF
    return bytes(b)


def root_dir(size: int) -> bytes:
    b = bytearray(ROOT_DIR_SECTORS * SECTOR)

    # Entry 0: the volume label, so the drive is named in the host's file
    # manager rather than showing as "Untitled".
    b[0:11] = VOLUME_LABEL
    b[11] = ATTR_VOLUME_ID

    # Entry 1: the guide itself, flagged read-only. The driver refuses writes
    # regardless; this is what makes the host show it as read-only up front.
    e = 32
    b[e:e + 11] = FILE_NAME
    b[e + 11] = ATTR_READ_ONLY
    struct.pack_into("<H", b, e + 22, 0)       # write time
    struct.pack_into("<H", b, e + 24, 0x5821)  # write date: 2024-01-01
    struct.pack_into("<H", b, e + 26, 2)       # first cluster
    struct.pack_into("<I", b, e + 28, size)
    return bytes(b)


def build_image(payload: bytes) -> bytes:
    clusters = (len(payload) + SECTOR - 1) // SECTOR
    if clusters > DATA_SECTORS:
        sys.exit(f"guide needs {clusters} clusters, volume holds {DATA_SECTORS}")

    img = bytearray()
    img += boot_sector()
    fat = fat_sector(clusters)
    img += fat * NUM_FATS
    img += root_dir(len(payload))
    img += payload
    img += bytes((TOTAL_SECTORS * SECTOR) - len(img))
    assert len(img) == TOTAL_SECTORS * SECTOR
    return bytes(img)


def verify(img: bytes, expect: bytes) -> None:
    """Parse the image the way a host would and confirm the file comes back."""
    assert img[510:512] == b"\x55\xaa", "missing boot signature"
    bps = struct.unpack_from("<H", img, 11)[0]
    spc = img[13]
    resv = struct.unpack_from("<H", img, 14)[0]
    nfat = img[16]
    rootent = struct.unpack_from("<H", img, 17)[0]
    total = struct.unpack_from("<H", img, 19)[0]
    fatsz = struct.unpack_from("<H", img, 22)[0]
    assert (bps, spc, total) == (SECTOR, SECTORS_PER_CLUSTER, TOTAL_SECTORS)

    root_off = (resv + nfat * fatsz) * bps
    data_off = root_off + (rootent * 32 + bps - 1) // bps * bps

    ent = None
    for i in range(rootent):
        e = img[root_off + i * 32: root_off + (i + 1) * 32]
        if e[0] and not (e[11] & ATTR_VOLUME_ID):
            ent = e
            break
    assert ent, "no file entry found in the root directory"
    assert ent[0:11] == FILE_NAME, f"unexpected name {ent[0:11]!r}"
    assert ent[11] & ATTR_READ_ONLY, "file is not marked read-only"
    size = struct.unpack_from("<I", ent, 28)[0]
    first = struct.unpack_from("<H", ent, 26)[0]
    assert size == len(expect), f"size {size} != {len(expect)}"

    # Walk the FAT chain rather than assuming the data is contiguous.
    fat_off = resv * bps

    def fat_entry(n):
        off = fat_off + (n * 3) // 2
        pair = img[off] | (img[off + 1] << 8)
        return (pair >> 4) if n % 2 else (pair & 0xFFF)

    out = bytearray()
    cl = first
    guard = 0
    while cl < 0xFF8:
        off = data_off + (cl - 2) * spc * bps
        out += img[off: off + spc * bps]
        cl = fat_entry(cl)
        guard += 1
        assert guard <= TOTAL_SECTORS, "FAT chain does not terminate"
    assert bytes(out[:size]) == expect, "round-tripped file does not match"


def main():
    payload = SRC_HTML.read_bytes()
    img = build_image(payload)
    verify(img, payload)

    # Only the used prefix is stored; the driver zero-fills the rest.
    used = len(img.rstrip(b"\x00"))
    stored = (used + SECTOR - 1) // SECTOR * SECTOR

    lines = [
        "/*",
        " * GENERATED by scripts/gen_usb_disk.py -- do not edit by hand.",
        " * Regenerate after changing docs/klor-keymap.html.",
        " *",
        " * A complete read-only FAT12 image. The firmware serves these bytes",
        " * verbatim; the host parses the filesystem, so there is no FAT code on",
        " * the device at all.",
        " *",
        " * Only the used prefix is here. Sectors past KLOR_DISK_STORED_BYTES are",
        " * blank and the driver returns zeros for them, so the host still sees a",
        " * full-size volume without the blank space costing any flash.",
        " */",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define KLOR_DISK_SECTOR_SIZE   {SECTOR}",
        f"#define KLOR_DISK_SECTOR_COUNT  {TOTAL_SECTORS}",
        f"#define KLOR_DISK_STORED_BYTES  {stored}",
        "",
        f"/* {FILE_NAME.decode().strip()} -- {len(payload):,} bytes of guide"
        f" in a {TOTAL_SECTORS * SECTOR // 1024} KB volume. */",
        f"static const uint8_t klor_disk_image[{stored}] = {{",
    ]
    body = img[:stored]
    for i in range(0, len(body), 16):
        lines.append("    " + " ".join(f"0x{b:02x}," for b in body[i:i + 16]))
    lines += ["};", ""]
    OUT.write_text("\n".join(lines))

    print(f"wrote {OUT.relative_to(ROOT)}")
    print(f"  guide      {len(payload):,} B  ({FILE_NAME.decode().strip()})")
    print(f"  volume     {TOTAL_SECTORS * SECTOR:,} B ({TOTAL_SECTORS} sectors)")
    print(f"  stored     {stored:,} B in flash  "
          f"({TOTAL_SECTORS * SECTOR - stored:,} B of blank sectors omitted)")
    print("  verified   image parsed back, file round-trips byte for byte")


if __name__ == "__main__":
    main()
