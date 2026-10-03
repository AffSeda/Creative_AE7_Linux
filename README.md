# Creative Sound Blaster AE-7 on Linux (ASMedia ASM1083 bridge revision)

The newer revision of the Creative Sound Blaster AE-7 hard-locks any Linux machine as
soon as `snd_hda_intel` initialises it. Kernel bugzilla 217510 and Debian #1115581 are
reports of this hang. The card works under Windows, and the older IDT-bridge revision of
the card is reported to work under Linux.

This repository holds a root-cause investigation, reproducible on real hardware, and a
working (blunt) fix:

- **Cause (proven on the test system):** a burst of back-to-back posted MMIO writes to
  the card's CA0132 HDA controller, through the ASMedia ASM1083/1085 PCIe-to-PCI bridge
  (`1b21:1080` rev 03) on the card, wedges the path. The next read never completes, and
  the machine locks up.
  - Each register write is harmless on its own.
  - The driver's exact init sequence hangs when replayed without reads in between. The
    same sequence with a read-back after each write does not.
- **Workaround (proven on the test system):** read back after every HDA register write,
  and after every CA0132 BAR2 write. With that, the card probes, the DSP loads, and audio
  plays through the line-out.

Start with **[ANALYSIS.md](ANALYSIS.md)**. It has the test matrix, the evidence, what is
proven and what is inferred, and a suggested shape for a proper upstream fix.

## Using the workaround now

This is for people with the ASM1083-bridge AE-7 who want sound before a real kernel fix
exists. It replaces four sound modules with rebuilt ones, so read the whole section first.

**Which card revision?** The affected revision has an ASMedia ASM1083/1085 bridge chip on
the card. In Linux it shows up in `lspci -nn` as `1b21:1080`, next to the card's own
`1102:0010`. The older IDT-bridge revision works without this. You do not have to be sure
before installing: on other hardware the workaround should only cost one extra register
read per HDA register write. It has only been run on the test machine.

**The catch: you cannot simply boot with the card fitted.** The stock driver attaches
while the system boots, so the machine may lock up before you reach a desktop. Use one of
these:

- **A. Install first, then fit the card (easiest).** Run the installer with the card
  removed. It will say the card was not found; answer `y`. Then shut down, fit the card,
  and boot normally.
- **B. The card is already fitted.** For one boot, stop the sound driver from loading.
  1. At the boot menu, select your normal entry and press `e` (GRUB and systemd-boot both
     use `e`).
  2. On the line starting with `linux` (GRUB) or on the options line (systemd-boot), add
     ` modprobe.blacklist=snd_hda_intel` at the end.
  3. Boot with Ctrl+X or F10 (GRUB) or Enter (systemd-boot).

  That boot has no sound at all, including onboard and HDMI, but it is stable. Run the
  installer, then reboot normally without the extra parameter. The edit only lasts for
  that one boot.

  If pressing `e` does nothing on systemd-boot, the editor is disabled (`editor no` in
  `loader/loader.conf`). Use option A: take the card out, install, then refit it.

**Requirements:**
- **Secure Boot must be off.** The rebuilt modules are unsigned, and the installer
  refuses to run with Secure Boot on. If you sign your own modules, you can adapt it.
- Root access and about 1 GB free (the kernel source package).
- **Be on the newest kernel your Debian release offers.** Update with `apt full-upgrade`
  and reboot first. The build needs Debian's kernel source at exactly your running
  kernel's version, and apt normally only offers the newest. The installer stops with an
  explanation if that version is missing.
  - Backports kernels need the same backports suite enabled.
  - The `linux-source` metapackage follows the stable kernel series, so on a backports
    kernel check `/var/log/ae7-hda-build.log` after each kernel update.
- A distribution and kernel in one of these groups:

| Your system | Route |
|---|---|
| **Debian** (tested on Debian forky, kernel 7.2.6) | Automated: `debian-stopgap/install.sh` below |
| Ubuntu, Mint, other Debian derivatives | Not tested. The script expects Debian's `linux-source-X.Y` package naming and versioning; it has not been checked against Ubuntu's. Use the manual route unless you adapt it. |
| Other distributions | Manual route and untested; I don't have access to any non-Debian machines. |
| Kernels that still keep HDA under `sound/pci/hda/` (older kernels) | The patch needs adapting to the old file paths. The change itself is small. |

### Debian: automated install

```bash
git clone https://github.com/AffSeda/Creative_AE7_Linux.git
cd Creative_AE7_Linux/debian-stopgap
sudo ./install.sh
```

The installer does the following:
1. Checks for the card and the bridge (only asking if they are missing), and refuses to
   run with Secure Boot on.
2. Installs the kernel headers, and the kernel source at exactly your running kernel's
   version.
   - If that version is not available from your apt sources, it stops and tells you to
     update your kernel first.
3. Builds patched modules for the running kernel and installs them in
   `/lib/modules/<kernel>/updates/ae7/`.
4. Sets up automatic rebuilds for future kernel updates.

If the installer lists settings you added earlier to keep the card away from
`snd_hda_intel` (a `blacklist`, or `vfio-pci`/`pci-stub` ids for `1102:0010`), remove
them. Then reboot. After the reboot:
- `lspci -k -d 1102:0010` should say `Kernel driver in use: snd_hda_intel`;
- the card should appear as an output device.
- On AE-7 the analogue output can be switched between Speakers and Headphone. Choose
  the port you want in your sound settings (PipeWire/PulseAudio).

