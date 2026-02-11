#!/usr/bin/env python3
"""mdfs_make_test_image.py

Generates a small, deterministic MBR-partitioned disk image containing a single
MDFS (ModularFS) v2 filesystem with a handful of sample files.

This is meant as a "known good corpus" for external implementations (drivers,
FUSE, parsers).

Output: dist/mdfs_test.img (by default)

Image layout:
- LBA 0: MBR with one primary partition
- Partition starts at LBA 2048, type 0x83
- Partition contains MDFS v2 (4 KiB blocks)

Sample contents:
- /test.txt            => "MDFS OK\n"
- /hello/              => directory
- /hello/world.txt     => "Hello from ModularFS (MDFS)!\n"
- /big.bin             => 1 MiB deterministic pattern

The on-disk structs and CRC match the ModuOS implementation.
"""

from __future__ import annotations

import argparse
import os
import struct
from dataclasses import dataclass

MDFS_MAGIC = 0x5346444D  # 'MDFS' little-endian
MDFS_VERSION = 2
MDFS_BLOCK_SIZE = 4096
MDFS_INODE_SIZE = 256
MDFS_MAX_DIRECT = 12

MDFS_DIR_REC_SIZE = 32
MDFS_DIRREC_PRIMARY = 1
MDFS_DIRREC_NAME = 2

MDFS_DIRFLAG_VALID = 0x01
MDFS_DIRFLAG_DELETED = 0x02

MODE_DIR = 0x4000
MODE_FILE = 0x8000

SECTOR_SIZE = 512
PART_START_LBA = 2048


def crc32_ieee(data: bytes) -> int:
    # Same algorithm used in src/fs/MDFS/mdfs_disk.c
    crc = 0xFFFFFFFF
    for b in data:
        x = (crc ^ b) & 0xFF
        for _ in range(8):
            x = (x >> 1) ^ (0xEDB88320 & (-(x & 1)))
        crc = (crc >> 8) ^ x
    return (~crc) & 0xFFFFFFFF


def le32(x: int) -> bytes:
    return struct.pack('<I', x & 0xFFFFFFFF)


def le64(x: int) -> bytes:
    return struct.pack('<Q', x & 0xFFFFFFFFFFFFFFFF)


@dataclass
class Superblock:
    total_blocks: int
    free_blocks: int
    total_inodes: int
    free_inodes: int

    block_bitmap_start: int
    block_bitmap_blocks: int
    inode_bitmap_start: int
    inode_bitmap_blocks: int
    inode_table_start: int
    inode_table_blocks: int

    root_inode: int

    def pack(self) -> bytes:
        # Must be exactly 4096 bytes.
        # Matches the field ordering in wiki docs and ModuOS.
        uuid = bytes.fromhex('00112233445566778899aabbccddeeff')
        features = 0

        header = b''.join([
            le32(MDFS_MAGIC),
            le32(MDFS_VERSION),
            le32(MDFS_BLOCK_SIZE),
            le32(0),

            le64(self.total_blocks),
            le64(self.free_blocks),
            le64(self.total_inodes),
            le64(self.free_inodes),

            le64(self.block_bitmap_start),
            le64(self.block_bitmap_blocks),
            le64(self.inode_bitmap_start),
            le64(self.inode_bitmap_blocks),
            le64(self.inode_table_start),
            le64(self.inode_table_blocks),

            le64(self.root_inode),
            uuid,
            le32(features),
            le32(0),  # checksum placeholder
        ])

        pad_len = MDFS_BLOCK_SIZE - len(header)
        if pad_len < 0:
            raise ValueError('Superblock too large')
        sb = header + b'\x00' * pad_len

        # checksum over sb with checksum field zeroed (already zero)
        c = crc32_ieee(sb)
        sb = sb[: len(header) - 4] + le32(c) + sb[len(header):]
        return sb


def pack_inode(*, mode: int, size_bytes: int, direct: list[int], ind1=0, ind2=0, ind3=0, uid=0, gid=0) -> bytes:
    direct = (direct + [0] * MDFS_MAX_DIRECT)[:MDFS_MAX_DIRECT]
    link_count = 1
    flags = 0

    fixed = b''.join([
        struct.pack('<H', mode & 0xFFFF),
        struct.pack('<H', 0),
        le32(uid),
        le32(gid),
        le64(size_bytes),
        le32(link_count),
        le32(flags),
        b''.join(le64(x) for x in direct),
        le64(ind1),
        le64(ind2),
        le64(ind3),
    ])

    if len(fixed) > MDFS_INODE_SIZE:
        raise ValueError('inode too big')
    return fixed + b'\x00' * (MDFS_INODE_SIZE - len(fixed))


