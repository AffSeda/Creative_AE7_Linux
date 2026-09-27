#!/bin/bash
# Remove the AE-7 stopgap. Afterwards the STOCK driver will hard-lock the machine again if the
# card is installed: remove the card, or keep it off snd_hda_intel (see the message printed at the end).
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
systemctl disable ae7-hda-build.service 2>/dev/null || true
rm -f /etc/systemd/system/ae7-hda-build.service /etc/apt/apt.conf.d/99ae7-hda \
	/etc/kernel/postinst.d/zz-ae7-hda /etc/modprobe.d/ae7-hda.conf \
	/usr/local/sbin/ae7-hda-build /usr/local/sbin/ae7-hda-guard
rm -rf /usr/local/lib/ae7 /lib/modules/*/updates/ae7
systemctl daemon-reload
for k in /lib/modules/*/; do depmod -a "$(basename "$k")"; done
update-initramfs -u -k all
echo "Removed. WARNING: with the card installed, the stock driver will hang this machine on next boot."
echo "Remove the card, or keep it off snd_hda_intel by handing it to vfio-pci, e.g. create"
echo "/etc/modprobe.d/ae7-park.conf containing these two lines and run update-initramfs -u:"
echo "  options vfio-pci ids=1102:0010"
echo "  softdep snd_hda_intel pre: vfio-pci"
