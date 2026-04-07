#!/usr/bin/env python3
"""
PredatorTune - Fan Control for Acer Predator Helios 16 (PHN16-71)
A GTK4/Adwaita GUI for controlling fans via NBFC on Linux.
"""

import gi
gi.require_version('Adw', '1')
gi.require_version('Gtk', '4.0')

from gi.repository import Adw, Gtk, GLib, Gdk, Gio, Pango
import subprocess
import os
import sys
import json
import re
import signal

# ---------------------------------------------------------------------------
# Hardware reading helpers
# ---------------------------------------------------------------------------

HWMON_FAN = None          # path like /sys/class/hwmon/hwmonX  (acer-wmi)
HWMON_CORETEMP = None     # path like /sys/class/hwmon/hwmonX  (coretemp)


def _discover_hwmon():
    """Locate hwmon paths for acer fan RPM and coretemp by name."""
    global HWMON_FAN, HWMON_CORETEMP
    base = "/sys/class/hwmon"
    try:
        for entry in os.listdir(base):
            name_path = os.path.join(base, entry, "name")
            try:
                with open(name_path) as f:
                    name = f.read().strip()
            except OSError:
                continue
            if name == "acer":
                HWMON_FAN = os.path.join(base, entry)
            elif name == "coretemp":
                HWMON_CORETEMP = os.path.join(base, entry)
    except OSError:
        pass


_discover_hwmon()


def read_fan_rpm(index: int) -> int | None:
    """Read fan RPM from hwmon sysfs.  index 0 = fan1, 1 = fan2."""
    if HWMON_FAN is None:
        return None
    path = os.path.join(HWMON_FAN, f"fan{index + 1}_input")
    try:
        with open(path) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def read_cpu_temp() -> float | None:
    """Read CPU package temperature (coretemp temp1 = Package id 0)."""
    if HWMON_CORETEMP is None:
        return None
    path = os.path.join(HWMON_CORETEMP, "temp1_input")
    try:
        with open(path) as f:
            return int(f.read().strip()) / 1000.0
    except (OSError, ValueError):
        return None


def read_gpu_temp() -> float | None:
    """Read NVIDIA GPU temperature via nvidia-smi."""
    try:
        out = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=temperature.gpu",
             "--format=csv,noheader"],
            timeout=3, stderr=subprocess.DEVNULL
        ).decode().strip()
        return float(out)
    except Exception:
        return None


# ---------------------------------------------------------------------------
# NBFC helpers
# ---------------------------------------------------------------------------

def nbfc_status() -> dict | None:
    """Parse `nbfc status -a` into a dict.
    Returns dict with keys: read_only, config, fans (list of dicts).
    """
    try:
        out = subprocess.check_output(
            ["nbfc", "status", "-a"], timeout=5, stderr=subprocess.DEVNULL
        ).decode()
    except Exception:
        return None

    result = {"fans": []}
    current_fan = None
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        if ":" not in line:
            continue
        key, _, val = line.partition(":")
        key = key.strip()
        val = val.strip()

        if key == "Read-only":
            result["read_only"] = val.lower() == "true"
        elif key == "Selected Config Name":
            result["config"] = val
        elif key == "Fan Display Name":
            current_fan = {"name": val}
            result["fans"].append(current_fan)
        elif current_fan is not None:
            if key == "Temperature":
                current_fan["temperature"] = _safe_float(val)
            elif key == "Auto Control Enabled":
                current_fan["auto"] = val.lower() == "true"
            elif key == "Current Fan Speed":
                current_fan["current_speed"] = _safe_float(val)
            elif key == "Target Fan Speed":
                current_fan["target_speed"] = _safe_float(val)
            elif key == "Requested Fan Speed":
                current_fan["requested_speed"] = _safe_float(val)

    return result if result.get("fans") else None


def _safe_float(s: str) -> float:
    try:
        return float(s)
    except ValueError:
        return 0.0


def nbfc_set_speed(fan_index: int, percent: float):
    """Set fan speed via pkexec nbfc set."""
    percent = max(0.0, min(100.0, percent))
    cmd = ["pkexec", "nbfc", "set", "-f", str(fan_index), "-s", f"{percent:.1f}"]
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def nbfc_set_auto(fan_index: int):
    """Set fan to auto via pkexec nbfc set."""
    cmd = ["pkexec", "nbfc", "set", "-f", str(fan_index), "-a"]
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ---------------------------------------------------------------------------
# Polkit helper: run nbfc commands through a small helper script so that
# a single polkit policy can authorise it without prompting every time.
# ---------------------------------------------------------------------------

