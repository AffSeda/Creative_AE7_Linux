#!/bin/bash
# Install the AE-7 (ASM1083 bridge) stopgap on a Debian machine. Run as root from this directory.
# See README.md in this directory for what each piece does, and ../README.md for the risks.
set -euo pipefail
cd "$(dirname "$0")"

die() { echo "install.sh: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo ./install.sh)"
command -v dpkg-query >/dev/null || die "this script is for Debian (dpkg-based) systems only"
K=$(uname -r)
dpkg-query -W "linux-image-$K" >/dev/null 2>&1 || die "running kernel $K is not a Debian linux-image package; use the manual route in ../README.md"

# The fix is for the AE-7 revision with the ASMedia ASM1083/1085 bridge. It is harmless on
# other hardware (one extra register read per HDA register write), so these are only prompts.
# Installing BEFORE fitting the card is the easiest route, so a missing card is allowed.
if ! lspci -n -d 1102:0010 | grep -q .; then
	echo "No Creative CA0132 (1102:0010) found. That is expected if you are installing before fitting the card." >&2
	read -r -p "Install anyway? [y/N] " a; [ "$a" = y ] || exit 1
elif ! lspci -n -d 1b21:1080 | grep -q .; then
	echo "Card found but no ASMedia ASM1083/1085 bridge (1b21:1080): this looks like the IDT revision, which does not need this." >&2
	read -r -p "Install anyway? [y/N] " a; [ "$a" = y ] || exit 1
fi

# Unsigned modules: refuse under Secure Boot rather than leave a guard that parks the card forever.
if command -v mokutil >/dev/null && mokutil --sb-state 2>/dev/null | grep -qi 'enabled'; then
	die "Secure Boot is enabled; the rebuilt modules are unsigned and will not load. Disable Secure Boot first (or arrange your own module signing)."
fi

MM=$(echo "$K" | cut -d. -f1,2)
echo "== installing build dependencies (linux-source, headers for $K, build tools)"
apt-get install -y linux-source "linux-headers-$K" build-essential patch xz-utils kmod

echo "== installing files"
install -D -m 0644 ae7-hda-flush-posted-writes.patch /usr/local/lib/ae7/ae7-hda-flush-posted-writes.patch
install -m 0755 ae7-hda-build ae7-hda-guard /usr/local/sbin/
install -m 0644 ae7-hda.conf /etc/modprobe.d/ae7-hda.conf
install -m 0755 zz-ae7-hda /etc/kernel/postinst.d/zz-ae7-hda
install -m 0644 99ae7-hda /etc/apt/apt.conf.d/99ae7-hda
install -m 0644 ae7-hda-build.service /etc/systemd/system/ae7-hda-build.service
systemctl daemon-reload
systemctl enable ae7-hda-build.service

echo "== building patched modules for $K (about a minute)"
if ! /usr/local/sbin/ae7-hda-build "$K"; then
	echo "Build did not complete; see /var/log/ae7-hda-build.log." >&2
	echo "The guard is installed, so the card stays parked (silent, no hang) on this kernel." >&2
	echo "Common cause: linux-source-$MM is not yet at the same version as linux-image-$K (apt upgrade, then re-run)." >&2
fi

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
