#!/usr/bin/env python3
"""Build a RingTail RTFS disk image. Usage:
    mkdisk.py -o disk.img hello:hello.bin count:count.bin welcome:welcome.txt
Each name:file pair becomes one file on the disk, with the given
name (max 31 characters, as in diskfs.c) and the file's bytes.
The layout matches src/fs/diskfs.c exactly: sector 0 superblock,
64 directory entries in sectors 1-6 (48 bytes each), an 8-sector
bitmap in sectors 7-14, data from sector 15. Files are packed
contiguously, in argument order, starting at the first data
sector. This script is a host tool, not OS code: it runs on the
machine that builds the disk image, and the OS never sees it.
The disk image itself is not checked in (it is a binary blob,
and push-files is text-only); rebuild it with this script
whenever the programs change. Default size is 16 MB.
"""
import argparse
import struct
import sys

SECTOR = 512
DIR_START = 1
DIR_COUNT = 64
DIR_ENTRY_SIZE = 48
DIR_SECTORS = (DIR_COUNT * DIR_ENTRY_SIZE + SECTOR - 1) // SECTOR
BITMAP_START = DIR_START + DIR_SECTORS  # 7
BITMAP_SECTORS = 8
DATA_START = BITMAP_START + BITMAP_SECTORS  # 15


def main():
    ap = argparse.ArgumentParser(description="Build a RingTail disk image")
    ap.add_argument("-o", "--output", required=True, help="disk image to write")
    ap.add_argument("--size-mb", type=int, default=16, help="image size in MB (default 16)")
    ap.add_argument("files", nargs="+", help="name:file pairs to pack")
    args = ap.parse_args()

    total_sectors = args.size_mb * 1024 * 1024 // SECTOR
    if total_sectors <= DATA_START:
        sys.exit("image too small")
    if len(args.files) > DIR_COUNT:
        sys.exit(f"too many files (max {DIR_COUNT})")

    entries = []  # (name, start_lba, size)
    blobs = []  # (start_lba, data bytes)
    next_sector = DATA_START
    bitmap = bytearray(BITMAP_SECTORS * SECTOR)
    for pair in args.files:
        if ":" not in pair:
            sys.exit(f"expected name:file, got {pair!r}")
        name, path = pair.split(":", 1)
        if len(name) > 31 or not name:
            sys.exit(f"bad name {name!r} (max 31 characters)")
        if any(e[0] == name for e in entries):
            sys.exit(f"duplicate name {name!r}")
        with open(path, "rb") as f:
            data = f.read()
        nsec = (len(data) + SECTOR - 1) // SECTOR
        start = next_sector
        if start + nsec > total_sectors:
            sys.exit(f"disk full packing {name!r}")
        for i in range(nsec):
            bit = start - DATA_START + i
            bitmap[bit // 8] |= 1 << (bit % 8)
        entries.append((name, start, len(data)))
        blobs.append((start, data))
        next_sector += nsec

    image = bytearray(total_sectors * SECTOR)
    sb = struct.pack("<4sIIIIIIII", b"RTFS", 1, total_sectors,
                     DIR_START, DIR_COUNT, BITMAP_START, BITMAP_SECTORS,
                     DATA_START, SECTOR)
    image[0:len(sb)] = sb
    for i, (name, start, size) in enumerate(entries):
        off = DIR_START * SECTOR + i * DIR_ENTRY_SIZE
        image[off:off + DIR_ENTRY_SIZE] = struct.pack(
            "<32sIIII", name.encode("ascii"), start, size, 1, 0)
    image[BITMAP_START * SECTOR:(BITMAP_START + BITMAP_SECTORS) * SECTOR] = bitmap
    for start, data in blobs:
        off = start * SECTOR
        image[off:off + len(data)] = data

    with open(args.output, "wb") as f:
        f.write(image)
    print(f"{args.output}: {len(entries)} files, {total_sectors} sectors, "
          f"{next_sector - DATA_START} data sectors used")


if __name__ == "__main__":
    main()
