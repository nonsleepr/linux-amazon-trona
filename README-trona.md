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
| Display, touchscreen, GPU (Panfrost, GLES 3.1), eMMC, backlight, RTC, watchdog | Working; glmark2 smooth, Firefox WebGL on the GPU |
| USB (gadget/peripheral only, no OTG) | Working |
| Wi-Fi (MT6630/MT6631 CONSYS) | **Working**, 5 GHz, autoconnects via NetworkManager |
| Buttons (power + volume) | **Working** |
| Battery/charger reporting (MT6358 gauge + MT6370 charger) | **Working**; charging current is whatever the source gives (500 mA on a PC port) |
| Audio (MT6358 ADDA + 2-GPIO speaker amp) | Sound card registers; **audible output not yet tested** |
| Accelerometer (STK8BA5x at 0x18) and light sensor (Lite-On at 0x53) | Nodes committed on `accel-buttons`; **not yet on the tablet** |
| Bluetooth | Driver on `bt-wmt-hci` (vendor init, IDME address); **not yet on the tablet**, untested |
| Type-C (MT6370 TCPC, sink-only) | On `typec-mt6370-tcpc`; **not yet on the tablet**. It does not change the charging limit by itself |
| Suspend | **Not working yet**: `MT8183_SPM_SUSPEND` is off, deep suspend never exercised |
| Idle power, thermal sensor | **Open**: `cpuidle.off=1`, no thermal zones; being worked on (`power-idle`, `thermal-gauge`) |
| Camera | **Not feasible** - no MediaTek camera-capture driver exists upstream for MT8183; see `camera.md` |
| MicroSD slot, vibration motor | Not yet investigated |

## Branches

- `device-amazon-trona` - main integration branch (this one).
- `wifi-consys`, `audio-adda` - merged. `accel-buttons` is merged up to the
  i2c1 enablement; its later light-sensor and accelerometer nodes are not.
- Not merged yet, all build-validated: `bt-wmt-hci`, `typec-mt6370-tcpc`,
  `accel-buttons` (later commits), `power-idle`, `thermal-gauge`.
- The next flashed kernel should merge all of them and fix the boot script
  (a leftover `/init` subshell spins at ~50% of a core after `switch_root`).
