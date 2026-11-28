#!/bin/sh
# Put these files on top of a fresh Buildroot 2025.02.12 tree.
#   buildroot/setup-buildroot.sh [directory]     (default: ./buildroot-2025.02.12)
# Afterwards add the firmware files listed in firmware-files.txt
# and then: make quby_toon2_defconfig && make
# (Toon 1: make quby_toon1_defconfig && make, no firmware files needed)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
# where the files are (the public source has another layout)
SRC_BOARD=$HERE/board/quby
SRC_CONFIGS=$HERE/configs
SRC_PACKAGES="$HERE/package/*/ $HERE/../retoon/*/"
FIRMWARE_LIST=$HERE/firmware-files.txt
DEST=${1:-$PWD/buildroot-2025.02.12}
command -v rsync > /dev/null || { echo "rsync is needed" >&2; exit 1; }
if [ ! -f "$DEST/Makefile" ]; then
	curl -L -o /tmp/buildroot-2025.02.12.tar.xz https://buildroot.org/downloads/buildroot-2025.02.12.tar.xz
	mkdir -p "$(dirname "$DEST")"
	# unpack in a folder of its own: next to DEST there can already be a
	# tree called buildroot-2025.02.12 (the Toon 2 one, when DEST is the
	# Toon 1 tree)
	tmp=$(mktemp -d "$(dirname "$DEST")/.buildroot-unpack.XXXXXX")
	tar -xf /tmp/buildroot-2025.02.12.tar.xz -C "$tmp"
	mv "$tmp/buildroot-2025.02.12" "$DEST"
	rmdir "$tmp"
fi
# Our files, the same as here: also on a tree that was set up before, where
# files that are gone here have to go there too (an old device tree would
# still be built). What you add yourself stays: the firmware files and the
# version name in the overlays.
mkdir -p "$DEST/board/quby"
rsync -a --delete --exclude '/*/rootfs-overlay/lib/firmware/' \
	--exclude '/*/rootfs-overlay/etc/retoon-version' "$SRC_BOARD/" "$DEST/board/quby/"
cp "$SRC_CONFIGS"/quby_*_defconfig "$DEST/configs/"
for dir in $SRC_PACKAGES; do
	rsync -a --delete "$dir" "$DEST/package/$(basename "$dir")/"
done
# Make every package of ours known to Buildroot's menu. Also on a tree that
# was set up before: a package added since then would otherwise be left out
# of the build without a word.
for dir in $SRC_PACKAGES; do
	name=$(basename "$dir")
	grep -q "package/$name/Config.in" "$DEST/package/Config.in" ||
		sed -i "s|^\tsource \"package/i2c-tools/Config.in\"\$|&\n\tsource \"package/$name/Config.in\"|" \
			"$DEST/package/Config.in"
	grep -q "package/$name/Config.in" "$DEST/package/Config.in" ||
		{ echo "could not add package/$name to $DEST/package/Config.in" >&2; exit 1; }
done
echo "Buildroot tree ready in $DEST"
echo "Missing firmware (not in this repository):"
sed 's/^/  /' "$FIRMWARE_LIST"
