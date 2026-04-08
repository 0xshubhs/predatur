/*
 * predatortune-helper - Privileged helper for PredatorTune
 * Called via pkexec for operations requiring root.
 *
 * Usage:
 *   predatortune-helper set-profile <profile>
 *   predatortune-helper set-fan-speed <cpu_pct> <gpu_pct>
 *   predatortune-helper set-fan-auto
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLATFORM_PROFILE "/sys/firmware/acpi/platform_profile"
#define FAN_SPEED_PATH   "/sys/kernel/predatortune/fan_speed"

static const char *valid_profiles[] = {
    "low-power", "quiet", "balanced", "balanced-performance", "performance"
};
#define N_VALID (sizeof(valid_profiles) / sizeof(valid_profiles[0]))

static int is_valid_profile(const char *name)
{
    for (int i = 0; i < (int)N_VALID; i++) {
        if (strcmp(valid_profiles[i], name) == 0)
            return 1;
    }
    return 0;
}

static int write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        return 1;
    }
    fputs(data, f);
    fclose(f);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <action> [args...]\n", argv[0]);
        return 1;
    }

    const char *action = argv[1];

    if (strcmp(action, "set-profile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Missing profile name\n");
            return 1;
        }
        if (!is_valid_profile(argv[2])) {
            fprintf(stderr, "Invalid profile: %s\n", argv[2]);
            return 1;
        }
        return write_file(PLATFORM_PROFILE, argv[2]);

    } else if (strcmp(action, "set-fan-speed") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s set-fan-speed <cpu_pct> <gpu_pct>\n", argv[0]);
            return 1;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%s,%s", argv[2], argv[3]);
        return write_file(FAN_SPEED_PATH, buf);

    } else if (strcmp(action, "set-fan-auto") == 0) {
        return write_file(FAN_SPEED_PATH, "0,0");

    } else {
        fprintf(stderr, "Unknown action: %s\n", action);
        return 1;
    }
}
