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
