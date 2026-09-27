#!/bin/bash
# Remove the AE-7 stopgap. Afterwards the STOCK driver will hard-lock the machine again if the
# card is installed: keep the card off snd_hda_intel yourself (e.g. pci-stub.ids=1102:0010) or remove it.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
systemctl disable ae7-hda-build.service 2>/dev/null || true
rm -f /etc/systemd/system/ae7-hda-build.service /etc/apt/apt.conf.d/99ae7-hda \
	/etc/kernel/postinst.d/zz-ae7-hda /etc/modprobe.d/ae7-hda.conf \
	/usr/local/sbin/ae7-hda-build /usr/local/sbin/ae7-hda-guard
rm -rf /usr/local/lib/ae7 /lib/modules/*/updates/ae7
systemctl daemon-reload
for k in /lib/modules/*/; do depmod -a "$(basename "$k")"; done
update-initramfs -u
echo "Removed. WARNING: with the card installed, the stock driver will hang this machine on next boot"
echo "unless you keep the card off snd_hda_intel (for example: kernel parameter pci-stub.ids=1102:0010)."
