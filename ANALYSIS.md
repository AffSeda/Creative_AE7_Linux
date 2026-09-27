# Creative Sound Blaster AE-7 hard lock under Linux: analysis

## Summary

On Linux, binding `snd_hda_intel` to a Creative Sound Blaster AE-7 whose CA0132 HDA
controller sits behind an **ASMedia ASM1083/1085 PCIe-to-PCI bridge** (`1b21:1080` rev 03)
hard-locks the machine within about a minute. The same card works under Windows. The
older revision of the card, which uses an IDT bridge, is reported to work under Linux.

The trigger is a **burst of back-to-back posted MMIO writes** to the controller. The
stock driver issues about 20 of them in `snd_hdac_bus_init_chip()`, in `azx_int_clear()`
and `snd_hdac_bus_init_cmd_io()`, before its first read. Replaying that exact sequence
from a diagnostic module hangs the host. The same sequence with a read-back after every
write completes. The full driver, rebuilt to read back after every HDA register write
(and after every CA0132 BAR2 write), probes the card, loads the DSP and plays audio. The
same build then came up correctly on normal reboots (warm reboots only; no cold power-on
with the patched build has been tested yet).

## Test system

| | |
|---|---|
| Card | Creative Sound Blaster AE-7, PCI `1102:0010` rev 01, subsystem `1102:0081` |
| Bridge on the card | ASMedia ASM1083/1085 PCIe-to-PCI bridge `1b21:1080` rev 03, PCIe x1 Gen1 |
| Upstream port | AMD 300 Series Chipset PCIe Port `1022:43b4` |
| Board | ASUS ROG ZENITH EXTREME ALPHA (X399), BIOS 2701, AMD Ryzen Threadripper 2990WX |
| Kernel | Debian `7.2.6+deb14-rt-amd64` (PREEMPT_RT), `pcie_aspm=off amd_iommu=on iommu=pt` |

Full `lspci -vvv` of the path is in [evidence/00-hardware.txt](evidence/00-hardware.txt).
The controller is a conventional-PCI endpoint (`GCAP=0x6401`: 6 output and 4 input
streams, 64OK bit set but masked by `AZX_DCAPS_NO_64BIT`). The codec is on SDI 1
(`STATESTS=0x0002`).

The card's owner reports the same hang on an AMD 990FX (FX-9590) machine and on other
PCs. This investigation only tested the system above.

## Symptom

- `snd_hda_intel` probe (debug on) logs `Force to non-snoop mode`,
  `Disabling 64bit DMA`, `Clearing TCSEL`, then nothing more for that device.
- About 4 s later the platform EC starts timing out (`ACPI Error: AE_TIME ... EmbeddedControl`).
  Config reads of the bridge (07:00.0) and the card (08:00.0) return `0xffff`.
- Tasks pile up in D state (RCU, hwmon, bluetooth). The host hard-locks 30-60 s after the bind.
- It happens at boot if nothing keeps the driver off the card. It is not a boot-order problem.

Evidence: [evidence/01-stock-driver-hang.txt](evidence/01-stock-driver-hang.txt).

## How it was localised

Every step ran on the real machine. Kernel messages went to a second machine over
netconsole. The card was held on `pci-stub` at boot and handed to a driver at runtime.

| # | Configuration | Result |
|---|---|---|
| 1 | Card held on `pci-stub`, or bound to `vfio-pci` | OK, boots and runs indefinitely |
| 2 | Stock `snd_hda_intel`, `enable_msi=0` | Hang |
| 3 | Stock, `single_cmd=1 enable_msi=0` | Hang |
| 4 | Stock, `amd_iommu=off` (swiotlb kept) | Hang |
| 5 | Stock, with the desktop sound server stopped | Hang |
| 6 | ftrace `function_graph` of the stock probe | CPU stops inside `snd_hdac_bus_init_cmd_io()`; `snd_hdac_bus_reset_link()` returned normally ([02](evidence/02-ftrace-probe-stops-in-init_cmd_io.txt)) |
| 7 | sysrq-l 1-5 s after the stock bind | CPUs 0 and 1 never answer the NMI; others spin in `cpa_flush()` IPIs to them ([03](evidence/03-nmi-backtraces-stock-driver.txt)) |
| 8 | `tools/ae7probe`: hand bring-up, one register op per step, with reads and log output between ops | OK: reset, CORB/RIRB DMA, `GET_PARAMETER` answered `0x11020011`, UNSOL, INTCTL, real INTx handler, write-combining pages ([04](evidence/04-hand-bringup-works.txt)) |
| 9 | `ae7probe intclr=1`: `azx_int_clear()` writes one at a time, each flushed | OK |
| 10 | `ae7probe exact=1 flush=0`: `snd_hdac_bus_init_chip()` replayed with the driver's widths, values and order, no intervening reads | **Hang** ([05](evidence/05-exact-init-sequence-hang-vs-flush.txt)) |
| 11 | `ae7probe exact=1 flush=1`: same, with a read of GCAP after every write | OK ([05](evidence/05-exact-init-sequence-hang-vs-flush.txt)) |
| 12 | Full stock driver rebuilt with a read-back after every HDA register write and every CA0132 BAR2 write | OK: probes, DSP loads, audio plays; normal warm reboots come up working ([06](evidence/06-patched-driver-works.txt)) |

