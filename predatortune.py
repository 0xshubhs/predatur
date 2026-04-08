#!/usr/bin/env python3
"""
PredatorTune - Fan & Thermal Control for Acer Predator Helios 16 (PHN16-71)
Uses kernel platform_profile + acer-wmi hwmon directly. No NBFC.
"""

import gi
gi.require_version('Adw', '1')
gi.require_version('Gtk', '4.0')

from gi.repository import Adw, Gtk, GLib, Gdk, Gio
import subprocess
import os
import sys
import signal

# ---------------------------------------------------------------------------
# Hardware paths (discovered at startup)
# ---------------------------------------------------------------------------

HWMON_FAN = None       # acer-wmi hwmon (fan RPM)
HWMON_CORETEMP = None  # coretemp hwmon (CPU temps)

PLATFORM_PROFILE = "/sys/firmware/acpi/platform_profile"
PLATFORM_PROFILE_CHOICES = "/sys/firmware/acpi/platform_profile_choices"
HELPER_PATH = "/usr/local/bin/predatortune-helper"

# Profile display config: (internal_name, display_label, icon, description)
PROFILES = [
    ("low-power",            "Power Saver", "battery-level-20-symbolic",          "Minimal fans, max battery life"),
    ("quiet",                "Quiet",       "audio-volume-muted-symbolic",        "Low fan noise, reduced performance"),
    ("balanced",             "Balanced",    "power-profile-balanced-symbolic",     "Default. Auto fan curves"),
    ("balanced-performance", "Boost",       "power-profile-performance-symbolic",  "Higher clocks, active cooling"),
    ("performance",          "Turbo",       "dialog-warning-symbolic",            "Max performance, fans unrestricted"),
]


def _discover_hwmon():
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


# ---------------------------------------------------------------------------
# Hardware reading helpers (all direct sysfs, no subprocess except GPU temp)
# ---------------------------------------------------------------------------

def read_fan_rpm(index: int) -> int | None:
    if HWMON_FAN is None:
        return None
    path = os.path.join(HWMON_FAN, f"fan{index + 1}_input")
    try:
        with open(path) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def read_cpu_temp() -> float | None:
    if HWMON_CORETEMP is None:
        return None
    try:
        with open(os.path.join(HWMON_CORETEMP, "temp1_input")) as f:
            return int(f.read().strip()) / 1000.0
    except (OSError, ValueError):
        return None


def read_cpu_core_temps() -> list[float]:
    """Read all core temps for min/max display."""
    if HWMON_CORETEMP is None:
        return []
    temps = []
    i = 2  # temp1 is package, temp2+ are cores
    while True:
        path = os.path.join(HWMON_CORETEMP, f"temp{i}_input")
        try:
            with open(path) as f:
                temps.append(int(f.read().strip()) / 1000.0)
        except (OSError, ValueError):
            break
        i += 1
    return temps


def read_gpu_temp() -> float | None:
    try:
        out = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=temperature.gpu", "--format=csv,noheader"],
            timeout=3, stderr=subprocess.DEVNULL
        ).decode().strip()
        return float(out)
    except Exception:
        return None


def read_gpu_power() -> float | None:
    try:
        out = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=power.draw", "--format=csv,noheader,nounits"],
            timeout=3, stderr=subprocess.DEVNULL
        ).decode().strip()
        return float(out)
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Platform profile helpers
# ---------------------------------------------------------------------------

def read_profile() -> str | None:
    try:
        with open(PLATFORM_PROFILE) as f:
            return f.read().strip()
    except OSError:
        return None


def read_profile_choices() -> list[str]:
    try:
        with open(PLATFORM_PROFILE_CHOICES) as f:
            return f.read().strip().split()
    except OSError:
        return []


def set_profile(name: str):
    if os.path.isfile(HELPER_PATH) and os.access(HELPER_PATH, os.X_OK):
        cmd = ["pkexec", HELPER_PATH, "set-profile", name]
    else:
        cmd = ["pkexec", "bash", "-c", f"echo {name} > {PLATFORM_PROFILE}"]
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


FAN_SPEED_SYSFS = "/sys/kernel/predatortune/fan_speed"


