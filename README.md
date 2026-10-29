# s5direct

A small Linux kernel module that fixes power-off and wake-on-LAN on the **Acer Predator G5910** (Intel P67 / 6-series PCH, AMI BIOS P01-A4).

On this board, `poweroff` blanks the screen but the PC stays on until you hold the power button. Reboot works. GRUB's `halt` works. With this module, power-off is normal and the PC can be woken with a magic packet.

Write-ups:
- [Linux won't power off](https://jeanyves.pro/aifindings/acer-predator-g5910-linux-wont-power-off.html)
- [Wake-on-LAN from power-off](https://jeanyves.pro/aifindings/acer-predator-g5910-linux-wake-on-lan.html)

## Why it happens

Before power-off, Linux runs the BIOS's ACPI `_PTS` method. On this board `_PTS` sets **SLP_SMI_EN** (bit 4 of the PCH's SMI_EN register, port `0x430`). With that bit set, the final "enter S5" write into PM1a_CNT doesn't power off. It traps into the BIOS's SMI handler instead, and under Linux that handler never finishes. GRUB's `halt` doesn't run `_PTS`, so its write works.

Skipping the SMI handler also skips its wake-on-LAN setup: the PCH's **WOL_EN_OVRD** bit (GEN_PMCON_3 bit 13, LPC bridge `00:1f.0` offset `0xA4`) stays off, so a magic packet can't wake the board from S5.

## What the module does

It registers two handlers in the kernel's power-off sequence:

1. **Prepare** (after ACPI's own prepare step): re-enables the ACPI wake GPEs, then sets GEN_PMCON_3 **SLP_LAN_DEFAULT** (bit 8, LAN PHY stays powered in S5) and **WOL_EN_OVRD** (bit 13), and clears **PME_B0_S5_DIS** (bit 15). If GPIO29 (SLP_LAN#) is configured as an output, it drives it high.
2. **Power-off** (after all drivers have shut down, before ACPI's own power-off): clears SLP_SMI_EN, then writes `SLP_TYP | SLP_EN` (`0x3C00`) to PM1a_CNT (port `0x404`).

The handlers run after the drivers shut down, so the disks get flushed normally and `e1000e` arms the NIC for wake-on-LAN. If the write somehow doesn't power off, the module returns and ACPI's normal power-off runs.

## Install

Requirements: `dkms`, kernel headers, `gcc`, `make`, `ethtool`. Secure Boot must be off, or you have to sign the module yourself. In the BIOS **Power** tab, set **Deep Power Off Mode** to *Disabled* and **Power On by Onboard LAN** to *Enabled*.

```sh
git clone https://github.com/jy-roda/acer-predator-g5910-linux-poweroff-wol.git
cd acer-predator-g5910-linux-poweroff-wol
sudo sh install.sh
```

The install script:

- builds and installs the module with DKMS, so it's rebuilt on kernel updates, and loads it at boot;
- adds a udev rule that runs `ethtool -s <nic> wol g` whenever the wired NIC appears;
- installs a fallback hook, `/usr/lib/systemd/system-shutdown/s5-direct.shutdown`, that does the same raw write if the module isn't loaded (power-off still works, but without wake-on-LAN).

To remove it: `sudo sh install.sh uninstall`.

## Other boards

The default values are this board's. Read yours before using it anywhere else, because writing the wrong I/O port can do anything:

```sh
sudo apt install acpica-tools
sudo acpidump -o acpi.dat && acpixtract -a acpi.dat && iasl -d facp.dat dsdt.dat
grep -E "PM1A Control Block Address" facp.dsl     # -> pm1a_cnt
grep -A6 "Name (_S5" dsdt.dsl                      # first value -> slp_typ
grep -E "Name \(PMBS|Name \(PM30" dsdt.dsl          # PMBASE + 0x30 -> smi_en
grep -B2 -A6 "Method (_PTS" dsdt.dsl               # does _PTS set an SMI enable bit?
```

Then pass the values as module parameters, for example `options s5direct pm1a_cnt=0x1804 slp_typ=7 smi_en=0x1830` in `/etc/modprobe.d/s5direct.conf`, and install with `--force`. Edit the fallback hook in `install.sh` the same way. The wake-on-LAN register bits are from the Intel 6 Series / C200 datasheet (324645). Other PCH generations may differ, and `wol=0` turns that part off.

## License

Public domain ([Unlicense](LICENSE)). The module still declares `MODULE_LICENSE("GPL")` because the kernel only lets GPL-compatible modules use `register_sys_off_handler()`.
