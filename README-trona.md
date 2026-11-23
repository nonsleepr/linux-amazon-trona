# Amazon Fire HD 10 (11th gen, 2021, "trona") bring-up

Mainline Linux port for the Amazon Fire HD 10 (2021), codename **trona**,
MediaTek MT8183. See the KiwiFS wiki
(`~/Documents/wiki/personal/devices/fire-hd10-trona/`) and
`/tmp/opencode/fire/research/*.md` for full narrative history, evidence, and
bring-up war stories. This file is the terse "how do I build and flash this"
reference.

## Building

```sh
cp arch/arm64/configs/trona_defconfig .config   # or: make ARCH=arm64 trona_defconfig
nix shell nixpkgs#pkgsCross.aarch64-multiplatform.stdenv.cc \
	nixpkgs#pkgsCross.aarch64-multiplatform.binutils \
	nixpkgs#flex nixpkgs#bison nixpkgs#bc nixpkgs#perl nixpkgs#pahole \
	nixpkgs#elfutils nixpkgs#cpio \
	-c bash -c '
	export CROSS_COMPILE=aarch64-unknown-linux-gnu-
	export CPATH="$(nix build nixpkgs#openssl.dev --no-link --print-out-paths)/include"
	export LIBRARY_PATH="$(nix build nixpkgs#openssl.out --no-link --print-out-paths)/lib"
	make -j$(nproc) ARCH=arm64 Image.gz dtbs modules'
```

`trona_defconfig` is a minimal (`savedefconfig`) snapshot of a `.config` that
has been built, flashed, and live-tested on real hardware (Wi-Fi scanning
real APs, buttons registering input events, battery/charger reporting real
telemetry, audio machine driver compiled clean). `.config` itself is
gitignored — always regenerate it from this defconfig, don't hand-edit a
stale one.

## Packaging and flashing a boot image

The board boots a tiny custom initramfs (`diag/root-v9` in the bring-up
scratch space, not part of this repo) that `switch_root`s into the real
rootfs on `mmcblk0p21` — see `nura-install.md` in the wiki for the full
boot-chain story (watchdog, initramfs size ceiling, etc.) and
`/tmp/opencode/fire/diag/build-diag.sh` for the packaging script. Flash with
`fastboot flash boot <img>` after forcing fastboot via the misc-partition
`bootonce-bootloader` trick (see wiki) — **sudo is required** for `fastboot`
to see the device (Amazon's USB vendor ID `1949` has no udev rule on the dev
host, so unprivileged `fastboot devices` silently finds nothing).

**Deploy the full `modules_install` output together, never individual
`.ko` files onto a running kernel built from a different `.config`.** Mixing
a rebuilt module with an unrelated running `vmlinux` caused a real
`kernel BUG at net/wireless/core.h:150!` crash once (an ABI/struct-layout
mismatch, not a driver bug) — see `wifi-consys-driver.md`.

```sh
make ARCH=arm64 INSTALL_MOD_PATH=/path/to/staging modules_install   # (inside the same nix shell/env as the build)
# then wholesale-replace /lib/modules/<release>/ on the device with the staging output
```

## Firmware (proprietary, not redistributable — extract from this unit's own stock partition)

WiFi/Bluetooth (MediaTek combo chip, MT6630+MT6631) needs **three** files in
`/lib/firmware/` on the device, not just the two originally assumed — see
the "UPDATE" section at the top of `wifi.md`/`wifi-consys-driver.md` for how
this was discovered:

```sh
mkdir -p /mnt/stock_vendor && mount -o ro /dev/mmcblk0p20 /mnt/stock_vendor
cp /mnt/stock_vendor/firmware/ROMv4_be_patch_1_0_hdr.bin /lib/firmware/
cp /mnt/stock_vendor/firmware/ROMv4_be_patch_1_1_hdr.bin /lib/firmware/
cp /mnt/stock_vendor/firmware/WMT_SOC.cfg                /lib/firmware/
cp /mnt/stock_vendor/firmware/WIFI_RAM_CODE_6759         /lib/firmware/
umount /mnt/stock_vendor
modprobe wlan_drv_gen3   # pulls in btif, wmt_drv, wmt_chrdev_wifi, cfg80211 automatically
```

These are proprietary MediaTek/Amazon binaries already legitimately present
on this specific unit's eMMC from the factory — legal to copy for personal
use on this device, **not legal to redistribute** (do not commit them here,
do not upload to pmaports/linux-firmware/any public repo).

## Hardware support status

See `~/Documents/wiki/personal/devices/fire-hd10-trona/index.md` for the
authoritative, up-to-date table. Snapshot as of this writing:

| Hardware | Status |
|---|---|
| Display, touchscreen, GPU, eMMC, backlight, RTC, watchdog | Working |
| USB (gadget/peripheral only, no OTG) | Working |
| Wi-Fi (MT6630/MT6631 CONSYS) | **Working**, confirmed live (real AP scan) |
| Buttons (power + volume) | **Working**, confirmed live |
| Battery/charging (MT6358 gauge + MT6370 charger) | **Working**, confirmed live (capped at 500mA — see Type-C note below) |
| Audio (MT6358 ADDA + 2-GPIO speaker amp) | Driver built, DT wired, **not yet tested live** |
| Accelerometer | `i2c1` enabled; live probe found neither predicted candidate chip — bus device identity still being pinned down |
| Bluetooth (same combo chip) | New HCI glue driver in progress (`bt-wmt-hci` branch) — the chip's power-on path works, but no adaptor existed in-tree at all before this |
| Type-C/USB-PD charging (MT6370 TCPC) | In progress (`typec-mt6370-tcpc` branch) — fixes the 500mA charging ceiling without touching the USB gadget's peripheral mode |
| Camera | **Not feasible** — no MediaTek camera-capture driver has ever existed in mainline for any MT8183 device; see `camera.md` |
| MicroSD slot, ALS/proximity sensor, vibration motor | Not yet investigated |

## Branches

- `device-amazon-trona` — main integration branch (this one).
- `wifi-consys`, `audio-adda`, `accel-buttons` — merged into `device-amazon-trona` already.
- `bt-wmt-hci`, `typec-mt6370-tcpc` — active feature branches, not yet merged (work in progress, build-validated but not live-tested as of this writing).
