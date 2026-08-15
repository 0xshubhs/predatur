/*
 * predatortune-daemon - keeps the fans matched to temperature, unattended.
 *
 * Reads the CPU package temperature (and the dGPU's, when it is already
 * awake), maps the hotter of the two through the curve in
 * /etc/predatortune/fan.conf, and writes the result to the kernel module.
 *
 * Runs as a system service from boot, so the fans respond without anyone
 * opening the GUI or picking a mode.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Overridable at compile time so the loop can be exercised off real hardware. */
#ifndef FAN_SPEED_PATH
#define FAN_SPEED_PATH "/sys/devices/platform/acer-wmi/predator_sense/fan_speed"
#endif
#ifndef CONFIG_PATH
#define CONFIG_PATH    "/etc/predatortune/fan.conf"
#endif
#ifndef MANUAL_FLAG
#define MANUAL_FLAG    "/run/predatortune/manual"
#endif

#define MAX_POINTS 24
#define NO_READING (-1000)

struct point {
	int temp;
	int cpu;
	int gpu;
};

static struct point curve[MAX_POINTS];
static int n_points;

static int poll_seconds = 3;
static int hysteresis   = 4;
static int min_percent  = 20;

static volatile sig_atomic_t running   = 1;
static volatile sig_atomic_t reload_me = 0;

static void on_term(int sig) { (void)sig; running = 0; }
static void on_hup(int sig)  { (void)sig; reload_me = 1; }

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

static void add_point(int temp, int cpu, int gpu)
{
	if (n_points >= MAX_POINTS)
		return;
	if (cpu < 0) cpu = 0;
	if (cpu > 100) cpu = 100;
	if (gpu < 0) gpu = 0;
	if (gpu > 100) gpu = 100;
	curve[n_points].temp = temp;
	curve[n_points].cpu = cpu;
	curve[n_points].gpu = gpu;
	n_points++;
}

static void default_curve(void)
{
	n_points = 0;
	add_point(55, 25, 25);
	add_point(65, 40, 40);
	add_point(75, 60, 60);
	add_point(83, 80, 80);
	add_point(90, 100, 100);
}

static int cmp_points(const void *a, const void *b)
{
	return ((const struct point *)a)->temp - ((const struct point *)b)->temp;
}

static void load_config(void)
{
	FILE *f = fopen(CONFIG_PATH, "r");
	char line[256];

	n_points = 0;

	if (!f) {
		fprintf(stderr, "no %s, using the built-in curve\n", CONFIG_PATH);
		default_curve();
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		int t, c, g, v;

		while (isspace((unsigned char)*p))
			p++;
		if (*p == '#' || *p == '\0')
			continue;

		if (sscanf(p, "curve %d %d %d", &t, &c, &g) == 3)
			add_point(t, c, g);
		else if (sscanf(p, "poll_seconds %d", &v) == 1)
			poll_seconds = v > 0 ? v : 1;
		else if (sscanf(p, "hysteresis_c %d", &v) == 1)
			hysteresis = v >= 0 ? v : 0;
		else if (sscanf(p, "min_percent %d", &v) == 1)
			min_percent = (v >= 0 && v <= 100) ? v : 20;
	}
	fclose(f);

	if (n_points == 0) {
		fprintf(stderr, "%s has no curve points, using the built-in curve\n",
			CONFIG_PATH);
		default_curve();
		return;
	}

	/* Points may be written in any order; the lookup below needs them sorted. */
	qsort(curve, n_points, sizeof(curve[0]), cmp_points);
}

/* ------------------------------------------------------------------ */
/* sensors                                                             */
/* ------------------------------------------------------------------ */

static int read_int_file(const char *path, int *out)
{
	FILE *f = fopen(path, "r");
	int ok;

	if (!f)
		return 0;
	ok = fscanf(f, "%d", out) == 1;
	fclose(f);
	return ok;
}

