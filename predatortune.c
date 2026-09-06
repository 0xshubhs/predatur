/*
 * PredatorTune - Fan & Thermal Control for Acer Predator Helios 16 (PHN16-71)
 * Uses kernel platform_profile + acer-wmi hwmon directly. No NBFC.
 *
 * GTK4/libadwaita GUI written in C.
 */

#include <adwaita.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* Hardware paths                                                             */
/* -------------------------------------------------------------------------- */

#define PLATFORM_PROFILE         "/sys/firmware/acpi/platform_profile"
#define PLATFORM_PROFILE_CHOICES "/sys/firmware/acpi/platform_profile_choices"
#define HELPER_PATH              "/usr/local/bin/predatortune-helper"
#define SENSE_DIR                "/sys/devices/platform/acer-wmi/predator_sense"
#define FAN_SPEED_SYSFS          SENSE_DIR "/fan_speed"
#define BATTERY_LIMIT_SYSFS      SENSE_DIR "/battery_limiter"
#define KB_ZONES_SYSFS \
    "/sys/devices/platform/acer-wmi/four_zoned_kb/per_zone_mode"

/* Four zones then a brightness: "rrggbb,rrggbb,rrggbb,rrggbb,brightness". */
typedef struct {
    const char *label;
    const char *hex;
} KbColour;

static const KbColour kb_colours[] = {
    { "Teal",   "00aec7" },
    { "Red",    "ff0000" },
    { "Green",  "00ff00" },
    { "Blue",   "0000ff" },
    { "Purple", "8000ff" },
    { "Orange", "ff6000" },
    { "Pink",   "ff00c0" },
    { "White",  "ffffff" },
};
#define N_KB_COLOURS (sizeof(kb_colours) / sizeof(kb_colours[0]))

static char hwmon_fan[256];       /* acer-wmi hwmon path */
static char hwmon_coretemp[256];  /* coretemp hwmon path */
static int  have_hwmon_fan;
static int  have_hwmon_coretemp;

/* Profile config */
typedef struct {
    const char *id;
    const char *label;
    const char *icon;
    const char *desc;
} ProfileInfo;

static const ProfileInfo profiles[] = {
    { "low-power",            "Power Saver", "battery-level-20-symbolic",         "Minimal fans, max battery life" },
    { "quiet",                "Quiet",       "audio-volume-muted-symbolic",       "Low fan noise, reduced performance" },
    { "balanced",             "Balanced",    "power-profile-balanced-symbolic",    "Default. Auto fan curves" },
    { "balanced-performance", "Boost",       "power-profile-performance-symbolic", "Higher clocks, active cooling" },
    { "performance",          "Turbo",       "dialog-warning-symbolic",           "Max performance, fans unrestricted" },
};
#define N_PROFILES (sizeof(profiles) / sizeof(profiles[0]))

/* -------------------------------------------------------------------------- */
/* Discover hwmon paths                                                       */
/* -------------------------------------------------------------------------- */

static void discover_hwmon(void)
{
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char name_path[512];
        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/%s/name", ent->d_name);

        FILE *f = fopen(name_path, "r");
        if (!f) continue;

        char name[64] = {0};
        if (fgets(name, sizeof(name), f)) {
            /* strip newline */
            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);

        if (strcmp(name, "acer") == 0) {
            snprintf(hwmon_fan, sizeof(hwmon_fan), "/sys/class/hwmon/%s", ent->d_name);
            have_hwmon_fan = 1;
        } else if (strcmp(name, "coretemp") == 0) {
            snprintf(hwmon_coretemp, sizeof(hwmon_coretemp), "/sys/class/hwmon/%s", ent->d_name);
            have_hwmon_coretemp = 1;
        }
    }
    closedir(dir);
}

/* -------------------------------------------------------------------------- */
/* Hardware reading helpers                                                   */
/* -------------------------------------------------------------------------- */

static int read_sysfs_int(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ret = (fscanf(f, "%d", out) == 1) ? 0 : -1;
    fclose(f);
    return ret;
}

static int read_fan_rpm(int index)
{
    if (!have_hwmon_fan) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/fan%d_input", hwmon_fan, index + 1);
    int rpm;
    if (read_sysfs_int(path, &rpm) == 0) return rpm;
    return -1;
}

static double read_cpu_temp(void)
{
    if (!have_hwmon_coretemp) return -1.0;
    char path[512];
    snprintf(path, sizeof(path), "%s/temp1_input", hwmon_coretemp);
    int milli;
    if (read_sysfs_int(path, &milli) == 0) return milli / 1000.0;
    return -1.0;
}

static int read_cpu_core_temps(double *temps, int max_count, double *min_out, double *max_out)
{
    if (!have_hwmon_coretemp) return 0;
    int count = 0;
    double mn = 999.0, mx = -999.0;
    for (int i = 2; count < max_count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/temp%d_input", hwmon_coretemp, i);
        int milli;
        if (read_sysfs_int(path, &milli) != 0) break;
        double t = milli / 1000.0;
        temps[count++] = t;
        if (t < mn) mn = t;
        if (t > mx) mx = t;
    }
    if (min_out) *min_out = mn;
    if (max_out) *max_out = mx;
    return count;
}

static double read_gpu_temp(void)
{
    FILE *p = popen("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader 2>/dev/null", "r");
    if (!p) return -1.0;
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), p) == NULL) { pclose(p); return -1.0; }
    pclose(p);
    char *end;
    double v = strtod(buf, &end);
    return (end != buf) ? v : -1.0;
}

