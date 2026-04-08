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
#define FAN_SPEED_SYSFS          "/sys/kernel/predatortune/fan_speed"

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

static void set_profile(const char *name)
{
    if (access(HELPER_PATH, X_OK) == 0) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "pkexec %s set-profile %s", HELPER_PATH, name);
        /* fire and forget */
        if (system(cmd)) { /* ignore */ }
    } else {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "pkexec bash -c 'echo %s > %s'", name, PLATFORM_PROFILE);
        if (system(cmd)) { /* ignore */ }
    }
}

static void set_fan_speed(int cpu_pct, int gpu_pct)
{
    FILE *f = fopen(FAN_SPEED_SYSFS, "w");
    if (f) {
        fprintf(f, "%d,%d", cpu_pct, gpu_pct);
        fclose(f);
    } else {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "pkexec %s set-fan-speed %d %d",
                 HELPER_PATH, cpu_pct, gpu_pct);
        if (system(cmd)) { /* ignore */ }
    }
}

static int fan_module_loaded(void)
{
    return access(FAN_SPEED_SYSFS, F_OK) == 0;
}

/* -------------------------------------------------------------------------- */
/* CSS                                                                        */
/* -------------------------------------------------------------------------- */

static const char *CSS =
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
        GtkWidget *no_mod = gtk_label_new(
            "Kernel module not loaded. Run: sudo insmod predatortune_fan.ko");
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

    /* ---- Status bar ---- */
    app_state.status_label = gtk_label_new("Starting...");
    gtk_widget_add_css_class(app_state.status_label, "status-bar");
    gtk_widget_set_halign(app_state.status_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root_box), app_state.status_label);

    /* ---- Refresh timer ---- */
    app_state.tick_id = g_timeout_add(2000, refresh, NULL);
    g_idle_add(refresh, NULL);

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
