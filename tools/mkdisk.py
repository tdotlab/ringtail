#!/usr/bin/env python3
"""Build a RingTail FAT16 disk image. Usage:
    mkdisk.py -o disk.img hello:hello.bin count:count.bin welcome:welcome.txt
Each name:file pair becomes one file on the disk, with the given
name (DOS 8.3, as in src/fs/diskfs.c) and the file's bytes.
The layout matches src/fs/diskfs.c exactly: sector 0 is a FAT16
boot sector with a BPB, then two identical FAT copies, then a
512-entry root directory, then the data area, where files are
chains of 1-sector clusters, allocated in argument order, so
an initial image's chains happen to be contiguous (later
create/delete cycles in the OS will fragment them, which is
the point of chains). This script is a host tool, not OS code:
it runs on the machine that builds the disk image, and the OS
never sees it. The disk image itself is not checked in (it is
a binary blob, and push-files is text-only); rebuild it with
this script whenever the programs change. Default size is
16 MB, which with 1-sector clusters gives a valid FAT16
cluster count (4085..65524), unlike a tiny test disk.
"""
import argparse
import struct
import sys

SECTOR = 512
SPC = 1  # sectors per cluster in our images; the kernel accepts more
RESERVED = 1
NUM_FATS = 2
ROOT_ENTRIES = 512
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR  # 32

FORBIDDEN = set(' " * + , / : ; < = > ? [ \\ ] |')


def to_raw(name):
    """User name -> 11-byte FAT raw form, or None if invalid."""
    if not name or name.count(".") > 1:
        return None
    if "." in name:
        base, ext = name.split(".", 1)
    else:
        base, ext = name, ""
    if not 1 <= len(base) <= 8 or len(ext) > 3:
        return None
    for ch in base + ext:
        if ord(ch) < 0x20 or ord(ch) == 0x7F or ch in FORBIDDEN:
            return None
    raw = base.upper().ljust(8) + ext.upper().ljust(3)
    return raw.encode("ascii")


def fat_layout(total_sectors):
    """Iterate FAT size to a fixed point; returns (fat_sectors, data_start, clusters)."""
    fat_sectors = 1
    while True:
        data_start = RESERVED + NUM_FATS * fat_sectors + ROOT_SECTORS
        data_sectors = total_sectors - data_start
        clusters = data_sectors // SPC
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if need == fat_sectors:
            return fat_sectors, data_start, clusters
        fat_sectors = need


def main():
    ap = argparse.ArgumentParser(description="Build a RingTail FAT16 disk image")
    ap.add_argument("-o", "--output", required=True, help="disk image to write")
    ap.add_argument("--size-mb", type=int, default=16, help="image size in MB (default 16)")
    ap.add_argument("files", nargs="+", help="name:file pairs to pack")
    args = ap.parse_args()

    total_sectors = args.size_mb * 1024 * 1024 // SECTOR
    fat_sectors, data_start, clusters = fat_layout(total_sectors)
    if clusters < 4085 or clusters >= 65525:
        sys.exit(f"size gives {clusters} clusters, not a valid FAT16 count")
    if len(args.files) > ROOT_ENTRIES:
        sys.exit(f"too many files (max {ROOT_ENTRIES})")

    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into("<H", fat, 0, 0xFFF8)  # media descriptor + FF
    struct.pack_into("<H", fat, 2, 0xFFFF)  # end of chain, cluster 1
    root = bytearray(ROOT_SECTORS * SECTOR)
    data_blobs = []  # (cluster_lba, data bytes for one cluster)
    next_cluster = 2
    entries = []
    for pair in args.files:
        if ":" not in pair:
            sys.exit(f"expected name:file, got {pair!r}")
        name, path = pair.split(":", 1)
        raw = to_raw(name)
        if raw is None:
            sys.exit(f"bad name {name!r} (must fit DOS 8.3)")
        if any(e[0] == raw for e in entries):
            sys.exit(f"duplicate name {name!r} (after 8.3 uppercasing)")
        with open(path, "rb") as f:
            data = f.read()
        nclusters = (len(data) + SPC * SECTOR - 1) // (SPC * SECTOR) if data else 0
        if next_cluster + nclusters > clusters + 2:
            sys.exit(f"disk full packing {name!r}")
        first = next_cluster if nclusters else 0
        for k in range(nclusters):
            cl = next_cluster + k
            nxt = cl + 1 if k + 1 < nclusters else 0xFFFF
            struct.pack_into("<H", fat, cl * 2, nxt)
            chunk = data[k * SPC * SECTOR:(k + 1) * SPC * SECTOR]
            data_blobs.append((data_start + (cl - 2) * SPC, chunk))
        entries.append((raw, first, len(data)))
        next_cluster += nclusters

    image = bytearray(total_sectors * SECTOR)
    bpb = bytearray(SECTOR)
    bpb[0:3] = bytes([0xEB, 0x3C, 0x90])
    bpb[3:11] = b"RINGTAIL"
    struct.pack_into("<H", bpb, 11, SECTOR)
    bpb[13] = SPC
    struct.pack_into("<H", bpb, 14, RESERVED)
    bpb[16] = NUM_FATS
    struct.pack_into("<H", bpb, 17, ROOT_ENTRIES)
    struct.pack_into("<H", bpb, 19, total_sectors if total_sectors < 65536 else 0)
    bpb[21] = 0xF8
    struct.pack_into("<H", bpb, 22, fat_sectors)
    struct.pack_into("<H", bpb, 24, 63)
    struct.pack_into("<H", bpb, 26, 255)
    struct.pack_into("<I", bpb, 28, 0)
    struct.pack_into("<I", bpb, 32, 0 if total_sectors < 65536 else total_sectors)
    bpb[36] = 0x80
    bpb[38] = 0x29
    struct.pack_into("<I", bpb, 39, 0x20261104)
    bpb[43:54] = b"RINGTAIL   "
    bpb[54:62] = b"FAT16   "
    bpb[510:512] = bytes([0x55, 0xAA])
    image[0:SECTOR] = bpb
    fat_start = RESERVED
    root_start = fat_start + NUM_FATS * fat_sectors
    for f in range(NUM_FATS):
        off = (fat_start + f * fat_sectors) * SECTOR
        image[off:off + len(fat)] = fat
    # Volume label entry first, matching the BPB label, so real
    # tools do not report a label mismatch. The kernel skips
    # volume entries when listing, finding, and counting files.
    vol_off = root_start * SECTOR
    image[vol_off:vol_off + 11] = b"RINGTAIL   "
    image[vol_off + 11] = 0x08  # volume label
    for i, (raw, first, size) in enumerate(entries):
        off = root_start * SECTOR + (i + 1) * 32
        image[off:off + 11] = raw
        image[off + 11] = 0x20  # archive, regular file
        struct.pack_into("<H", image, off + 26, first)
        struct.pack_into("<I", image, off + 28, size)
    for lba, chunk in data_blobs:
        off = lba * SECTOR
        image[off:off + len(chunk)] = chunk

    with open(args.output, "wb") as f:
        f.write(image)
    print(f"{args.output}: {len(entries)} files, {total_sectors} sectors, "
          f"FAT {fat_sectors} sectors x2, root {ROOT_SECTORS} sectors, "
          f"{next_cluster - 2} clusters used")


if __name__ == "__main__":
    main()