static double read_gpu_power(void)
{
    FILE *p = popen("nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!p) return -1.0;
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), p) == NULL) { pclose(p); return -1.0; }
    pclose(p);
    char *end;
    double v = strtod(buf, &end);
    return (end != buf) ? v : -1.0;
}

/* -------------------------------------------------------------------------- */
/* Platform profile helpers                                                   */
/* -------------------------------------------------------------------------- */

static int read_profile(char *buf, size_t len)
{
    FILE *f = fopen(PLATFORM_PROFILE, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)len, f)) { fclose(f); return -1; }
    fclose(f);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return 0;
}

static int read_profile_choices(char choices[][32], int max_count)
{
    FILE *f = fopen(PLATFORM_PROFILE_CHOICES, "r");
    if (!f) return 0;
    char line[512] = {0};
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    fclose(f);

    int count = 0;
    char *tok = strtok(line, " \t\n");
    while (tok && count < max_count) {
        strncpy(choices[count], tok, 31);
        choices[count][31] = '\0';
        count++;
        tok = strtok(NULL, " \t\n");
    }
    return count;
}

/*
 * Never call system() from a button handler. It waits for the command to
 * finish, and pkexec does not return until the password dialog is answered,
 * so the window stops redrawing and the desktop offers to force quit it.
 */
static void run_detached(char **argv)
{
    g_spawn_async(NULL, argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                      | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static int fan_module_loaded(void)
{
    return access(FAN_SPEED_SYSFS, F_OK) == 0;
}

/*
 * tmpfiles opens these to 0666 so the window can write them directly. If that
 * has not happened, fall back to the helper — which takes named actions, never
 * a path, so an argument can only decide what is written and not where.
 */
static void write_sysfs(const char *path, const char *value, char **helper_argv)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(value, f);
        fclose(f);
        return;
    }
    run_detached(helper_argv);
}

static int battery_limit_supported(void)
{
    return access(BATTERY_LIMIT_SYSFS, F_OK) == 0;
}

static int battery_limit_on(void)
{
    int v;

    return read_sysfs_int(BATTERY_LIMIT_SYSFS, &v) == 0 && v == 1;
}

static void set_battery_limit(int on)
{
    char *argv[] = { "pkexec", (char *)HELPER_PATH, "set-battery-limit",
                     on ? "1" : "0", NULL };
    write_sysfs(BATTERY_LIMIT_SYSFS, on ? "1" : "0", argv);
}

static int kb_supported(void)
{
    return access(KB_ZONES_SYSFS, F_OK) == 0;
}

/*
 * Split "rrggbb,rrggbb,rrggbb,rrggbb,brightness" into its parts. Either
 * output may be NULL if the caller only wants the other.
 */
static int kb_read_zones(char zones[4][8], int *brightness)
{
    FILE *f = fopen(KB_ZONES_SYSFS, "r");
    char line[160], *tok;
    int i = 0;

    if (!f)
        return -1;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    for (tok = strtok(line, ",\n"); tok && i < 5; tok = strtok(NULL, ",\n"), i++) {
        if (i < 4) {
            if (zones)
                snprintf(zones[i], 8, "%s", tok);
        } else if (brightness) {
            *brightness = atoi(tok);
        }
    }
    return i >= 4 ? 0 : -1;
}

/* First zone's colour, which is what the presets set all four to. */
static int kb_read_colour(char *out, size_t n)
{
    char zones[4][8];

    if (kb_read_zones(zones, NULL) != 0)
        return -1;
    snprintf(out, n, "%s", zones[0]);
    return 0;
}

/* Set the four zones independently. */
static void kb_set_zones(char zones[4][8], int brightness)
{
    char value[160];
    char b[8];
    char *argv[] = { "pkexec", (char *)HELPER_PATH, "set-kb-zones",
                     zones[0], zones[1], zones[2], zones[3], b, NULL };

    if (brightness < 0)   brightness = 0;
    if (brightness > 100) brightness = 100;
    snprintf(b, sizeof(b), "%d", brightness);

    snprintf(value, sizeof(value), "%s,%s,%s,%s,%d",
             zones[0], zones[1], zones[2], zones[3], brightness);
    write_sysfs(KB_ZONES_SYSFS, value, argv);
}

/* Keep whatever brightness is set; only the colour is being changed. */
static void kb_set_colour(const char *hex)
{
    FILE *f = fopen(KB_ZONES_SYSFS, "r");
    char line[128], bright[16] = "100", value[128];
    char *argv[] = { "pkexec", (char *)HELPER_PATH, "set-kb-colour",
                     (char *)hex, NULL };

    if (f) {
        if (fgets(line, sizeof(line), f)) {
            char *tok = strtok(line, ",\n");
            for (int i = 0; tok && i < 5; i++) {
                if (i == 4)
                    snprintf(bright, sizeof(bright), "%s", tok);
                tok = strtok(NULL, ",\n");
            }
        }
        fclose(f);
    }

    snprintf(value, sizeof(value), "%s,%s,%s,%s,%s", hex, hex, hex, hex, bright);
    write_sysfs(KB_ZONES_SYSFS, value, argv);
}

/*
 * With Secure Boot on, the kernel refuses any module whose signing key the
 * firmware does not trust — including everything DKMS builds here. The
 * distinguishing sign is that the module is installed but will not load.
 */
