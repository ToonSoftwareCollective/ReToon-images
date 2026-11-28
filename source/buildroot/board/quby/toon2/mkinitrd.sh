#!/bin/sh
# Post-image script: build the small initrd (images/toon2-initrd.cpio.gz) from
# the target tree. It holds only what is needed before the root image on p4 is
# mounted: busybox, the processor firmware the built-in drivers load at boot
# (the WiFi chip's own firmware is on p4, see initrd/init), and a WiFi + SSH
# rescue environment. No credentials: the WiFi configuration lives
# on p4 (toon-os/config/wpa_supplicant.conf).
set -e
IMAGES=$1
BOARD=$(cd "$(dirname "$0")" && pwd)
WORK=$BUILD_DIR/toon2-initrd
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$TARGET_DIR"

FILES="
bin/busybox
lib/ld-linux-armhf.so.3 lib/libc.so.6 lib/libresolv.so.2 usr/lib/libcrypt.so.2
usr/lib/libnl-3.so.200 usr/lib/libnl-genl-3.so.200
usr/sbin/wpa_supplicant usr/sbin/dropbear
usr/bin/toon-ui usr/bin/toon-wifi usr/sbin/iw usr/sbin/wpa_passphrase
usr/bin/toon-splash
usr/share/toon/fonts/Inter-Regular.ttf usr/share/toon/fonts/Inter-SemiBold.ttf
usr/share/toon/fonts/materialdesignicons-webfont.ttf usr/share/toon/fonts/mdi.map
usr/share/udhcpc/default.script
etc/passwd etc/shadow etc/group etc/hosts etc/hostname etc/profile etc/shells
etc/nsswitch.conf etc/resolv.conf etc/inittab etc/fstab
"
for f in $FILES; do
	[ -e "$f" ] || [ -L "$f" ] || { echo "mkinitrd: missing $f" >&2; exit 1; }
	mkdir -p "$WORK/$(dirname "$f")"
	cp -a "$f" "$WORK/$f"
	# follow symlinks one level so the real file comes along too
	if [ -L "$f" ]; then
		t=$(readlink "$f")
		case "$t" in /*) ;; *) t="$(dirname "$f")/$t" ;; esac
		[ -e "$t" ] && [ ! -L "$t" ] && cp -a "$t" "$WORK/$t"
	fi
done
# The libraries those programs ask for and the list above does not name:
# wpa_supplicant uses OpenSSL when the build has it, and without libssl and
# libcrypto here rescue mode would have no WiFi.
READELF=$(ls "$HOST_DIR"/bin/*-readelf | head -n 1)
needed() { "$READELF" -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
todo="bin/busybox usr/sbin/wpa_supplicant usr/sbin/dropbear usr/bin/toon-ui usr/sbin/iw usr/sbin/wpa_passphrase usr/bin/toon-splash"
while [ -n "$todo" ]; do
	next=""
	for b in $todo; do
		for lib in $(needed "$b"); do
			[ -e "$WORK/lib/$lib" ] || [ -e "$WORK/usr/lib/$lib" ] && continue
			for d in lib usr/lib; do
				[ -e "$d/$lib" ] || continue
				mkdir -p "$WORK/$d"
				cp -L "$d/$lib" "$WORK/$d/$lib"
				echo "mkinitrd: added $d/$lib (needed by $b)"
				next="$next $d/$lib"
				break
			done
			[ -e "$WORK/lib/$lib" ] || [ -e "$WORK/usr/lib/$lib" ] ||
				{ echo "mkinitrd: $b needs $lib, which is not in the target tree" >&2; exit 1; }
		done
	done
	todo=$next
done
cp -a lib/firmware "$WORK/lib/"

cd "$WORK"
mkdir -p proc sys dev tmp run var mnt root sbin usr/bin etc/init.d
# busybox applet links, as the target tree has them
(cd "$TARGET_DIR" && find bin sbin usr/bin usr/sbin -type l) | while read -r l; do
	case "$(readlink "$TARGET_DIR/$l")" in
	*busybox) mkdir -p "$(dirname "$l")"; ln -sf /bin/busybox "$l" ;;
	esac
done
cat > etc/init.d/rcS <<'RCS'
#!/bin/sh
RCS
cat > etc/init.d/rcK <<'RCK'
#!/bin/sh
RCK
chmod +x etc/init.d/rcS etc/init.d/rcK
install -m 0755 "$BOARD/initrd/init" init
# dropbear is multi-call: scp is needed to push files to a unit in rescue mode
ln -sf ../sbin/dropbear usr/bin/scp
ln -sf ../sbin/dropbear usr/bin/dropbearkey

"$HOST_DIR/bin/fakeroot" sh -c "mknod -m 600 dev/console c 5 1; chown -R 0:0 .; find . | LC_ALL=C sort | cpio --quiet -o -H newc" | gzip -9 > "$IMAGES/toon2-initrd.cpio.gz"
echo "mkinitrd: $(du -k "$IMAGES/toon2-initrd.cpio.gz" | cut -f1) kB -> $IMAGES/toon2-initrd.cpio.gz"
