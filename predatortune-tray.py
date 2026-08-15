#!/usr/bin/env python3
"""
PredatorTune tray indicator.

Sits in the top-right next to the other indicators, shows the temperature it
is actually reacting to, and lets the fans be taken off automatic without
opening the full window.
"""

import fcntl
import os
import re
import signal
import subprocess
import sys

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("AyatanaAppIndicator3", "0.1")
from gi.repository import AyatanaAppIndicator3 as AppIndicator  # noqa: E402
from gi.repository import GLib, Gtk  # noqa: E402

FAN_SPEED = "/sys/devices/platform/acer-wmi/predator_sense/fan_speed"
PROFILE = "/sys/firmware/acpi/platform_profile"
PROFILE_CHOICES = "/sys/firmware/acpi/platform_profile_choices"
MANUAL_FLAG = "/run/predatortune/manual"
HELPER = "/usr/local/bin/predatortune-helper"

# linuwu_sense hangs these off the acer-wmi platform device.
SENSE = "/sys/devices/platform/acer-wmi/predator_sense"
BATTERY_LIMIT = f"{SENSE}/battery_limiter"
KB_ZONES = "/sys/devices/platform/acer-wmi/four_zoned_kb/per_zone_mode"

# Four zones plus a brightness, as "rrggbb,rrggbb,rrggbb,rrggbb,brightness".
KB_COLOURS = [
    ("Teal", "00aec7"),
    ("Red", "ff0000"),
    ("Green", "00ff00"),
    ("Blue", "0000ff"),
    ("Purple", "8000ff"),
    ("Orange", "ff6000"),
    ("White", "ffffff"),
]

REFRESH_SECONDS = 3

# What the tray can set by hand. Auto hands the fans back to the firmware.
PRESETS = [
    ("Automatic (follow temperature)", None),
    ("Quiet", (30, 30)),
    ("Balanced", (55, 55)),
    ("Maximum", (100, 100)),
]


def read(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def cpu_temp():
    """Package temperature, found by name because hwmon numbering moves."""
    for entry in sorted(os.listdir("/sys/class/hwmon")):
        base = os.path.join("/sys/class/hwmon", entry)
        if read(os.path.join(base, "name")) != "coretemp":
            continue
        for label_path in sorted(os.listdir(base)):
            if not label_path.endswith("_label"):
                continue
            if "Package id 0" in (read(os.path.join(base, label_path)) or ""):
                raw = read(os.path.join(base, label_path.replace("_label", "_input")))
                if raw:
                    return int(raw) // 1000
        raw = read(os.path.join(base, "temp1_input"))
        if raw:
            return int(raw) // 1000
    return None


def gpu_device():
    for entry in os.listdir("/sys/bus/pci/devices"):
        base = os.path.join("/sys/bus/pci/devices", entry)
        if read(os.path.join(base, "vendor")) != "0x10de":
            continue
        if not (read(os.path.join(base, "class")) or "").startswith("0x0300"):
            continue
        return base
    return None


GPU_DEV = gpu_device()


def gpu_temp():
    """Skipped while the dGPU is asleep — asking would wake it."""
    if not GPU_DEV:
        return None
    if read(os.path.join(GPU_DEV, "power/runtime_status")) != "active":
        return None
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=temperature.gpu", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=4,
        )
        m = re.search(r"\d+", out.stdout)
        return int(m.group()) if m else None
    except (OSError, subprocess.SubprocessError):
        return None


def fan_percent():
    raw = read(FAN_SPEED)
    if not raw:
        return None
    try:
        cpu, gpu = (int(x) for x in raw.split(",")[:2])
        return cpu, gpu
    except ValueError:
        return None


def secure_boot_blocking():
    """
    With Secure Boot on, the kernel refuses modules signed by a key the
    firmware does not trust — which is everything DKMS builds locally. The
    tell is that the module is installed but its sysfs file never appears.
    """
    if os.path.exists(FAN_SPEED):
        return False
    mode = read("/sys/kernel/security/lockdown") or ""
    return "[integrity]" in mode or "[confidentiality]" in mode


def battery_limited():
    """True when charging is capped at 80%. None when unsupported."""
    v = read(BATTERY_LIMIT)
    return None if v is None else v.strip() == "1"


def set_battery_limit(on):
    return write_sysfs(BATTERY_LIMIT, "1" if on else "0",
                       ["set-battery-limit", "1" if on else "0"])


def kb_brightness():
    """The trailing field of per_zone_mode, kept when only the colour changes."""
    cur = read(KB_ZONES)
    if not cur:
        return "100"
    parts = cur.split(",")
    return parts[4] if len(parts) > 4 else "100"