static int secure_boot_blocking(void)
{
    FILE *f = fopen("/sys/kernel/security/lockdown", "r");
    char buf[128];
    int locked = 0;

    if (!f)
        return 0;
    if (fgets(buf, sizeof(buf), f)) {
        /* The active mode is the one in brackets. "none" means no lockdown. */
        locked = strstr(buf, "[integrity]") != NULL
                 || strstr(buf, "[confidentiality]") != NULL;
    }
    fclose(f);
    return locked && !fan_module_loaded();
}

static void set_profile(const char *name)
{
    if (access(HELPER_PATH, X_OK) == 0) {
        char *argv[] = { "pkexec", (char *)HELPER_PATH, "set-profile",
                         (char *)name, NULL };
        run_detached(argv);
    } else {
        char script[256];
        snprintf(script, sizeof(script), "echo %s > %s", name, PLATFORM_PROFILE);
        char *argv[] = { "pkexec", "bash", "-c", script, NULL };
        run_detached(argv);
    }
}

static void set_fan_speed(int cpu_pct, int gpu_pct)
{
    FILE *f = fopen(FAN_SPEED_SYSFS, "w");
    if (f) {
        fprintf(f, "%d,%d", cpu_pct, gpu_pct);
        fclose(f);
        return;
    }

    /*
     * With the module absent there is nothing for the helper to write to, so
     * asking for a password would buy a guaranteed failure. The banner in the
     * window already says why.
     */
    if (!fan_module_loaded())
        return;

    char cpu[8], gpu[8];
    snprintf(cpu, sizeof(cpu), "%d", cpu_pct);
    snprintf(gpu, sizeof(gpu), "%d", gpu_pct);
    char *argv[] = { "pkexec", (char *)HELPER_PATH, "set-fan-speed",
                     cpu, gpu, NULL };
    run_detached(argv);
}

/* -------------------------------------------------------------------------- */
/* CSS                                                                        */
/* -------------------------------------------------------------------------- */

static const char *CSS =
    /* Keyboard swatches. One class per colour so the button shows the colour
     * itself rather than only naming it. */
    ".kb-swatch { border-radius: 8px; border: 1px solid alpha(#fff, 0.25); }\n"
    ".kb-swatch.current { border: 2px solid #fff; }\n"
    ".kb-00aec7 { background: #00aec7; }\n"
    ".kb-ff0000 { background: #ff0000; }\n"
    ".kb-00ff00 { background: #00ff00; }\n"
    ".kb-0000ff { background: #0000ff; }\n"
    ".kb-8000ff { background: #8000ff; }\n"
    ".kb-ff6000 { background: #ff6000; }\n"
    ".kb-ff00c0 { background: #ff00c0; }\n"
    ".kb-ffffff { background: #ffffff; }\n"
    "\n"
    ".temp-green  { color: #57e389; }\n"
    ".temp-yellow { color: #f9f06b; }\n"
    ".temp-red    { color: #ed333b; }\n"
    "\n"
    ".temp-big {\n"
    "    font-size: 36px;\n"
    "    font-weight: bold;\n"
    "    font-variant-numeric: tabular-nums;\n"
    "}\n"
    "\n"
    ".temp-sub {\n"
    "    font-size: 11px;\n"
    "    opacity: 0.6;\n"
    "    font-variant-numeric: tabular-nums;\n"
    "}\n"
    "\n"
    ".section-title {\n"
    "    font-size: 11px;\n"
    "    font-weight: bold;\n"
    "    letter-spacing: 2px;\n"
    "    opacity: 0.55;\n"
    "}\n"
    "\n"
    ".fan-rpm {\n"
    "    font-size: 24px;\n"
    "    font-weight: bold;\n"
    "    font-variant-numeric: tabular-nums;\n"
    "}\n"
    "\n"
    ".fan-label {\n"
    "    font-size: 12px;\n"
    "    opacity: 0.6;\n"
    "}\n"
    "\n"
    ".profile-btn {\n"
    "    min-height: 64px;\n"
    "    min-width: 90px;\n"
    "}\n"
    "\n"
    ".profile-active {\n"
    "    background: alpha(@accent_color, 0.3);\n"
    "    border: 2px solid @accent_color;\n"
    "}\n"
    "\n"
    ".profile-desc {\n"
    "    font-size: 10px;\n"
    "    opacity: 0.5;\n"
    "}\n"
    "\n"
    ".status-bar {\n"
    "    font-size: 11px;\n"
    "    opacity: 0.5;\n"
    "    padding: 4px 12px;\n"
    "}\n"
    "\n"
    ".gpu-power {\n"
    "    font-size: 11px;\n"
    "    opacity: 0.6;\n"
    "}\n"
    "\n"
    ".fan-speed-value {\n"
    "    font-size: 18px;\n"
    "    font-weight: bold;\n"
    "    font-variant-numeric: tabular-nums;\n"
    "    min-width: 48px;\n"
    "}\n"
    "\n"
    ".fan-auto-badge {\n"
    "    font-size: 11px;\n"
    "    font-weight: bold;\n"
    "    color: #57e389;\n"
    "}\n";

