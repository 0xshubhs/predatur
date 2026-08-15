#!/bin/bash
# predatortune selfcheck — is everything actually up after a boot?
#
# This package has had three separate boot-time failures (module in the wrong
# place, Secure Boot rejection, a permissions race), and each looked identical
# from the desktop: fan control silently does nothing. This answers "what is
# broken" in one command.
#
#   ./selfcheck.sh

SENSE=/sys/devices/platform/acer-wmi/predator_sense
KB=/sys/devices/platform/acer-wmi/four_zoned_kb
fail=0

ok()   { printf '  \033[32m ok \033[0m %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
note() { printf '       %s\n' "$1"; }

echo "PredatorTune selfcheck — kernel $(uname -r), up $(uptime -p)"
echo

# ---- kernel module -------------------------------------------------------
echo "Driver"
if lsmod | grep -q '^linuwu_sense '; then
    ok "linuwu_sense loaded"
else
    bad "linuwu_sense NOT loaded"
    if journalctl -b --no-pager 2>/dev/null | grep -qi 'Key was rejected'; then
        note "Secure Boot refused it. Enrol this machine's signing key:"
        note "  sudo mokutil --import /var/lib/shim-signed/mok/MOK.der"
        note "  then reboot and choose Enroll MOK."
    elif ! dkms status -m linuwu-sense 2>/dev/null | grep -q "$(uname -r)"; then
        note "No DKMS build for this kernel. Try:"
        note "  sudo dkms autoinstall -k $(uname -r)"
    fi
fi

lsmod | grep -q '^acer_wmi ' && bad "in-tree acer_wmi is loaded, it conflicts" \
                             || ok "in-tree acer_wmi correctly out of the way"
lsmod | grep -q '^predatortune_fan ' && bad "retired predatortune_fan still resident" \
                                     || ok "old fan module retired"

# ---- the interfaces it provides -----------------------------------------
echo
echo "Interfaces"
for f in fan_speed battery_limiter; do
    [ -e "$SENSE/$f" ] && ok "$f present" || bad "$f missing"
done
[ -e "$KB/per_zone_mode" ] && ok "keyboard zones present" || bad "keyboard zones missing"

# ---- permissions ---------------------------------------------------------
echo
echo "Permissions (the tray writes these without a password)"
for f in "$SENSE/fan_speed" "$SENSE/battery_limiter" "$KB/per_zone_mode"; do
    [ -e "$f" ] || continue
    if [ -w "$f" ]; then
        ok "$(basename "$f") writable"
    else
        bad "$(basename "$f") not writable by you"
        note "sudo systemd-tmpfiles --create /usr/lib/tmpfiles.d/predatortune.conf"
    fi
done

# ---- services ------------------------------------------------------------
echo
echo "Services"
[ "$(systemctl is-active predatortune-daemon)" = active ] \
    && ok "fan daemon running" || bad "fan daemon not running"
[ "$(systemctl is-enabled predatortune-daemon 2>/dev/null)" = enabled ] \
    && ok "fan daemon enabled at boot" || bad "fan daemon not enabled"
pgrep -f 'predatortune-tray' >/dev/null \
    && ok "tray running" || bad "tray not running"

# ---- other things that share the machine --------------------------------
echo
echo "Not ours, but broken by the same causes"
[ -n "$(cat /sys/firmware/acpi/platform_profile 2>/dev/null)" ] \
    && ok "performance profiles ($(cat /sys/firmware/acpi/platform_profile))" \
    || bad "platform_profile gone"
if nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader >/dev/null 2>&1; then
    ok "nvidia driver ($(nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader)°C)"
else
    bad "nvidia driver not responding"
fi
rfkill list wifi >/dev/null 2>&1 && ok "wifi/rfkill intact" || bad "rfkill missing"

# ---- current settings ----------------------------------------------------
echo
echo "Settings"
note "battery limiter : $(cat "$SENSE/battery_limiter" 2>/dev/null || echo '—')  (1 = stop at 80%)"
note "fan speed       : $(cat "$SENSE/fan_speed" 2>/dev/null || echo '—')  (0,0 = firmware auto)"
note "keyboard        : $(cat "$KB/per_zone_mode" 2>/dev/null || echo '—')"
note "battery         : $(cat /sys/class/power_supply/BAT1/capacity 2>/dev/null)% $(cat /sys/class/power_supply/BAT1/status 2>/dev/null)"

echo
echo "Fan daemon, last few decisions:"
journalctl -u predatortune-daemon -b --no-pager -o cat 2>/dev/null | tail -4 | sed 's/^/  /'

echo
if [ "$fail" -eq 0 ]; then
    printf '\033[32mAll good.\033[0m\n'
else
    printf '\033[31m%d check(s) failed.\033[0m\n' "$fail"
fi
exit $fail
