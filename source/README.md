# Building your own ReToon image

This folder has the source of ReToon: everything needed to build the same
images as the releases in this repository, for the Toon 2 and the Toon 1.

| Folder | What is in it |
|---|---|
| `buildroot/` | The Buildroot additions: board files of both Toons (kernel patches, device trees, kernel and busybox settings, start-up), the two configurations (`configs/quby_toon2_defconfig`, `configs/quby_toon1_defconfig`), the extra packages, `setup-buildroot.sh` and `make-release.sh` |
| `retoon/` | The ReToon programs (screen, web interface, thermostat, MQTT and Home Assistant, Z-Wave, ESPHome, sound, M4 firmware, Zigbee/Thread radio), each with its Buildroot package files |
| `COPYING` | The licence: GPL-2.0-or-later, unless a file says otherwise |

## What you need

- A Linux computer (x86_64 or aarch64) with the tools Buildroot and the
  kernel need. On Debian or Ubuntu:

  ```sh
  sudo apt install build-essential bc bison flex cpio file git patch perl \
    python3 rsync unzip wget curl xz-utils libssl-dev libncurses-dev
  ```

- About 25 GB free disk space per Toon model, and an internet connection
  (Buildroot downloads the sources of all packages).
- Time: a first build takes one to a few hours per Toon model, depending on
  the computer. Later builds only redo what changed.

## 1. Get the source

```sh
git clone https://github.com/ToonSoftwareCollective/ReToon-images.git
cd ReToon-images/source
```

## 2. Prepare a Buildroot tree

Use a separate tree for each Toon model. `setup-buildroot.sh` downloads
Buildroot 2025.02.12 when the folder does not exist yet and puts the ReToon
files on top of it:

```sh
./buildroot/setup-buildroot.sh ~/retoon-toon2      # for a Toon 2
./buildroot/setup-buildroot.sh ~/retoon-toon1      # for a Toon 1
```

Run it again after you change something here or pull a newer version: it
copies the files again.

## 3. Toon 2 only: one firmware file

The Toon 2 image needs the i.MX SDMA firmware from linux-firmware (tag
20250211), as listed in `buildroot/firmware-files.txt` with its checksum:

```sh
cd ~/retoon-toon2
mkdir -p board/quby/toon2/rootfs-overlay/lib/firmware/imx/sdma
curl -L -o board/quby/toon2/rootfs-overlay/lib/firmware/imx/sdma/sdma-imx6q.bin \
  https://gitlab.com/kernel-firmware/linux-firmware/-/raw/20250211/imx/sdma/sdma-imx6q.bin
sha256sum board/quby/toon2/rootfs-overlay/lib/firmware/imx/sdma/sdma-imx6q.bin
# must be 8295b3a4e1aa7b41cabca166da2357848fd172203893e94ecb84fe42143f495e
```

The WiFi and Bluetooth firmware are not in the image: ReToon uses the files
that are already on the Toon.

The Toon 1 needs no extra files.

## 4. Build

Give your build a version name first; the screen and the web interface show
it (the releases write their number there the same way):

```sh
cd ~/retoon-toon2
echo my-1 > board/quby/toon2/rootfs-overlay/etc/retoon-version
make quby_toon2_defconfig
make
```

On a Toon 1 the file is `board/quby/toon1/rootfs-overlay/etc/retoon-version`.

For a Toon 1, the same in its own tree with `make quby_toon1_defconfig`.
The Toon 1 configuration builds with 2 jobs at a time; to use more, run
`make menuconfig` first and set *Build options → Number of jobs to run
simultaneously* (0 uses all cores).

The results are in `output/images/`.

## 5. Pack the image

`make-release.sh` collects the files under the names the ReToon setup app
and the updater use, with their checksums, and makes one archive:

```sh
cd ~/ReToon-images/source
./buildroot/make-release.sh ~/retoon-toon2 my-1 ~/retoon-release toon2
./buildroot/make-release.sh ~/retoon-toon1 my-1 ~/retoon-release toon1
```

`my-1` is the version name you give it. This makes
`~/retoon-release/retoon-my-1.tar.gz` (Toon 2) and
`~/retoon-release/retoon-toon1-my-1.tar.gz` (Toon 1), each with a folder of
the same name next to it that holds the files and `files.sha256`.

## 6. Put it on a Toon

**A Toon that runs ReToon already.** Serve the archive and its checksum
file from your computer under the names of a release, and let the Toon's
own updater fetch them. On the computer (Toon 2; for a Toon 1 use `toon1`
and `retoon-toon1-my-1`):

```sh
mkdir -p ~/retoon-serve && cd ~/retoon-serve
cp ~/retoon-release/retoon-my-1.tar.gz retoon-toon2.tar.gz
cp ~/retoon-release/retoon-my-1/files.sha256 files-toon2.sha256
python3 -m http.server 8000
```

Then log in on the Toon over SSH as `root` (the password set in the web
interface, or `toon`) and run:

```sh
TOON_UPDATE_SRC=http://<address of your computer>:8000 toon-update install
cat /run/toon/update     # wait until it says the new version is in place
reboot
```

**A Toon with the original software.** Install the ReToon app as usual, and
put your archive in the app's folder (`/qmf/qml/apps/retoon`, next to
`retoon.sh`) under the name `retoon-toon2.tar.gz` (Toon 1:
`retoon-toon1.tar.gz`) before you press the install button. The app then
uses your archive instead of downloading the latest release.

**Automatic updates.** In the web interface, under System, "Update by
itself" installs the newest official release when it finds one, and that
replaces your own image. Set it to off while you run your own build.

## Updating to a newer version

```sh
cd ~/ReToon-images && git pull
cd source
./buildroot/setup-buildroot.sh ~/retoon-toon2
```

`setup-buildroot.sh` makes the ReToon files in the tree the same as here,
also removing files that are gone; your firmware file and version name stay.
Then build everything again: Buildroot does not remove what a package
installed once, even when it is no longer used, so start from a clean
output (the downloads in `dl/` are kept, so this goes faster than the first
time):

```sh
cd ~/retoon-toon2
echo my-2 > board/quby/toon2/rootfs-overlay/etc/retoon-version
make clean
make quby_toon2_defconfig
make
```

The same for a Toon 1 in its own tree. Then pack and install as above.

## Changing things

The ReToon programs are in `retoon/<program>/src`; the web interface is in
`retoon/toon-ui/web`. After a change, run `setup-buildroot.sh` again for
your tree, then in the tree:

```sh
make <package>-dirclean && make
```

for example `make toon-ui-dirclean && make`, and pack and install as above.