/* -------------------------------------------------------------------------- */
/* Application state                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    AdwApplicationWindow *window;

    /* Temperature labels */
    GtkWidget *cpu_temp_label;
    GtkWidget *cpu_minmax_label;
    GtkWidget *gpu_temp_label;
    GtkWidget *gpu_power_label;

    /* Fan RPM labels */
    GtkWidget *fan_rpm_labels[2];

    /* Fan speed control */
    GtkWidget *fan_sliders[2];
    GtkWidget *fan_speed_labels[2];
    GtkWidget *fan_auto_label;
    int        fan_manual;

    /* Profile buttons */
    GtkWidget *profile_buttons[N_PROFILES];
    int        profile_button_count;

    /* Available profiles */
    char       available_profiles[16][32];
    int        available_profile_count;

    /* Battery */
    GtkWidget *battery_switch;
    GtkWidget *battery_note;

    /* Keyboard zones */
    GtkWidget *kb_buttons[N_KB_COLOURS];
    GtkWidget *kb_zone_pickers[4];
    GtkWidget *kb_brightness;

    /* Set while the refresh tick writes the widgets, so echoing a hardware
     * value back into a switch does not look like the user toggling it. */
    int        syncing;

    /* Status bar */
    GtkWidget *status_label;

    guint      tick_id;
} AppState;

static AppState app_state;

/* -------------------------------------------------------------------------- */
/* UI helpers                                                                 */
/* -------------------------------------------------------------------------- */

static void set_temp_label(GtkWidget *label, double temp)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f\u00b0C", temp);
    gtk_label_set_label(GTK_LABEL(label), buf);

    gtk_widget_remove_css_class(label, "temp-green");
    gtk_widget_remove_css_class(label, "temp-yellow");
    gtk_widget_remove_css_class(label, "temp-red");

    if (temp < 60.0)
        gtk_widget_add_css_class(label, "temp-green");
    else if (temp < 80.0)
        gtk_widget_add_css_class(label, "temp-yellow");
    else
        gtk_widget_add_css_class(label, "temp-red");
}

static void highlight_profile(const char *active_id)
{
    for (int i = 0; i < (int)N_PROFILES; i++) {
        if (!app_state.profile_buttons[i]) continue;
        if (strcmp(profiles[i].id, active_id) == 0)
            gtk_widget_add_css_class(app_state.profile_buttons[i], "profile-active");
        else
            gtk_widget_remove_css_class(app_state.profile_buttons[i], "profile-active");
    }
}

/* -------------------------------------------------------------------------- */
/* Callbacks                                                                  */
/* -------------------------------------------------------------------------- */

static void on_fan_slider_changed(GtkRange *range, gpointer user_data)
{
    int index = GPOINTER_TO_INT(user_data);
    int speed = (int)gtk_range_get_value(range);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", speed);
    gtk_label_set_label(GTK_LABEL(app_state.fan_speed_labels[index]), buf);

    app_state.fan_manual = 1;
    gtk_label_set_label(GTK_LABEL(app_state.fan_auto_label), "");

    int cpu_pct = (int)gtk_range_get_value(GTK_RANGE(app_state.fan_sliders[0]));
    int gpu_pct = (int)gtk_range_get_value(GTK_RANGE(app_state.fan_sliders[1]));
    set_fan_speed(cpu_pct, gpu_pct);
}

static void on_fan_auto_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn; (void)user_data;
    app_state.fan_manual = 0;
    gtk_label_set_label(GTK_LABEL(app_state.fan_auto_label), "Auto");
    set_fan_speed(0, 0);
}

static void on_battery_toggled(GObject *sw, GParamSpec *spec, gpointer user_data)
{
    (void)spec;
    (void)user_data;
    if (app_state.syncing)
        return;
    set_battery_limit(gtk_switch_get_active(GTK_SWITCH(sw)));
}

static void on_kb_colour_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    kb_set_colour((const char *)user_data);
}

/* Read all four pickers and the brightness, and push the lot at once — the
 * hardware takes the zones as a single value, not one at a time. */
static void kb_apply_from_widgets(void)
{
    char zones[4][8];
    int brightness = 100;

    if (app_state.syncing)
        return;

    for (int i = 0; i < 4; i++) {
        const GdkRGBA *c;

        if (!app_state.kb_zone_pickers[i])
            return;
        c = gtk_color_dialog_button_get_rgba(
                GTK_COLOR_DIALOG_BUTTON(app_state.kb_zone_pickers[i]));
        snprintf(zones[i], sizeof(zones[i]), "%02x%02x%02x",
                 (int)(c->red   * 255.0 + 0.5),
                 (int)(c->green * 255.0 + 0.5),
                 (int)(c->blue  * 255.0 + 0.5));
    }

    if (app_state.kb_brightness)
        brightness = (int)gtk_range_get_value(GTK_RANGE(app_state.kb_brightness));

    kb_set_zones(zones, brightness);
}

static void on_kb_zone_changed(GObject *btn, GParamSpec *spec, gpointer user_data)
{
    (void)btn;
    (void)spec;
    (void)user_data;
    kb_apply_from_widgets();
}

static void on_kb_brightness_changed(GtkRange *range, gpointer user_data)
{
    (void)range;
    (void)user_data;
    kb_apply_from_widgets();
}

static void on_profile_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    const char *profile_id = (const char *)user_data;
    set_profile(profile_id);
    highlight_profile(profile_id);
}

/* -------------------------------------------------------------------------- */
/* Refresh timer                                                              */
/* -------------------------------------------------------------------------- */

