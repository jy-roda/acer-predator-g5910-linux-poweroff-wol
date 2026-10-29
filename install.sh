#!/bin/sh
# Install the s5direct kernel module (DKMS, rebuilt on kernel updates) and enable wake-on-LAN.
#   sudo sh install.sh            install
#   sudo sh install.sh --force    install on a board other than the Acer Predator G5910
#   sudo sh install.sh uninstall  remove (the s5-direct.shutdown hook stays as a fallback)
set -e
[ "$(id -u)" = 0 ] || { echo "Run with sudo"; exit 1; }
VER=1.0
DIR=$(cd "$(dirname "$0")" && pwd)
HOOK=/usr/lib/systemd/system-shutdown/s5-direct.shutdown
RULE=/etc/udev/rules.d/80-wol.rules

if [ "$1" = uninstall ]; then
    rm -f /etc/modules-load.d/s5direct.conf "$RULE"
    modprobe -r s5direct 2>/dev/null || true
    dkms remove s5direct/$VER --all 2>/dev/null || true
    rm -rf /usr/src/s5direct-$VER
    echo "Removed. To remove the fallback hook too: rm $HOOK"
    exit 0
fi

# The port numbers and SLP_TYP below are this board's. Check yours (see README) first.
if [ "$(cat /sys/class/dmi/id/product_name 2>/dev/null)" != "Predator G5910" ] && [ "$1" != --force ]; then
    echo "This is not an Acer Predator G5910. Check the values in README.md, then run with --force."
    exit 1
fi

# 1. Module, built for every installed kernel and loaded at boot
modprobe -r s5direct 2>/dev/null || true
dkms remove s5direct/$VER --all 2>/dev/null || true
mkdir -p /usr/src/s5direct-$VER
cp "$DIR"/s5direct.c "$DIR"/Makefile "$DIR"/dkms.conf /usr/src/s5direct-$VER/
dkms add s5direct/$VER
dkms install s5direct/$VER
echo s5direct > /etc/modules-load.d/s5direct.conf
modprobe s5direct

# 2. Fallback shell hook, only used when the module isn't loaded
#    (it runs before the kernel's driver shutdown, so there is no wake-on-LAN with it)
cat > "$HOOK" <<'EOF'
#!/bin/sh
# Linux's ACPI power-off leaves this board on, but GRUB's halt works.
# Do what GRUB does: write SLP_TYP=7 (from _S5) | SLP_EN into PM1a_CNT (port 0x404, from the FADT).
[ "$1" = poweroff ] || exit 0
[ -d /sys/module/s5direct ] && exit 0  # the s5direct module does it, after drivers shut down
sync
sleep 1
printf '\000\074' | dd of=/dev/port bs=1 seek=1028 count=2 2>/dev/null
EOF
chmod 755 "$HOOK"

# 3. Wake on magic packet, set every time the wired NIC appears
IFACE=$(ip route show default | awk '{print $5; exit}')
if [ -n "$IFACE" ]; then
    MAC=$(cat /sys/class/net/"$IFACE"/address)
    echo "ACTION==\"add\", SUBSYSTEM==\"net\", ATTR{address}==\"$MAC\", RUN+=\"/usr/sbin/ethtool -s \$name wol g\"" > "$RULE"
    ethtool -s "$IFACE" wol g
    echo "Wake-on-LAN: $IFACE, MAC $MAC"
fi

echo "---"
dmesg | grep s5direct | tail -2
[ -n "$IFACE" ] && ethtool "$IFACE" | grep Wake-on