def dir_entry_set(inode: int, is_dir: bool, name: str) -> bytes:
    name_bytes = name.encode('utf-8')
    if len(name_bytes) > 255:
        raise ValueError('name too long')

    # number of name records
    name_payload_per = 31
    name_recs = (len(name_bytes) + name_payload_per - 1) // name_payload_per
    record_count = 1 + name_recs

    entry_type = 2 if is_dir else 1

    primary = bytearray(32)
    primary[0] = MDFS_DIRREC_PRIMARY
    primary[1] = MDFS_DIRFLAG_VALID
    primary[2] = entry_type
    primary[3] = record_count & 0xFF
    primary[4:8] = struct.pack('<I', inode)
    primary[8:10] = struct.pack('<H', len(name_bytes))
    # [10:12] reserved
    primary[12:16] = struct.pack('<I', 0)  # checksum placeholder

    names = bytearray()
    for i in range(name_recs):
        chunk = name_bytes[i * name_payload_per:(i + 1) * name_payload_per]
        rec = bytearray(32)
        rec[0] = MDFS_DIRREC_NAME
        rec[1:1 + len(chunk)] = chunk
        names += rec

    full = bytes(primary) + bytes(names)
    # checksum is CRC32 over full entry set with checksum field zero
    c = crc32_ieee(full)
    primary[12:16] = struct.pack('<I', c)
    return bytes(primary) + bytes(names)