def kb_current_colour():
    cur = read(KB_ZONES)
    return cur.split(",")[0] if cur else None


def set_kb_colour(hex6):
    zones = ",".join([hex6] * 4)
    return write_sysfs(KB_ZONES, f"{zones},{kb_brightness()}",
                       ["set-kb-colour", hex6])


def write_sysfs(path, value, helper_args):
    """
    tmpfiles opens these to 0666 so the tray can write directly. If that has
    not run yet, fall back to the helper — which takes a named action, never
    an arbitrary path, since polkit now grants it without a password.
    """
    try:
        with open(path, "w") as f:
            f.write(value)
        return True
    except OSError:
        return run_helper(helper_args)


def manual_mode():
    return os.path.exists(MANUAL_FLAG)


def set_manual(on):
    """The daemon stands down while this flag exists."""
    try:
        if on:
            os.makedirs(os.path.dirname(MANUAL_FLAG), exist_ok=True)
            open(MANUAL_FLAG, "w").close()
        elif os.path.exists(MANUAL_FLAG):
            os.remove(MANUAL_FLAG)
        return True
    except OSError:
        return False


def write_fan(cpu, gpu):
    try:
        with open(FAN_SPEED, "w") as f:
            f.write(f"{cpu},{gpu}")
        return True
    except OSError:
        return run_helper(["set-fan-speed", str(cpu), str(gpu)])


def run_helper(args):
    try:
        subprocess.Popen(["pkexec", HELPER, *args])
        return True
    except OSError:
        return False