static gboolean refresh(gpointer user_data)
{
    (void)user_data;
    char buf[256];

    /* The window takes every label with it when it closes, so a tick that
     * outlives it writes freed memory — a burst of Gtk-CRITICALs on exit.
     * on_window_destroy clears the state; this is the tick noticing. */
    if (!app_state.window)
        return G_SOURCE_REMOVE;

    /* Battery limiter: show what the hardware says, not what was last clicked,
     * so an external change (the tray, PredatorSense on a dual boot) shows up. */
    if (app_state.battery_switch) {
        int supported = battery_limit_supported();
        gtk_widget_set_sensitive(app_state.battery_switch, supported);
        if (supported) {
            app_state.syncing = 1;
            gtk_switch_set_active(GTK_SWITCH(app_state.battery_switch),
                                  battery_limit_on());
            app_state.syncing = 0;
        } else {
            gtk_label_set_label(GTK_LABEL(app_state.battery_note),
                                "Needs the linuwu_sense driver.");
        }
    }

    /* Mark whichever keyboard colour is actually set. */
    char kb_now[32] = "";
    if (kb_supported() && kb_read_colour(kb_now, sizeof(kb_now)) == 0) {
        for (int i = 0; i < (int)N_KB_COLOURS; i++) {
            if (!app_state.kb_buttons[i])
                continue;
            GtkWidget *swatch = gtk_widget_get_first_child(
                gtk_button_get_child(GTK_BUTTON(app_state.kb_buttons[i])));
            if (!swatch)
                continue;
            if (g_ascii_strcasecmp(kb_now, kb_colours[i].hex) == 0)
                gtk_widget_add_css_class(swatch, "current");
            else
                gtk_widget_remove_css_class(swatch, "current");
        }
    }

    /* CPU temp */
    double cpu_t = read_cpu_temp();
    if (cpu_t >= 0.0) {
        set_temp_label(app_state.cpu_temp_label, cpu_t);
        double temps[64], mn, mx;
        int n = read_cpu_core_temps(temps, 64, &mn, &mx);
        if (n > 0) {
            snprintf(buf, sizeof(buf), "Cores: %.0f\u00b0 \u2013 %.0f\u00b0C", mn, mx);
            gtk_label_set_label(GTK_LABEL(app_state.cpu_minmax_label), buf);
        }
    } else {
        gtk_label_set_label(GTK_LABEL(app_state.cpu_temp_label), "--\u00b0C");
        gtk_label_set_label(GTK_LABEL(app_state.cpu_minmax_label), "");
    }

    /* GPU temp + power */
    double gpu_t = read_gpu_temp();
    if (gpu_t >= 0.0)
        set_temp_label(app_state.gpu_temp_label, gpu_t);
    else
        gtk_label_set_label(GTK_LABEL(app_state.gpu_temp_label), "--\u00b0C");

    double gpu_w = read_gpu_power();
    if (gpu_w >= 0.0) {
        snprintf(buf, sizeof(buf), "%.1f W", gpu_w);
        gtk_label_set_label(GTK_LABEL(app_state.gpu_power_label), buf);
    } else {
        gtk_label_set_label(GTK_LABEL(app_state.gpu_power_label), "");
    }

    /* Fan RPMs */
    for (int i = 0; i < 2; i++) {
        int rpm = read_fan_rpm(i);
        if (rpm >= 0)
            snprintf(buf, sizeof(buf), "%d RPM", rpm);
        else
            snprintf(buf, sizeof(buf), "-- RPM");
        gtk_label_set_label(GTK_LABEL(app_state.fan_rpm_labels[i]), buf);
    }

    /* Profile */
    char profile[64];
    if (read_profile(profile, sizeof(profile)) == 0) {
        highlight_profile(profile);
        const char *display = profile;
        for (int i = 0; i < (int)N_PROFILES; i++) {
            if (strcmp(profiles[i].id, profile) == 0) {
                display = profiles[i].label;
                break;
            }
        }
        char cpu_str[16] = "--";
        char gpu_str[16] = "--";
        if (cpu_t >= 0.0) snprintf(cpu_str, sizeof(cpu_str), "%.0f", cpu_t);
        if (gpu_t >= 0.0) snprintf(gpu_str, sizeof(gpu_str), "%.0f", gpu_t);
        snprintf(buf, sizeof(buf),
                 "Mode: %s  |  CPU %s\u00b0C  GPU %s\u00b0C  |  Predator PHN16-71",
                 display, cpu_str, gpu_str);
        gtk_label_set_label(GTK_LABEL(app_state.status_label), buf);
    } else {
        gtk_label_set_label(GTK_LABEL(app_state.status_label),
                            "platform_profile not available");
    }

    return G_SOURCE_CONTINUE;
}

/* The same handler drives the 2s tick and the initial paint, but an idle
 * callback returning G_SOURCE_CONTINUE is re-dispatched as fast as the main
 * loop can run it — which is not a refresh every 2s, it is a spin that held a
 * core at 40% for as long as the window was open. Once is once. */
static gboolean refresh_once(gpointer user_data)
{
    refresh(user_data);
    return G_SOURCE_REMOVE;
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    (void)user_data;

    if (app_state.tick_id) {
        g_source_remove(app_state.tick_id);
        app_state.tick_id = 0;
    }
    /* Every widget pointer in here belongs to the window that is going away.
     * build_window fills them in again if the app is activated a second time. */
    memset(&app_state, 0, sizeof(app_state));
}

/* -------------------------------------------------------------------------- */
/* Window construction                                                        */
/* -------------------------------------------------------------------------- */

static int is_profile_available(const char *id)
{
    for (int i = 0; i < app_state.available_profile_count; i++) {
        if (strcmp(app_state.available_profiles[i], id) == 0)
            return 1;
    }
    return 0;
}