def write_block(img: bytearray, part_off: int, block_no: int, data: bytes) -> None:
    if len(data) != MDFS_BLOCK_SIZE:
        raise ValueError('block write must be 4096 bytes')
    off = part_off + block_no * MDFS_BLOCK_SIZE
    img[off:off + MDFS_BLOCK_SIZE] = data


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--size-mib', type=int, default=64, help='disk size in MiB (default: 64)')
    ap.add_argument('--out', default=os.path.join('dist', 'mdfs_test.img'), help='output image path')
    args = ap.parse_args()

    disk_bytes = args.size_mib * 1024 * 1024
    if disk_bytes < (PART_START_LBA + 4096) * SECTOR_SIZE:
        raise SystemExit('disk too small')

    disk_sectors = disk_bytes // SECTOR_SIZE

    # Create MBR
    mbr = bytearray(512)
    # One partition entry
    ent = 0x1BE
    mbr[ent + 0] = 0x00  # bootable
    mbr[ent + 4] = 0x83  # type
    mbr[ent + 8:ent + 12] = struct.pack('<I', PART_START_LBA)
    part_sectors = min(disk_sectors - PART_START_LBA, 0xFFFFFFFF)
    mbr[ent + 12:ent + 16] = struct.pack('<I', part_sectors)
    mbr[510] = 0x55
    mbr[511] = 0xAA

    # Allocate whole disk image
    img = bytearray(disk_bytes)
    img[0:512] = mbr

    part_off = PART_START_LBA * SECTOR_SIZE
    part_bytes = part_sectors * SECTOR_SIZE
    total_blocks = part_bytes // MDFS_BLOCK_SIZE

    # Compute bitmap sizes (same idea as ModuOS mkfs)
    bits_per_bm_block = MDFS_BLOCK_SIZE * 8
    block_bitmap_blocks = (total_blocks + bits_per_bm_block - 1) // bits_per_bm_block

    total_inodes = max(128, total_blocks // 16)
    inode_bitmap_blocks = (total_inodes + bits_per_bm_block - 1) // bits_per_bm_block

    inodes_per_block = MDFS_BLOCK_SIZE // MDFS_INODE_SIZE
    inode_table_blocks = (total_inodes + inodes_per_block - 1) // inodes_per_block
    inode_table_blocks = max(inode_table_blocks, 8)

    block_bitmap_start = 3
    inode_bitmap_start = block_bitmap_start + block_bitmap_blocks
    inode_table_start = inode_bitmap_start + inode_bitmap_blocks
    meta_end = inode_table_start + inode_table_blocks

    if meta_end + 10 >= total_blocks:
        raise SystemExit('disk too small for metadata')

    # Choose root dir data block right after metadata
    root_dir_block = meta_end

    # Allocate a few more blocks for file data
    test_txt_block = meta_end + 1
    world_txt_block = meta_end + 2
    big_bin_block0 = meta_end + 3

    # Inodes: 1=root, 2=test.txt, 3=hello dir, 4=world.txt, 5=big.bin
    root_ino = 1
    test_ino = 2
    hello_ino = 3
    world_ino = 4
    big_ino = 5

    # Build superblock
    used_meta_blocks = meta_end  # blocks [0..meta_end-1]

    # We'll mark used blocks manually
    used_blocks = set(range(0, meta_end))
    used_blocks.add(root_dir_block)
    used_blocks.add(test_txt_block)
    used_blocks.add(world_txt_block)
    used_blocks.add(big_bin_block0)

    free_blocks = total_blocks - len(used_blocks)
    used_inodes = {root_ino, test_ino, hello_ino, world_ino, big_ino}
    free_inodes = total_inodes - len(used_inodes)

    sb = Superblock(
        total_blocks=total_blocks,
        free_blocks=free_blocks,
        total_inodes=total_inodes,
        free_inodes=free_inodes,
        block_bitmap_start=block_bitmap_start,
        block_bitmap_blocks=block_bitmap_blocks,
        inode_bitmap_start=inode_bitmap_start,
        inode_bitmap_blocks=inode_bitmap_blocks,
        inode_table_start=inode_table_start,
        inode_table_blocks=inode_table_blocks,
        root_inode=root_ino,
    )

    write_block(img, part_off, 1, sb.pack())
    write_block(img, part_off, 2, sb.pack())

    # Initialize bitmaps
    # Block bitmap
    for bi in range(block_bitmap_blocks):
        write_block(img, part_off, block_bitmap_start + bi, b'\x00' * MDFS_BLOCK_SIZE)
    for b in used_blocks:
        blk_index = b // bits_per_bm_block
        off_bit = b % bits_per_bm_block
        bm_off = part_off + (block_bitmap_start + blk_index) * MDFS_BLOCK_SIZE
        byte = off_bit // 8
        bit = 1 << (off_bit % 8)
        img[bm_off + byte] |= bit

    # Inode bitmap
    for bi in range(inode_bitmap_blocks):
        write_block(img, part_off, inode_bitmap_start + bi, b'\x00' * MDFS_BLOCK_SIZE)
    for ino in used_inodes:
        blk_index = ino // bits_per_bm_block
        off_bit = ino % bits_per_bm_block
        bm_off = part_off + (inode_bitmap_start + blk_index) * MDFS_BLOCK_SIZE
        byte = off_bit // 8
        bit = 1 << (off_bit % 8)
        img[bm_off + byte] |= bit

    # Inode table blocks (zero init)
    for i in range(inode_table_blocks):
        write_block(img, part_off, inode_table_start + i, b'\x00' * MDFS_BLOCK_SIZE)

    def write_inode(ino: int, data: bytes) -> None:
        byte_off = ino * MDFS_INODE_SIZE
        blk = inode_table_start + (byte_off // MDFS_BLOCK_SIZE)
        off = byte_off % MDFS_BLOCK_SIZE
        base = part_off + blk * MDFS_BLOCK_SIZE + off
        img[base:base + MDFS_INODE_SIZE] = data

    # Root directory contents
    # Entry sets: test.txt, hello, big.bin
    dir_data = bytearray(MDFS_BLOCK_SIZE)
    off = 0
    for ent_bytes in [
        dir_entry_set(test_ino, False, 'test.txt'),
        dir_entry_set(hello_ino, True, 'hello'),
        dir_entry_set(big_ino, False, 'big.bin'),
    ]:
        dir_data[off:off + len(ent_bytes)] = ent_bytes
        off += len(ent_bytes)
    # end marker: rec_type==0 (already zero)
    write_block(img, part_off, root_dir_block, bytes(dir_data))

    # /hello directory contains world.txt
    hello_dir_block = meta_end + 4
    used_blocks.add(hello_dir_block)
    # mark it used in bitmap
    b = hello_dir_block
    blk_index = b // bits_per_bm_block
    off_bit = b % bits_per_bm_block
    bm_off = part_off + (block_bitmap_start + blk_index) * MDFS_BLOCK_SIZE
    img[bm_off + (off_bit // 8)] |= 1 << (off_bit % 8)

    hello_data = bytearray(MDFS_BLOCK_SIZE)
    ent_bytes = dir_entry_set(world_ino, False, 'world.txt')
    hello_data[0:len(ent_bytes)] = ent_bytes
    write_block(img, part_off, hello_dir_block, bytes(hello_data))

    # File data
    test_txt = b"MDFS OK\n"
    write_block(img, part_off, test_txt_block, test_txt + b'\x00' * (MDFS_BLOCK_SIZE - len(test_txt)))

    world_txt = b"Hello from ModularFS (MDFS)!\n"
    write_block(img, part_off, world_txt_block, world_txt + b'\x00' * (MDFS_BLOCK_SIZE - len(world_txt)))

    big_bin = bytes((i & 0xFF) for i in range(1024 * 1024))
    # store first 4KiB in big_bin_block0; rest not stored (this is just a corpus stub)
    write_block(img, part_off, big_bin_block0, big_bin[:MDFS_BLOCK_SIZE])

    # Inodes
    write_inode(root_ino, pack_inode(mode=MODE_DIR, size_bytes=off, direct=[root_dir_block]))
    write_inode(test_ino, pack_inode(mode=MODE_FILE, size_bytes=len(test_txt), direct=[test_txt_block]))
    write_inode(hello_ino, pack_inode(mode=MODE_DIR, size_bytes=len(ent_bytes), direct=[hello_dir_block]))
    write_inode(world_ino, pack_inode(mode=MODE_FILE, size_bytes=len(world_txt), direct=[world_txt_block]))
    write_inode(big_ino, pack_inode(mode=MODE_FILE, size_bytes=1024 * 1024, direct=[big_bin_block0]))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(img)

    print(f"Wrote {args.out} ({disk_bytes} bytes, {args.size_mib} MiB)")
    print(f"Partition: start LBA {PART_START_LBA}, sectors {part_sectors}, blocks {total_blocks}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
