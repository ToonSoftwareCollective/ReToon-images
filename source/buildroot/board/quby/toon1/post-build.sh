#!/bin/sh
# Runs on the target tree before the image is made ($1).
# linux-firmware's "Realtek 87xx" set is there for the RTL8712 stick
# (rtlwifi/rtl8712u.bin, r92su); its RTL8723 files are not needed.
rm -f "$1"/lib/firmware/rtlwifi/rtl8723*
# r8712u's options (up to 1.61): a build tree that stays keeps the file
rm -f "$1"/etc/modprobe.d/r8712u.conf
rmdir "$1"/etc/modprobe.d 2>/dev/null || true