def set_fan_speed(cpu_pct: int, gpu_pct: int):
    """Set fan speed (0-100) via WMI kernel module."""
    # Try direct write first (if permissions allow), else use helper
    try:
        with open(FAN_SPEED_SYSFS, "w") as f:
            f.write(f"{cpu_pct},{gpu_pct}")
    except PermissionError:
        cmd = ["pkexec", HELPER_PATH, "set-fan-speed", str(cpu_pct), str(gpu_pct)]
        subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def set_fan_auto():
    """Restore automatic fan control."""
    set_fan_speed(0, 0)


def fan_module_loaded() -> bool:
    """Check if the predatortune_fan kernel module is loaded."""
    return os.path.exists(FAN_SPEED_SYSFS)


# ---------------------------------------------------------------------------
# CSS
# ---------------------------------------------------------------------------

CSS = """
.temp-green  { color: #57e389; }
.temp-yellow { color: #f9f06b; }
.temp-red    { color: #ed333b; }

.temp-big {
    font-size: 36px;
    font-weight: bold;
    font-variant-numeric: tabular-nums;
}

.temp-sub {
    font-size: 11px;
    opacity: 0.6;
    font-variant-numeric: tabular-nums;
}

.section-title {
    font-size: 11px;
    font-weight: bold;
    letter-spacing: 2px;
    opacity: 0.55;
}

.fan-rpm {
    font-size: 24px;
    font-weight: bold;
    font-variant-numeric: tabular-nums;
}

.fan-label {
    font-size: 12px;
    opacity: 0.6;
}

.profile-btn {
    min-height: 64px;
    min-width: 90px;
}

.profile-active {
    background: alpha(@accent_color, 0.3);
    border: 2px solid @accent_color;
}

.profile-desc {
    font-size: 10px;
    opacity: 0.5;
}

.status-bar {
    font-size: 11px;
    opacity: 0.5;
    padding: 4px 12px;
}

.gpu-power {
    font-size: 11px;
    opacity: 0.6;
}

.fan-speed-value {
    font-size: 18px;
    font-weight: bold;
    font-variant-numeric: tabular-nums;
    min-width: 48px;
}

.fan-auto-badge {
    font-size: 11px;
    font-weight: bold;
    color: #57e389;
}
"""

# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