**Safety net:** a kernel can end up without a patched build. For example, a kernel update
may arrive before its matching source package, or the patch may stop applying to a newer
kernel. In that case a guard keeps the card away from the driver at boot. The card is
silent on that kernel and the machine does not hang. Check `/var/log/ae7-hda-build.log`
and `journalctl -t ae7-hda-guard`.
- If the guard cannot confirm the card is parked, it refuses to load the sound driver at
  all for that boot. You then get no HDA audio on any device, rather than a hang.
- The guard parks the card at runtime with `pci-stub`. The manual method below uses
  `vfio-pci`, because that one can be written as static configuration.

**To remove it:** run `sudo ./uninstall.sh`. Before rebooting, either take the card out
or keep it away from the stock driver, or the machine will hang again. To keep the card
away, hand it to `vfio-pci`:
1. Create `/etc/modprobe.d/ae7-park.conf` containing
   `options vfio-pci ids=1102:0010` and `softdep snd_hda_intel pre: vfio-pci`,
   one per line.
2. Run `update-initramfs -u`.

`uninstall.sh` prints these steps too.

### Manual route (any distribution)

This route assumes you know how to build kernel modules.

1. Get the source for **exactly** your running kernel, and the matching headers or build tree.
2. Apply `patches/workaround-flush-every-hda-write.patch`. It touches
   `include/sound/hdaudio.h` and `sound/hda/codecs/ca0132.c`.
3. Build `snd-hda-core`, `snd-hda-codec`, `snd-hda-intel` and `snd-hda-codec-ca0132`
   with the patched `hdaudio.h`. All four inline the changed register-write helpers.
   - For out-of-tree builds with `make M=...`: the kernel's own include path comes first,
     so put the patched header ahead of it. `debian-stopgap/ae7-hda-build` shows one way:
     `NOSTDINC_FLAGS="-nostdinc -I<dir containing sound/hdaudio.h>"`.
4. Install the four `.ko` files where they override the stock ones, such as
   `/lib/modules/$(uname -r)/updates/`. Then run `depmod -a` and rebuild your initramfs.
5. Protect yourself from kernel updates: either rebuild for every new kernel, or keep
   the card off `snd_hda_intel` on kernels you have not rebuilt for. The guard in
   `debian-stopgap/ae7-hda-guard` is one way to do that.
   - Until the patched modules are installed, use option A or B above to get a stable boot.

### Reporting results

Reports are useful, whether it works or not. Include your board or chipset, `lspci -nn`
(the bridge revision), your kernel version, and what happened. They help establish how
widely the fix applies (other boards, bridge revisions, the AE-9).

## Layout

| Path | What |
|---|---|
| [ANALYSIS.md](ANALYSIS.md) | Full write-up for kernel/ALSA developers |
| [evidence/](evidence) | Hardware description and trimmed kernel logs for each finding (netconsole, ftrace, NMI backtraces) |
| [patches/workaround-flush-every-hda-write.patch](patches/workaround-flush-every-hda-write.patch) | The workaround that makes the card work, against Debian `linux-source-7.2` 7.2.6 |
| [tools/ae7probe/](tools/ae7probe) | The diagnostic kernel module used to bisect the HDA bring-up sequence (some modes hard-lock by design) |
| [debian-stopgap/](debian-stopgap) | Scripts that keep an AE-7 working on a Debian machine across kernel updates until a real fix lands, with a guard so a missing build never causes a hang |

## Status and limits

- Tested on one machine: AMD X399 (Threadripper 2990WX), Debian kernel 7.2.6 PREEMPT_RT.
- With the patched build, only warm reboots have been tested. A cold power-on (for
  example after option A's shut-down-and-fit step) has not been tested yet.
- The card's owner reports the same hang on other PCs, including an AMD 990FX system.
- Only the rev 03 bridge was available.
- The workaround flushes writes on **every** HDA controller in the system. That is fine
  for a single machine but not the right upstream shape; see "Suggested upstream shape"
  in ANALYSIS.md. That upstream patch has **not** been written or tested.
- Not yet measured: the minimum write burst that triggers the lockup, and whether other
  devices behind the same bridge are affected.

## How this was produced: AI disclosure

This investigation was driven by AI models, directed by the repository owner. They did
the diagnosis, test design, diagnostic module, workaround patch, stopgap scripts and this
write-up.
- The final root-cause work was done with Anthropic's Claude (Claude Opus 5.5, via Claude Code).
- Earlier exploration used Google Gemini and other models the owner worked with during
  the investigation.

What that means for a reader:
- Every result here was measured on real hardware. The logs are real. They are trimmed
  to the relevant lines, and hostnames and addresses are redacted; nothing else was changed.
- Claims are labelled as proven (with evidence) or inferred.
- The patch is offered as a reference. It carries no `Signed-off-by`. An upstream
  submission needs a developer who has reviewed it to write and sign their own version.
  Use as much or as little of this as is useful.

## Licence

The code in `patches/` and `tools/` is kernel code and is offered under GPL-2.0, the
same terms as the files it modifies.
