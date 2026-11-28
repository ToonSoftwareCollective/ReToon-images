#!/bin/sh
# Post-image script: build the small initrd (images/toon1-initrd.uboot, a
# U-Boot ramdisk, for network boots) and the flash kernel with it built in
# from the target tree. It holds only what is needed before the root image on
# the original UBIFS is mounted, plus a serial login for toon.rescue. No
# credentials.
set -e
IMAGES=$1
BOARD=$(cd "$(dirname "$0")" && pwd)
WORK=$BUILD_DIR/toon1-initrd
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$TARGET_DIR"

FILES="
etc/passwd etc/shadow etc/group etc/hosts etc/hostname etc/profile etc/shells
etc/nsswitch.conf etc/inittab etc/fstab
"
for f in $FILES; do
	[ -e "$f" ] || [ -L "$f" ] || { echo "mkinitrd: missing $f" >&2; exit 1; }
	mkdir -p "$WORK/$(dirname "$f")"
	cp -a "$f" "$WORK/$f"
done

# A busybox of its own: static and with only what the boot manager uses
# (initrd/busybox.options), from the same source as the system's busybox.
# The system's busybox would bring glibc along: about 600 kB more in the
# kernel partition.
BBSRC=$(ls -d "$BUILD_DIR"/busybox-[0-9]* | head -n 1)
BB=$BUILD_DIR/toon1-initrd-busybox
CROSS=$(ls "$HOST_DIR"/bin/*-linux-gnueabi-gcc | head -n 1); CROSS=${CROSS%gcc}
# initrd/busybox-*.patch: fixes for this busybox only (ubiattach -O)
newer=
for f in "$BOARD"/initrd/busybox.options "$BOARD"/initrd/busybox-*.patch "$BBSRC/.stamp_built"; do
	[ "$f" -nt "$BB/busybox" ] && newer=1
done
if [ ! -x "$BB/busybox" ] || [ -n "$newer" ]; then
	rm -rf "$BB"
	cp -a "$BBSRC" "$BB"
	for p in "$BOARD"/initrd/busybox-*.patch; do
		patch -s -p1 -d "$BB" < "$p" || { echo "mkinitrd: $p does not apply" >&2; exit 1; }
	done
	make -s -C "$BB" distclean > /dev/null 2>&1 || true
	make -s -C "$BB" allnoconfig > /dev/null
	for o in $(grep -v '^#' "$BOARD/initrd/busybox.options"); do
		sed -i "s/^# CONFIG_$o is not set\$/CONFIG_$o=y/" "$BB/.config"
		grep -q "^CONFIG_$o=y" "$BB/.config" ||
			{ echo "mkinitrd: busybox has no CONFIG_$o" >&2; exit 1; }
	done
	yes '' | make -s -C "$BB" oldconfig > /dev/null
	make -s -C "$BB" CROSS_COMPILE="$CROSS" -j2 busybox busybox.links > /dev/null
fi
mkdir -p "$WORK/bin"
install -m 0755 "$BB/busybox" "$WORK/bin/busybox"
cd "$WORK"
mkdir -p proc sys dev tmp run var mnt root sbin usr/bin etc/init.d
# the applets of that busybox, as links
while read -r l; do
	l=${l#/}
	[ "$l" = bin/busybox ] && continue
	mkdir -p "$(dirname "$l")"
	ln -sf /bin/busybox "$l"
done < "$BB/busybox.links"
printf '#!/bin/sh\n' > etc/init.d/rcS
printf '#!/bin/sh\n' > etc/init.d/rcK
chmod +x etc/init.d/rcS etc/init.d/rcK
install -m 0755 "$BOARD/initrd/init" init
# the start-up picture right away (toon-ui's static build, see initrd/init)
[ -x "$IMAGES/toon-splash-static" ] || { echo "mkinitrd: no $IMAGES/toon-splash-static" >&2; exit 1; }
install -m 0755 "$IMAGES/toon-splash-static" bin/toon-splash

"$HOST_DIR/bin/fakeroot" sh -c "mknod -m 600 dev/console c 5 1; chown -R 0:0 .; find . | LC_ALL=C sort | cpio --quiet -o -H newc" \
	> "$IMAGES/toon1-initrd.cpio"
xz -9 --check=crc32 < "$IMAGES/toon1-initrd.cpio" > "$IMAGES/toon1-initrd.cpio.xz"
"$HOST_DIR/bin/mkimage" -A arm -O linux -T ramdisk -C none -n toon1-initrd \
	-d "$IMAGES/toon1-initrd.cpio.xz" "$IMAGES/toon1-initrd.uboot" > /dev/null
echo "mkinitrd: $(du -k "$IMAGES/toon1-initrd.uboot" | cut -f1) kB -> $IMAGES/toon1-initrd.uboot"

# The flash kernel (images/uImage-toon1-flash, for the "kernel" partition):
# the same kernel with this initrd built in, because the Toon's own U-Boot's
# boot_nand starts it without a ramdisk. One xz stream for both is also
# smaller. Must fit the 3 MiB partition with room for bad blocks.
LINUX=$(ls -d "$BUILD_DIR"/linux-[0-9]* | head -n 1)
CROSS=$(ls "$HOST_DIR"/bin/*-linux-gnueabi-gcc | head -n 1); CROSS=${CROSS%gcc}
"$LINUX/scripts/config" --file "$LINUX/.config" \
	--set-str INITRAMFS_SOURCE "$IMAGES/toon1-initrd.cpio" \
	--set-val INITRAMFS_ROOT_UID 0 --set-val INITRAMFS_ROOT_GID 0 \
	-e INITRAMFS_COMPRESSION_NONE -d INITRAMFS_COMPRESSION_XZ
make -s -C "$LINUX" ARCH=arm CROSS_COMPILE="$CROSS" KCFLAGS=-Wno-attribute-alias olddefconfig
make -s -C "$LINUX" ARCH=arm CROSS_COMPILE="$CROSS" KCFLAGS=-Wno-attribute-alias -j2 zImage
DTB=$(find "$LINUX/arch/arm/boot/dts" -name imx27-toon1.dtb | head -n 1)
cat "$LINUX/arch/arm/boot/zImage" "$DTB" > "$BUILD_DIR/zImage-flash"
"$HOST_DIR/bin/mkimage" -A arm -O linux -T kernel -C none -a 0xa0008000 -e 0xa0008000 \
	-n toon1-retoon -d "$BUILD_DIR/zImage-flash" "$IMAGES/uImage-toon1-flash" > /dev/null
size=$(wc -c < "$IMAGES/uImage-toon1-flash")
echo "mkinitrd: flash kernel $size bytes -> $IMAGES/uImage-toon1-flash"
# 3 MiB less one erase block of room for a bad block
[ "$size" -le $((0x300000 - 0x20000)) ] || { echo "mkinitrd: flash kernel too big" >&2; exit 1; }
