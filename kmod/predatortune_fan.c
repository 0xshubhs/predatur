/*
 * predatortune_fan.c - Minimal WMI fan speed control for Acer Predator
 *
 * Uses the same WMI methods as PredatorSense on Windows.
 * Based on reverse engineering from Linuwu-Sense project.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/wmi.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

#define WMID_GUID4 "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"

#define WMI_SET_FAN_BEHAVIOR  14
#define WMI_SET_FAN_SPEED     16

static struct kobject *fan_kobj;
static int cpu_fan_pct;
static int gpu_fan_pct;

static acpi_status wmi_gaming_call(u32 method_id, u64 input)
{
	struct acpi_buffer acpi_input = { sizeof(u64), &input };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	status = wmi_evaluate_method(WMID_GUID4, 0, method_id, &acpi_input, &result);
	kfree(result.pointer);
	return status;
}

static u64 fan_val_calc(int percentage, int fan_index)
{
	return (((percentage * 25600) / 100) & 0xFF00) + fan_index;
}

static int set_fan_speed(int cpu_pct, int gpu_pct)
{
	acpi_status status;

	if (cpu_pct == 0 && gpu_pct == 0) {
		/* Auto mode */
		status = wmi_gaming_call(WMI_SET_FAN_BEHAVIOR, 0x410009);
		if (ACPI_FAILURE(status))
			return -EIO;
	} else if (cpu_pct == 100 && gpu_pct == 100) {
		/* Max fan mode */
		status = wmi_gaming_call(WMI_SET_FAN_BEHAVIOR, 0x820009);
		if (ACPI_FAILURE(status))
			return -EIO;
	} else {
		/* Custom mode - set behavior to custom */
		status = wmi_gaming_call(WMI_SET_FAN_BEHAVIOR, 0xC30009);
		if (ACPI_FAILURE(status))
			return -EIO;
		/* Set CPU fan speed */
		status = wmi_gaming_call(WMI_SET_FAN_SPEED,
					fan_val_calc(cpu_pct, 1));
		if (ACPI_FAILURE(status))
			return -EIO;
		/* Set GPU fan speed */
		status = wmi_gaming_call(WMI_SET_FAN_SPEED,
					fan_val_calc(gpu_pct, 4));
		if (ACPI_FAILURE(status))
			return -EIO;
	}
	return 0;
}

static ssize_t fan_speed_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d,%d\n", cpu_fan_pct, gpu_fan_pct);
}

static ssize_t fan_speed_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	int cpu, gpu, ret;

	if (sscanf(buf, "%d,%d", &cpu, &gpu) != 2)
		return -EINVAL;
	if (cpu < 0 || cpu > 100 || gpu < 0 || gpu > 100)
		return -EINVAL;

	ret = set_fan_speed(cpu, gpu);
	if (ret)
		return ret;

	cpu_fan_pct = cpu;
	gpu_fan_pct = gpu;
	return count;
}

static struct kobj_attribute fan_speed_attr =
	__ATTR(fan_speed, 0666, fan_speed_show, fan_speed_store);

static int __init predatortune_fan_init(void)
{
	int ret;

	if (!wmi_has_guid(WMID_GUID4)) {
		pr_err("predatortune_fan: WMI GUID %s not found\n", WMID_GUID4);
		return -ENODEV;
	}

	fan_kobj = kobject_create_and_add("predatortune", kernel_kobj);
	if (!fan_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(fan_kobj, &fan_speed_attr.attr);
	if (ret) {
		kobject_put(fan_kobj);
		return ret;
	}

	pr_info("predatortune_fan: loaded. Control via /sys/kernel/predatortune/fan_speed\n");
	return 0;
}

static void __exit predatortune_fan_exit(void)
{
	/* Restore auto mode on unload */
	set_fan_speed(0, 0);
	sysfs_remove_file(fan_kobj, &fan_speed_attr.attr);
	kobject_put(fan_kobj);
	pr_info("predatortune_fan: unloaded, fans set to auto\n");
}

module_init(predatortune_fan_init);
module_exit(predatortune_fan_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PredatorTune");
MODULE_DESCRIPTION("WMI fan speed control for Acer Predator laptops");