class Tray:
    def __init__(self):
        self.indicator = AppIndicator.Indicator.new(
            "predatortune",
            "temperature-symbolic",
            AppIndicator.IndicatorCategory.HARDWARE,
        )
        self.indicator.set_status(AppIndicator.IndicatorStatus.ACTIVE)
        self.indicator.set_title("PredatorTune")

        self.items = {}
        self.indicator.set_menu(self.build_menu())
        self.refresh()
        GLib.timeout_add_seconds(REFRESH_SECONDS, self.refresh)

    # -- menu ---------------------------------------------------------

    def build_menu(self):
        menu = Gtk.Menu()

        for key in ("cpu", "gpu", "fan", "mode"):
            item = Gtk.MenuItem(label="…")
            item.set_sensitive(False)
            menu.append(item)
            self.items[key] = item

        menu.append(Gtk.SeparatorMenuItem())

        header = Gtk.MenuItem(label="Fans")
        header.set_sensitive(False)
        menu.append(header)

        # Gtk.RadioMenuItem's "group" property wants a sibling item, not the
        # list that get_group() hands back.
        first = None
        for label, speeds in PRESETS:
            item = Gtk.RadioMenuItem(label=f"   {label}")
            if first is None:
                first = item
            else:
                item.set_property("group", first)
            item.connect("activate", self.on_preset, speeds)
            menu.append(item)
            self.items[f"preset:{label}"] = item

        menu.append(Gtk.SeparatorMenuItem())

        # Battery: the 80% cap PredatorSense offers on Windows.
        self.items["battery"] = Gtk.CheckMenuItem(label="Stop charging at 80%")
        self.items["battery"].connect("toggled", self.on_battery)
        menu.append(self.items["battery"])

        # Keyboard: four zones, all set to one colour. Per-zone lives in the
        # window; a tray menu is the wrong place for a colour picker.
        kb = Gtk.MenuItem(label="Keyboard colour")
        kb_menu = Gtk.Menu()
        kfirst = None
        for name, hex6 in KB_COLOURS:
            item = Gtk.RadioMenuItem(label=name)
            if kfirst is None:
                kfirst = item
            else:
                item.set_property("group", kfirst)
            item.connect("activate", self.on_kb_colour, hex6)
            kb_menu.append(item)
            self.items[f"kb:{hex6}"] = item
        kb.set_submenu(kb_menu)
        menu.append(kb)
        self.items["kb-parent"] = kb

        menu.append(Gtk.SeparatorMenuItem())

        choices = (read(PROFILE_CHOICES) or "").split()
        if choices:
            header = Gtk.MenuItem(label="Performance profile")
            header.set_sensitive(False)
            menu.append(header)
            pfirst = None
            for name in choices:
                item = Gtk.RadioMenuItem(label=f"   {name.replace('-', ' ')}")
                if pfirst is None:
                    pfirst = item
                else:
                    item.set_property("group", pfirst)
                item.connect("activate", self.on_profile, name)
                menu.append(item)
                self.items[f"profile:{name}"] = item
            menu.append(Gtk.SeparatorMenuItem())

        # The window is a separate build that needs libadwaita; only offer it
        # if it is actually installed.
        if os.path.exists("/usr/bin/predatortune"):
            window = Gtk.MenuItem(label="Open PredatorTune")
            window.connect("activate", lambda *_: subprocess.Popen(["predatortune"]))
            menu.append(window)

        quit_item = Gtk.MenuItem(label="Quit")
        quit_item.connect("activate", lambda *_: Gtk.main_quit())
        menu.append(quit_item)

        menu.show_all()
        return menu

    # -- actions ------------------------------------------------------

    def on_preset(self, item, speeds):
        # Radio items fire on deactivation too; only act on the chosen one.
        if not item.get_active() or self.updating:
            return
        if speeds is None:
            set_manual(False)
            write_fan(0, 0)
        else:
            set_manual(True)
            write_fan(*speeds)
        self.refresh()

    def on_battery(self, item):
        if self.updating:
            return
        set_battery_limit(item.get_active())

    def on_kb_colour(self, item, hex6):
        if not item.get_active() or self.updating:
            return
        set_kb_colour(hex6)

    def on_profile(self, item, name):
        if not item.get_active() or self.updating:
            return
        if read(PROFILE) == name:
            return
        try:
            with open(PROFILE, "w") as f:
                f.write(name)
        except OSError:
            run_helper(["set-profile", name])

    # -- display ------------------------------------------------------

    updating = False

    def refresh(self):
        cpu = cpu_temp()
        gpu = gpu_temp()
        fan = fan_percent()
        manual = manual_mode()

        # Both temperatures side by side in the panel, so it is obvious which
        # one is driving the fans. The dGPU shows a dash while it is asleep.
        parts = []
        if cpu is not None:
            parts.append(f"C {cpu}°")
        parts.append(f"G {gpu}°" if gpu is not None else "G –")
        self.indicator.set_label("  ".join(parts), "C 100°  G 100°")

        self.items["cpu"].set_label(
            f"CPU  {cpu}°C" if cpu is not None else "CPU  —"
        )
        self.items["gpu"].set_label(
            f"GPU  {gpu}°C" if gpu is not None else "GPU  asleep"
        )

        if fan is None:
            self.items["fan"].set_label(
                "Fans  blocked by Secure Boot" if secure_boot_blocking()
                else "Fans  module not loaded"
            )
        elif fan == (0, 0):
            self.items["fan"].set_label("Fans  firmware auto")
        else:
            self.items["fan"].set_label(f"Fans  CPU {fan[0]}%  GPU {fan[1]}%")

        self.items["mode"].set_label(
            "Set by hand" if manual else "Following temperature"
        )

        # Nothing to write to, so do not offer speeds that cannot be applied.
        for label, _ in PRESETS:
            self.items[f"preset:{label}"].set_sensitive(fan is not None)

        limited = battery_limited()
        self.items["battery"].set_sensitive(limited is not None)
        if limited is not None:
            self.updating = True
            self.items["battery"].set_active(limited)
            self.updating = False

        colour = kb_current_colour()
        self.items["kb-parent"].set_sensitive(colour is not None)
        key = f"kb:{colour}"
        if key in self.items:
            self.updating = True
            self.items[key].set_active(True)
            self.updating = False

        # Reflect reality in the radio items without re-triggering them.
        self.updating = True
        if not manual:
            self.items["preset:Automatic (follow temperature)"].set_active(True)
        else:
            for label, speeds in PRESETS:
                if speeds and fan == speeds:
                    self.items[f"preset:{label}"].set_active(True)
                    break

        current = read(PROFILE)
        key = f"profile:{current}"
        if key in self.items:
            self.items[key].set_active(True)
        self.updating = False

        return True


def claim_singleton():
    """
    Install-time startup and XDG autostart can both fire, and the user can
    launch it by hand as well. Without this you get one icon per copy.
    The lock is held for the life of the process and released by the kernel
    when it exits, so a crash cannot leave it stuck.
    """
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/tmp/runtime-{os.getuid()}"
    os.makedirs(runtime, exist_ok=True)
    handle = open(os.path.join(runtime, "predatortune-tray.lock"), "w")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return None
    return handle


def main():
    lock = claim_singleton()
    if lock is None:
        print("predatortune-tray is already running", file=sys.stderr)
        return 0

    # Ctrl-C in a terminal should still close it.
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    Tray()
    Gtk.main()


if __name__ == "__main__":
    sys.exit(main())