HELPER_PATH = "/usr/local/bin/predatortune-helper"


def _use_helper() -> bool:
    return os.path.isfile(HELPER_PATH) and os.access(HELPER_PATH, os.X_OK)


def nbfc_set_speed_helper(fan_index: int, percent: float):
    percent = max(0.0, min(100.0, percent))
    if _use_helper():
        cmd = ["pkexec", HELPER_PATH, "set-speed", str(fan_index), f"{percent:.1f}"]
    else:
        cmd = ["pkexec", "nbfc", "set", "-f", str(fan_index), "-s", f"{percent:.1f}"]
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def nbfc_set_auto_helper(fan_index: int):
    if _use_helper():
        cmd = ["pkexec", HELPER_PATH, "set-auto", str(fan_index)]
    else:
        cmd = ["pkexec", "nbfc", "set", "-f", str(fan_index), "-a"]
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ---------------------------------------------------------------------------
# Application CSS
# ---------------------------------------------------------------------------

CSS = """
.temp-green  { color: #57e389; }
.temp-yellow { color: #f9f06b; }
.temp-red    { color: #ed333b; }

.rpm-label {
    font-size: 13px;
    font-weight: bold;
}

.speed-pct {
    font-size: 22px;
    font-weight: bold;
    font-variant-numeric: tabular-nums;
}

.section-title {
    font-size: 11px;
    font-weight: bold;
    letter-spacing: 2px;
    opacity: 0.55;
}

.preset-btn {
    min-width: 100px;
}

.temp-big {
    font-size: 32px;
    font-weight: bold;
    font-variant-numeric: tabular-nums;
}

.fan-name {
    font-size: 14px;
    font-weight: bold;
}

.status-bar {
    font-size: 11px;
    opacity: 0.6;
    padding: 4px 12px;
}

.preset-active {
    background: alpha(@accent_color, 0.25);
    border-color: @accent_color;
}
"""

# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class PredatorTuneWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="PredatorTune")
        self.set_default_size(520, 680)
        self.set_resizable(False)

        # Track state
        self._slider_held = [False, False]
        self._pending_speed = [None, None]
        self._auto_mode = [True, True]
        self._current_preset = None

        # ---- Main layout ----
        root_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.set_content(root_box)

        # Header bar
        header = Adw.HeaderBar()
        header.set_title_widget(Gtk.Label(label="PredatorTune"))
        root_box.append(header)

        # Scrollable content
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        root_box.append(scroll)

        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        content.set_margin_top(12)
        content.set_margin_bottom(12)
        content.set_margin_start(16)
        content.set_margin_end(16)
        scroll.set_child(content)

        # ---- Temperature overview ----
        temp_group = Adw.PreferencesGroup(title="Temperatures")
        content.append(temp_group)

        temp_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=24,
                           halign=Gtk.Align.CENTER)
        temp_box.set_margin_top(8)
        temp_box.set_margin_bottom(8)
        temp_group.add(temp_box)

        # CPU temp
        cpu_col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4,
                          halign=Gtk.Align.CENTER)
        lbl = Gtk.Label(label="CPU")
        lbl.add_css_class("section-title")
        cpu_col.append(lbl)
        self.cpu_temp_label = Gtk.Label(label="--\u00b0C")
        self.cpu_temp_label.add_css_class("temp-big")
        cpu_col.append(self.cpu_temp_label)
        temp_box.append(cpu_col)

        # GPU temp
        gpu_col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4,
                          halign=Gtk.Align.CENTER)
        lbl = Gtk.Label(label="GPU")
        lbl.add_css_class("section-title")
        gpu_col.append(lbl)
        self.gpu_temp_label = Gtk.Label(label="--\u00b0C")
        self.gpu_temp_label.add_css_class("temp-big")
        gpu_col.append(self.gpu_temp_label)
        temp_box.append(gpu_col)

        # ---- Fan cards ----
        self.fan_cards = []
        for i, name in enumerate(["CPU Fan", "GPU Fan"]):
            card = self._build_fan_card(i, name)
            content.append(card)
            self.fan_cards.append(card)

        # ---- Presets ----
        preset_group = Adw.PreferencesGroup(title="Presets")
        content.append(preset_group)

        preset_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12,
                             halign=Gtk.Align.CENTER, homogeneous=True)
        preset_box.set_margin_top(4)
        preset_box.set_margin_bottom(4)
        preset_group.add(preset_box)

        self.preset_buttons = {}
        presets = [
            ("Silent",      "weather-clear-symbolic",   20),
            ("Balanced",    "power-profile-balanced-symbolic", -1),  # -1 = auto
            ("Performance", "power-profile-performance-symbolic", 80),
            ("Max",         "dialog-warning-symbolic",  100),
        ]
        for label, icon, _speed in presets:
            btn = Gtk.Button()
            btn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6,
                              halign=Gtk.Align.CENTER)
            btn_box.append(Gtk.Image.new_from_icon_name(icon))
            btn_box.append(Gtk.Label(label=label))
            btn.set_child(btn_box)
            btn.add_css_class("preset-btn")
            btn.connect("clicked", self._on_preset, label, _speed)
            preset_box.append(btn)
            self.preset_buttons[label] = btn

        # ---- Status bar ----
        self.status_label = Gtk.Label(label="Initializing...")
        self.status_label.add_css_class("status-bar")
        self.status_label.set_halign(Gtk.Align.START)
        root_box.append(self.status_label)

        # ---- Start refresh timer ----
        self._tick_id = GLib.timeout_add(2000, self._refresh)
        # Do an immediate refresh
        GLib.idle_add(self._refresh)

    # ----- Fan card builder -----
    def _build_fan_card(self, index: int, name: str) -> Adw.PreferencesGroup:
        group = Adw.PreferencesGroup(title=name)

        # Info row: RPM and current speed %
        info_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        info_box.set_margin_top(4)

        rpm_label = Gtk.Label(label="-- RPM")
        rpm_label.add_css_class("rpm-label")
        rpm_label.set_halign(Gtk.Align.START)
        rpm_label.set_hexpand(True)
        info_box.append(rpm_label)

        speed_label = Gtk.Label(label="--%")
        speed_label.add_css_class("speed-pct")
        speed_label.set_halign(Gtk.Align.END)
        info_box.append(speed_label)

        group.add(info_box)

        # Auto toggle row
        auto_row = Adw.ActionRow(title="Automatic Control",
                                  subtitle="Let NBFC manage this fan")
        auto_switch = Gtk.Switch(valign=Gtk.Align.CENTER)
        auto_switch.set_active(True)
        auto_switch.connect("state-set", self._on_auto_toggle, index)
        auto_row.add_suffix(auto_switch)
        auto_row.set_activatable_widget(auto_switch)
        group.add(auto_row)

        # Slider row
        slider_row = Adw.ActionRow(title="Manual Speed")
        slider_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        slider_box.set_hexpand(True)
        slider_box.set_margin_top(8)
        slider_box.set_margin_bottom(8)

        slider = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 100, 5)
        slider.set_value(50)
        slider.set_hexpand(True)
        slider.set_draw_value(True)
        slider.set_value_pos(Gtk.PositionType.RIGHT)
        for mark in (0, 25, 50, 75, 100):
            slider.add_mark(mark, Gtk.PositionType.BOTTOM, f"{mark}%")
        slider.set_sensitive(False)  # disabled when auto

        # Track press/release so we don't fight with refresh while dragging
        press_ctrl = Gtk.GestureClick.new()
        press_ctrl.connect("pressed", self._on_slider_pressed, index)
        press_ctrl.connect("released", self._on_slider_released, index)
        slider.add_controller(press_ctrl)

        slider.connect("value-changed", self._on_slider_changed, index)
        slider_box.append(slider)
        slider_row.add_suffix(slider_box)
        group.add(slider_row)

        # Store widget refs on the group for easy access
        group._pt_rpm_label = rpm_label
        group._pt_speed_label = speed_label
        group._pt_auto_switch = auto_switch
        group._pt_slider = slider
        group._pt_index = index

        return group

    # ----- Callbacks -----
    def _on_auto_toggle(self, switch, state, fan_index):
        self._auto_mode[fan_index] = state
        card = self.fan_cards[fan_index]
        card._pt_slider.set_sensitive(not state)
        self._current_preset = None
        self._update_preset_highlight()
        if state:
            nbfc_set_auto_helper(fan_index)
        else:
            speed = card._pt_slider.get_value()
            nbfc_set_speed_helper(fan_index, speed)
        return False

    def _on_slider_pressed(self, gesture, n, x, y, fan_index):
        self._slider_held[fan_index] = True

    def _on_slider_released(self, gesture, n, x, y, fan_index):
        self._slider_held[fan_index] = False
        # Apply any pending speed
        if self._pending_speed[fan_index] is not None:
            nbfc_set_speed_helper(fan_index, self._pending_speed[fan_index])
            self._pending_speed[fan_index] = None

    def _on_slider_changed(self, slider, fan_index):
        if self._auto_mode[fan_index]:
            return
        speed = slider.get_value()
        self._current_preset = None
        self._update_preset_highlight()
        if self._slider_held[fan_index]:
            self._pending_speed[fan_index] = speed
        else:
            nbfc_set_speed_helper(fan_index, speed)

    def _on_preset(self, btn, label, speed):
        self._current_preset = label
        self._update_preset_highlight()
        for i in range(2):
            card = self.fan_cards[i]
            if speed < 0:
                # Auto
                card._pt_auto_switch.set_active(True)
            else:
                card._pt_auto_switch.set_active(False)
                card._pt_slider.set_value(speed)
                nbfc_set_speed_helper(i, speed)

    def _update_preset_highlight(self):
        for name, btn in self.preset_buttons.items():
            if name == self._current_preset:
                btn.add_css_class("preset-active")
            else:
                btn.remove_css_class("preset-active")

    # ----- Periodic refresh -----
    def _refresh(self) -> bool:
        # CPU temp from hwmon (fast, no subprocess)
        cpu_t = read_cpu_temp()
        if cpu_t is not None:
            self._set_temp_label(self.cpu_temp_label, cpu_t)
        else:
            self.cpu_temp_label.set_label("--\u00b0C")

        # GPU temp from nvidia-smi (subprocess, but lightweight)
        gpu_t = read_gpu_temp()
        if gpu_t is not None:
            self._set_temp_label(self.gpu_temp_label, gpu_t)
        else:
            self.gpu_temp_label.set_label("--\u00b0C")

        # Fan RPM from hwmon
        for i in range(2):
            rpm = read_fan_rpm(i)
            card = self.fan_cards[i]
            if rpm is not None:
                card._pt_rpm_label.set_label(f"{rpm} RPM")
            else:
                card._pt_rpm_label.set_label("-- RPM")

        # NBFC status for speed %  and auto state
        status = nbfc_status()
        if status and status.get("fans"):
            for i, fan in enumerate(status["fans"][:2]):
                card = self.fan_cards[i]
                spd = fan.get("current_speed", 0)
                card._pt_speed_label.set_label(f"{spd:.0f}%")

                # Sync auto switch (don't trigger callback)
                auto = fan.get("auto", True)
                sw = card._pt_auto_switch
                if sw.get_active() != auto:
                    sw.handler_block_by_func(self._on_auto_toggle)
                    sw.set_active(auto)
                    card._pt_slider.set_sensitive(not auto)
                    sw.handler_unblock_by_func(self._on_auto_toggle)
                self._auto_mode[i] = auto

                # If not dragging slider and in auto mode, sync slider to
                # current speed so it reflects reality
                if auto and not self._slider_held[i]:
                    card._pt_slider.handler_block_by_func(self._on_slider_changed)
                    card._pt_slider.set_value(spd)
                    card._pt_slider.handler_unblock_by_func(self._on_slider_changed)

            cfg = status.get("config", "unknown")
            cpu_str = f"{cpu_t:.0f}" if cpu_t is not None else "--"
            gpu_str = f"{gpu_t:.0f}" if gpu_t is not None else "--"
            self.status_label.set_label(
                f"NBFC active  |  Profile: {cfg}  |  "
                f"CPU {cpu_str}\u00b0C  GPU {gpu_str}\u00b0C"
            )
        else:
            self.status_label.set_label("NBFC service not responding")

        return True  # keep timer alive

    @staticmethod
    def _set_temp_label(label: Gtk.Label, temp: float):
        label.set_label(f"{temp:.0f}\u00b0C")
        for cls in ("temp-green", "temp-yellow", "temp-red"):
            label.remove_css_class(cls)
        if temp < 60:
            label.add_css_class("temp-green")
        elif temp < 80:
            label.add_css_class("temp-yellow")
        else:
            label.add_css_class("temp-red")


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

class PredatorTuneApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id="com.predatortune.app",
                         flags=Gio.ApplicationFlags.DEFAULT_FLAGS)
        self.connect("activate", self._on_activate)

    def _on_activate(self, app):
        # Load CSS
        css_provider = Gtk.CssProvider()
        css_provider.load_from_string(CSS)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(),
            css_provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )

        # Force dark theme
        style_mgr = Adw.StyleManager.get_default()
        style_mgr.set_color_scheme(Adw.ColorScheme.PREFER_DARK)

        win = PredatorTuneWindow(app)
        win.present()


def main():
    # Allow Ctrl+C to kill the app
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    app = PredatorTuneApp()
    app.run(sys.argv)


if __name__ == "__main__":
    main()
