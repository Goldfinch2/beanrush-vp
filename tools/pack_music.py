#!/usr/bin/env python3
"""Packs Bean Rush's music into one scrambled file, music.dat, so the tracks
are not loose .ogg files a player could copy out of the game (the music's
licence forbids letting them be extracted). src/music.c reads it back.

    pack_music.py OUT.dat TRACK.ogg...

Layout: "BRM1", the track count (u32 LE), then the table (count entries of
offset u32, size u32, name 40 bytes: the file name without .ogg), then the
tracks. Everything after the first 8 bytes is XORed with a keystream
(xorshift32) seeded from KEY + the region's offset in the file.
"""
import os
import struct
import sys

KEY = 0x6A3B5E21
NAME = 40


def scramble(data, offset):
    s = (KEY + offset) & 0xFFFFFFFF or 1
    out = bytearray(data)
    for i in range(len(out)):
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        out[i] ^= s & 0xFF
    return bytes(out)


def main():
    out, tracks = sys.argv[1], sys.argv[2:]
    blobs = []
    for t in tracks:
        with open(t, "rb") as f:
            blobs.append((os.path.splitext(os.path.basename(t))[0].encode(), f.read()))
    table_at = 8
    at = table_at + len(blobs) * (8 + NAME)
    table, body = b"", b""
    for name, data in blobs:
        if len(name) >= NAME:
            sys.exit("track name too long: %s" % name.decode())
        table += struct.pack("<II", at, len(data)) + name.ljust(NAME, b"\0")
        body += scramble(data, at)
        at += len(data)
    with open(out, "wb") as f:
        f.write(b"BRM1" + struct.pack("<I", len(blobs)))
        f.write(scramble(table, table_at))
        f.write(body)


if __name__ == "__main__":
    main()
