#!/usr/bin/env python3
"""Extract raw ETC1 blocks from KTX (etc2comp) or PVR (etcpak) into our
.bin format (int32 w, int32 h, then bw*bh*8 bytes), so gl_decode can read them."""
import sys, struct
from pathlib import Path

def parse_ktx(data):
    # KTX1 magic
    if data[:12] != b"\xabKTX 11\xbb\r\n\x1a\n":
        return None
    endian = struct.unpack_from("<I", data, 12)[0]
    fmt_off = 12 + 4
    # 13 uint32 fields after endian
    fields = struct.unpack_from("<13I", data, fmt_off)
    (gltype, gltype_size, glformat, glinternalformat, glbasefmt,
     w, h, depth, num_array, num_faces, num_mips, kv_size) = fields
    payload_off = fmt_off + 52 + kv_size
    blocks_size = struct.unpack_from("<I", data, payload_off)[0]
    blocks = data[payload_off + 4 : payload_off + 4 + blocks_size]
    return w, h, blocks

def parse_pvr(data):
    # PVR3 header
    if data[:4] != b"PVR\x03":
        return None
    flags, fmt_low, fmt_high, color_space, channel_type, h, w, depth, num_surface, num_face, num_mip, meta = \
        struct.unpack_from("<IIIIIIIIIIII", data, 4)
    payload_off = 52 + meta
    blocks = data[payload_off:]
    return w, h, blocks

def main():
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    data = src.read_bytes()
    r = parse_ktx(data) or parse_pvr(data)
    if not r:
        print(f"unknown format: {src}", file=sys.stderr); sys.exit(1)
    w, h, blocks = r
    bw, bh = w//4, h//4
    expected = bw*bh*8
    blocks = blocks[:expected]
    if len(blocks) != expected:
        print(f"WARN: got {len(blocks)} bytes, expected {expected}", file=sys.stderr)
    with open(dst, "wb") as f:
        f.write(struct.pack("<ii", w, h))
        f.write(blocks)
    print(f"wrote {dst}: {w}x{h} ({len(blocks)} block bytes)")

if __name__ == "__main__":
    main()
