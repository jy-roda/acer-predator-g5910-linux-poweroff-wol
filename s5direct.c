// SPDX-License-Identifier: Unlicense
/*
 * s5direct: power off (and wake on LAN) on the Acer Predator G5910 (Intel P67,
 * BIOS P01-A4), where Linux's ACPI power-off leaves the board on.
 *
 * Why ACPI's way fails: _PTS (SPTS) sets SLP_SMI_EN in the PCH's SMI_EN register,
 * so writing SLP_EN traps into the BIOS's SMI handler instead of entering S5,
 * and that handler never powers the board off under Linux. GRUB's halt works
 * because it never runs _PTS.
 *
 * This module clears SLP_SMI_EN and writes SLP_TYP | SLP_EN itself, after the
 * drivers have shut down (so e1000e arms wake-on-LAN). Skipping the BIOS handler
 * also skips its wake-on-LAN setup, so the matching PCH bits are set here too
 * (Intel 6 Series datasheet 324645, GEN_PMCON_3 and SLP_LAN#).
 */
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <asm/special_insns.h>

static ushort pm1a_cnt = 0x404;	/* FADT PM1a control block */
static ushort slp_typ = 7;	/* first value of \_S5 */
static ushort smi_en = 0x430;	/* PMBASE + 0x30, PCH SMI_EN */
module_param(pm1a_cnt, ushort, 0444);
module_param(slp_typ, ushort, 0444);
module_param(smi_en, ushort, 0444);

static bool wol = true;		/* keep the LAN PHY powered and allowed to wake in S5 */
module_param(wol, bool, 0644);

#define SLP_SMI_EN	(1 << 4)

/* LPC bridge D31:F0 config registers */
#define GPIOBASE	0x48
#define GEN_PMCON_3	0xa4
#define  PWR_FLR		(1 << 1)	/* R/WC */
#define  SLP_LAN_DEFAULT	(1 << 8)	/* SLP_LAN# value in Sx when nothing else sets it */
#define  GEN_RST_STS		(1 << 9)	/* R/WC */
#define  WOL_EN_OVRD		(1 << 13)	/* LAN may wake from S5 whatever PME_B0_EN says */
#define  SUS_PWR_FLR		(1 << 14)	/* R/WC */
#define  PME_B0_S5_DIS		(1 << 15)
#define PMCON3_RWC	(PWR_FLR | GEN_RST_STS | SUS_PWR_FLR)
/* GPIO registers, from GPIOBASE */
#define GP_IO_SEL	0x04
#define GP_LVL		0x0c
#define GPIO29		(1 << 29)	/* SLP_LAN#: if set as output, its level wins in Sx */

static struct sys_off_handler *prepare_handler, *power_off_handler;

static void lan_wake_setup(bool apply)
{
	struct pci_dev *lpc = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0x1f, 0));
	u16 pmcon3;
	u32 gpiobase, io_sel = 0, lvl = 0;

	if (!lpc)
		return;
	pci_read_config_word(lpc, GEN_PMCON_3, &pmcon3);
	pci_read_config_dword(lpc, GPIOBASE, &gpiobase);
	gpiobase &= 0xff80;
	if (gpiobase) {
		io_sel = inl(gpiobase + GP_IO_SEL);
		lvl = inl(gpiobase + GP_LVL);
	}
	pr_info("s5direct: GEN_PMCON_3=%04x GPIO29 %s level %d\n", pmcon3,
		io_sel & GPIO29 ? "in" : "out", !!(lvl & GPIO29));

	if (apply) {
		pmcon3 = (pmcon3 & ~(PMCON3_RWC | PME_B0_S5_DIS)) | SLP_LAN_DEFAULT | WOL_EN_OVRD;
		pci_write_config_word(lpc, GEN_PMCON_3, pmcon3);
		if (gpiobase && !(io_sel & GPIO29))
			outl(lvl | GPIO29, gpiobase + GP_LVL);
	}
	pci_dev_put(lpc);
}

/* Runs after ACPI's prepare (_PTS, all GPEs disabled): re-enable only the wake GPEs */
static int s5direct_prepare(struct sys_off_data *data)
{
	acpi_enable_all_wakeup_gpes();
	if (wol)
		lan_wake_setup(true);
	return NOTIFY_DONE;
}

/* Runs before ACPI's power-off, after device shutdown */
static int s5direct_power_off(struct sys_off_data *data)
{
	outl(inl(smi_en) & ~SLP_SMI_EN, smi_en);	/* undo _PTS: no SMI on SLP_EN */
	wbinvd();
	outw((slp_typ << 10) | (1 << 13), pm1a_cnt);	/* SLP_TYP | SLP_EN, SCI_EN cleared like GRUB */
	mdelay(1000);
	return NOTIFY_DONE;	/* still on: let ACPI try */
}

static int __init s5direct_init(void)
{
	prepare_handler = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF_PREPARE,
						   SYS_OFF_PRIO_FIRMWARE - 1,
						   s5direct_prepare, NULL);
	if (IS_ERR(prepare_handler))
		return PTR_ERR(prepare_handler);

	power_off_handler = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
						     SYS_OFF_PRIO_FIRMWARE + 1,
						     s5direct_power_off, NULL);
	if (IS_ERR(power_off_handler)) {
		unregister_sys_off_handler(prepare_handler);
		return PTR_ERR(power_off_handler);
	}
	pr_info("s5direct: power-off writes 0x%04x to port 0x%x\n",
		(slp_typ << 10) | (1 << 13), pm1a_cnt);
	lan_wake_setup(false);	/* log only */
	return 0;
}

static void __exit s5direct_exit(void)
{
	unregister_sys_off_handler(power_off_handler);
	unregister_sys_off_handler(prepare_handler);
}

module_init(s5direct_init);
module_exit(s5direct_exit);
MODULE_DESCRIPTION("Raw ACPI S5 power-off for boards where ACPICA's S5 entry fails");
/* The code is public domain; the kernel only lets GPL-compatible modules use register_sys_off_handler() */
MODULE_LICENSE("GPL");