static int read_str_file(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	size_t len;

	if (!f)
		return 0;
	if (!fgets(buf, (int)n, f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	len = strlen(buf);
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
		buf[--len] = '\0';
	return 1;
}

/*
 * hwmon numbering is not stable across boots, so find coretemp by name
 * rather than hardcoding a path, and prefer the package sensor over an
 * individual core.
 */
static int find_cpu_sensor(char *out, size_t n)
{
	DIR *d = opendir("/sys/class/hwmon");
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;

	while (!found && (e = readdir(d))) {
		char base[320], path[420], name[64];
		int i;

		if (strncmp(e->d_name, "hwmon", 5) != 0)
			continue;

		snprintf(base, sizeof(base), "/sys/class/hwmon/%s", e->d_name);
		snprintf(path, sizeof(path), "%s/name", base);
		if (!read_str_file(path, name, sizeof(name)))
			continue;
		if (strcmp(name, "coretemp") != 0)
			continue;

		for (i = 1; i <= 40; i++) {
			char label[64];

			snprintf(path, sizeof(path), "%s/temp%d_label", base, i);
			if (!read_str_file(path, label, sizeof(label)))
				continue;
			if (strstr(label, "Package id 0")) {
				snprintf(out, n, "%s/temp%d_input", base, i);
				found = 1;
				break;
			}
		}
		if (!found) {
			snprintf(out, n, "%s/temp1_input", base);
			found = 1;
		}
	}

	closedir(d);
	return found;
}

/* Locate the discrete NVIDIA GPU so its power state can be checked. */
static int find_gpu_device(char *out, size_t n)
{
	DIR *d = opendir("/sys/bus/pci/devices");
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;

	while (!found && (e = readdir(d))) {
		char base[320], path[420], val[32];

		if (e->d_name[0] == '.')
			continue;

		snprintf(base, sizeof(base), "/sys/bus/pci/devices/%s", e->d_name);
		snprintf(path, sizeof(path), "%s/vendor", base);
		if (!read_str_file(path, val, sizeof(val)) || strcmp(val, "0x10de"))
			continue;
		snprintf(path, sizeof(path), "%s/class", base);
		if (!read_str_file(path, val, sizeof(val)) ||
		    strncmp(val, "0x0300", 6))
			continue;

		snprintf(out, n, "%s", base);
		found = 1;
	}

	closedir(d);
	return found;
}

/*
 * Only ask the GPU its temperature when it is already powered up. Querying a
 * runtime-suspended dGPU wakes it, which would cost battery for a reading we
 * know would be cold anyway.
 */
static int read_gpu_temp(const char *gpu_dev)
{
	char path[420], status[32], line[64];
	FILE *p;
	int temp;

	if (!gpu_dev[0])
		return NO_READING;

	snprintf(path, sizeof(path), "%s/power/runtime_status", gpu_dev);
	if (read_str_file(path, status, sizeof(status)) &&
	    strcmp(status, "active") != 0)
		return NO_READING;

	p = popen("nvidia-smi --query-gpu=temperature.gpu "
		  "--format=csv,noheader 2>/dev/null", "r");
	if (!p)
		return NO_READING;
	if (!fgets(line, sizeof(line), p)) {
		pclose(p);
		return NO_READING;
	}
	pclose(p);

	if (sscanf(line, "%d", &temp) != 1)
		return NO_READING;
	return temp;
}

/* ------------------------------------------------------------------ */
/* curve                                                               */
/* ------------------------------------------------------------------ */

/*
 * Linear interpolation between the surrounding points, so the fan ramps
 * smoothly rather than stepping between a handful of fixed speeds.
 * Below the first point the firmware's own idle handling is better than
 * anything we can do, so hand back 0 and let it run the fans quietly.
 */
static void curve_lookup(int temp, int *cpu, int *gpu)
{
	int i;

	if (temp < curve[0].temp) {
		*cpu = 0;
		*gpu = 0;
		return;
	}

	for (i = 0; i < n_points - 1; i++) {
		const struct point *a = &curve[i];
		const struct point *b = &curve[i + 1];
		int span;

		if (temp > b->temp)
			continue;

		span = b->temp - a->temp;
		if (span <= 0) {
			*cpu = b->cpu;
			*gpu = b->gpu;
			return;
		}
		*cpu = a->cpu + (b->cpu - a->cpu) * (temp - a->temp) / span;
		*gpu = a->gpu + (b->gpu - a->gpu) * (temp - a->temp) / span;
		return;
	}

	*cpu = curve[n_points - 1].cpu;
	*gpu = curve[n_points - 1].gpu;
}

static int apply(int cpu, int gpu)
{
	FILE *f = fopen(FAN_SPEED_PATH, "w");

	if (!f)
		return 0;
	fprintf(f, "%d,%d", cpu, gpu);
	if (fclose(f) != 0)
		return 0;
	return 1;
}

static int file_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

/* ------------------------------------------------------------------ */

/* Print the loaded curve across a temperature sweep and exit. */
static int selftest(void)
{
	int t, i;

	load_config();
	printf("curve from %s (%d points, hysteresis %dC, floor %d%%)\n",
	       CONFIG_PATH, n_points, hysteresis, min_percent);
	for (i = 0; i < n_points; i++)
		printf("  point  %3dC -> %3d%% / %3d%%\n",
		       curve[i].temp, curve[i].cpu, curve[i].gpu);

	printf("\n  temp   cpu   gpu   mode\n");
	for (t = 40; t <= 100; t += 5) {
		int cpu, gpu;

		curve_lookup(t, &cpu, &gpu);
		if (cpu || gpu) {
			if (cpu < min_percent) cpu = min_percent;
			if (gpu < min_percent) gpu = min_percent;
		}
		printf("  %3dC  %3d%%  %3d%%   %s\n", t, cpu, gpu,
		       (!cpu && !gpu) ? "firmware auto"
		       : (cpu == 100 && gpu == 100) ? "max" : "custom");
	}
	return 0;
}

int main(int argc, char *argv[])
{
	char cpu_path[512] = "";
	char gpu_dev[320] = "";
	int last_cpu = -1, last_gpu = -1;
	int governing = NO_READING;
	int waited = 0;

	if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
		return selftest();
	(void)argv;

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGHUP, on_hup);

	load_config();

	if (!find_cpu_sensor(cpu_path, sizeof(cpu_path))) {
		fprintf(stderr, "no coretemp sensor found, nothing to control on\n");
		return 1;
	}
	fprintf(stderr, "cpu sensor: %s\n", cpu_path);

	if (find_gpu_device(gpu_dev, sizeof(gpu_dev)))
		fprintf(stderr, "gpu: %s\n", gpu_dev);

	/*
	 * The module is loaded by systemd-modules-load at about the same time
	 * we start, so its sysfs file may not be there for the first moment.
	 * Wait for it rather than exiting and leaving the fans unmanaged.
	 */
	while (running && !file_exists(FAN_SPEED_PATH)) {
		if (!waited) {
			fprintf(stderr, "waiting for %s (is predatortune_fan loaded?)\n",
				FAN_SPEED_PATH);
			waited = 1;
		}
		sleep(2);
	}
	if (waited && running)
		fprintf(stderr, "%s appeared, taking over\n", FAN_SPEED_PATH);

	while (running) {
		int raw, cpu_temp, gpu_temp, hottest, cpu, gpu;

		if (reload_me) {
			reload_me = 0;
			load_config();
			last_cpu = last_gpu = -1;
			governing = NO_READING;
			fprintf(stderr, "config reloaded\n");
		}

		/* The GUI can take the wheel; do not fight it. */
		if (file_exists(MANUAL_FLAG)) {
			last_cpu = last_gpu = -1;
			governing = NO_READING;
			sleep(poll_seconds);
			continue;
		}

		if (!read_int_file(cpu_path, &raw)) {
			/* hwmon can disappear on suspend/resume; re-find it. */
			if (!find_cpu_sensor(cpu_path, sizeof(cpu_path))) {
				sleep(poll_seconds);
				continue;
			}
			if (!read_int_file(cpu_path, &raw)) {
				sleep(poll_seconds);
				continue;
			}
		}
		cpu_temp = raw / 1000;

		gpu_temp = read_gpu_temp(gpu_dev);
		hottest = (gpu_temp != NO_READING && gpu_temp > cpu_temp)
			? gpu_temp : cpu_temp;

		/*
		 * Hysteresis: follow a rise immediately, but only follow a fall
		 * once it is clearly a fall. Otherwise a couple of degrees of
		 * noise makes the fans surge up and down audibly.
		 */
		if (governing == NO_READING || hottest > governing ||
		    hottest < governing - hysteresis)
			governing = hottest;

		curve_lookup(governing, &cpu, &gpu);

		/*
		 * 0,0 means "hand back to firmware auto" and 100,100 means
		 * "max". Anything in between is custom mode, where a very low
		 * number would nearly stop a fan, so hold a floor there.
		 */
		if (cpu || gpu) {
			if (cpu < min_percent) cpu = min_percent;
			if (gpu < min_percent) gpu = min_percent;
		}

		if (cpu != last_cpu || gpu != last_gpu) {
			if (apply(cpu, gpu)) {
				fprintf(stderr, "%dC -> cpu %d%% gpu %d%%\n",
					governing, cpu, gpu);
				last_cpu = cpu;
				last_gpu = gpu;
			} else if (!file_exists(FAN_SPEED_PATH)) {
				fprintf(stderr, "%s went away, waiting for it\n",
					FAN_SPEED_PATH);
				while (running && !file_exists(FAN_SPEED_PATH))
					sleep(2);
				last_cpu = last_gpu = -1;
			}
		}

		sleep(poll_seconds);
	}

	/* Leave the machine as we found it: firmware back in charge. */
	if (file_exists(FAN_SPEED_PATH)) {
		apply(0, 0);
		fprintf(stderr, "stopping, fans returned to auto\n");
	}
	return 0;
}
