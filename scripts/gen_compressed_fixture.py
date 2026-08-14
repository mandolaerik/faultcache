#!/usr/bin/env python3
# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

"""Generate a small test fixture for the compressed-file faultcache example.

The fixture is a toy "independently-block-compressed" file format: unlike
plain gzip (one continuous stream), each chunk is deflated independently so
that any chunk can be decompressed on its own, without decoding the chunks
before it. That is what makes lazily mmap()-ing a compressed file through
faultcache possible.

Layout (all integers little-endian):
    magic            4 bytes, b"FCFX"
    nchunks           u32
    per chunk (x nchunks):
        uncompressed_size   u64
        compressed_size     u64
        compressed_offset   u64   (offset from the start of the blob area)
    blob                    compressed bytes of every chunk, concatenated
"""
import struct
import sys
import zlib

MAGIC = b"FCFX"

# Deliberately not page-aligned/multiple-of-anything sizes, to exercise the
# same page-boundary-sharing logic as tests/test_boundary.c.
CHUNK_SIZES = [100, 4000, 4096, 9000, 42, 8192, 777]


def chunk_pattern(chunk_index: int, size: int) -> bytes:
    """Deterministic, easily-reproduced-in-C fill pattern for a chunk."""
    return bytes((chunk_index * 131 + i * 7) & 0xFF for i in range(size))


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT", file=sys.stderr)
        return 1

    out_path = sys.argv[1]

    compressed_chunks = [
        zlib.compress(chunk_pattern(i, size), 9)
        for i, size in enumerate(CHUNK_SIZES)
    ]

    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(CHUNK_SIZES)))
        offset = 0
        for size, blob in zip(CHUNK_SIZES, compressed_chunks):
            f.write(struct.pack("<QQQ", size, len(blob), offset))
            offset += len(blob)
        for blob in compressed_chunks:
            f.write(blob)

    return 0


if __name__ == "__main__":
    sys.exit(main())
