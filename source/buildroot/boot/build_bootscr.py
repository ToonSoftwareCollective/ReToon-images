"""Build a U-Boot legacy IH_TYPE_SCRIPT image (boot.scr) with the multi-file TOC
the Toon 2's U-Boot requires: [64B header][u32 len][u32 0][script bytes].
usage: build_bootscr.py <script.txt> <out.scr> <name> [ih_time]
"""
import struct, sys, zlib, time

src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
ih_time = int(sys.argv[4]) if len(sys.argv) > 4 else int(time.time())
script = open(src, "rb").read()
payload = struct.pack(">II", len(script), 0) + script
dcrc = zlib.crc32(payload) & 0xffffffff
# os=linux(5) arch=arm(2) type=script(6) comp=none(0)
tail = bytes([5, 2, 6, 0]) + name.encode()[:32].ljust(32, b"\0")
hdr = struct.pack(">7I", 0x27051956, 0, ih_time, len(payload), 0, 0, dcrc) + tail
hcrc = zlib.crc32(hdr) & 0xffffffff
hdr = struct.pack(">7I", 0x27051956, hcrc, ih_time, len(payload), 0, 0, dcrc) + tail
open(out, "wb").write(hdr + payload)
print("wrote", out, len(hdr + payload), "bytes")
