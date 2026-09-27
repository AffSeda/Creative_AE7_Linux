// SPDX-License-Identifier: GPL-2.0
/*
 * ae7probe - diagnostic module used to localise the Creative Sound Blaster AE-7
 * (CA0132 behind an ASMedia ASM1083/1085 PCIe-to-PCI bridge) hard lock.
 *
 * This is the code exactly as it was run during the investigation (only this
 * comment block was added afterwards). It is a throwaway research tool, not a driver:
 *  - it does NOT bind to the device; the card must be held by pci-stub
 *    (pci-stub.ids=1102:0010 and snd_hda_intel kept away from it);
 *  - the device address is hard-coded to 0000:08:00.0 below - edit it for your system;
 *  - it always returns -EAGAIN from init, so it never stays loaded;
 *  - every step logs "AE7 MOD ..." via pr_err; run netconsole to another machine,
 *    because some parameter combinations hard-lock the host by design.
 *
 * Parameters:
 *   stop=N    run steps S1..SN of a hand-written HDA bring-up (S4-S9 CORB/RIRB,
 *             S9 sends GET_PARAMETER(vendor id) to codec address 1, S10-S12 UNSOL and
 *             interrupt enables). A marker is printed before every register access.
 *   wc=1      make the CORB/RIRB pages write-combining (as snd_hda_intel does for
 *             non-snoop controllers such as this one).
 *   irq=1     pci_enable_device() + a real shared INTx handler before S1.
 *   intclr=1  after S3, replay azx_int_clear() one write at a time, each followed by a
 *             flushing read (all survive).
 *   exact=1   after S3, replay snd_hdac_bus_init_chip() (reset_link, azx_int_clear,
 *             init_cmd_io) with the driver's exact widths, values and order and NO
 *             intervening reads. Requires stop>=3.
 *   flush=1   with exact=1: read offset 0 (GCAP) back after every write.
 *
 * Results on the test system (see ../../evidence): every stop/wc/irq/intclr combination
 * completes and the codec answers 0x11020011; exact=1 flush=0 hard-locks the host;
 * exact=1 flush=1 completes.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/atomic.h>
#include <asm/set_memory.h>
static int stop = 99; module_param(stop, int, 0444);
static int wc; module_param(wc, int, 0444);
static void __iomem *r;
static int irq; module_param(irq, int, 0444);
static int intclr; module_param(intclr, int, 0444);
static int exact; module_param(exact, int, 0444);
static int flush; module_param(flush, int, 0444); /* 1 = read-back after every write in exact mode */
#define STS_MASK 0xff
#define INT_MASK 0x7fffffff
static atomic_t hits, mine; static bool have_irq;
static irqreturn_t ae7_isr(int n, void *d)
{
	u32 st; atomic_inc(&hits); if (!r) return IRQ_NONE;
	st = readl(r + 0x24); if (!st || st == 0xffffffff) return IRQ_NONE;
	atomic_inc(&mine); writeb(0x05, r + 0x5d); writew(readw(r + 0xe), r + 0xe); return IRQ_HANDLED;
}
static unsigned long corb, rirb; static struct pci_dev *pd;
#define M(fmt, ...) pr_err("AE7 MOD [irq hits=%d mine=%d] " fmt "\n", atomic_read(&hits), atomic_read(&mine), ##__VA_ARGS__)
static bool poll16(int o, u16 mask, u16 want) { int i; for (i = 0; i < 1000; i++) { if ((readw(r + o) & mask) == want) return true; udelay(10); } return false; }
static bool poll32(int o, u32 mask, u32 want) { int i; for (i = 0; i < 1000; i++) { if ((readl(r + o) & mask) == want) return true; udelay(100); } return false; }
static int __init ae7_init(void)
{
	u8 b; u32 pc, pr;
	pd = pci_get_domain_bus_and_slot(0, 8, PCI_DEVFN(0, 0));
	if (!pd || pd->vendor != 0x1102) { M("no card"); return -ENODEV; }
	corb = get_zeroed_page(GFP_KERNEL | GFP_DMA32); rirb = get_zeroed_page(GFP_KERNEL | GFP_DMA32);
	pc = virt_to_phys((void *)corb); pr = virt_to_phys((void *)rirb);
	if (wc) { M("set_memory_wc on both pages"); set_memory_wc(corb, 1); set_memory_wc(rirb, 1); }
	M("pages corb=0x%x rirb=0x%x stop=%d", pc, pr, stop);
	r = ioremap(pci_resource_start(pd, 0), 0x4000);
	pci_write_config_word(pd, PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	pci_read_config_byte(pd, 0x44, &b); pci_write_config_byte(pd, 0x44, b & ~7);
	if (irq) { int e = pci_enable_device(pd); M("pci_enable_device=%d irq=%d pin=%d", e, pd->irq, pd->pin);
	  e = request_irq(pd->irq, ae7_isr, IRQF_SHARED, "ae7probe", &hits); have_irq = !e; M("request_irq=%d", e);
	  pci_write_config_word(pd, PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER); }
	M("S0 mem+master on, TCSEL was 0x%02x", b);
	M("S1 GCAP=0x%04x VMAJ=%u GCTL=0x%08x STATESTS=0x%04x", readw(r), readb(r + 3), readl(r + 8), readw(r + 0xe));
	if (stop < 2) goto out;
	M("S2 enter reset ..."); writel(readl(r + 8) & ~1, r + 8); M("  entered=%d", poll32(8, 1, 0));
	if (stop < 3) goto out;
	M("S3 exit reset ..."); writel(readl(r + 8) | 1, r + 8); M("  exited=%d", poll32(8, 1, 1)); msleep(2);
	M("  STATESTS=0x%04x", readw(r + 0xe));
	if (exact) {
		int i, t; u16 rp;
#define W(op, v, o) do { op(v, r + (o)); if (flush) (void)readw(r); } while (0)
		M("SX exact driver init_chip, flush=%d ...", flush); msleep(300);
		if (readb(r + 8) & 1) W(writew, 0xff, 0xe);
		W(writel, readl(r + 8) & ~1, 8);
		for (t = 0; t < 200 && (readb(r + 8) & 1); t++) usleep_range(500, 1000);
		usleep_range(500, 1000);
		W(writeb, readb(r + 8) | 1, 8);
		for (t = 0; t < 200 && !readb(r + 8); t++) usleep_range(500, 1000);
		usleep_range(1000, 1200);
		(void)readb(r + 8); (void)readw(r + 0xe);
		for (i = 0; i < 10; i++) W(writeb, 0x1c, 0x80 + i * 0x20 + 3);
		W(writew, 0xff, 0xe); W(writeb, 0x05, 0x5d); W(writel, 0x7fffffff, 0x24);
		W(writel, pc, 0x40); W(writel, 0, 0x44); W(writeb, 0x02, 0x4e); W(writew, 0, 0x48); W(writew, 0x8000, 0x4a);
		for (t = 1000; t > 0 && !(readw(r + 0x4a) & 0x8000); t--) udelay(1);
		W(writew, 0, 0x4a);
		for (t = 1000; t > 0 && readw(r + 0x4a); t--) udelay(1);
		W(writeb, 0x02, 0x4c);
		W(writel, pr, 0x50); W(writel, 0, 0x54); W(writeb, 0x02, 0x5e); W(writew, 0x8000, 0x58); W(writew, 1, 0x5a); W(writeb, 0x03, 0x5c);
		W(writel, readl(r + 8) | 0x100, 8);
		rp = readw(r + 0x4a);
		M("SX survived: GCTL=0x%08x CORBRP=0x%04x RIRBCTL=0x%02x", readl(r + 8), rp, readb(r + 0x5c));
		goto out;
	}
	if (intclr) {
		int i;
		for (i = 0; i < 10; i++) {
			M("S3b SD_STS stream %d (writeb 0x1c @0x%x) ...", i, 0x80 + i * 0x20 + 3); msleep(150);
			writeb(0x1c, r + 0x80 + i * 0x20 + 3); (void)readw(r);
		}
		M("S3c STATESTS writew 0x%x ...", STS_MASK); msleep(150); writew(STS_MASK, r + 0xe); (void)readw(r);
		M("S3d RIRBSTS writeb 0x05 ..."); msleep(150); writeb(0x05, r + 0x5d); (void)readw(r);
		M("S3e INTSTS writel 0x%x ...", INT_MASK); msleep(150); writel(INT_MASK, r + 0x24); (void)readw(r);
		M("S3f int_clear replay done"); msleep(150);
	}
	if (stop < 4) goto out;
	M("S4 CORB base/size ..."); writel(pc, r + 0x40); writel(0, r + 0x44); writeb((readb(r + 0x4e) & ~3) | 2, r + 0x4e);
	M("  CORBLBASE=0x%08x CORBSIZE=0x%02x", readl(r + 0x40), readb(r + 0x4e));
	if (stop < 5) goto out;
	M("S5 CORBWP=0, CORBRP reset ..."); writew(0, r + 0x48); writew(0x8000, r + 0x4a);
	M("  rp_set=%d", poll16(0x4a, 0x8000, 0x8000)); writew(0, r + 0x4a); M("  rp_clear=%d CORBRP=0x%04x", poll16(0x4a, 0x8000, 0), readw(r + 0x4a));
	if (stop < 6) goto out;
	M("S6 CORBCTL RUN (card DMA-reads CORB) ..."); writeb(2, r + 0x4c); msleep(50);
	M("  CORBCTL=0x%02x CORBRP=0x%04x", readb(r + 0x4c), readw(r + 0x4a));
	if (stop < 7) goto out;
	M("S7 RIRB base, RIRBWP reset, RINTCNT ..."); writel(pr, r + 0x50); writel(0, r + 0x54); M("  RIRBSIZE before=0x%02x", readb(r + 0x5e)); writeb(2, r + 0x5e); M("  RIRBSIZE written"); writew(0x8000, r + 0x58); writew(1, r + 0x5a);
	M("  RIRBLBASE=0x%08x RIRBWP=0x%04x", readl(r + 0x50), readw(r + 0x58));
	if (stop < 8) goto out;
	M("S8 RIRBCTL DMAEN (card may DMA-write RIRB) ..."); writeb(2, r + 0x5c); msleep(50);
	M("  RIRBCTL=0x%02x RIRBWP=0x%04x", readb(r + 0x5c), readw(r + 0x58));
	if (stop < 9) goto out;
	M("S9 GET_PARAM vendor-id codec1 via CORB ..."); ((u32 *)corb)[1] = (1u << 28) | 0xF0000; wmb(); writew(1, r + 0x48);
	{ bool got = poll16(0x58, 0xff, 1); u16 wp = readw(r + 0x58), rp = readw(r + 0x4a); u32 a, x; rmb(); a = ((u32 *)rirb)[2]; x = ((u32 *)rirb)[3];
	  M("  got=%d RIRBWP=0x%04x CORBRP=0x%04x resp=0x%08x ex=0x%08x", got, wp, rp, a, x); }
	if (stop < 10) goto out;
	M("S10 GCTL UNSOL on ..."); writel(readl(r + 8) | 0x100, r + 8); msleep(100);
	M("  GCTL=0x%08x INTSTS=0x%08x RIRBSTS=0x%02x", readl(r + 8), readl(r + 0x24), readb(r + 0x5d));
	if (stop < 11) goto out;
	M("S11 RIRBCTL INTCTL on (card may assert INTx) ..."); writeb(3, r + 0x5c); msleep(200);
	M("  INTSTS=0x%08x RIRBSTS=0x%02x INTCTL=0x%08x", readl(r + 0x24), readb(r + 0x5d), readl(r + 0x20));
	if (stop < 12) goto out;
	M("S12 INTCTL GIE|CIE (interrupt output enabled) ..."); writel(0xC0000000, r + 0x20); msleep(500);
	M("  INTSTS=0x%08x RIRBSTS=0x%02x", readl(r + 0x24), readb(r + 0x5d));
	M("  cmd2 ..."); ((u32 *)corb)[2] = (1u << 28) | 0xF0000; wmb(); writew(2, r + 0x48); msleep(500);
	M("  RIRBWP=0x%04x INTSTS=0x%08x RIRBSTS=0x%02x", readw(r + 0x58), readl(r + 0x24), readb(r + 0x5d));
out:
	if (r) writel(0, r + 0x20);
	M("stopping DMA, reset, mem off"); writeb(0, r + 0x5c); writeb(0, r + 0x4c); msleep(5); writel(readl(r + 8) & ~1, r + 8);
	if (have_irq) free_irq(pd->irq, &hits); if (irq) pci_disable_device(pd);
	pci_write_config_word(pd, PCI_COMMAND, 0); iounmap(r); r = NULL; if (wc) { set_memory_wb(corb, 1); set_memory_wb(rirb, 1); } free_page(corb); free_page(rirb); pci_dev_put(pd);
	M("clean exit"); return -EAGAIN; /* never stays loaded */
}
module_init(ae7_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AE-7 / ASM1083 HDA bring-up diagnostic (research tool)");
