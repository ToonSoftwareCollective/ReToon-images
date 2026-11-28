"""Wrap a gzip'd cpio as a U-Boot legacy ramdisk image (same as
mkimage -A arm -O linux -T ramdisk -C gzip).
usage: build_initrd_uimage.py <initrd.cpio.gz> <out.u-boot> [name]
"""
import struct, sys, time, zlib

data = open(sys.argv[1], "rb").read()
name = (sys.argv[3] if len(sys.argv) > 3 else "toon2-initrd").encode()[:32].ljust(32, b"\0")
t = int(time.time())
dcrc = zlib.crc32(data) & 0xffffffff
tail = bytes([5, 2, 3, 1]) + name		# linux, arm, ramdisk, gzip
hdr = struct.pack(">7I", 0x27051956, 0, t, len(data), 0, 0, dcrc) + tail
hdr = struct.pack(">7I", 0x27051956, zlib.crc32(hdr) & 0xffffffff, t, len(data), 0, 0, dcrc) + tail
open(sys.argv[2], "wb").write(hdr + data)
print("wrote", sys.argv[2], len(hdr) + len(data), "bytes")