class PredatorTuneWindow(Adw.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="PredatorTune")
        self.set_default_size(480, 780)
        self.set_resizable(True)

        self._available_profiles = read_profile_choices()

        root_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.set_content(root_box)

        # Header
        header = Adw.HeaderBar()
        title_label = Gtk.Label(label="PredatorTune")
        title_label.add_css_class("heading")
        header.set_title_widget(title_label)
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

        # ---- Temperatures ----
        temp_group = Adw.PreferencesGroup(title="Temperatures")
        content.append(temp_group)

        temp_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=32,
                           halign=Gtk.Align.CENTER)
        temp_box.set_margin_top(8)
        temp_box.set_margin_bottom(8)
        temp_group.add(temp_box)

        # CPU
        cpu_col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2,
                          halign=Gtk.Align.CENTER)
        lbl = Gtk.Label(label="CPU")
        lbl.add_css_class("section-title")
        cpu_col.append(lbl)
        self.cpu_temp_label = Gtk.Label(label="--\u00b0C")
        self.cpu_temp_label.add_css_class("temp-big")
        cpu_col.append(self.cpu_temp_label)
        self.cpu_minmax_label = Gtk.Label(label="")
        self.cpu_minmax_label.add_css_class("temp-sub")
        cpu_col.append(self.cpu_minmax_label)
        temp_box.append(cpu_col)

        # GPU
        gpu_col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2,
                          halign=Gtk.Align.CENTER)
        lbl = Gtk.Label(label="GPU")
        lbl.add_css_class("section-title")
        gpu_col.append(lbl)
        self.gpu_temp_label = Gtk.Label(label="--\u00b0C")
        self.gpu_temp_label.add_css_class("temp-big")
        gpu_col.append(self.gpu_temp_label)
        self.gpu_power_label = Gtk.Label(label="")
        self.gpu_power_label.add_css_class("gpu-power")
        gpu_col.append(self.gpu_power_label)
        temp_box.append(gpu_col)

        # ---- Fans ----
        fan_group = Adw.PreferencesGroup(title="Fans")
        content.append(fan_group)

        fan_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=32,
                          halign=Gtk.Align.CENTER)
        fan_box.set_margin_top(8)
        fan_box.set_margin_bottom(8)
        fan_group.add(fan_box)

        self.fan_rpm_labels = []
        self.fan_name_labels = []
        for name in ["CPU Fan", "GPU Fan"]:
            col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2,
                          halign=Gtk.Align.CENTER)
            nlbl = Gtk.Label(label=name)
            nlbl.add_css_class("section-title")
            col.append(nlbl)
            self.fan_name_labels.append(nlbl)

            rpm_lbl = Gtk.Label(label="-- RPM")
            rpm_lbl.add_css_class("fan-rpm")
            col.append(rpm_lbl)
            self.fan_rpm_labels.append(rpm_lbl)

            fan_box.append(col)

        # ---- Fan Speed Control ----
        fan_ctrl_group = Adw.PreferencesGroup(title="Fan Speed Control")
        if not fan_module_loaded():
            no_mod = Gtk.Label(label="Kernel module not loaded. Run: sudo insmod predatortune_fan.ko")
            no_mod.add_css_class("fan-label")
            fan_ctrl_group.add(no_mod)
        content.append(fan_ctrl_group)

        self._fan_manual = False
        self.fan_sliders = []
        self.fan_speed_labels = []

        for i, name in enumerate(["CPU Fan", "GPU Fan"]):
            row_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
            row_box.set_margin_start(4)
            row_box.set_margin_end(4)

            lbl = Gtk.Label(label=name, width_chars=8, xalign=0)
            lbl.add_css_class("fan-label")
            row_box.append(lbl)

            slider = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 100, 5)
            slider.set_hexpand(True)
            slider.set_value(50)
            slider.set_draw_value(False)
            slider.connect("value-changed", self._on_fan_slider_changed, i)
            row_box.append(slider)
            self.fan_sliders.append(slider)

            val_lbl = Gtk.Label(label="50%")
            val_lbl.add_css_class("fan-speed-value")
            row_box.append(val_lbl)
            self.fan_speed_labels.append(val_lbl)

            fan_ctrl_group.add(row_box)

        # Auto button
        auto_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8,
                           halign=Gtk.Align.CENTER)
        auto_box.set_margin_top(4)
        self.fan_auto_btn = Gtk.Button(label="Reset to Auto")
        self.fan_auto_btn.connect("clicked", self._on_fan_auto_clicked)
        auto_box.append(self.fan_auto_btn)
        self.fan_auto_label = Gtk.Label(label="Auto")
        self.fan_auto_label.add_css_class("fan-auto-badge")
        auto_box.append(self.fan_auto_label)
        fan_ctrl_group.add(auto_box)

        # ---- Performance Mode ----
        mode_group = Adw.PreferencesGroup(title="Performance Mode")
        content.append(mode_group)

        self.profile_buttons = {}
        self.profile_desc_labels = {}

        mode_flow = Gtk.FlowBox()
        mode_flow.set_selection_mode(Gtk.SelectionMode.NONE)
        mode_flow.set_homogeneous(True)
        mode_flow.set_max_children_per_line(5)
        mode_flow.set_min_children_per_line(3)
        mode_flow.set_row_spacing(8)
        mode_flow.set_column_spacing(8)
        mode_group.add(mode_flow)

        for prof_id, label, icon, desc in PROFILES:
            if prof_id not in self._available_profiles:
                continue

            btn = Gtk.Button()
            btn.add_css_class("profile-btn")
            btn_content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4,
                                  halign=Gtk.Align.CENTER, valign=Gtk.Align.CENTER)
            btn_content.append(Gtk.Image.new_from_icon_name(icon))
            btn_content.append(Gtk.Label(label=label))

            desc_lbl = Gtk.Label(label=desc)
            desc_lbl.add_css_class("profile-desc")
            desc_lbl.set_wrap(True)
            desc_lbl.set_max_width_chars(14)
            desc_lbl.set_justify(Gtk.Justification.CENTER)
            btn_content.append(desc_lbl)

            btn.set_child(btn_content)
            btn.connect("clicked", self._on_profile_clicked, prof_id)
            mode_flow.append(btn)

            self.profile_buttons[prof_id] = btn
            self.profile_desc_labels[prof_id] = desc_lbl

        # ---- Status bar ----
        self.status_label = Gtk.Label(label="Starting...")
        self.status_label.add_css_class("status-bar")
        self.status_label.set_halign(Gtk.Align.START)
        root_box.append(self.status_label)

        # ---- Refresh timer ----
        self._tick_id = GLib.timeout_add(2000, self._refresh)
        GLib.idle_add(self._refresh)

    # ----- Fan speed control -----
    def _on_fan_slider_changed(self, slider, fan_index):
        speed = int(slider.get_value())
        self.fan_speed_labels[fan_index].set_label(f"{speed}%")
        self._fan_manual = True
        self.fan_auto_label.set_label("")
        cpu_pct = int(self.fan_sliders[0].get_value())
        gpu_pct = int(self.fan_sliders[1].get_value())
        set_fan_speed(cpu_pct, gpu_pct)

    def _on_fan_auto_clicked(self, btn):
        self._fan_manual = False
        self.fan_auto_label.set_label("Auto")
        set_fan_auto()

    # ----- Profile switching -----
    def _on_profile_clicked(self, btn, profile_id):
        set_profile(profile_id)
        # Optimistic UI update
        self._highlight_profile(profile_id)

    def _highlight_profile(self, active_id):
        for pid, btn in self.profile_buttons.items():
            if pid == active_id:
                btn.add_css_class("profile-active")
            else:
                btn.remove_css_class("profile-active")

    # ----- Refresh -----
    def _refresh(self) -> bool:
        # CPU temp
        cpu_t = read_cpu_temp()
        if cpu_t is not None:
            self._set_temp_label(self.cpu_temp_label, cpu_t)
            cores = read_cpu_core_temps()
            if cores:
                self.cpu_minmax_label.set_label(
                    f"Cores: {min(cores):.0f}\u00b0 \u2013 {max(cores):.0f}\u00b0C")
        else:
            self.cpu_temp_label.set_label("--\u00b0C")
            self.cpu_minmax_label.set_label("")

        # GPU temp + power
        gpu_t = read_gpu_temp()
        if gpu_t is not None:
            self._set_temp_label(self.gpu_temp_label, gpu_t)
        else:
            self.gpu_temp_label.set_label("--\u00b0C")

        gpu_w = read_gpu_power()
        if gpu_w is not None:
            self.gpu_power_label.set_label(f"{gpu_w:.1f} W")
        else:
            self.gpu_power_label.set_label("")

        # Fan RPMs
        for i in range(2):
            rpm = read_fan_rpm(i)
            if rpm is not None:
                self.fan_rpm_labels[i].set_label(f"{rpm} RPM")
            else:
                self.fan_rpm_labels[i].set_label("-- RPM")

        # Profile
        profile = read_profile()
        if profile:
            self._highlight_profile(profile)
            # Find display name
            display = profile
            for pid, lbl, _, _ in PROFILES:
                if pid == profile:
                    display = lbl
                    break
            cpu_str = f"{cpu_t:.0f}" if cpu_t is not None else "--"
            gpu_str = f"{gpu_t:.0f}" if gpu_t is not None else "--"
            self.status_label.set_label(
                f"Mode: {display}  |  CPU {cpu_str}\u00b0C  GPU {gpu_str}\u00b0C  |  "
                f"Predator PHN16-71")
        else:
            self.status_label.set_label("platform_profile not available")

        return True

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
        css_provider = Gtk.CssProvider()
        css_provider.load_from_string(CSS)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(),
            css_provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )

        style_mgr = Adw.StyleManager.get_default()
        style_mgr.set_color_scheme(Adw.ColorScheme.PREFER_DARK)

        win = PredatorTuneWindow(app)
        win.present()


def main():
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    app = PredatorTuneApp()
    app.run(sys.argv)


if __name__ == "__main__":
    main()
