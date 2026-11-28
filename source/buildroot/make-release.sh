#!/bin/sh
# Collect the files a Toon needs from a finished Buildroot build, under the
# names the ReToon setup app and the updater use, with their checksums, and
# pack them as one archive.
#   buildroot/make-release.sh <buildroot directory> <version> [output directory] [toon2|toon1]
# Result: <output directory>/retoon-<version>.tar.gz (Toon 2) or
# retoon-toon1-<version>.tar.gz (Toon 1) and, unpacked next to it, the
# folder of the same name (default output directory: ./release).
#   Toon 2: kernel, device tree, initrd, system image, boot script
#   Toon 1: kernel with its start-up built in, system image
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BR=${1:?buildroot directory}
VERSION=${2:?version}
OUT=${3:-$PWD/release}
BOARD=${4:-toon2}
IMAGES=$BR/output/images

case "$BOARD" in
toon2) NAME=retoon-$VERSION ;;
toon1) NAME=retoon-toon1-$VERSION ;;
*) echo "unknown board $BOARD" >&2; exit 2 ;;
esac
DIR=$OUT/$NAME
rm -rf "$DIR"
mkdir -p "$DIR"
if [ "$BOARD" = toon2 ]; then
	cp "$IMAGES/zImage" "$DIR/toon2-zImage"
	cp "$IMAGES/imx6sx-toon2.dtb" "$DIR/toon2.dtb"
	cp "$IMAGES/rootfs.squashfs" "$DIR/rootfs.squashfs"
	python3 "$HERE/boot/build_initrd_uimage.py" "$IMAGES/toon2-initrd.cpio.gz" \
		"$DIR/toon2-initrd.cpio.gz.u-boot"
	python3 "$HERE/boot/build_bootscr.py" "$HERE/boot/boot_toon2.cmd" "$DIR/boot.scr" toon2-boot
	FILES="toon2-zImage toon2.dtb toon2-initrd.cpio.gz.u-boot rootfs.squashfs boot.scr"
else
	cp "$IMAGES/uImage-toon1-flash" "$DIR/uImage-toon1-flash"
	cp "$IMAGES/rootfs.squashfs" "$DIR/rootfs.squashfs"
	FILES="uImage-toon1-flash rootfs.squashfs"
fi
chmod 644 "$DIR"/*
# the checksums, as files-toon2.sha256 (files-toon1.sha256) of a release
(cd "$DIR" && sha256sum $FILES > files.sha256)
echo "$VERSION" > "$DIR/version.txt"
tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "made $OUT/$NAME.tar.gz:"
tar -tzvf "$OUT/$NAME.tar.gz"
