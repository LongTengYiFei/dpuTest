#!/usr/bin/env python3
"""Pack one raw object as independent zlib frames for DOCA decompression."""

import argparse
import pathlib
import struct
import zlib


OBJECT_MAGIC = 0x4344424F
FORMAT_VERSION = 2
FRAME_BYTES = 1024 * 1024
OBJECT_HEADER = struct.Struct("<IIQQII")
FRAME_HEADER = struct.Struct("<II")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    raw = args.input.read_bytes()
    frames = []
    for offset in range(0, len(raw), FRAME_BYTES):
        frame = raw[offset : offset + FRAME_BYTES]
        compressed = zlib.compress(frame, level=6)
        frames.append((len(frame), compressed))

    compressed_size = sum(len(data) for _, data in frames)
    header = OBJECT_HEADER.pack(
        OBJECT_MAGIC,
        FORMAT_VERSION,
        len(raw),
        compressed_size,
        zlib.crc32(raw),
        len(frames),
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as output:
        output.write(header)
        for raw_size, compressed in frames:
            output.write(FRAME_HEADER.pack(raw_size, len(compressed)))
            output.write(compressed)


if __name__ == "__main__":
    main()
