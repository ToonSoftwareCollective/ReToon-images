#!/bin/sh
# Put these files on top of a fresh Buildroot 2025.02.12 tree.
#   buildroot/setup-buildroot.sh [directory]     (default: ./buildroot-2025.02.12)
# Afterwards add the firmware files listed in firmware-files.txt
# and then: make quby_toon2_defconfig && make
# (Toon 1: make quby_toon1_defconfig && make, no firmware files needed)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-$PWD/buildroot-2025.02.12}
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
cp -r "$HERE/board" "$HERE/configs" "$HERE/package" "$DEST/"
cp -r "$HERE"/../retoon/* "$DEST/package/"
# Make every package of ours known to Buildroot's menu. Also on a tree that
# was set up before: a package added since then would otherwise be left out
# of the build without a word.
for dir in "$HERE"/package/*/ "$HERE"/../retoon/*/; do
	name=$(basename "$dir")
	grep -q "package/$name/Config.in" "$DEST/package/Config.in" ||
		sed -i "s|^\tsource \"package/i2c-tools/Config.in\"\$|&\n\tsource \"package/$name/Config.in\"|" \
			"$DEST/package/Config.in"
	grep -q "package/$name/Config.in" "$DEST/package/Config.in" ||
		{ echo "could not add package/$name to $DEST/package/Config.in" >&2; exit 1; }
done
echo "Buildroot tree ready in $DEST"
echo "Missing firmware (not in this repository):"
sed 's/^/  /' "$HERE/firmware-files.txt"
