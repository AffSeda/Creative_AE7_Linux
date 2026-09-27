# Debian stopgap: keep an AE-7 working across kernel updates

This is a local workaround for a Debian machine with the ASM1083-bridge
AE-7. It is only needed until a real fix is in the kernel. It builds the patched HDA
modules for each installed kernel from Debian's own `linux-source` package. A boot guard
makes sure the stock driver never touches the card on a kernel without the patch, so a
failed or missing build means a silent AE-7, never a frozen machine.

**Requirements**
- Secure Boot off, or your own module signing arranged. The rebuilt modules are unsigned.
- The following packages, all installed by `install.sh`:
  - `linux-headers-$(uname -r)`, `build-essential`, `patch`, `xz-utils`, `kmod`;
  - `linux-source-X.Y` at exactly the running kernel's version;
  - the `linux-source` metapackage, so the source follows future kernel updates.
- `pciutils`, which provides `lspci`.
- The newest kernel your Debian release offers, which you boot into before installing.

**Quick install:** `sudo ./install.sh`. **Remove:** `sudo ./uninstall.sh`, then keep the
card off the stock driver or it will hang again. The details follow.

## Pieces

| File | Install to | Purpose |
|---|---|---|
| `ae7-hda-flush-posted-writes.patch` | `/usr/local/lib/ae7/` | The workaround patch (same as `../patches/`) |
| `ae7-hda-build` | `/usr/local/sbin/` (0755) | Builds and installs patched `snd-hda-core`, `snd-hda-codec`, `snd-hda-intel` and `snd-hda-codec-ca0132` into `/lib/modules/<kver>/updates/ae7/`; `--all` for every kernel |
| `ae7-hda-guard` | `/usr/local/sbin/` (0755) | modprobe `install` hook: if this kernel has no patched build, parks `1102:0010` on `pci-stub` before loading the stock `snd_hda_intel` |
| `ae7-hda.conf` | `/etc/modprobe.d/` | Wires the guard into `modprobe snd_hda_intel` |
| `zz-ae7-hda` | `/etc/kernel/postinst.d/` (0755) | Builds for each newly installed kernel |
| `99ae7-hda` | `/etc/apt/apt.conf.d/` | Retries after every dpkg run (covers linux-source arriving after linux-image in the same upgrade) |
| `ae7-hda-build.service` | `/etc/systemd/system/` (enable) | Retries at boot |

After installing:
1. Remove any `vfio-pci ids=1102:0010` or `pci-stub` settings you added earlier to keep
   the card off `snd_hda_intel`.
2. Run `ae7-hda-build $(uname -r)`, then `update-initramfs -u`, then reboot.

## How it behaves

- The build only runs when the installed `linux-source-<major.minor>` is exactly the
  same Debian version as `linux-image-<kver>`. If it isn't yet, the build logs
  `waiting for linux-source-...` to `/var/log/ae7-hda-build.log` and retries on the next
  apt run or boot.
- If the patch stops applying to a future source version, the build fails and logs it.
  The guard keeps the card parked, so nothing hangs.
- The guard checks that every AE-7 really is held by `pci-stub` (or `vfio-pci`) before it
  loads `snd_hda_intel`. If it cannot confirm that, it does not load the driver at all
  (no HDA audio that boot) and logs why.
- On a kernel that is parked, the journal shows
  `ae7-hda-guard: no patched HDA modules for <kver>: parking AE-7 ...`.
- To undo:
  1. Remove the files above and `/lib/modules/*/updates/ae7`.
  2. Run `depmod -a`.
  3. Run `update-initramfs -u -k all`.

## Tested

On the test machine (Debian `7.2.6+deb14-rt-amd64`):
- The build completed in about 20 s on a 32-core machine. Slower machines take longer.
- A normal reboot came up with the AE-7 on the patched driver and playing audio.
- Booting an older installed kernel that has no patched build parked the card on
  `pci-stub` as designed. The other audio devices worked and there was no hang.

The kernel-update path (`postinst.d` and the apt hook firing on a real new kernel
version) has not happened yet.