static void build_window(AdwApplication *adw_app)
{
    /* Read available profiles */
    app_state.available_profile_count =
        read_profile_choices(app_state.available_profiles, 16);

    /* Window */
    app_state.window = ADW_APPLICATION_WINDOW(
        adw_application_window_new(GTK_APPLICATION(adw_app)));
    gtk_window_set_title(GTK_WINDOW(app_state.window), "PredatorTune");
    gtk_window_set_default_size(GTK_WINDOW(app_state.window), 480, 780);
    gtk_window_set_resizable(GTK_WINDOW(app_state.window), TRUE);

    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_application_window_set_content(app_state.window, root_box);

    /* Header bar */
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *title_label = gtk_label_new("PredatorTune");
    gtk_widget_add_css_class(title_label, "heading");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title_label);
    gtk_box_append(GTK_BOX(root_box), header);

    /* Scrollable content */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(root_box), scroll);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);

    /* ---- Temperatures ---- */
    GtkWidget *temp_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(temp_group), "Temperatures");
    gtk_box_append(GTK_BOX(content), temp_group);

    GtkWidget *temp_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_halign(temp_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(temp_box, 8);
    gtk_widget_set_margin_bottom(temp_box, 8);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(temp_group), temp_box);

    /* CPU column */
    GtkWidget *cpu_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(cpu_col, GTK_ALIGN_CENTER);
    GtkWidget *lbl = gtk_label_new("CPU");
    gtk_widget_add_css_class(lbl, "section-title");
    gtk_box_append(GTK_BOX(cpu_col), lbl);
    app_state.cpu_temp_label = gtk_label_new("--\u00b0C");
    gtk_widget_add_css_class(app_state.cpu_temp_label, "temp-big");
    gtk_box_append(GTK_BOX(cpu_col), app_state.cpu_temp_label);
    app_state.cpu_minmax_label = gtk_label_new("");
    gtk_widget_add_css_class(app_state.cpu_minmax_label, "temp-sub");
    gtk_box_append(GTK_BOX(cpu_col), app_state.cpu_minmax_label);
    gtk_box_append(GTK_BOX(temp_box), cpu_col);

    /* GPU column */
    GtkWidget *gpu_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(gpu_col, GTK_ALIGN_CENTER);
    lbl = gtk_label_new("GPU");
    gtk_widget_add_css_class(lbl, "section-title");
    gtk_box_append(GTK_BOX(gpu_col), lbl);
    app_state.gpu_temp_label = gtk_label_new("--\u00b0C");
    gtk_widget_add_css_class(app_state.gpu_temp_label, "temp-big");
    gtk_box_append(GTK_BOX(gpu_col), app_state.gpu_temp_label);
    app_state.gpu_power_label = gtk_label_new("");
    gtk_widget_add_css_class(app_state.gpu_power_label, "gpu-power");
    gtk_box_append(GTK_BOX(gpu_col), app_state.gpu_power_label);
    gtk_box_append(GTK_BOX(temp_box), gpu_col);

    /* ---- Fans ---- */
    GtkWidget *fan_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(fan_group), "Fans");
    gtk_box_append(GTK_BOX(content), fan_group);

    GtkWidget *fan_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_halign(fan_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(fan_box, 8);
    gtk_widget_set_margin_bottom(fan_box, 8);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(fan_group), fan_box);

    const char *fan_names[] = { "CPU Fan", "GPU Fan" };
    for (int i = 0; i < 2; i++) {
        GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_halign(col, GTK_ALIGN_CENTER);
        GtkWidget *nlbl = gtk_label_new(fan_names[i]);
        gtk_widget_add_css_class(nlbl, "section-title");
        gtk_box_append(GTK_BOX(col), nlbl);

        app_state.fan_rpm_labels[i] = gtk_label_new("-- RPM");
        gtk_widget_add_css_class(app_state.fan_rpm_labels[i], "fan-rpm");
        gtk_box_append(GTK_BOX(col), app_state.fan_rpm_labels[i]);

        gtk_box_append(GTK_BOX(fan_box), col);
    }

    /* ---- Fan Speed Control ---- */
    GtkWidget *fan_ctrl_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(fan_ctrl_group), "Fan Speed Control");
    if (!fan_module_loaded()) {
        /*
         * Telling someone to insmod it is useless advice under Secure Boot,
         * which refuses the module however it is loaded. Name the real cause
         * and the one command that fixes it.
         */
        GtkWidget *no_mod = gtk_label_new(
            secure_boot_blocking()
                ? "Fan control is blocked by Secure Boot: this machine's module "
                  "signing key is not enrolled yet.\n"
                  "Fix with:  sudo mokutil --import /var/lib/shim-signed/mok/MOK.der\n"
                  "then reboot and choose Enroll MOK. Performance modes work "
                  "either way."
                : "Fan control module is not loaded.\n"
                  "Try:  sudo modprobe predatortune_fan");
        gtk_label_set_justify(GTK_LABEL(no_mod), GTK_JUSTIFY_CENTER);
        gtk_label_set_wrap(GTK_LABEL(no_mod), TRUE);
        gtk_widget_add_css_class(no_mod, "fan-label");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(fan_ctrl_group), no_mod);
    }
    gtk_box_append(GTK_BOX(content), fan_ctrl_group);

    app_state.fan_manual = 0;

    for (int i = 0; i < 2; i++) {
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(row_box, 4);
        gtk_widget_set_margin_end(row_box, 4);

        GtkWidget *flbl = gtk_label_new(fan_names[i]);
        gtk_label_set_width_chars(GTK_LABEL(flbl), 8);
        gtk_label_set_xalign(GTK_LABEL(flbl), 0.0);
        gtk_widget_add_css_class(flbl, "fan-label");
        gtk_box_append(GTK_BOX(row_box), flbl);

        GtkWidget *slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
        gtk_widget_set_hexpand(slider, TRUE);
        gtk_range_set_value(GTK_RANGE(slider), 50);
        gtk_scale_set_draw_value(GTK_SCALE(slider), FALSE);
        g_signal_connect(slider, "value-changed",
                         G_CALLBACK(on_fan_slider_changed), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(row_box), slider);
        app_state.fan_sliders[i] = slider;

        GtkWidget *val_lbl = gtk_label_new("50%");
        gtk_widget_add_css_class(val_lbl, "fan-speed-value");
        gtk_box_append(GTK_BOX(row_box), val_lbl);
        app_state.fan_speed_labels[i] = val_lbl;

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(fan_ctrl_group), row_box);
    }

    /* Auto button */
    GtkWidget *auto_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(auto_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(auto_box, 4);

    GtkWidget *auto_btn = gtk_button_new_with_label("Reset to Auto");
    g_signal_connect(auto_btn, "clicked", G_CALLBACK(on_fan_auto_clicked), NULL);
    gtk_box_append(GTK_BOX(auto_box), auto_btn);

    app_state.fan_auto_label = gtk_label_new("Auto");
    gtk_widget_add_css_class(app_state.fan_auto_label, "fan-auto-badge");
    gtk_box_append(GTK_BOX(auto_box), app_state.fan_auto_label);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(fan_ctrl_group), auto_box);

    /* ---- Performance Mode ---- */
    GtkWidget *mode_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(mode_group), "Performance Mode");
    gtk_box_append(GTK_BOX(content), mode_group);

    GtkWidget *mode_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(mode_flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(mode_flow), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(mode_flow), 5);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(mode_flow), 3);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(mode_flow), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(mode_flow), 8);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(mode_group), mode_flow);

    for (int i = 0; i < (int)N_PROFILES; i++) {
        app_state.profile_buttons[i] = NULL;
        if (!is_profile_available(profiles[i].id))
            continue;

        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "profile-btn");

        GtkWidget *btn_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_halign(btn_content, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(btn_content, GTK_ALIGN_CENTER);

        GtkWidget *icon = gtk_image_new_from_icon_name(profiles[i].icon);
        gtk_box_append(GTK_BOX(btn_content), icon);
        gtk_box_append(GTK_BOX(btn_content), gtk_label_new(profiles[i].label));

        GtkWidget *desc_lbl = gtk_label_new(profiles[i].desc);
        gtk_widget_add_css_class(desc_lbl, "profile-desc");
        gtk_label_set_wrap(GTK_LABEL(desc_lbl), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(desc_lbl), 14);
        gtk_label_set_justify(GTK_LABEL(desc_lbl), GTK_JUSTIFY_CENTER);
        gtk_box_append(GTK_BOX(btn_content), desc_lbl);

        gtk_button_set_child(GTK_BUTTON(btn), btn_content);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_profile_clicked),
                         (gpointer)profiles[i].id);
        gtk_flow_box_append(GTK_FLOW_BOX(mode_flow), btn);

        app_state.profile_buttons[i] = btn;
    }

    /* ---- Battery ---- */
    GtkWidget *bat_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(bat_group), "Battery");
    gtk_box_append(GTK_BOX(content), bat_group);

    GtkWidget *bat_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(bat_row, 4);
    gtk_widget_set_margin_end(bat_row, 4);

    GtkWidget *bat_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(bat_text, TRUE);
    GtkWidget *bat_title = gtk_label_new("Stop charging at 80%");
    gtk_label_set_xalign(GTK_LABEL(bat_title), 0.0);
    gtk_box_append(GTK_BOX(bat_text), bat_title);

    app_state.battery_note = gtk_label_new(
        "Caps future charging. It will not discharge down to 80% on its own.");
    gtk_label_set_xalign(GTK_LABEL(app_state.battery_note), 0.0);
    gtk_widget_add_css_class(app_state.battery_note, "profile-desc");
    gtk_box_append(GTK_BOX(bat_text), app_state.battery_note);
    gtk_box_append(GTK_BOX(bat_row), bat_text);

    app_state.battery_switch = gtk_switch_new();
    gtk_widget_set_valign(app_state.battery_switch, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(app_state.battery_switch, battery_limit_supported());
    g_signal_connect(app_state.battery_switch, "notify::active",
                     G_CALLBACK(on_battery_toggled), NULL);
    gtk_box_append(GTK_BOX(bat_row), app_state.battery_switch);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(bat_group), bat_row);

    /* ---- Keyboard ---- */
    GtkWidget *kb_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(kb_group),
                                    "Keyboard Backlight");
    gtk_box_append(GTK_BOX(content), kb_group);

    if (!kb_supported()) {
        GtkWidget *no_kb = gtk_label_new("Keyboard zones are not available.");
        gtk_widget_add_css_class(no_kb, "fan-label");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(kb_group), no_kb);
    } else {
        GtkWidget *kb_flow = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(kb_flow), GTK_SELECTION_NONE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(kb_flow), TRUE);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(kb_flow), 8);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(kb_flow), 4);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(kb_flow), 8);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(kb_flow), 8);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(kb_group), kb_flow);

        for (int i = 0; i < (int)N_KB_COLOURS; i++) {
            GtkWidget *btn = gtk_button_new();
            gtk_widget_add_css_class(btn, "profile-btn");

            GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            gtk_widget_set_halign(col, GTK_ALIGN_CENTER);

            /* A filled swatch, so the colour is the label. */
            GtkWidget *swatch = gtk_drawing_area_new();
            gtk_widget_set_size_request(swatch, 34, 34);
            char css_class[32];
            snprintf(css_class, sizeof(css_class), "kb-%s", kb_colours[i].hex);
            gtk_widget_add_css_class(swatch, css_class);
            gtk_widget_add_css_class(swatch, "kb-swatch");
            gtk_box_append(GTK_BOX(col), swatch);

            GtkWidget *lbl = gtk_label_new(kb_colours[i].label);
            gtk_widget_add_css_class(lbl, "profile-desc");
            gtk_box_append(GTK_BOX(col), lbl);

            gtk_button_set_child(GTK_BUTTON(btn), col);
            g_signal_connect(btn, "clicked", G_CALLBACK(on_kb_colour_clicked),
                             (gpointer)kb_colours[i].hex);
            gtk_flow_box_append(GTK_FLOW_BOX(kb_flow), btn);
            app_state.kb_buttons[i] = btn;
        }

        /* ---- per zone ---- */
        GtkWidget *zone_hdr = gtk_label_new("Or set each zone separately");
        gtk_label_set_xalign(GTK_LABEL(zone_hdr), 0.0);
        gtk_widget_add_css_class(zone_hdr, "profile-desc");
        gtk_widget_set_margin_top(zone_hdr, 10);
        gtk_widget_set_margin_start(zone_hdr, 4);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(kb_group), zone_hdr);

        GtkWidget *zone_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(zone_row, 4);
        gtk_widget_set_margin_end(zone_row, 4);
        gtk_widget_set_margin_top(zone_row, 4);
        gtk_box_set_homogeneous(GTK_BOX(zone_row), TRUE);

        char cur_zones[4][8];
        int cur_bright = 100;
        int have_cur = kb_read_zones(cur_zones, &cur_bright) == 0;

        for (int i = 0; i < 4; i++) {
            GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

            char zlabel[16];
            snprintf(zlabel, sizeof(zlabel), "Zone %d", i + 1);
            GtkWidget *zl = gtk_label_new(zlabel);
            gtk_widget_add_css_class(zl, "profile-desc");
            gtk_box_append(GTK_BOX(cell), zl);

            GtkColorDialog *dialog = gtk_color_dialog_new();
            gtk_color_dialog_set_with_alpha(dialog, FALSE);
            GtkWidget *picker = gtk_color_dialog_button_new(dialog);

            if (have_cur) {
                GdkRGBA rgba;
                char spec[16];
                snprintf(spec, sizeof(spec), "#%s", cur_zones[i]);
                if (gdk_rgba_parse(&rgba, spec))
                    gtk_color_dialog_button_set_rgba(
                        GTK_COLOR_DIALOG_BUTTON(picker), &rgba);
            }

            g_signal_connect(picker, "notify::rgba",
                             G_CALLBACK(on_kb_zone_changed), NULL);
            gtk_box_append(GTK_BOX(cell), picker);
            app_state.kb_zone_pickers[i] = picker;

            gtk_box_append(GTK_BOX(zone_row), cell);
        }
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(kb_group), zone_row);

        /* ---- brightness ---- */
        GtkWidget *br_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(br_row, 4);
        gtk_widget_set_margin_end(br_row, 4);
        gtk_widget_set_margin_top(br_row, 6);

        GtkWidget *br_lbl = gtk_label_new("Brightness");
        gtk_label_set_width_chars(GTK_LABEL(br_lbl), 10);
        gtk_label_set_xalign(GTK_LABEL(br_lbl), 0.0);
        gtk_widget_add_css_class(br_lbl, "fan-label");
        gtk_box_append(GTK_BOX(br_row), br_lbl);

        app_state.kb_brightness =
            gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 10);
        gtk_widget_set_hexpand(app_state.kb_brightness, TRUE);
        gtk_scale_set_draw_value(GTK_SCALE(app_state.kb_brightness), TRUE);
        gtk_scale_set_value_pos(GTK_SCALE(app_state.kb_brightness), GTK_POS_RIGHT);
        gtk_range_set_value(GTK_RANGE(app_state.kb_brightness),
                            have_cur ? cur_bright : 100);
        g_signal_connect(app_state.kb_brightness, "value-changed",
                         G_CALLBACK(on_kb_brightness_changed), NULL);
        gtk_box_append(GTK_BOX(br_row), app_state.kb_brightness);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(kb_group), br_row);
    }

    /* ---- Status bar ---- */
    app_state.status_label = gtk_label_new("Starting...");
    gtk_widget_add_css_class(app_state.status_label, "status-bar");
    gtk_widget_set_halign(app_state.status_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root_box), app_state.status_label);

    /* ---- Refresh timer ---- */
    g_signal_connect(app_state.window, "destroy",
                     G_CALLBACK(on_window_destroy), NULL);
    app_state.tick_id = g_timeout_add(2000, refresh, NULL);
    g_idle_add(refresh_once, NULL);

    gtk_window_present(GTK_WINDOW(app_state.window));
}

/* -------------------------------------------------------------------------- */
/* Application activate                                                       */
/* -------------------------------------------------------------------------- */

static void on_activate(AdwApplication *app, gpointer user_data)
{
    (void)user_data;

    /* Load CSS */
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css_provider, CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    /* Dark theme */
    AdwStyleManager *style_mgr = adw_style_manager_get_default();
    adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_PREFER_DARK);

    build_window(app);
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    signal(SIGINT, SIG_DFL);
    discover_hwmon();

    AdwApplication *app = adw_application_new("com.predatortune.app",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
