#!/bin/bash
# Install the AE-7 (ASM1083 bridge) stopgap on a Debian machine. Run as root from this directory.
# See README.md in this directory for what each piece does, and ../README.md for the risks.
set -euo pipefail
cd "$(dirname "$0")"

die() { echo "install.sh: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo ./install.sh)"
command -v dpkg-query >/dev/null || die "this script is for Debian (dpkg-based) systems only"
command -v lspci >/dev/null || die "lspci not found: install pciutils first (apt-get install pciutils)"
K=$(uname -r)
IMGVER=$(dpkg-query -W -f='${Version}' "linux-image-$K" 2>/dev/null) || die "running kernel $K is not a Debian linux-image package; use the manual route in ../README.md"
MM=$(echo "$K" | cut -d. -f1,2)

# The fix is for the AE-7 revision with the ASMedia ASM1083/1085 bridge. On other hardware it
# should only cost one extra register read per HDA register write, so these are only prompts.
# Installing BEFORE fitting the card is the easiest route, so a missing card is allowed.
if ! lspci -n -d 1102:0010 | grep -q .; then
	echo "No Creative CA0132 (1102:0010) found. That is expected if you are installing before fitting the card." >&2
	read -r -p "Install anyway? [y/N] " a; [ "$a" = y ] || exit 1
elif ! lspci -n -d 1b21:1080 | grep -q .; then
	echo "Card found but no ASMedia ASM1083/1085 bridge (1b21:1080): this looks like the IDT revision, which does not need this." >&2
	read -r -p "Install anyway? [y/N] " a; [ "$a" = y ] || exit 1
fi

# Unsigned modules: refuse under Secure Boot rather than leave a guard that parks the card forever.
# Read the EFI variable directly (works without mokutil); a non-EFI (BIOS) boot has no Secure Boot.
SBVAR=/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-e0f901e2c1ab
if [ -r "$SBVAR" ] && [ "$(od -An -t u1 "$SBVAR" | awk '{print $NF}')" = 1 ]; then
	die "Secure Boot is enabled; the rebuilt modules are unsigned and will not load. Disable Secure Boot first (or arrange your own module signing)."
fi

echo "== installing build dependencies (headers for $K, build tools)"
apt-get install -y "linux-headers-$K" build-essential patch xz-utils kmod
# The patch must be built from the source of EXACTLY the running kernel's version.
echo "== installing linux-source-$MM version $IMGVER (must match the running kernel)"
if ! apt-get install -y "linux-source-$MM=$IMGVER"; then
	die "linux-source-$MM version $IMGVER is not available from your apt sources. This usually means
your running kernel is not the newest one Debian offers. Update first (apt full-upgrade), reboot
into the new kernel (see ../README.md for how to boot safely with the card fitted), then re-run.
If you run a backports kernel, enable the same backports suite so its linux-source is available."
fi

echo "== installing files"
install -D -m 0644 ae7-hda-flush-posted-writes.patch /usr/local/lib/ae7/ae7-hda-flush-posted-writes.patch
install -m 0755 ae7-hda-build ae7-hda-guard /usr/local/sbin/
install -m 0644 ae7-hda.conf /etc/modprobe.d/ae7-hda.conf
install -m 0755 zz-ae7-hda /etc/kernel/postinst.d/zz-ae7-hda
install -m 0644 99ae7-hda /etc/apt/apt.conf.d/99ae7-hda
install -m 0644 ae7-hda-build.service /etc/systemd/system/ae7-hda-build.service
systemctl daemon-reload
systemctl enable ae7-hda-build.service

echo "== building patched modules for $K (under a minute on a fast machine, longer on slow ones)"
if ! /usr/local/sbin/ae7-hda-build "$K"; then
	echo "Build did not complete; see /var/log/ae7-hda-build.log." >&2
	echo "The guard is installed, so the card stays parked (silent, no hang) on this kernel." >&2
fi

# Keep the kernel source following future kernel updates, so the hooks can rebuild for them.
# (The build above already used the exact-version source; upgrading it now is harmless.)
echo "== installing the linux-source metapackage (tracks future kernel updates)"
apt-get install -y linux-source || echo "note: could not install the linux-source metapackage; future kernels will need linux-source-X.Y installed by hand" >&2

echo "== refreshing initramfs"
update-initramfs -u -k "$K"

echo
echo "Done. Before rebooting, remove any workaround you added earlier to keep the card off"
echo "snd_hda_intel, or it will stay parked:"
grep -rsHnE '^[[:space:]]*(blacklist[[:space:]]+snd_hda_intel|install[[:space:]]+snd_hda_intel|options[[:space:]]+(vfio-pci|vfio_pci|pci-stub|pci_stub)[[:space:]].*1102:0010|softdep[[:space:]]+snd_hda_intel[[:space:]]+pre:.*vfio)' /etc/modprobe.d/ \
	| grep -v '/ae7-hda.conf:' | sed 's/^/  remove or comment out: /' || true
grep -qE 'pci-stub.ids=[^ ]*1102:0010|vfio-pci.ids=[^ ]*1102:0010' /proc/cmdline \
	&& echo "  check: your kernel command line has a pci-stub/vfio/blacklist entry for the card" || true
grep -qE 'modprobe.blacklist=[^ ]*snd_hda_intel' /proc/cmdline \
	&& echo "  (this boot used modprobe.blacklist=snd_hda_intel from the boot menu; that is fine, just do not add it next time)" || true
echo "Then reboot (or shut down and fit the card). Afterwards 'lspci -k -d 1102:0010' should show"
echo "'Kernel driver in use: snd_hda_intel'."