Rows 10 and 11 are the key pair. The two runs differ only by the flushing reads.

### Reading of the evidence

- **Proven on this system:** the driver's initial write burst wedges the path, and
  flushing each write prevents it.
- Each write in that burst is harmless on its own (rows 8 and 9).
- Card DMA through the bridge works (row 8), and so do interrupts.
- The IOMMU, MSI vs INTx, write-combining buffers, and concurrent activity on other
  controllers were each excluded individually.
- **Inferred, not proven:** the defect is in the ASM1083 rev 03's handling of queued
  posted writes to this conventional-PCI target (a buffer or retry issue). The wedged
  bridge then stalls the CPU's next non-posted read forever. A CPU stuck on an MMIO
  read cannot take an NMI, which matches row 7. Any other access to the same chipset
  path then stalls too, which would explain why the platform EC stops answering.
  - The facts that point at the bridge: the IDT-bridge revision of the card is
    reported to work, and the controller's own register interface behaves correctly
    once writes are flushed.
- **Not measured:**
  - how many back-to-back writes the bridge tolerates;
  - whether only particular registers or byte-lane patterns matter;
  - whether other devices behind an ASM1083 rev 03 are affected;
  - whether Windows avoids the problem by access pattern or by pacing.

`pcie_aspm=off` is in effect, so ASPM is not the mechanism. An ASPM-related change for
this bridge discussed in the bug reports below did not prevent the hang.

## The workaround used here

[patches/workaround-flush-every-hda-write.patch](patches/workaround-flush-every-hda-write.patch),
against Debian's `linux-source-7.2` 7.2.6:

1. `include/sound/hdaudio.h`: `snd_hdac_reg_writeb/w/l()` read the same register back
   after writing. This affects every HDA controller in the system, not just the AE-7.
2. `sound/hda/codecs/ca0132.c`: after each of the 45 `writeb/w/l(..., spec->mem_base + ...)`
   (BAR2, the CA0113 MMIO GPIO/command window used on AE-5/AE-7), read BAR0 offset 0 (GCAP).
   Reading back Creative's own command registers was avoided because their read side
   effects are unknown. Some of these writes are in tight loops, e.g. 8 writes to +0x304.

This is a blunt, proven workaround, **not** a proposed upstream patch. It costs one
non-posted read per register write on every HDA controller in the system.

## Suggested upstream shape

This is a sketch for a maintainer. It is **not built or tested** in this form.

1. Add a bus flag next to the existing `aligned_mmio` pattern, for example
   `bool flush_posted_writes:1` in `struct hdac_bus`. Make `snd_hdac_reg_write{b,w,l}()`
   follow each write with `(void)readl(bus->remap_addr)` (a GCAP read) when it is set.
   The existing `snd_hdac_reg_writel` macro becomes an inline function.
2. Set the flag in `azx_first_init()` (sound/hda/controllers/intel.c) when
   `pci_upstream_bridge(pci)` is an ASMedia ASM1083/1085 (`PCI_VENDOR_ID_ASMEDIA`, device
   `0x1080`), with a `dev_info()`. This must happen before `hda_intel_init_chip()`.
   - Matching on the bridge rather than the card's subsystem ID would also cover any other
     conventional-PCI HDA controller behind this bridge. Matching on SSID `1102:0081` is
     the narrower alternative if maintainers prefer it.
3. In `ca0132.c`, route the `spec->mem_base` writes through one helper that flushes when
   `codec->core.bus->flush_posted_writes` is set.
4. Leave every other system unchanged.

Open questions a maintainer may want answered before merging:
- The rev 01 and rev 02 bridges: this system only has rev 03.
- The AE-9.
- The minimum burst that triggers it. `tools/ae7probe` can find that by moving the
  `flush` point.

## Reproducing

1. Boot with `pci-stub.ids=1102:0010 modprobe.blacklist=snd_hda_intel`, and
   `netconsole=...` pointing at another machine.
2. Build `tools/ae7probe` against the running kernel and adjust the hard-coded BDF.
3. `insmod ae7probe.ko stop=12 wc=1 irq=1`: completes. Then
   `insmod ae7probe.ko stop=3 exact=1 flush=1`: completes. Then
   `insmod ae7probe.ko stop=3 exact=1 flush=0`: hard-locks. The last marker and the EC
   timeouts show on the netconsole receiver.
4. Or bind the stock driver:
   `echo 0000:08:00.0 > /sys/bus/pci/drivers/pci-stub/unbind`,
   `echo snd_hda_intel > /sys/bus/pci/devices/0000:08:00.0/driver_override`,
   `echo 0000:08:00.0 > /sys/bus/pci/drivers/snd_hda_intel/bind`.

Under Secure Boot kernel lockdown, `setpci` writes and mapping `resource0` are refused.
The module needs lockdown off (or signing) to load.

## References

- Kernel bugzilla 217510; Debian bug #1115581 (the existing reports of this hang).
