// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Small userspace service for the uniwill-laptop driver.
 *
 * The service exposes the driver's sysfs interfaces through a JSON-lines
 * Unix socket. Fan control remains with the EC unless explicitly enabled.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <blockdev/blockdev.h>
#include <blockdev/fs.h>
#include <blockdev/nvme.h>
#include <blockdev/smart.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET_PATH "/run/uniwilld.sock"
#define DEFAULT_STATE_PATH "/var/lib/uniwilld/state.conf"
#define DEFAULT_INTERVAL_MS 2000
#define HWMON_STARTUP_WAIT_MS 120000
#define HWMON_STARTUP_POLL_MS 250
#define CPUFREQ_POLICY_PATH "/sys/devices/system/cpu/cpufreq"
#define CPU_MIN_FREQUENCY_KHZ 800000
#define MAX_FAN_CONTROL_INTERVAL_MS 3000
#define PLATFORM_REFRESH_BATCH 3
#define POWER_PROFILES_BUS_NAME "org.freedesktop.UPower.PowerProfiles"
#define POWER_PROFILES_OBJECT_PATH "/org/freedesktop/UPower/PowerProfiles"
#define POWER_PROFILES_INTERFACE "org.freedesktop.UPower.PowerProfiles"
#define MAX_ENDPOINTS 128
#define MAX_LINE 8192
#define MAX_RESPONSE 65536
#define MAX_CURVE_POINTS 16
#define PWM_MAX 255
#define PWM_MIN_ON 64
#define CURVE_FAILSAFE_TEMP_C 100
#define PROFILE_COUNT 3
#define POWER_SOURCE_COUNT 2
#define FAN_MODE_PERFORMANCE 1
#define FAN_MODE_STANDARD 2
#define FAN_MODE_QUIET 3
#define FAN_MODE_WHISPER 4
#define FAN_MODE_BENCHMARK 5
#define TOUCHPAD_I2C_NAME "i2c-UNIW0001:00"
#define LIGHTBAR_CONFIG_MAX 255
#define KEYBOARD_BRIGHTNESS_MAX 50
#define KEYBOARD_EFFECT_SPEED_MAX 10
#define MAX_DMI_RAW 4096

#ifndef UNIWILLD_VERSION
#define UNIWILLD_VERSION "0.0.0"
#endif
#ifndef UNIWILLD_BUILD_NUMBER
#define UNIWILLD_BUILD_NUMBER "0"
#endif

static const unsigned char touchpad_selective_reporting_marker[] = {
	0x05, 0x0d, 0x09, 0x22, 0xa1, 0x00, 0x09, 0x57, 0x09, 0x58,
};

enum power_source {
	POWER_SOURCE_BATTERY = 0,
	POWER_SOURCE_AC = 1,
};

enum keyboard_effect {
	KEYBOARD_EFFECT_SOLID = 0,
	KEYBOARD_EFFECT_BREATHING,
	KEYBOARD_EFFECT_WAVE,
	KEYBOARD_EFFECT_RANDOM,
	KEYBOARD_EFFECT_RAINBOW,
	KEYBOARD_EFFECT_RIPPLE,
	KEYBOARD_EFFECT_MARQUEE,
	KEYBOARD_EFFECT_RAINDROP,
	KEYBOARD_EFFECT_AURORA,
	KEYBOARD_EFFECT_FIREWORKS,
	KEYBOARD_EFFECT_COUNT,
};

struct profile_branch {
	int power_mode;
	int fan_mode;
};

struct profile_slot {
	struct profile_branch branch[POWER_SOURCE_COUNT];
};

struct endpoint {
	char name[64];
	char path[PATH_MAX];
	char value[MAX_LINE];
	int read_err;
	bool writable;
};

struct curve_point {
	int temp_c;
	int pwm;
};

struct fan_curve {
	char name[16];
	struct curve_point points[MAX_CURVE_POINTS];
	size_t count;
};

struct source_control_state {
	struct fan_curve cpu_curve;
	struct fan_curve gpu_curve;
	bool cpu_curve_valid;
	bool gpu_curve_valid;
	int fan_mode;
	bool fan_mode_valid;
	bool passive_cooling;
	bool passive_cooling_valid;
};

struct lightbar_source_state {
	bool enabled;
	int brightness;
	int red;
	int green;
	int blue;
	bool rainbow;
	bool breathing;
};

struct keyboard_light_source_state {
	bool enabled;
	int brightness;
	int red;
	int green;
	int blue;
	int effect;
	int speed;
	int direction;
	bool reactive;
};

struct uniwilld {
	char socket_path[PATH_MAX];
	char state_path[PATH_MAX];
	char socket_group[64];
	mode_t socket_mode;
	char hwmon_path[PATH_MAX];
	char platform_path[PATH_MAX];
	struct endpoint endpoints[MAX_ENDPOINTS];
	size_t endpoint_count;
	struct fan_curve cpu_curve;
	struct fan_curve gpu_curve;
	struct source_control_state source_controls[POWER_SOURCE_COUNT];
	int active_curve_mode;
	int last_cpu_pwm;
	int last_gpu_pwm;
	int interval_ms;
	bool fan_curve_control_enabled;
	bool fan_boost_restore_valid;
	bool fan_boost_restore_curve_control;
	struct profile_slot profiles[PROFILE_COUNT];
	bool lightbar_enabled;
	struct lightbar_source_state lightbar[POWER_SOURCE_COUNT];
	int last_applied_lightbar_source;
	bool keyboard_light_enabled;
	struct keyboard_light_source_state keyboard_light[POWER_SOURCE_COUNT];
	int last_applied_keyboard_light_source;
	int active_profile;
	int active_power_source;
	int last_applied_profile;
	int last_applied_power_source;
	int last_applied_fan_mode;
	int last_synced_power_profile;
	char system_power_mode[64];
	int system_power_mode_err;
	pthread_rwlock_t state_lock;
	pthread_mutex_t hardware_lock;
	pthread_mutex_t profile_apply_lock;
	pthread_mutex_t client_lock;
	pthread_cond_t client_cond;
	size_t client_count;
	pthread_mutex_t event_lock;
	unsigned long long event_revision;
	bool once;
	pthread_mutex_t fan_control_lock;
};

static volatile sig_atomic_t stop_requested;
static pthread_once_t storage_library_once = PTHREAD_ONCE_INIT;
static bool nvme_library_available;
static bool ata_smart_library_available;
static bool filesystem_library_available;

struct storage_space_info {
	bool available;
	guint64 free_bytes;
	unsigned int free_percent;
};

static int dbus_read_system_power_mode(char *mode, size_t mode_size);
static int endpoint_read(struct uniwilld *svc, const char *name, char *value, size_t size);
static int endpoint_write_int(struct uniwilld *svc, const char *name, int value);
static int apply_active_profile(struct uniwilld *svc, bool force);
static const char *power_source_name(int source);
static int json_get_string(const char *json, const char *key, char *out, size_t out_size);
static void import_current_lightbar_state(struct uniwilld *svc);
static void import_current_keyboard_light_state(struct uniwilld *svc);

static unsigned long long state_event_revision(struct uniwilld *svc)
{
	unsigned long long revision;

	pthread_mutex_lock(&svc->event_lock);
	revision = svc->event_revision;
	pthread_mutex_unlock(&svc->event_lock);
	return revision;
}

static void publish_state_event(struct uniwilld *svc)
{
	pthread_mutex_lock(&svc->event_lock);
	svc->event_revision++;
	pthread_mutex_unlock(&svc->event_lock);
}

static bool command_changes_state(const char *req)
{
	char cmd[64];

	if (json_get_string(req, "cmd", cmd, sizeof(cmd)) < 0)
		return false;

	return !strncmp(cmd, "set", 3) ||
		!strncmp(cmd, "enable", 6) ||
		!strncmp(cmd, "disable", 7) ||
		!strncmp(cmd, "sync", 4) ||
		!strcmp(cmd, "activate_profile") ||
		!strcmp(cmd, "reload") ||
		!strcmp(cmd, "tick");
}

static void handle_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static bool path_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

static void init_storage_libraries(void)
{
	BDPluginSpec nvme = {
		.name = BD_PLUGIN_NVME,
		.so_name = NULL,
	};
	BDPluginSpec smart = {
		.name = BD_PLUGIN_SMART,
		.so_name = NULL,
	};
	BDPluginSpec fs = {
		.name = BD_PLUGIN_FS,
		.so_name = NULL,
	};
	BDPluginSpec *plugins[] = { &nvme, &smart, &fs, NULL };
	GError *error = NULL;

	if (!bd_ensure_init(plugins, NULL, &error)) {
		if (error)
			g_error_free(error);
		nvme_library_available = false;
		ata_smart_library_available = false;
		filesystem_library_available = false;
		return;
	}

	nvme_library_available = bd_is_plugin_available(BD_PLUGIN_NVME);
	ata_smart_library_available = bd_is_plugin_available(BD_PLUGIN_SMART);
	filesystem_library_available = bd_is_plugin_available(BD_PLUGIN_FS);
}

static int touchpad_report_id_from_descriptor(const unsigned char *descriptor, size_t size)
{
	for (size_t i = 0;
	     i + sizeof(touchpad_selective_reporting_marker) + 1 < size; i++) {
		if (memcmp(&descriptor[i], touchpad_selective_reporting_marker,
		           sizeof(touchpad_selective_reporting_marker)))
			continue;

		for (size_t j = i + sizeof(touchpad_selective_reporting_marker);
		     j + 1 < size; j++) {
			if (descriptor[j] == 0x85)
				return descriptor[j + 1];
			if (descriptor[j] == 0xc0)
				break;
		}
	}

	return -ENOENT;
}

static bool is_uniwill_touchpad_hidraw(const char *hidraw_sysfs)
{
	char device_link[PATH_MAX];
	char resolved[PATH_MAX];

	if (snprintf(device_link, sizeof(device_link), "%s/device", hidraw_sysfs) >=
	    (int)sizeof(device_link) || !realpath(device_link, resolved))
		return false;

	return strstr(resolved, "/" TOUCHPAD_I2C_NAME "/") != NULL;
}

static int read_touchpad_report_id(const char *hidraw_sysfs)
{
	unsigned char descriptor[HID_MAX_DESCRIPTOR_SIZE];
	char descriptor_path[PATH_MAX];
	ssize_t size;
	int fd;

	if (snprintf(descriptor_path, sizeof(descriptor_path),
	             "%s/device/report_descriptor", hidraw_sysfs) >=
	    (int)sizeof(descriptor_path))
		return -ENAMETOOLONG;

	fd = open(descriptor_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	size = read(fd, descriptor, sizeof(descriptor));
	if (size < 0) {
		int err = -errno;

		close(fd);
		return err;
	}
	close(fd);
	return touchpad_report_id_from_descriptor(descriptor, (size_t)size);
}

static int set_touchpad_hid_state(bool enabled)
{
	glob_t matches = { 0 };
	int last_err = -ENOENT;
	int found = 0;

	if (glob("/sys/class/hidraw/hidraw*", 0, NULL, &matches) != 0)
		return -ENOENT;

	for (size_t i = 0; i < matches.gl_pathc; i++) {
		const char *base;
		unsigned char report[2];
		char devnode[PATH_MAX];
		int report_id;
		int fd;

		if (!is_uniwill_touchpad_hidraw(matches.gl_pathv[i]))
			continue;
		found++;

		base = strrchr(matches.gl_pathv[i], '/');
		if (!base || snprintf(devnode, sizeof(devnode), "/dev/%s", base + 1) >=
		             (int)sizeof(devnode)) {
			last_err = -ENAMETOOLONG;
			continue;
		}

		report_id = read_touchpad_report_id(matches.gl_pathv[i]);
		if (report_id < 0) {
			last_err = report_id;
			continue;
		}

		fd = open(devnode, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			last_err = -errno;
			continue;
		}

		report[0] = (unsigned char)report_id;
		report[1] = enabled ? 0x03 : 0x00;
		if (ioctl(fd, HIDIOCSFEATURE(sizeof(report)), report) < 0)
			last_err = -errno;
		else
			last_err = 0;
		close(fd);
	}

	globfree(&matches);
	return found ? last_err : -ENOENT;
}

static int read_text(const char *path, char *buf, size_t size)
{
	int fd;
	ssize_t len;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	len = read(fd, buf, size - 1);
	if (len < 0) {
		int err = -errno;

		close(fd);
		return err;
	}

	close(fd);
	buf[len] = '\0';
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	return 0;
}

static int write_text(const char *path, const char *value)
{
	int fd;
	ssize_t len;

	fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	len = write(fd, value, strlen(value));
	if (len < 0) {
		int err = -errno;

		close(fd);
		return err;
	}

	close(fd);
	return 0;
}

static int write_int(const char *path, int value)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%d\n", value);
	return write_text(path, buf);
}

static bool file_writable(const char *path)
{
	return access(path, W_OK) == 0;
}

static bool file_readable(const char *path)
{
	return access(path, R_OK) == 0;
}

static int join_path(char *dst, size_t size, const char *base, const char *name)
{
	size_t base_len = strlen(base);
	size_t name_len = strlen(name);

	if (base_len + 1 + name_len + 1 > size) {
		if (size)
			dst[0] = '\0';
		return -ENAMETOOLONG;
	}

	memcpy(dst, base, base_len);
	dst[base_len] = '/';
	memcpy(dst + base_len + 1, name, name_len);
	dst[base_len + 1 + name_len] = '\0';
	return 0;
}

static int add_endpoint(struct uniwilld *svc, const char *name, const char *path)
{
	struct endpoint *ep;

	if (!file_readable(path) || svc->endpoint_count >= MAX_ENDPOINTS)
		return 0;

	ep = &svc->endpoints[svc->endpoint_count++];
	snprintf(ep->name, sizeof(ep->name), "%s", name);
	snprintf(ep->path, sizeof(ep->path), "%s", path);
	ep->writable = file_writable(path);
	return 0;
}

static bool has_prefix(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void add_platform_endpoints(struct uniwilld *svc)
{
	static const char * const names[] = {
		"fn_lock_toggle_enable",
		"super_key_toggle_enable",
		"touchpad_toggle_enable",
		"rainbow_animation",
		"breathing_in_suspend",
		"ctgp_offset",
		"db_offset",
		"pl1_setting",
		"pl2_setting",
		"pl4_setting",
		"performance_profile",
		"hardware_power_mode",
		"fan_mode",
		"fan_boost",
		"passive_cooling",
	};
	char path[PATH_MAX];
	size_t i;

	if (!svc->platform_path[0])
		return;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		join_path(path, sizeof(path), svc->platform_path, names[i]);
		add_endpoint(svc, names[i], path);
	}
}

static void add_hwmon_endpoints(struct uniwilld *svc)
{
	static const char * const names[] = {
		"temp1_input",
		"temp1_label",
		"temp2_input",
		"temp2_label",
		"fan1_input",
		"fan1_label",
		"fan2_input",
		"fan2_label",
		"pwm1",
		"pwm1_enable",
		"pwm2",
		"pwm2_enable",
	};
	char path[PATH_MAX];
	size_t i;

	if (!svc->hwmon_path[0])
		return;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		join_path(path, sizeof(path), svc->hwmon_path, names[i]);
		add_endpoint(svc, names[i], path);
	}
}

static void add_power_supply_endpoints(struct uniwilld *svc)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir("/sys/class/power_supply");
	if (!dir)
		return;

	while ((de = readdir(dir))) {
		char base[PATH_MAX];
		char type_path[PATH_MAX];
		char type[64];
		char attr_path[PATH_MAX];
		char name[64];

		if (de->d_name[0] == '.')
			continue;

		join_path(base, sizeof(base), "/sys/class/power_supply", de->d_name);
		join_path(type_path, sizeof(type_path), base, "type");
		if (read_text(type_path, type, sizeof(type)) < 0 || strcmp(type, "Battery"))
			continue;

		join_path(attr_path, sizeof(attr_path), base, "charge_control_end_threshold");
		snprintf(name, sizeof(name), "%.24s_charge_control_end_threshold", de->d_name);
		add_endpoint(svc, name, attr_path);

		join_path(attr_path, sizeof(attr_path), base, "health");
		snprintf(name, sizeof(name), "%.48s_health", de->d_name);
		add_endpoint(svc, name, attr_path);
	}

	closedir(dir);
}

static void add_lightbar_endpoints(struct uniwilld *svc)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir("/sys/class/leds");
	if (!dir)
		return;

	while ((de = readdir(dir))) {
		char base[PATH_MAX];
		char path[PATH_MAX];

		if (strncmp(de->d_name, "uniwill:", 8))
			continue;

		join_path(base, sizeof(base), "/sys/class/leds", de->d_name);
		join_path(path, sizeof(path), base, "brightness");
		add_endpoint(svc, "lightbar_brightness", path);
		join_path(path, sizeof(path), base, "multi_intensity");
		add_endpoint(svc, "lightbar_multi_intensity", path);
		join_path(path, sizeof(path), base, "multi_index");
		add_endpoint(svc, "lightbar_multi_index", path);
		join_path(path, sizeof(path), base, "max_brightness");
		add_endpoint(svc, "lightbar_max_brightness", path);
	}

	closedir(dir);
}

static void add_keyboard_backlight_endpoints(struct uniwilld *svc)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir("/sys/class/leds");
	if (!dir)
		return;

	while ((de = readdir(dir))) {
		char base[PATH_MAX];
		char path[PATH_MAX];

		if (!strstr(de->d_name, ":kbd_backlight"))
			continue;

		join_path(base, sizeof(base), "/sys/class/leds", de->d_name);
		join_path(path, sizeof(path), base, "brightness");
		if (!file_readable(path))
			continue;

		add_endpoint(svc, "keyboard_backlight_brightness", path);
		join_path(path, sizeof(path), base, "max_brightness");
		add_endpoint(svc, "keyboard_backlight_max_brightness", path);
		join_path(path, sizeof(path), base, "color");
		if (file_readable(path))
			add_endpoint(svc, "keyboard_backlight_color", path);
		join_path(path, sizeof(path), base, "multi_intensity");
		if (file_readable(path))
			add_endpoint(svc, "keyboard_backlight_multi_intensity", path);
		join_path(path, sizeof(path), base, "effect");
		if (file_readable(path))
			add_endpoint(svc, "keyboard_backlight_effect", path);
		join_path(path, sizeof(path), base, "effect_speed");
		if (file_readable(path))
			add_endpoint(svc, "keyboard_backlight_effect_speed", path);
		join_path(path, sizeof(path), base, "effect_direction");
		if (file_readable(path))
			add_endpoint(svc, "keyboard_backlight_effect_direction", path);
		join_path(path, sizeof(path), base, "effect_reactive");
		if (file_readable(path))
			add_endpoint(svc, "keyboard_backlight_effect_reactive", path);
		join_path(path, sizeof(path), base, "effect_color");
		if (file_readable(path))
			add_endpoint(svc, "keyboard_backlight_effect_color", path);
		break;
	}

	closedir(dir);
}

static int discover_hwmon(struct uniwilld *svc)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir("/sys/class/hwmon");
	if (!dir)
		return -errno;

	while ((de = readdir(dir))) {
		char base[PATH_MAX];
		char name_path[PATH_MAX];
		char name[64];

		if (!has_prefix(de->d_name, "hwmon"))
			continue;

		join_path(base, sizeof(base), "/sys/class/hwmon", de->d_name);
		join_path(name_path, sizeof(name_path), base, "name");
		if (read_text(name_path, name, sizeof(name)) == 0 && !strcmp(name, "uniwill")) {
			snprintf(svc->hwmon_path, sizeof(svc->hwmon_path), "%s", base);
			closedir(dir);
			return 0;
		}
	}

	closedir(dir);
	return -ENOENT;
}

static int wait_for_hwmon(struct uniwilld *svc)
{
	int ret;
	unsigned int waited_ms = 0;

	for (;;) {
		ret = discover_hwmon(svc);
		if (ret == 0) {
			if (waited_ms)
				fprintf(stderr, "uniwilld: uniwill hwmon device became available after %u ms\n",
					waited_ms);
			return 0;
		}
		if (ret != -ENOENT || waited_ms >= HWMON_STARTUP_WAIT_MS)
			return ret;

		if (!waited_ms)
			fprintf(stderr, "uniwilld: waiting for the uniwill hwmon device\n");
		usleep(HWMON_STARTUP_POLL_MS * 1000);
		waited_ms += HWMON_STARTUP_POLL_MS;
	}
}

static int discover_platform(struct uniwilld *svc)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir("/sys/bus/platform/devices");
	if (!dir)
		return -errno;

	while ((de = readdir(dir))) {
		char base[PATH_MAX];
		char probe[PATH_MAX];

		if (!has_prefix(de->d_name, "INOU0000:"))
			continue;

		join_path(base, sizeof(base), "/sys/bus/platform/devices", de->d_name);
		join_path(probe, sizeof(probe), base, "ec_debug_dump");
		if (path_exists(probe)) {
			snprintf(svc->platform_path, sizeof(svc->platform_path), "%s", base);
			closedir(dir);
			return 0;
		}
	}

	closedir(dir);
	return -ENOENT;
}

static struct endpoint *find_endpoint(struct uniwilld *svc, const char *name)
{
	size_t i;

	for (i = 0; i < svc->endpoint_count; i++) {
		if (!strcmp(svc->endpoints[i].name, name))
			return &svc->endpoints[i];
	}

	return NULL;
}

static int refresh_endpoint_cache_by_name(struct uniwilld *svc, const char *name)
{
	char path[PATH_MAX];
	char value[MAX_LINE] = "";
	int err;

	pthread_rwlock_rdlock(&svc->state_lock);
	struct endpoint *ep = find_endpoint(svc, name);
	if (!ep) {
		pthread_rwlock_unlock(&svc->state_lock);
		return -ENOENT;
	}
	snprintf(path, sizeof(path), "%s", ep->path);
	pthread_rwlock_unlock(&svc->state_lock);

	err = read_text(path, value, sizeof(value));

	pthread_rwlock_wrlock(&svc->state_lock);
	ep = find_endpoint(svc, name);
	if (ep) {
		ep->read_err = err;
		if (err < 0)
			ep->value[0] = '\0';
		else
			snprintf(ep->value, sizeof(ep->value), "%s", value);
	}
	pthread_rwlock_unlock(&svc->state_lock);

	return err;
}

static void cache_endpoint_value(struct uniwilld *svc, const char *name, const char *value)
{
	char normalized[MAX_LINE];
	size_t len;

	snprintf(normalized, sizeof(normalized), "%s", value);
	len = strlen(normalized);
	while (len > 0 && (normalized[len - 1] == '\n' || normalized[len - 1] == '\r'))
		normalized[--len] = '\0';

	pthread_rwlock_wrlock(&svc->state_lock);
	struct endpoint *ep = find_endpoint(svc, name);
	if (ep) {
		ep->read_err = 0;
		snprintf(ep->value, sizeof(ep->value), "%s", normalized);
	}
	pthread_rwlock_unlock(&svc->state_lock);
}

static void refresh_endpoint_cache(struct uniwilld *svc)
{
	struct {
		char name[64];
		char path[PATH_MAX];
	} endpoints[MAX_ENDPOINTS];
	size_t count;

	pthread_rwlock_rdlock(&svc->state_lock);
	count = svc->endpoint_count;
	for (size_t i = 0; i < count; i++) {
		snprintf(endpoints[i].name, sizeof(endpoints[i].name), "%s",
			 svc->endpoints[i].name);
		snprintf(endpoints[i].path, sizeof(endpoints[i].path), "%s",
			 svc->endpoints[i].path);
	}
	pthread_rwlock_unlock(&svc->state_lock);

	for (size_t i = 0; i < count; i++) {
		char value[MAX_LINE] = "";
		int err = read_text(endpoints[i].path, value, sizeof(value));

		pthread_rwlock_wrlock(&svc->state_lock);
		struct endpoint *ep = find_endpoint(svc, endpoints[i].name);
		if (ep) {
			ep->read_err = err;
			if (err < 0)
				ep->value[0] = '\0';
			else
				snprintf(ep->value, sizeof(ep->value), "%s", value);
		}
		pthread_rwlock_unlock(&svc->state_lock);
	}
}

static bool refresh_runtime_endpoint_cache(struct uniwilld *svc, size_t *platform_cursor)
{
	struct {
		char name[64];
		char path[PATH_MAX];
		bool platform;
	} endpoints[MAX_ENDPOINTS];
	size_t count;
	size_t platform_count = 0;
	size_t platform_index = 0;
	bool changed = false;

	pthread_rwlock_rdlock(&svc->state_lock);
	count = svc->endpoint_count;
	for (size_t i = 0; i < count; i++) {
		snprintf(endpoints[i].name, sizeof(endpoints[i].name), "%s",
			 svc->endpoints[i].name);
		snprintf(endpoints[i].path, sizeof(endpoints[i].path), "%s",
			 svc->endpoints[i].path);
		endpoints[i].platform = svc->platform_path[0] &&
			has_prefix(svc->endpoints[i].path, svc->platform_path);
		if (endpoints[i].platform)
			platform_count++;
	}
	pthread_rwlock_unlock(&svc->state_lock);

	for (size_t i = 0; i < count; i++) {
		char value[MAX_LINE] = "";
		bool refresh = true;
		int err;

		if (endpoints[i].platform) {
			size_t distance = platform_count ?
				(platform_index + platform_count - *platform_cursor) % platform_count : 0;

			refresh = distance < PLATFORM_REFRESH_BATCH;
			platform_index++;
		}
		if (!refresh)
			continue;

		err = read_text(endpoints[i].path, value, sizeof(value));
		pthread_rwlock_wrlock(&svc->state_lock);
		struct endpoint *ep = find_endpoint(svc, endpoints[i].name);
		if (ep) {
			if (ep->read_err != err ||
			    (err < 0 ? ep->value[0] != '\0' : strcmp(ep->value, value)))
				changed = true;
			ep->read_err = err;
			if (err < 0)
				ep->value[0] = '\0';
			else
				snprintf(ep->value, sizeof(ep->value), "%s", value);
		}
		pthread_rwlock_unlock(&svc->state_lock);
	}

	if (platform_count)
		*platform_cursor = (*platform_cursor + PLATFORM_REFRESH_BATCH) % platform_count;
	return changed;
}

static bool refresh_system_power_cache(struct uniwilld *svc)
{
	char mode[64] = "";
	int err = dbus_read_system_power_mode(mode, sizeof(mode));
	bool changed;

	pthread_rwlock_wrlock(&svc->state_lock);
	changed = svc->system_power_mode_err != err ||
		(err < 0 ? svc->system_power_mode[0] != '\0' :
		 strcmp(svc->system_power_mode, mode));
	svc->system_power_mode_err = err;
	if (err < 0)
		svc->system_power_mode[0] = '\0';
	else
		snprintf(svc->system_power_mode, sizeof(svc->system_power_mode), "%s", mode);
	pthread_rwlock_unlock(&svc->state_lock);
	return changed;
}

static void load_default_curves(struct uniwilld *svc, int fan_mode)
{
	struct fan_curve cpu;
	struct fan_curve gpu;

	switch (fan_mode) {
	case FAN_MODE_PERFORMANCE:
		cpu = (struct fan_curve) {
			.name = "cpu",
			.points = { { 38, 0 }, { 43, 64 }, { 50, 89 }, { 60, 128 },
				    { 70, 166 }, { 80, 217 }, { 90, 255 } },
			.count = 7,
		};
		gpu = (struct fan_curve) {
			.name = "gpu",
			.points = { { 40, 0 }, { 45, 64 }, { 53, 89 }, { 63, 128 },
				    { 73, 179 }, { 82, 230 }, { 90, 255 } },
			.count = 7,
		};
		break;
	case FAN_MODE_QUIET:
		cpu = (struct fan_curve) {
			.name = "cpu",
			.points = { { 46, 0 }, { 51, 64 }, { 59, 69 }, { 69, 94 },
				    { 79, 128 }, { 89, 179 }, { 95, 255 } },
			.count = 7,
		};
		gpu = (struct fan_curve) {
			.name = "gpu",
			.points = { { 49, 0 }, { 54, 64 }, { 62, 69 }, { 72, 94 },
				    { 82, 140 }, { 90, 191 }, { 95, 255 } },
			.count = 7,
		};
		break;
	case FAN_MODE_WHISPER:
		cpu = (struct fan_curve) {
			.name = "cpu",
			.points = { { 50, 0 }, { 55, 64 }, { 64, 69 }, { 74, 89 },
				    { 84, 115 }, { 92, 166 }, { 98, 255 } },
			.count = 7,
		};
		gpu = (struct fan_curve) {
			.name = "gpu",
			.points = { { 52, 0 }, { 57, 64 }, { 66, 69 }, { 76, 89 },
				    { 86, 128 }, { 94, 191 }, { 98, 255 } },
			.count = 7,
		};
		break;
	case FAN_MODE_BENCHMARK:
		cpu = (struct fan_curve) {
			.name = "cpu",
			.points = { { 35, 0 }, { 40, 64 }, { 48, 115 }, { 58, 153 },
				    { 68, 191 }, { 78, 230 }, { 88, 255 } },
			.count = 7,
		};
		gpu = (struct fan_curve) {
			.name = "gpu",
			.points = { { 38, 0 }, { 43, 64 }, { 51, 115 }, { 61, 153 },
				    { 71, 191 }, { 80, 230 }, { 88, 255 } },
			.count = 7,
		};
		break;
	case FAN_MODE_STANDARD:
	default:
		fan_mode = FAN_MODE_STANDARD;
		cpu = (struct fan_curve) {
			.name = "cpu",
			.points = { { 42, 0 }, { 47, 64 }, { 55, 82 }, { 65, 107 },
				    { 75, 140 }, { 85, 191 }, { 92, 255 } },
			.count = 7,
		};
		gpu = (struct fan_curve) {
			.name = "gpu",
			.points = { { 45, 0 }, { 50, 64 }, { 58, 82 }, { 68, 107 },
				    { 78, 153 }, { 86, 204 }, { 92, 255 } },
			.count = 7,
		};
		break;
	}

	svc->cpu_curve = cpu;
	svc->gpu_curve = gpu;
	svc->active_curve_mode = fan_mode;
	svc->last_cpu_pwm = -1;
	svc->last_gpu_pwm = -1;
}

static void init_default_profiles(struct uniwilld *svc)
{
	for (int profile = 1; profile <= PROFILE_COUNT; profile++) {
		for (int source = 0; source < POWER_SOURCE_COUNT; source++) {
			svc->profiles[profile - 1].branch[source].power_mode = profile;
			svc->profiles[profile - 1].branch[source].fan_mode = profile;
		}
	}
	for (int source = 0; source < POWER_SOURCE_COUNT; source++) {
		snprintf(svc->source_controls[source].cpu_curve.name,
			 sizeof(svc->source_controls[source].cpu_curve.name), "cpu");
		snprintf(svc->source_controls[source].gpu_curve.name,
			 sizeof(svc->source_controls[source].gpu_curve.name), "gpu");
		svc->source_controls[source].passive_cooling = true;
		svc->lightbar[source] = (struct lightbar_source_state) {
			.enabled = true,
			.brightness = source == POWER_SOURCE_AC ? 160 : 96,
			.red = source == POWER_SOURCE_AC ? 255 : 80,
			.green = source == POWER_SOURCE_AC ? 132 : 150,
			.blue = source == POWER_SOURCE_AC ? 48 : 255,
			.rainbow = false,
			.breathing = false,
		};
		svc->keyboard_light[source] = (struct keyboard_light_source_state) {
			.enabled = true,
			.brightness = source == POWER_SOURCE_AC ? 50 : 25,
			.red = source == POWER_SOURCE_AC ? 255 : 92,
			.green = source == POWER_SOURCE_AC ? 112 : 128,
			.blue = source == POWER_SOURCE_AC ? 46 : 255,
			.effect = KEYBOARD_EFFECT_SOLID,
			.speed = 5,
			.direction = 2,
			.reactive = false,
		};
	}

	svc->lightbar_enabled = true;
	svc->last_applied_lightbar_source = -1;
	svc->keyboard_light_enabled = true;
	svc->last_applied_keyboard_light_source = -1;
	svc->active_profile = 2;
	svc->active_power_source = -1;
	svc->last_applied_profile = -1;
	svc->last_applied_power_source = -1;
	svc->last_applied_fan_mode = -1;
	svc->last_synced_power_profile = -1;
}

static int ensure_state_directory(const char *path)
{
	char directory[PATH_MAX];
	char *slash;

	if (strlen(path) >= sizeof(directory))
		return -ENAMETOOLONG;

	snprintf(directory, sizeof(directory), "%s", path);
	slash = strrchr(directory, '/');
	if (!slash || slash == directory)
		return 0;
	*slash = '\0';

	if (mkdir(directory, 0750) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

static int write_saved_curve(int fd, const char *source, const char *fan,
			     const struct fan_curve *curve, bool valid)
{
	if (dprintf(fd, "curve.%s.%s.valid=%d\ncurve.%s.%s.points=",
		    source, fan, valid, source, fan) < 0)
		return -errno;
	for (size_t i = 0; i < curve->count; i++) {
		if (dprintf(fd, "%s%d:%d", i ? "," : "",
			    curve->points[i].temp_c, curve->points[i].pwm) < 0)
			return -errno;
	}
	return dprintf(fd, "\n") < 0 ? -errno : 0;
}

static int parse_saved_curve(const char *text, struct fan_curve *curve)
{
	struct curve_point points[MAX_CURVE_POINTS];
	char copy[512];
	char *saveptr = NULL;
	char *token;
	size_t count = 0;

	if (snprintf(copy, sizeof(copy), "%s", text) >= (int)sizeof(copy))
		return -E2BIG;
	for (token = strtok_r(copy, ",\r\n", &saveptr); token;
	     token = strtok_r(NULL, ",\r\n", &saveptr)) {
		int temp;
		int pwm;

		if (count >= MAX_CURVE_POINTS || sscanf(token, "%d:%d", &temp, &pwm) != 2 ||
		    temp < 0 || temp > 130 || pwm < 0 || pwm > PWM_MAX ||
		    (count > 0 && temp <= points[count - 1].temp_c))
			return -EINVAL;
		points[count++] = (struct curve_point){ temp, pwm };
	}
	if (!count || points[count - 1].temp_c > CURVE_FAILSAFE_TEMP_C ||
	    points[count - 1].pwm != PWM_MAX)
		return -EINVAL;

	memcpy(curve->points, points, count * sizeof(points[0]));
	curve->count = count;
	return 0;
}

static int save_profile_state_locked(struct uniwilld *svc)
{
	char temp_path[PATH_MAX];
	int fd;
	int ret = 0;

	ret = ensure_state_directory(svc->state_path);
	if (ret < 0)
		return ret;

	if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld", svc->state_path,
		     (long)getpid()) >= (int)sizeof(temp_path))
		return -ENAMETOOLONG;

	fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;

	if (dprintf(fd,
		    "version=4\nactive_profile=%d\nfan_control=%d\n"
		    "lightbar.enabled=%d\nkeyboard_light.enabled=%d\n",
		    svc->active_profile, svc->fan_curve_control_enabled,
		    svc->lightbar_enabled, svc->keyboard_light_enabled) < 0)
		ret = -errno;

	for (int profile = 1; ret == 0 && profile <= PROFILE_COUNT; profile++) {
		for (int source = 0; source < POWER_SOURCE_COUNT; source++) {
			const char *source_name = source == POWER_SOURCE_AC ? "ac" : "battery";
			struct profile_branch *branch =
				&svc->profiles[profile - 1].branch[source];

			if (dprintf(fd, "profile.%d.%s.power=%d\nprofile.%d.%s.fan=%d\n",
				    profile, source_name, branch->power_mode,
				    profile, source_name, branch->fan_mode) < 0) {
				ret = -errno;
				break;
			}
		}
	}

	for (int source = 0; ret == 0 && source < POWER_SOURCE_COUNT; source++) {
		const char *name = source == POWER_SOURCE_AC ? "ac" : "battery";
		struct source_control_state *state = &svc->source_controls[source];

		if (dprintf(fd,
			    "control.%s.fan_mode.valid=%d\ncontrol.%s.fan_mode=%d\n"
			    "control.%s.passive.valid=%d\ncontrol.%s.passive=%d\n",
			    name, state->fan_mode_valid, name, state->fan_mode,
			    name, state->passive_cooling_valid, name,
			    state->passive_cooling) < 0)
			ret = -errno;
		if (ret == 0)
			ret = write_saved_curve(fd, name, "cpu", &state->cpu_curve,
						state->cpu_curve_valid);
		if (ret == 0)
			ret = write_saved_curve(fd, name, "gpu", &state->gpu_curve,
						state->gpu_curve_valid);
		if (ret == 0) {
			struct lightbar_source_state *lightbar = &svc->lightbar[source];

			if (dprintf(fd,
				    "lightbar.%s.enabled=%d\nlightbar.%s.brightness=%d\n"
				    "lightbar.%s.red=%d\nlightbar.%s.green=%d\n"
				    "lightbar.%s.blue=%d\nlightbar.%s.rainbow=%d\n"
				    "lightbar.%s.breathing=%d\n",
				    name, lightbar->enabled, name, lightbar->brightness,
				    name, lightbar->red, name, lightbar->green,
				    name, lightbar->blue, name, lightbar->rainbow,
				    name, lightbar->breathing) < 0)
				ret = -errno;
		}
		if (ret == 0) {
			struct keyboard_light_source_state *keyboard =
				&svc->keyboard_light[source];

			if (dprintf(fd,
				    "keyboard_light.%s.enabled=%d\n"
				    "keyboard_light.%s.brightness=%d\n"
				    "keyboard_light.%s.red=%d\n"
				    "keyboard_light.%s.green=%d\n"
				    "keyboard_light.%s.blue=%d\n"
				    "keyboard_light.%s.effect=%d\n"
				    "keyboard_light.%s.speed=%d\n"
				    "keyboard_light.%s.direction=%d\n"
				    "keyboard_light.%s.reactive=%d\n",
				    name, keyboard->enabled, name, keyboard->brightness,
				    name, keyboard->red, name, keyboard->green,
				    name, keyboard->blue, name, keyboard->effect,
				    name, keyboard->speed, name, keyboard->direction,
				    name, keyboard->reactive) < 0)
				ret = -errno;
		}
	}

	if (ret == 0 && fsync(fd) < 0)
		ret = -errno;
	if (close(fd) < 0 && ret == 0)
		ret = -errno;

	if (ret == 0 && rename(temp_path, svc->state_path) < 0)
		ret = -errno;
	if (ret < 0)
		unlink(temp_path);

	return ret;
}

static int load_profile_state(struct uniwilld *svc)
{
	FILE *file;
	char line[256];
	int version = 0;

	file = fopen(svc->state_path, "re");
	if (!file) {
		if (errno == ENOENT) {
			import_current_lightbar_state(svc);
			import_current_keyboard_light_state(svc);
			return 0;
		}
		return -errno;
	}

	while (fgets(line, sizeof(line), file)) {
		int profile;
		int value;
		char source_name[16];
		char field[16];
		int source;

		if (sscanf(line, "version=%d", &value) == 1) {
			version = value;
			continue;
		}
		if (sscanf(line, "active_profile=%d", &value) == 1) {
			if (value >= 1 && value <= PROFILE_COUNT)
				svc->active_profile = value;
			continue;
		}
		if (sscanf(line, "fan_control=%d", &value) == 1) {
			svc->fan_curve_control_enabled = !!value;
			continue;
		}
		if (sscanf(line, "lightbar.enabled=%d", &value) == 1) {
			svc->lightbar_enabled = !!value;
			continue;
		}
		if (sscanf(line, "keyboard_light.enabled=%d", &value) == 1) {
			svc->keyboard_light_enabled = !!value;
			continue;
		}
		if (sscanf(line, "lightbar.%15[^.].%15[^=]=%d",
			   source_name, field, &value) == 3) {
			struct lightbar_source_state *lightbar;

			if (!strcmp(source_name, "ac"))
				source = POWER_SOURCE_AC;
			else if (!strcmp(source_name, "battery"))
				source = POWER_SOURCE_BATTERY;
			else
				continue;
			lightbar = &svc->lightbar[source];
			if (!strcmp(field, "enabled"))
				lightbar->enabled = !!value;
			else if (!strcmp(field, "brightness") && value >= 0 &&
				 value <= LIGHTBAR_CONFIG_MAX)
				lightbar->brightness = value;
			else if (!strcmp(field, "red") && value >= 0 && value <= 255)
				lightbar->red = value;
			else if (!strcmp(field, "green") && value >= 0 && value <= 255)
				lightbar->green = value;
			else if (!strcmp(field, "blue") && value >= 0 && value <= 255)
				lightbar->blue = value;
			else if (!strcmp(field, "rainbow"))
				lightbar->rainbow = !!value;
			else if (!strcmp(field, "breathing"))
				lightbar->breathing = !!value;
			continue;
		}
		if (sscanf(line, "keyboard_light.%15[^.].%15[^=]=%d",
			   source_name, field, &value) == 3) {
			struct keyboard_light_source_state *keyboard;

			if (!strcmp(source_name, "ac"))
				source = POWER_SOURCE_AC;
			else if (!strcmp(source_name, "battery"))
				source = POWER_SOURCE_BATTERY;
			else
				continue;
			keyboard = &svc->keyboard_light[source];
			if (!strcmp(field, "enabled"))
				keyboard->enabled = !!value;
			else if (!strcmp(field, "brightness") && value >= 0 &&
				 value <= KEYBOARD_BRIGHTNESS_MAX)
				keyboard->brightness = value;
			else if (!strcmp(field, "red") && value >= 0 && value <= 255)
				keyboard->red = value;
			else if (!strcmp(field, "green") && value >= 0 && value <= 255)
				keyboard->green = value;
			else if (!strcmp(field, "blue") && value >= 0 && value <= 255)
				keyboard->blue = value;
			else if (!strcmp(field, "effect") && value >= 0 &&
				 value < KEYBOARD_EFFECT_COUNT)
				keyboard->effect = value;
			else if (!strcmp(field, "speed") && value >= 0 &&
				 value <= KEYBOARD_EFFECT_SPEED_MAX)
				keyboard->speed = value;
			else if (!strcmp(field, "direction") && value >= 1 && value <= 4)
				keyboard->direction = value;
			else if (!strcmp(field, "reactive"))
				keyboard->reactive = !!value;
			continue;
		}
		if (sscanf(line, "control.%15[^.].fan_mode.valid=%d",
			   source_name, &value) == 2 ||
		    sscanf(line, "control.%15[^.].passive.valid=%d",
			   source_name, &value) == 2) {
			bool fan_field = strstr(line, ".fan_mode.valid=") != NULL;

			if (!strcmp(source_name, "ac"))
				source = POWER_SOURCE_AC;
			else if (!strcmp(source_name, "battery"))
				source = POWER_SOURCE_BATTERY;
			else
				continue;
			if (fan_field)
				svc->source_controls[source].fan_mode_valid = !!value;
			else
				svc->source_controls[source].passive_cooling_valid = !!value;
			continue;
		}
		if (sscanf(line, "control.%15[^.].fan_mode=%d", source_name, &value) == 2 ||
		    sscanf(line, "control.%15[^.].passive=%d", source_name, &value) == 2) {
			bool fan_field = strstr(line, ".fan_mode=") != NULL;

			if (!strcmp(source_name, "ac"))
				source = POWER_SOURCE_AC;
			else if (!strcmp(source_name, "battery"))
				source = POWER_SOURCE_BATTERY;
			else
				continue;
			if (fan_field && value >= FAN_MODE_PERFORMANCE &&
			    value <= FAN_MODE_BENCHMARK)
				svc->source_controls[source].fan_mode = value;
			else if (!fan_field)
				svc->source_controls[source].passive_cooling = !!value;
			continue;
		}
		if (sscanf(line, "curve.%15[^.].%15[^.].valid=%d",
			   source_name, field, &value) == 3) {
			if (!strcmp(source_name, "ac"))
				source = POWER_SOURCE_AC;
			else if (!strcmp(source_name, "battery"))
				source = POWER_SOURCE_BATTERY;
			else
				continue;
			if (!strcmp(field, "cpu"))
				svc->source_controls[source].cpu_curve_valid = !!value;
			else if (!strcmp(field, "gpu"))
				svc->source_controls[source].gpu_curve_valid = !!value;
			continue;
		}
		if (sscanf(line, "curve.%15[^.].%15[^.].points=",
			   source_name, field) == 2) {
			struct fan_curve *curve = NULL;
			char *points = strchr(line, '=');

			if (!strcmp(source_name, "ac"))
				source = POWER_SOURCE_AC;
			else if (!strcmp(source_name, "battery"))
				source = POWER_SOURCE_BATTERY;
			else
				continue;
			if (!strcmp(field, "cpu"))
				curve = &svc->source_controls[source].cpu_curve;
			else if (!strcmp(field, "gpu"))
				curve = &svc->source_controls[source].gpu_curve;
			if (curve && points && points[1] &&
			    points[1 + strspn(points + 1, " \t\r\n")] &&
			    parse_saved_curve(points + 1, curve) < 0) {
				fclose(file);
				return -EINVAL;
			}
			continue;
		}
		if (sscanf(line, "profile.%d.%15[^.].%15[^=]=%d",
			   &profile, source_name, field, &value) != 4)
			continue;
		if (profile < 1 || profile > PROFILE_COUNT || value < 1 || value > 3)
			continue;
		if (!strcmp(source_name, "ac"))
			source = POWER_SOURCE_AC;
		else if (!strcmp(source_name, "battery") || !strcmp(source_name, "dc"))
			source = POWER_SOURCE_BATTERY;
		else
			continue;

		if (!strcmp(field, "power"))
			svc->profiles[profile - 1].branch[source].power_mode = value;
		else if (!strcmp(field, "fan"))
			svc->profiles[profile - 1].branch[source].fan_mode = value;
	}

	if (ferror(file)) {
		int ret = -errno;
		fclose(file);
		return ret;
	}
	fclose(file);

	if (version == 1 || version == 2)
		import_current_lightbar_state(svc);
	if (version >= 1 && version <= 3)
		import_current_keyboard_light_state(svc);
	return version >= 1 && version <= 4 ? 0 : -EINVAL;
}

static int curve_pwm(const struct fan_curve *curve, int temp_c, int previous_pwm)
{
	size_t i;
	int pwm;

	if (curve->count == 0)
		return 0;

	/*
	 * A leading zero/non-zero point pair is an explicit on/off hysteresis
	 * band.  A stopped fan does not start before the second point, while a
	 * running fan stays at its minimum speed until it cools to the first.
	 */
	if (curve->count >= 2 && curve->points[0].pwm == 0 &&
	    curve->points[1].pwm > 0) {
		if (previous_pwm <= 0 && temp_c < curve->points[1].temp_c)
			return 0;
		if (previous_pwm > 0 && temp_c <= curve->points[0].temp_c)
			return 0;
		if (previous_pwm > 0 && temp_c < curve->points[1].temp_c)
			return PWM_MIN_ON;
	}

	if (temp_c <= curve->points[0].temp_c)
		pwm = curve->points[0].pwm;
	else {
		for (i = 1; i < curve->count; i++) {
			const struct curve_point *prev = &curve->points[i - 1];
			const struct curve_point *next = &curve->points[i];
			int span;

			if (temp_c > next->temp_c)
				continue;

			span = next->temp_c - prev->temp_c;
			if (span <= 0)
				pwm = next->pwm;
			else
				pwm = prev->pwm + (next->pwm - prev->pwm) *
					(temp_c - prev->temp_c) / span;
			goto clamp;
		}

		pwm = curve->points[curve->count - 1].pwm;
	}

clamp:
	if (pwm > 0 && pwm < PWM_MIN_ON)
		pwm = PWM_MIN_ON;
	return pwm;
}

static int set_manual_fan_mode(struct uniwilld *svc)
{
	int err;
	int first_err = 0;

	err = endpoint_write_int(svc, "pwm1_enable", 1);
	if (err < 0)
		first_err = err;

	err = endpoint_write_int(svc, "pwm2_enable", 1);
	if (err < 0 && !first_err)
		first_err = err;

	return first_err;
}

static int set_auto_fan_mode(struct uniwilld *svc)
{
	int err;
	int first_err = 0;

	err = endpoint_write_int(svc, "pwm1_enable", 2);
	if (err < 0)
		first_err = err;

	err = endpoint_write_int(svc, "pwm2_enable", 2);
	if (err < 0 && !first_err)
		first_err = err;

	return first_err;
}

static bool fan_mode_suspends_curve_control(int fan_mode)
{
	return fan_mode == FAN_MODE_WHISPER || fan_mode == FAN_MODE_BENCHMARK;
}

static int active_fan_mode_locked(const struct uniwilld *svc)
{
	int profile = svc->active_profile;
	int source = svc->active_power_source;

	if (source >= 0 && source < POWER_SOURCE_COUNT &&
	    svc->source_controls[source].fan_mode_valid &&
	    fan_mode_suspends_curve_control(svc->source_controls[source].fan_mode))
		return svc->source_controls[source].fan_mode;
	if (profile >= 1 && profile <= PROFILE_COUNT &&
	    source >= 0 && source < POWER_SOURCE_COUNT)
		return svc->profiles[profile - 1].branch[source].fan_mode;
	return svc->last_applied_fan_mode;
}

static int active_fan_mode(struct uniwilld *svc)
{
	int fan_mode;

	pthread_rwlock_rdlock(&svc->state_lock);
	fan_mode = active_fan_mode_locked(svc);
	pthread_rwlock_unlock(&svc->state_lock);
	return fan_mode;
}

static bool is_direct_fan_speed_endpoint(const char *name)
{
	return !strcmp(name, "pwm1") || !strcmp(name, "pwm2") ||
		!strcmp(name, "pwm1_enable") || !strcmp(name, "pwm2_enable") ||
		!strcmp(name, "fan_boost") || !strcmp(name, "fan_mode");
}

/*
 * Benchmark mode is an exclusive firmware fan-boost layer. Whisper mode also
 * owns the firmware fan policy. Keep the user's curve enabled as persisted
 * intent, but do not let manual PWM writes override either mode.
 */
static int apply_fan_mode_hardware(struct uniwilld *svc, int fan_mode,
				   bool curve_control)
{
	int err;

	pthread_mutex_lock(&svc->fan_control_lock);
	if (curve_control && !fan_mode_suspends_curve_control(fan_mode)) {
		err = endpoint_write_int(svc, "fan_mode", fan_mode);
		if (err == 0)
			err = set_manual_fan_mode(svc);
	} else {
		/* Drop the userspace curve first so the driver can install the
		 * selected firmware table/boost as the final fan operation. */
		err = set_auto_fan_mode(svc);
		if (err == 0)
			err = endpoint_write_int(svc, "fan_mode", fan_mode);
	}
	pthread_mutex_unlock(&svc->fan_control_lock);
	return err;
}

static int read_hwmon_temp(struct uniwilld *svc, int channel, int *temp_mdeg, int *temp_c)
{
	const char *name;
	int err;
	char value[64];

	switch (channel) {
	case 0:
		name = "temp1_input";
		break;
	case 1:
		name = "temp2_input";
		break;
	default:
		return -EINVAL;
	}

	err = endpoint_read(svc, name, value, sizeof(value));
	if (err < 0)
		return err;

	*temp_mdeg = atoi(value);
	*temp_c = *temp_mdeg / 1000;
	return 0;
}

static int read_hwmon_fan_rpm(struct uniwilld *svc, int channel, int *rpm)
{
	const char *name;
	char value[64];
	int err;

	switch (channel) {
	case 0:
		name = "fan1_input";
		break;
	case 1:
		name = "fan2_input";
		break;
	default:
		return -EINVAL;
	}

	err = endpoint_read(svc, name, value, sizeof(value));
	if (err < 0)
		return err;

	*rpm = atoi(value);
	return 0;
}

static int fan_control_tick_locked(struct uniwilld *svc)
{
	struct fan_curve cpu_curve;
	struct fan_curve gpu_curve;
	bool enabled;
	int previous_cpu_pwm;
	int previous_gpu_pwm;
	int temp_mdeg;
	int pwm;
	int err;
	int cpu_temp_c;
	int gpu_temp_c;
	int fan_mode;

	pthread_rwlock_rdlock(&svc->state_lock);
	enabled = svc->fan_curve_control_enabled;
	fan_mode = active_fan_mode_locked(svc);
	cpu_curve = svc->cpu_curve;
	gpu_curve = svc->gpu_curve;
	previous_cpu_pwm = svc->last_cpu_pwm;
	previous_gpu_pwm = svc->last_gpu_pwm;
	pthread_rwlock_unlock(&svc->state_lock);

	if (!enabled || fan_mode_suspends_curve_control(fan_mode))
		return 0;
	if (!svc->hwmon_path[0])
		return -ENODEV;

	err = set_manual_fan_mode(svc);
	if (err < 0)
		return err;

	err = read_hwmon_temp(svc, 0, &temp_mdeg, &cpu_temp_c);
	if (err < 0)
		goto fail_safe;
	err = read_hwmon_temp(svc, 1, &temp_mdeg, &gpu_temp_c);
	if (err < 0)
		goto fail_safe;

	pwm = curve_pwm(&cpu_curve, cpu_temp_c, previous_cpu_pwm);
	if (pwm != previous_cpu_pwm) {
		err = endpoint_write_int(svc, "pwm1", pwm);
		if (err < 0)
			goto fail_safe;
	}
	pthread_rwlock_wrlock(&svc->state_lock);
	svc->last_cpu_pwm = pwm;
	pthread_rwlock_unlock(&svc->state_lock);

	pwm = curve_pwm(&gpu_curve, gpu_temp_c, previous_gpu_pwm);
	if (pwm != previous_gpu_pwm) {
		err = endpoint_write_int(svc, "pwm2", pwm);
		if (err < 0)
			goto fail_safe;
	}
	pthread_rwlock_wrlock(&svc->state_lock);
	svc->last_gpu_pwm = pwm;
	pthread_rwlock_unlock(&svc->state_lock);

	return 0;

fail_safe:
	pthread_rwlock_wrlock(&svc->state_lock);
	svc->fan_curve_control_enabled = false;
	svc->last_cpu_pwm = -1;
	svc->last_gpu_pwm = -1;
	pthread_rwlock_unlock(&svc->state_lock);
	set_auto_fan_mode(svc);
	return err;
}

static int fan_control_tick(struct uniwilld *svc)
{
	int err;

	pthread_mutex_lock(&svc->fan_control_lock);
	err = fan_control_tick_locked(svc);
	pthread_mutex_unlock(&svc->fan_control_lock);
	return err;
}

static void json_escape(char *dst, size_t dst_size, const char *src)
{
	size_t pos = 0;

	while (*src && pos + 2 < dst_size) {
		if (*src == '"' || *src == '\\') {
			if (pos + 3 >= dst_size)
				break;
			dst[pos++] = '\\';
			dst[pos++] = *src++;
		} else if (*src == '\n') {
			if (pos + 3 >= dst_size)
				break;
			dst[pos++] = '\\';
			dst[pos++] = 'n';
			src++;
		} else {
			dst[pos++] = *src++;
		}
	}

	dst[pos] = '\0';
}

static int appendf(char *buf, size_t size, size_t *pos, const char *fmt, ...)
{
	va_list ap;
	int ret;

	if (*pos >= size)
		return -ENOSPC;

	va_start(ap, fmt);
	ret = vsnprintf(buf + *pos, size - *pos, fmt, ap);
	va_end(ap);
	if (ret < 0)
		return ret;
	if ((size_t)ret >= size - *pos)
		return -ENOSPC;

	*pos += ret;
	return 0;
}

static bool safe_block_device_name(const char *name)
{
	if (!name || !*name)
		return false;

	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
			return false;
	}
	return true;
}

static bool physical_block_device(const char *name)
{
	char path[PATH_MAX];

	if (!safe_block_device_name(name) ||
	    !strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) ||
	    !strncmp(name, "zram", 4) || !strncmp(name, "dm-", 3))
		return false;

	if (snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name) >=
	    (int)sizeof(path) || path_exists(path))
		return false;
	if (snprintf(path, sizeof(path), "/sys/class/block/%s/device", name) >=
	    (int)sizeof(path))
		return false;
	return path_exists(path);
}

static bool read_block_device_size(const char *sysfs_path, guint64 *size_bytes)
{
	char size_path[PATH_MAX];
	char value[64];
	char *end;
	unsigned long long sectors;

	if (join_path(size_path, sizeof(size_path), sysfs_path, "size") < 0 ||
	    read_text(size_path, value, sizeof(value)) < 0)
		return false;

	errno = 0;
	sectors = strtoull(value, &end, 10);
	if (errno || end == value || *end || sectors > G_MAXUINT64 / 512)
		return false;

	*size_bytes = (guint64)sectors * 512;
	return true;
}

/*
 * Returns 1 when free space was read, 0 when no filesystem was detected, and
 * -1 when the filesystem is known but its free-space backend is unavailable.
 */
static int read_filesystem_free_space(const char *device, guint64 *free_bytes)
{
	GError *error = NULL;
	gchar *fstype;
	guint64 value;

	if (!filesystem_library_available)
		return -1;

	fstype = bd_fs_get_fstype(device, &error);
	if (!fstype) {
		if (error)
			g_error_free(error);
		return 0;
	}

	value = bd_fs_get_free_space(device, fstype, &error);
	g_free(fstype);
	if (error) {
		g_error_free(error);
		return -1;
	}

	*free_bytes = value;
	return 1;
}

static void add_storage_free_bytes(guint64 *total, guint64 value)
{
	if (G_MAXUINT64 - *total < value)
		*total = G_MAXUINT64;
	else
		*total += value;
}

static struct storage_space_info read_disk_space_info(const char *name)
{
	struct storage_space_info info = { 0 };
	char disk_sysfs[PATH_MAX];
	char device[PATH_MAX];
	DIR *dir;
	struct dirent *de;
	guint64 disk_size;
	guint64 partition_bytes = 0;
	guint64 filesystem_free_bytes = 0;
	bool has_partition = false;
	bool has_filesystem_space = false;

	if (!filesystem_library_available ||
	    snprintf(disk_sysfs, sizeof(disk_sysfs),
		     "/sys/class/block/%s", name) >= (int)sizeof(disk_sysfs) ||
	    !read_block_device_size(disk_sysfs, &disk_size) || !disk_size)
		return info;

	dir = opendir(disk_sysfs);
	if (!dir)
		return info;

	while ((de = readdir(dir))) {
		char partition_sysfs[PATH_MAX];
		char partition_marker[PATH_MAX];
		guint64 size_bytes;
		guint64 free_bytes;

		if (de->d_name[0] == '.' || !safe_block_device_name(de->d_name) ||
		    join_path(partition_sysfs, sizeof(partition_sysfs),
			      disk_sysfs, de->d_name) < 0 ||
		    join_path(partition_marker, sizeof(partition_marker),
			      partition_sysfs, "partition") < 0 ||
		    !path_exists(partition_marker))
			continue;

		has_partition = true;
		if (read_block_device_size(partition_sysfs, &size_bytes))
			add_storage_free_bytes(&partition_bytes, size_bytes);
		if (snprintf(device, sizeof(device), "/dev/%s", de->d_name) >=
		    (int)sizeof(device) ||
		    read_filesystem_free_space(device, &free_bytes) != 1)
			continue;

		add_storage_free_bytes(&filesystem_free_bytes, free_bytes);
		has_filesystem_space = true;
	}
	closedir(dir);

	if (has_partition) {
		guint64 unallocated_bytes = disk_size > partition_bytes ?
			disk_size - partition_bytes : 0;

		info.free_bytes = unallocated_bytes;
		add_storage_free_bytes(&info.free_bytes, filesystem_free_bytes);
		info.available = has_filesystem_space || unallocated_bytes > 0;
	} else {
		guint64 free_bytes;
		int result;

		if (snprintf(device, sizeof(device), "/dev/%s", name) >=
		    (int)sizeof(device))
			return info;
		result = read_filesystem_free_space(device, &free_bytes);
		if (result == 1) {
			info.free_bytes = free_bytes;
			info.available = true;
		} else if (result == 0) {
			info.free_bytes = disk_size;
			info.available = true;
		}
	}

	if (info.available) {
		if (info.free_bytes > disk_size)
			info.free_bytes = disk_size;
		info.free_percent = (unsigned int)
			(((unsigned __int128)info.free_bytes * 100 +
			  disk_size / 2) / disk_size);
		if (info.free_percent > 100)
			info.free_percent = 100;
	}

	return info;
}

static int nvme_controller_device(const char *name, char *device, size_t size)
{
	const char *p;
	const char *namespace_marker;

	if (strncmp(name, "nvme", 4))
		return -EINVAL;
	p = name + 4;
	if (!isdigit((unsigned char)*p))
		return -EINVAL;
	namespace_marker = p;
	while (isdigit((unsigned char)*namespace_marker))
		namespace_marker++;
	if (*namespace_marker != 'n' ||
	    !isdigit((unsigned char)namespace_marker[1]))
		return -EINVAL;

	if (snprintf(device, size, "/dev/nvme%.*s",
		     (int)(namespace_marker - p), p) >= (int)size)
		return -ENOSPC;
	return 0;
}

static int ata_health_percent(const BDSmartATA *smart)
{
	if (!smart || !smart->attributes)
		return -1;

	for (size_t i = 0; smart->attributes[i]; i++) {
		const BDSmartATAAttribute *attr = smart->attributes[i];
		const char *name = attr->well_known_name ?
			attr->well_known_name : attr->name;
		char normalized[128] = "";
		size_t out = 0;

		if (!name)
			continue;
		for (const unsigned char *p = (const unsigned char *)name;
		     *p && out + 1 < sizeof(normalized); p++) {
			if (isalnum(*p))
				normalized[out++] = (char)tolower(*p);
		}
		normalized[out] = '\0';

		if (strstr(normalized, "percentlifetimeused")) {
			guint64 used = attr->value_raw > 100 ? 100 : attr->value_raw;
			return 100 - (int)used;
		}
		if (strstr(normalized, "percentlifetimeremain") ||
		    strstr(normalized, "ssdlifeleft") ||
		    strstr(normalized, "mediawearoutindicator") ||
		    strstr(normalized, "wearlevelingcount")) {
			if (attr->value >= 0 && attr->value <= 100)
				return attr->value;
			if (attr->value_raw <= 100)
				return (int)attr->value_raw;
		}
	}
	return -1;
}

static guint64 ata_total_bytes_written(const BDSmartATA *smart)
{
	if (!smart || !smart->attributes)
		return 0;

	for (size_t i = 0; smart->attributes[i]; i++) {
		const BDSmartATAAttribute *attr = smart->attributes[i];
		const char *name = attr->well_known_name ?
			attr->well_known_name : attr->name;
		char normalized[128] = "";
		size_t out = 0;

		if (!name)
			continue;
		for (const unsigned char *p = (const unsigned char *)name;
		     *p && out + 1 < sizeof(normalized); p++) {
			if (isalnum(*p))
				normalized[out++] = (char)tolower(*p);
		}
		normalized[out] = '\0';

		if (strstr(normalized, "totallbaswritten"))
			return attr->value_raw > UINT64_MAX / 512 ?
				UINT64_MAX : attr->value_raw * 512;
		if (strstr(normalized, "hostwrites32mib"))
			return attr->value_raw > UINT64_MAX / (32ULL * 1024 * 1024) ?
				UINT64_MAX : attr->value_raw * 32ULL * 1024 * 1024;
	}
	return 0;
}

static guint64 ata_media_errors(const BDSmartATA *smart)
{
	guint64 errors = 0;

	if (!smart || !smart->attributes)
		return 0;
	for (size_t i = 0; smart->attributes[i]; i++) {
		const BDSmartATAAttribute *attr = smart->attributes[i];

		if (attr->id == 5 || attr->id == 197 || attr->id == 198) {
			if (UINT64_MAX - errors < attr->value_raw)
				return UINT64_MAX;
			errors += attr->value_raw;
		}
	}
	return errors;
}

static guint64 ata_unsafe_shutdowns(const BDSmartATA *smart)
{
	if (!smart || !smart->attributes)
		return 0;
	for (size_t i = 0; smart->attributes[i]; i++) {
		const BDSmartATAAttribute *attr = smart->attributes[i];

		if (attr->id == 174)
			return attr->value_raw;
	}
	return 0;
}

static void append_nvme_smart_json(char *buf, size_t size, size_t *pos,
				   const char *name,
				   const struct storage_space_info *space)
{
	char controller[64];
	BDNVMEControllerInfo *info = NULL;
	BDNVMESmartLog *smart = NULL;
	GError *error = NULL;
	char model[256] = "";
	char serial[256] = "";
	char firmware[256] = "";
	char free_bytes[32] = "null";
	char free_percent[16] = "null";

	if (space->available) {
		snprintf(free_bytes, sizeof(free_bytes), "%llu",
			 (unsigned long long)space->free_bytes);
		snprintf(free_percent, sizeof(free_percent), "%u",
			 space->free_percent);
	}

	if (!nvme_library_available ||
	    nvme_controller_device(name, controller, sizeof(controller)) < 0)
		goto unavailable;

	info = bd_nvme_get_controller_info(controller, &error);
	if (error) {
		g_error_free(error);
		error = NULL;
	}
	smart = bd_nvme_get_smart_log(controller, &error);
	if (!smart)
		goto unavailable;

	if (info) {
		json_escape(model, sizeof(model), info->model_number ?
			    info->model_number : "");
		json_escape(serial, sizeof(serial), info->serial_number ?
			    info->serial_number : "");
		json_escape(firmware, sizeof(firmware), info->firmware_ver ?
			    info->firmware_ver : "");
	}
	appendf(buf, size, pos,
		"{\"name\":\"%s\",\"available\":true,\"protocol\":\"NVMe\","
		"\"model\":\"%s\",\"serial_number\":\"%s\","
		"\"firmware\":\"%s\",\"available_bytes\":%s,"
		"\"available_percent\":%s,\"smart_passed\":%s,"
		"\"health_percent\":%u,\"percentage_used\":%u,"
		"\"available_spare_percent\":%u,"
		"\"temperature_c\":%d,\"power_on_hours\":%llu,"
		"\"power_cycles\":%llu,\"unsafe_shutdowns\":%llu,"
		"\"media_errors\":%llu,\"total_bytes_written\":%llu}",
		name, model, serial, firmware, free_bytes, free_percent,
		smart->critical_warning == 0 ? "true" : "false",
		smart->percent_used >= 100 ? 0 : 100 - smart->percent_used,
		smart->percent_used, smart->avail_spare,
		smart->temperature >= 273 ? (int)smart->temperature - 273 : -1,
		(unsigned long long)smart->power_on_hours,
		(unsigned long long)smart->power_cycles,
		(unsigned long long)smart->unsafe_shutdowns,
		(unsigned long long)smart->media_errors,
		(unsigned long long)smart->total_data_written);
	goto out;

unavailable:
	appendf(buf, size, pos,
		"{\"name\":\"%s\",\"available\":false,\"protocol\":\"NVMe\","
		"\"available_bytes\":%s,\"available_percent\":%s}",
		name, free_bytes, free_percent);
out:
	if (error)
		g_error_free(error);
	if (smart)
		bd_nvme_smart_log_free(smart);
	if (info)
		bd_nvme_controller_info_free(info);
}

static void append_ata_smart_json(char *buf, size_t size, size_t *pos,
				  const char *name,
				  const struct storage_space_info *space)
{
	char device[PATH_MAX];
	BDSmartATA *smart = NULL;
	GError *error = NULL;
	int health;
	guint64 bytes_written;
	guint64 media_errors;
	guint64 unsafe_shutdowns;
	char health_text[16];
	char free_bytes[32] = "null";
	char free_percent[16] = "null";

	if (space->available) {
		snprintf(free_bytes, sizeof(free_bytes), "%llu",
			 (unsigned long long)space->free_bytes);
		snprintf(free_percent, sizeof(free_percent), "%u",
			 space->free_percent);
	}

	if (!ata_smart_library_available ||
	    snprintf(device, sizeof(device), "/dev/%s", name) >=
	    (int)sizeof(device))
		goto unavailable;

	smart = bd_smart_ata_get_info(device, NULL, &error);
	if (!smart)
		goto unavailable;

	health = ata_health_percent(smart);
	bytes_written = ata_total_bytes_written(smart);
	media_errors = ata_media_errors(smart);
	unsafe_shutdowns = ata_unsafe_shutdowns(smart);
	if (health < 0)
		snprintf(health_text, sizeof(health_text), "null");
	else
		snprintf(health_text, sizeof(health_text), "%d", health);
	appendf(buf, size, pos,
		"{\"name\":\"%s\",\"available\":%s,\"protocol\":\"ATA\","
		"\"available_bytes\":%s,\"available_percent\":%s,"
		"\"smart_passed\":%s,\"health_percent\":%s,"
		"\"temperature_c\":%d,\"power_on_hours\":%u,"
		"\"power_cycles\":%llu,\"unsafe_shutdowns\":%llu,"
		"\"media_errors\":%llu,\"total_bytes_written\":%llu}",
		name, smart->smart_supported ? "true" : "false",
		free_bytes, free_percent,
		smart->overall_status_passed ? "true" : "false",
		health_text,
		smart->temperature >= 273 ? (int)smart->temperature - 273 : -1,
		smart->power_on_time / 60,
		(unsigned long long)smart->power_cycle_count,
		(unsigned long long)unsafe_shutdowns,
		(unsigned long long)media_errors,
		(unsigned long long)bytes_written);
	goto out;

unavailable:
	appendf(buf, size, pos,
		"{\"name\":\"%s\",\"available\":false,\"protocol\":\"ATA\","
		"\"available_bytes\":%s,\"available_percent\":%s}",
		name, free_bytes, free_percent);
out:
	if (error)
		g_error_free(error);
	if (smart)
		bd_smart_ata_free(smart);
}

static unsigned short read_le16(const unsigned char *data)
{
	return (unsigned short)data[0] | ((unsigned short)data[1] << 8);
}

static unsigned int read_le32(const unsigned char *data)
{
	return (unsigned int)data[0] |
		((unsigned int)data[1] << 8) |
		((unsigned int)data[2] << 16) |
		((unsigned int)data[3] << 24);
}

static int read_binary_file(const char *path, unsigned char *buf, size_t size,
			    size_t *length)
{
	int fd;
	size_t pos = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	while (pos < size) {
		ssize_t ret = read(fd, buf + pos, size - pos);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0) {
			int err = -errno;
			close(fd);
			return err;
		}
		if (ret == 0)
			break;
		pos += (size_t)ret;
	}
	close(fd);
	*length = pos;
	return 0;
}

static void dmi_string(const unsigned char *raw, size_t raw_size,
		       size_t formatted_size, unsigned int index,
		       char *out, size_t out_size)
{
	const unsigned char *p;
	const unsigned char *end = raw + raw_size;
	unsigned int current = 1;

	out[0] = '\0';
	if (index == 0 || formatted_size >= raw_size)
		return;
	p = raw + formatted_size;
	while (p < end && *p) {
		const unsigned char *start = p;
		size_t len;

		while (p < end && *p)
			p++;
		if (current++ != index) {
			if (p < end)
				p++;
			continue;
		}
		len = (size_t)(p - start);
		if (len >= out_size)
			len = out_size - 1;
		memcpy(out, start, len);
		out[len] = '\0';
		return;
	}
}

static const char *dmi_memory_type(unsigned int type)
{
	switch (type) {
	case 0x12: return "DDR";
	case 0x13: return "DDR2";
	case 0x18: return "DDR3";
	case 0x1a: return "DDR4";
	case 0x1b: return "LPDDR";
	case 0x1c: return "LPDDR2";
	case 0x1d: return "LPDDR3";
	case 0x1e: return "LPDDR4";
	case 0x22: return "DDR5";
	case 0x23: return "LPDDR5";
	default: return "Unknown";
	}
}

static const char *dmi_memory_form_factor(unsigned int form_factor)
{
	switch (form_factor) {
	case 0x09: return "DIMM";
	case 0x0d: return "SODIMM";
	case 0x0f: return "FB-DIMM";
	case 0x10: return "Die";
	default: return "Unknown";
	}
}

static unsigned long long dmi_memory_size(const unsigned char *raw,
					  size_t formatted_size)
{
	unsigned int size_value;

	if (formatted_size < 0x0e)
		return 0;
	size_value = read_le16(raw + 0x0c);
	if (size_value == 0 || size_value == 0xffff)
		return 0;
	if (size_value == 0x7fff && formatted_size >= 0x20)
		return (unsigned long long)(read_le32(raw + 0x1c) & 0x7fffffffU) *
			1024ULL * 1024ULL;
	if (size_value & 0x8000)
		return (unsigned long long)(size_value & 0x7fff) * 1024ULL;
	return (unsigned long long)size_value * 1024ULL * 1024ULL;
}

static unsigned int dmi_system_memory_slots_from_raw(
	const unsigned char *raw, size_t raw_size)
{
	size_t formatted_size;

	if (raw_size < 0x0f || raw[0] != 16)
		return 0;
	formatted_size = raw[1];
	if (formatted_size < 0x0f || formatted_size > raw_size ||
	    raw[0x05] != 0x03)
		return 0;
	return read_le16(raw + 0x0d);
}

static unsigned int dmi_system_memory_slots(void)
{
	DIR *dir;
	struct dirent *de;
	unsigned int slots = 0;

	dir = opendir("/sys/firmware/dmi/entries");
	if (!dir)
		return 0;

	while ((de = readdir(dir))) {
		char path[PATH_MAX];
		unsigned char raw[MAX_DMI_RAW];
		size_t raw_size = 0;
		unsigned int array_slots;

		if (strncmp(de->d_name, "16-", 3) ||
		    snprintf(path, sizeof(path),
			     "/sys/firmware/dmi/entries/%s/raw", de->d_name) >=
		    (int)sizeof(path) ||
		    read_binary_file(path, raw, sizeof(raw), &raw_size) < 0)
			continue;
		array_slots = dmi_system_memory_slots_from_raw(raw, raw_size);
		if (UINT_MAX - slots < array_slots) {
			slots = UINT_MAX;
			break;
		}
		slots += array_slots;
	}
	closedir(dir);
	return slots;
}

static void append_memory_devices_json(char *buf, size_t size, size_t *pos)
{
	DIR *dir;
	struct dirent *de;
	unsigned int slots;
	bool first = true;

	appendf(buf, size, pos, "{\"slots\":");
	slots = dmi_system_memory_slots();
	dir = opendir("/sys/firmware/dmi/entries");
	if (!dir) {
		appendf(buf, size, pos, "null,\"devices\":[]}");
		return;
	}

	if (slots == 0) {
		while ((de = readdir(dir))) {
			if (!strncmp(de->d_name, "17-", 3))
				slots++;
		}
		rewinddir(dir);
	}
	appendf(buf, size, pos, "%u,\"devices\":[", slots);

	while ((de = readdir(dir))) {
		char path[PATH_MAX];
		unsigned char raw[MAX_DMI_RAW];
		size_t raw_size = 0;
		size_t formatted_size;
		unsigned long long module_size;
		unsigned int speed;
		unsigned int configured_speed;
		char locator[256], bank[256], manufacturer[256];
		char serial[256], part[256];
		char escaped_locator[512], escaped_bank[512], escaped_manufacturer[512];
		char escaped_serial[512], escaped_part[512];

		if (strncmp(de->d_name, "17-", 3) ||
		    snprintf(path, sizeof(path),
			     "/sys/firmware/dmi/entries/%s/raw", de->d_name) >=
		    (int)sizeof(path) ||
		    read_binary_file(path, raw, sizeof(raw), &raw_size) < 0 ||
		    raw_size < 0x1b || raw[0] != 17)
			continue;

		formatted_size = raw[1];
		if (formatted_size < 0x1b || formatted_size > raw_size)
			continue;
		module_size = dmi_memory_size(raw, formatted_size);
		if (module_size == 0)
			continue;
		speed = formatted_size >= 0x17 ? read_le16(raw + 0x15) : 0;
		configured_speed = formatted_size >= 0x22 ?
			read_le16(raw + 0x20) : 0;

		dmi_string(raw, raw_size, formatted_size, raw[0x10],
			   locator, sizeof(locator));
		dmi_string(raw, raw_size, formatted_size, raw[0x11],
			   bank, sizeof(bank));
		dmi_string(raw, raw_size, formatted_size, raw[0x17],
			   manufacturer, sizeof(manufacturer));
		dmi_string(raw, raw_size, formatted_size, raw[0x18],
			   serial, sizeof(serial));
		dmi_string(raw, raw_size, formatted_size, raw[0x1a],
			   part, sizeof(part));
		json_escape(escaped_locator, sizeof(escaped_locator), locator);
		json_escape(escaped_bank, sizeof(escaped_bank), bank);
		json_escape(escaped_manufacturer, sizeof(escaped_manufacturer),
			    manufacturer);
		json_escape(escaped_serial, sizeof(escaped_serial), serial);
		json_escape(escaped_part, sizeof(escaped_part), part);

		appendf(buf, size, pos,
			"%s{\"locator\":\"%s\",\"bank_locator\":\"%s\","
			"\"manufacturer\":\"%s\",\"serial_number\":\"%s\","
			"\"part_number\":\"%s\",\"memory_type\":\"%s\","
			"\"form_factor\":\"%s\",\"size_bytes\":%llu,"
			"\"speed_mt_s\":%u,\"configured_speed_mt_s\":%u}",
			first ? "" : ",", escaped_locator, escaped_bank,
			escaped_manufacturer, escaped_serial, escaped_part,
			dmi_memory_type(raw[0x12]),
			dmi_memory_form_factor(raw[0x0e]), module_size,
			speed == 0xffff ? 0 : speed,
			configured_speed == 0xffff ? 0 : configured_speed);
		first = false;
	}
	closedir(dir);
	appendf(buf, size, pos, "]}");
}

static void append_storage_smart_json(char *buf, size_t size, size_t *pos)
{
	DIR *dir;
	struct dirent *de;
	bool first = true;

	pthread_once(&storage_library_once, init_storage_libraries);
	appendf(buf, size, pos, "[");
	dir = opendir("/sys/class/block");
	if (!dir) {
		appendf(buf, size, pos, "]");
		return;
	}

	while ((de = readdir(dir))) {
		struct storage_space_info space;

		if (!physical_block_device(de->d_name))
			continue;
		space = read_disk_space_info(de->d_name);
		appendf(buf, size, pos, "%s", first ? "" : ",");
		if (!strncmp(de->d_name, "nvme", 4))
			append_nvme_smart_json(buf, size, pos, de->d_name,
					      &space);
		else
			append_ata_smart_json(buf, size, pos, de->d_name,
					     &space);
		first = false;
	}
	closedir(dir);
	appendf(buf, size, pos, "]");
}

static const char *skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
	char pattern[64];
	const char *p;
	size_t len = 0;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (!p)
		return -ENOENT;
	p += strlen(pattern);
	p = strchr(p, ':');
	if (!p)
		return -EINVAL;
	p = skip_ws(p + 1);
	if (*p != '"')
		return -EINVAL;
	p++;

	while (*p && *p != '"' && len + 1 < out_size) {
		if (*p == '\\' && p[1])
			p++;
		out[len++] = *p++;
	}
	if (*p != '"')
		return -EINVAL;

	out[len] = '\0';
	return 0;
}

static int json_get_int(const char *json, const char *key, int *out)
{
	char pattern[64];
	const char *p;
	char *end;
	long value;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (!p)
		return -ENOENT;
	p += strlen(pattern);
	p = strchr(p, ':');
	if (!p)
		return -EINVAL;
	p = skip_ws(p + 1);

	errno = 0;
	value = strtol(p, &end, 10);
	if (errno || end == p || value < INT_MIN || value > INT_MAX)
		return -EINVAL;

	*out = (int)value;
	return 0;
}

static int parse_curve_points(const char *json, struct fan_curve *curve)
{
	const char *p;
	struct curve_point points[MAX_CURVE_POINTS];
	size_t count = 0;

	p = strstr(json, "\"points\"");
	if (!p || !(p = strchr(p, '[')))
		return -EINVAL;
	p++;

	for (;;) {
		char object[128];
		const char *end;
		size_t object_len;
		int temp;
		int pwm;

		p = skip_ws(p);
		if (*p == ']')
			break;
		if (count > 0) {
			if (*p != ',')
				return -EINVAL;
			p = skip_ws(p + 1);
		}
		if (*p != '{' || !(end = strchr(p, '}')))
			return -EINVAL;

		object_len = (size_t)(end - p + 1);
		if (object_len >= sizeof(object))
			return -EINVAL;
		memcpy(object, p, object_len);
		object[object_len] = '\0';

		/* serde_json does not guarantee object key order. Parse temp and
		 * pwm inside the same point object instead of assuming temp first. */
		if (json_get_int(object, "temp", &temp) < 0 ||
		    json_get_int(object, "pwm", &pwm) < 0)
			return -EINVAL;

		if (count >= MAX_CURVE_POINTS || temp < 0 || temp > 130 || pwm < 0 || pwm > PWM_MAX)
			return -EINVAL;
		if (count > 0 && temp <= points[count - 1].temp_c)
			return -EINVAL;

		points[count++] = (struct curve_point){ temp, pwm };
		p = end + 1;
	}

	if (count == 0 || points[count - 1].temp_c > CURVE_FAILSAFE_TEMP_C ||
	    points[count - 1].pwm != PWM_MAX)
		return -EINVAL;

	memcpy(curve->points, points, sizeof(points[0]) * count);
	curve->count = count;
	return 0;
}

static struct fan_curve *select_curve(struct uniwilld *svc, const char *fan)
{
	if (!strcmp(fan, "cpu") || !strcmp(fan, "0") || !strcmp(fan, "main"))
		return &svc->cpu_curve;
	if (!strcmp(fan, "gpu") || !strcmp(fan, "1") || !strcmp(fan, "secondary"))
		return &svc->gpu_curve;
	return NULL;
}

static struct fan_curve *select_source_curve(struct source_control_state *state,
					      const char *fan, bool **valid)
{
	if (!strcmp(fan, "cpu") || !strcmp(fan, "0") || !strcmp(fan, "main")) {
		*valid = &state->cpu_curve_valid;
		return &state->cpu_curve;
	}
	if (!strcmp(fan, "gpu") || !strcmp(fan, "1") || !strcmp(fan, "secondary")) {
		*valid = &state->gpu_curve_valid;
		return &state->gpu_curve;
	}
	return NULL;
}

static void curve_to_json(char *buf, size_t size, size_t *pos, const struct fan_curve *curve)
{
	size_t i;

	appendf(buf, size, pos, "{\"fan\":\"%s\",\"points\":[", curve->name);
	for (i = 0; i < curve->count; i++) {
		appendf(buf, size, pos, "%s{\"temp\":%d,\"pwm\":%d}",
			i ? "," : "", curve->points[i].temp_c, curve->points[i].pwm);
	}
	appendf(buf, size, pos, "]}");
}

static void make_error(char *resp, size_t size, int err, const char *msg)
{
	snprintf(resp, size, "{\"ok\":false,\"error\":%d,\"message\":\"%s\"}\n", -err, msg);
}

static int find_battery_path(const char *requested_name, char *path, size_t path_size,
			     char *name, size_t name_size)
{
	DIR *dir;
	struct dirent *de;
	int ret = -ENOENT;

	dir = opendir("/sys/class/power_supply");
	if (!dir)
		return -errno;

	while ((de = readdir(dir))) {
		char base[PATH_MAX];
		char type_path[PATH_MAX];
		char type[64];

		if (de->d_name[0] == '.')
			continue;
		if (requested_name && requested_name[0] && strcmp(requested_name, de->d_name))
			continue;

		join_path(base, sizeof(base), "/sys/class/power_supply", de->d_name);
		join_path(type_path, sizeof(type_path), base, "type");
		if (read_text(type_path, type, sizeof(type)) < 0 || strcmp(type, "Battery"))
			continue;

		snprintf(path, path_size, "%s", base);
		if (name_size) {
			size_t len = strnlen(de->d_name, name_size - 1);

			memcpy(name, de->d_name, len);
			name[len] = '\0';
		}
		ret = 0;
		break;
	}

	closedir(dir);
	return ret;
}

static void battery_attr_to_json(char *buf, size_t size, size_t *pos, const char *base,
				 const char *name, const char *json_name, bool numeric)
{
	char path[PATH_MAX];
	char value[128];
	char escaped[sizeof(value) * 2];

	join_path(path, sizeof(path), base, name);
	if (read_text(path, value, sizeof(value)) < 0) {
		appendf(buf, size, pos, ",\"%s\":null", json_name);
		return;
	}

	if (numeric)
		appendf(buf, size, pos, ",\"%s\":%ld", json_name, strtol(value, NULL, 10));
	else {
		json_escape(escaped, sizeof(escaped), value);
		appendf(buf, size, pos, ",\"%s\":\"%s\"", json_name, escaped);
	}
}

static int endpoint_read(struct uniwilld *svc, const char *name, char *value, size_t size)
{
	int err;

	pthread_rwlock_rdlock(&svc->state_lock);
	struct endpoint *ep = find_endpoint(svc, name);
	if (!ep)
		err = -ENOENT;
	else if (ep->read_err < 0)
		err = ep->read_err;
	else {
		size_t len = size > 0 ? strnlen(ep->value, size - 1) : 0;

		if (size > 0) {
			memcpy(value, ep->value, len);
			value[len] = '\0';
		}
		err = 0;
	}
	pthread_rwlock_unlock(&svc->state_lock);

	if (!ep)
		return -ENOENT;

	return err;
}

static int endpoint_write_string(struct uniwilld *svc, const char *name, const char *value)
{
	char path[PATH_MAX];
	int err;

	pthread_rwlock_rdlock(&svc->state_lock);
	struct endpoint *ep = find_endpoint(svc, name);
	if (!ep) {
		pthread_rwlock_unlock(&svc->state_lock);
		return -ENOENT;
	}
	if (!ep->writable) {
		pthread_rwlock_unlock(&svc->state_lock);
		return -EACCES;
	}
	snprintf(path, sizeof(path), "%s", ep->path);
	pthread_rwlock_unlock(&svc->state_lock);

	pthread_mutex_lock(&svc->hardware_lock);
	err = write_text(path, value);
	pthread_mutex_unlock(&svc->hardware_lock);
	if (err < 0)
		return err;

	/* fan_mode needs an immediate hardware readback because benchmark mode is
	 * encoded as performance plus a separate boost bit. Other direct-value
	 * attributes can use the accepted value and be verified by the monitor. */
	if (!strcmp(name, "fan_mode"))
		return refresh_endpoint_cache_by_name(svc, name);

	cache_endpoint_value(svc, name, value);
	return 0;
}

static int endpoint_write_int(struct uniwilld *svc, const char *name, int value)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%d\n", value);
	return endpoint_write_string(svc, name, buf);
}

static int endpoint_write_bool(struct uniwilld *svc, const char *name, int value)
{
	return endpoint_write_int(svc, name, !!value);
}

static const char *keyboard_effect_name(int effect)
{
	static const char *const names[KEYBOARD_EFFECT_COUNT] = {
		"solid", "breathing", "wave", "random", "rainbow",
		"ripple", "marquee", "raindrop", "aurora", "fireworks",
	};

	if (effect < 0 || effect >= KEYBOARD_EFFECT_COUNT)
		return "solid";
	return names[effect];
}

static int keyboard_effect_value(const char *name)
{
	for (int effect = 0; effect < KEYBOARD_EFFECT_COUNT; effect++)
		if (!strcmp(name, keyboard_effect_name(effect)))
			return effect;
	return -EINVAL;
}

static const char *keyboard_direction_name(int direction)
{
	switch (direction) {
	case 1:
		return "right";
	case 2:
		return "left";
	case 3:
		return "up";
	case 4:
		return "down";
	default:
		return "left";
	}
}

static int keyboard_direction_value(const char *name)
{
	if (!strcmp(name, "right"))
		return 1;
	if (!strcmp(name, "left"))
		return 2;
	if (!strcmp(name, "up"))
		return 3;
	if (!strcmp(name, "down"))
		return 4;
	return -EINVAL;
}

static void import_current_lightbar_state(struct uniwilld *svc)
{
	char brightness[64] = "";
	char color[64] = "";
	char rainbow[64] = "";
	char breathing[64] = "";
	int red;
	int green;
	int blue;

	endpoint_read(svc, "lightbar_brightness", brightness, sizeof(brightness));
	endpoint_read(svc, "lightbar_multi_intensity", color, sizeof(color));
	endpoint_read(svc, "rainbow_animation", rainbow, sizeof(rainbow));
	endpoint_read(svc, "breathing_in_suspend", breathing, sizeof(breathing));

	pthread_rwlock_wrlock(&svc->state_lock);
	svc->lightbar_enabled = atoi(brightness) > 0;
	for (int source = 0; source < POWER_SOURCE_COUNT; source++) {
		if (atoi(brightness) > 0)
			svc->lightbar[source].brightness = atoi(brightness);
		if (sscanf(color, "%d %d %d", &red, &green, &blue) == 3) {
			svc->lightbar[source].red = red;
			svc->lightbar[source].green = green;
			svc->lightbar[source].blue = blue;
		}
		svc->lightbar[source].rainbow = atoi(rainbow) > 0;
		svc->lightbar[source].breathing = atoi(breathing) > 0;
	}
	pthread_rwlock_unlock(&svc->state_lock);
}

static void import_current_keyboard_light_state(struct uniwilld *svc)
{
	char brightness[64] = "";
	char color[64] = "";
	char effect[64] = "";
	char speed[64] = "";
	char direction[64] = "";
	char reactive[64] = "";
	unsigned int rgb;
	int parsed_effect;
	int parsed_direction;

	endpoint_read(svc, "keyboard_backlight_brightness", brightness,
		      sizeof(brightness));
	if (endpoint_read(svc, "keyboard_backlight_effect_color", color,
			  sizeof(color)) < 0)
		endpoint_read(svc, "keyboard_backlight_color", color, sizeof(color));
	endpoint_read(svc, "keyboard_backlight_effect", effect, sizeof(effect));
	endpoint_read(svc, "keyboard_backlight_effect_speed", speed, sizeof(speed));
	endpoint_read(svc, "keyboard_backlight_effect_direction", direction,
		      sizeof(direction));
	endpoint_read(svc, "keyboard_backlight_effect_reactive", reactive,
		      sizeof(reactive));
	parsed_effect = keyboard_effect_value(effect);
	parsed_direction = keyboard_direction_value(direction);

	pthread_rwlock_wrlock(&svc->state_lock);
	svc->keyboard_light_enabled = atoi(brightness) > 0;
	for (int source = 0; source < POWER_SOURCE_COUNT; source++) {
		struct keyboard_light_source_state *state =
			&svc->keyboard_light[source];

		if (atoi(brightness) > 0 && atoi(brightness) <= KEYBOARD_BRIGHTNESS_MAX)
			state->brightness = atoi(brightness);
		if (sscanf(color, "%x", &rgb) == 1 && rgb <= 0xffffff) {
			state->red = (rgb >> 16) & 0xff;
			state->green = (rgb >> 8) & 0xff;
			state->blue = rgb & 0xff;
		}
		if (parsed_effect >= 0)
			state->effect = parsed_effect;
		if (atoi(speed) >= 0 && atoi(speed) <= KEYBOARD_EFFECT_SPEED_MAX)
			state->speed = atoi(speed);
		if (parsed_direction >= 0)
			state->direction = parsed_direction;
		state->reactive = atoi(reactive) > 0;
	}
	pthread_rwlock_unlock(&svc->state_lock);
}

static int parse_profile_value(const char *req, int *profile)
{
	char mode[32];

	if (json_get_int(req, "profile", profile) == 0 || json_get_int(req, "value", profile) == 0)
		return 0;

	if (json_get_string(req, "mode", mode, sizeof(mode)) < 0 &&
	    json_get_string(req, "value", mode, sizeof(mode)) < 0)
		return -EINVAL;

	if (!strcmp(mode, "performance") || !strcmp(mode, "perf")) {
		*profile = 1;
		return 0;
	}
	if (!strcmp(mode, "standard") || !strcmp(mode, "balanced") || !strcmp(mode, "balance")) {
		*profile = 2;
		return 0;
	}
	if (!strcmp(mode, "quiet") || !strcmp(mode, "silent")) {
		*profile = 3;
		return 0;
	}

	return -EINVAL;
}

static const char *profile_name(const char *profile)
{
	if (!strcmp(profile, "1"))
		return "performance";
	if (!strcmp(profile, "2"))
		return "standard";
	if (!strcmp(profile, "3"))
		return "quiet";
	return "unknown";
}

static const char *profile_system_power_mode(int profile)
{
	switch (profile) {
	case 1:
		return "performance";
	case 2:
		return "balanced";
	case 3:
		return "power-saver";
	default:
		return NULL;
	}
}

static const char *profile_energy_performance_preference(int profile)
{
	switch (profile) {
	case 1:
		return "balance_performance";
	case 2:
		return "balance_power";
	case 3:
		return "power";
	default:
		return NULL;
	}
}

static int apply_cpu_frequency_policy_at(const char *base_path, int profile)
{
	const char *epp = profile_energy_performance_preference(profile);
	DIR *dir;
	struct dirent *de;
	int first_error = 0;

	if (!epp)
		return -EINVAL;

	dir = opendir(base_path);
	if (!dir)
		return errno == ENOENT ? 0 : -errno;

	while ((de = readdir(dir))) {
		char policy_path[PATH_MAX];
		char path[PATH_MAX];
		char value[64];
		long minimum = CPU_MIN_FREQUENCY_KHZ;
		long maximum;
		int ret;

		if (!has_prefix(de->d_name, "policy"))
			continue;
		if (join_path(policy_path, sizeof(policy_path), base_path, de->d_name) < 0)
			continue;

		join_path(path, sizeof(path), policy_path, "cpuinfo_min_freq");
		if (read_text(path, value, sizeof(value)) == 0) {
			long hardware_minimum = strtol(value, NULL, 10);

			if (hardware_minimum > minimum)
				minimum = hardware_minimum;
		}

		join_path(path, sizeof(path), policy_path, "scaling_max_freq");
		if (read_text(path, value, sizeof(value)) == 0) {
			maximum = strtol(value, NULL, 10);
			if (maximum > 0 && minimum > maximum)
				minimum = maximum;
		}

		join_path(path, sizeof(path), policy_path, "scaling_min_freq");
		ret = write_int(path, (int)minimum);
		if (ret < 0 && !first_error)
			first_error = ret;

		join_path(path, sizeof(path), policy_path,
			  "energy_performance_preference");
		if (path_exists(path)) {
			ret = write_text(path, epp);
			if (ret < 0 && !first_error)
				first_error = ret;
		}
	}

	closedir(dir);
	return first_error;
}

static int apply_cpu_frequency_policy(int profile)
{
	return apply_cpu_frequency_policy_at(CPUFREQ_POLICY_PATH, profile);
}

static const char *level_name(int level)
{
	switch (level) {
	case 1:
		return "performance";
	case 2:
		return "balanced";
	case 3:
		return "battery_saver";
	default:
		return "unknown";
	}
}

static const char *fan_level_name(int level)
{
	switch (level) {
	case 1:
		return "performance";
	case 2:
		return "standard";
	case 3:
		return "quiet";
	default:
		return "unknown";
	}
}

static bool json_has_key(const char *req, const char *key)
{
	char pattern[64];

	if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern))
		return false;
	return strstr(req, pattern) != NULL;
}

static int parse_level_field(const char *req, const char *key, int *level)
{
	char value[32];

	if (json_get_int(req, key, level) == 0)
		return *level >= 1 && *level <= 3 ? 0 : -EINVAL;
	if (json_get_string(req, key, value, sizeof(value)) < 0)
		return -EINVAL;

	if (!strcmp(value, "performance") || !strcmp(value, "perf") ||
	    !strcmp(value, "high_performance") || !strcmp(value, "high-performance"))
		*level = 1;
	else if (!strcmp(value, "balanced") || !strcmp(value, "balance") ||
		 !strcmp(value, "standard"))
		*level = 2;
	else if (!strcmp(value, "battery_saver") || !strcmp(value, "battery-saver") ||
		 !strcmp(value, "power_saver") || !strcmp(value, "power-saver") ||
		 !strcmp(value, "quiet") || !strcmp(value, "silent"))
		*level = 3;
	else
		return -EINVAL;
	return 0;
}

static int parse_power_source_value(const char *req, int *source)
{
	char value[32];

	if (json_get_string(req, "source", value, sizeof(value)) < 0)
		return -EINVAL;
	if (!strcmp(value, "ac") || !strcmp(value, "adapter") || !strcmp(value, "mains")) {
		*source = POWER_SOURCE_AC;
		return 0;
	}
	if (!strcmp(value, "battery") || !strcmp(value, "dc")) {
		*source = POWER_SOURCE_BATTERY;
		return 0;
	}
	return -EINVAL;
}

static void append_profile_state_json(struct uniwilld *svc, char *buf, size_t size, size_t *pos)
{
	pthread_rwlock_rdlock(&svc->state_lock);
	appendf(buf, size, pos,
		"{\"active_profile\":%d,\"active_profile_name\":\"%s\","
		"\"active_source\":\"%s\",\"profiles\":[",
		svc->active_profile, level_name(svc->active_profile),
		svc->active_power_source < 0 ? "unknown" :
		power_source_name(svc->active_power_source));
	for (int profile = 1; profile <= PROFILE_COUNT; profile++) {
		struct profile_branch ac =
			svc->profiles[profile - 1].branch[POWER_SOURCE_AC];
		struct profile_branch battery =
			svc->profiles[profile - 1].branch[POWER_SOURCE_BATTERY];

		appendf(buf, size, pos,
			"%s{\"profile\":%d,\"name\":\"%s\","
			"\"ac\":{\"power_mode\":%d,\"power_mode_name\":\"%s\","
			"\"fan_mode\":%d,\"fan_mode_name\":\"%s\"},"
			"\"battery\":{\"power_mode\":%d,\"power_mode_name\":\"%s\","
			"\"fan_mode\":%d,\"fan_mode_name\":\"%s\"}}",
			profile == 1 ? "" : ",", profile, level_name(profile),
			ac.power_mode, level_name(ac.power_mode), ac.fan_mode,
			fan_level_name(ac.fan_mode),
			battery.power_mode, level_name(battery.power_mode),
			battery.fan_mode, fan_level_name(battery.fan_mode));
	}
	appendf(buf, size, pos, "]}");
	pthread_rwlock_unlock(&svc->state_lock);
}

static int parse_system_power_mode_value(const char *req, char *mode, size_t mode_size)
{
	char value[32];

	if (json_get_string(req, "mode", value, sizeof(value)) < 0 &&
	    json_get_string(req, "value", value, sizeof(value)) < 0)
		return -EINVAL;

	if (!strcmp(value, "performance") || !strcmp(value, "perf") ||
	    !strcmp(value, "high-performance") || !strcmp(value, "high_performance")) {
		snprintf(mode, mode_size, "performance");
		return 0;
	}
	if (!strcmp(value, "balanced") || !strcmp(value, "balance") ||
	    !strcmp(value, "standard")) {
		snprintf(mode, mode_size, "balanced");
		return 0;
	}
	if (!strcmp(value, "power-saver") || !strcmp(value, "powersave") ||
	    !strcmp(value, "power_saver") || !strcmp(value, "quiet") ||
	    !strcmp(value, "silent") || !strcmp(value, "save-power")) {
		snprintf(mode, mode_size, "power-saver");
		return 0;
	}

	return -EINVAL;
}

static int dbus_set_system_power_mode(const char *mode)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus *bus = NULL;
	int ret;

	ret = sd_bus_open_system(&bus);
	if (ret < 0)
		return ret;

	ret = sd_bus_set_property(bus,
				  POWER_PROFILES_BUS_NAME,
				  POWER_PROFILES_OBJECT_PATH,
				  POWER_PROFILES_INTERFACE,
				  "ActiveProfile",
				  &error,
				  "s",
				  mode);

	sd_bus_error_free(&error);
	sd_bus_unref(bus);
	return ret;
}

static int dbus_read_system_power_mode(char *mode, size_t mode_size)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus *bus = NULL;
	char *active_profile = NULL;
	int ret;

	ret = sd_bus_open_system(&bus);
	if (ret < 0)
		return ret;

	ret = sd_bus_get_property_string(bus,
					 POWER_PROFILES_BUS_NAME,
					 POWER_PROFILES_OBJECT_PATH,
					 POWER_PROFILES_INTERFACE,
					 "ActiveProfile",
					 &error,
					 &active_profile);
	if (ret >= 0)
		snprintf(mode, mode_size, "%s", active_profile);

	free(active_profile);
	sd_bus_error_free(&error);
	sd_bus_unref(bus);
	return ret;
}

static const char *power_source_name(int source)
{
	return source == POWER_SOURCE_AC ? "ac" : "battery";
}

static int read_power_source(int *source)
{
	DIR *dir;
	struct dirent *de;
	bool found_external = false;
	bool battery_discharging = false;
	bool found_battery = false;

	dir = opendir("/sys/class/power_supply");
	if (!dir)
		return -errno;

	while ((de = readdir(dir))) {
		char base[PATH_MAX];
		char path[PATH_MAX];
		char type[64];
		char value[64];

		if (de->d_name[0] == '.')
			continue;
		join_path(base, sizeof(base), "/sys/class/power_supply", de->d_name);
		join_path(path, sizeof(path), base, "type");
		if (read_text(path, type, sizeof(type)) < 0)
			continue;

		if (!strcmp(type, "Battery")) {
			found_battery = true;
			join_path(path, sizeof(path), base, "status");
			if (read_text(path, value, sizeof(value)) == 0 &&
			    !strcmp(value, "Discharging"))
				battery_discharging = true;
			continue;
		}

		join_path(path, sizeof(path), base, "online");
		if (read_text(path, value, sizeof(value)) < 0)
			continue;
		found_external = true;
		if (atoi(value) > 0) {
			closedir(dir);
			*source = POWER_SOURCE_AC;
			return 0;
		}
	}

	closedir(dir);
	if (found_external || found_battery) {
		*source = battery_discharging ? POWER_SOURCE_BATTERY : POWER_SOURCE_AC;
		return 0;
	}
	return -ENOENT;
}

static int apply_lightbar_source(struct uniwilld *svc, int source)
{
	struct lightbar_source_state config;
	char rgb[64];
	char max_value[64];
	bool globally_enabled;
	int max_brightness = LIGHTBAR_CONFIG_MAX;
	int brightness;
	int ret;

	if (source < 0 || source >= POWER_SOURCE_COUNT)
		return -EINVAL;

	pthread_rwlock_rdlock(&svc->state_lock);
	config = svc->lightbar[source];
	globally_enabled = svc->lightbar_enabled;
	pthread_rwlock_unlock(&svc->state_lock);

	if (endpoint_read(svc, "lightbar_max_brightness", max_value,
			  sizeof(max_value)) == 0) {
		int discovered_max = atoi(max_value);

		if (discovered_max > 0)
			max_brightness = discovered_max;
	}
	brightness = config.brightness < max_brightness ?
		config.brightness : max_brightness;

	snprintf(rgb, sizeof(rgb), "%d %d %d\n",
		 config.red, config.green, config.blue);
	ret = endpoint_write_string(svc, "lightbar_multi_intensity", rgb);
	if (ret < 0)
		return ret;
	ret = endpoint_write_bool(svc, "breathing_in_suspend", config.breathing);
	if (ret < 0)
		return ret;
	ret = endpoint_write_bool(svc, "rainbow_animation", config.rainbow);
	if (ret < 0)
		return ret;
	ret = endpoint_write_int(svc, "lightbar_brightness",
				 globally_enabled && config.enabled ? brightness : 0);
	if (ret < 0)
		return ret;

	/*
	 * A non-zero LED-class brightness write selects solid mode. Restore the
	 * requested effect after brightness, while leaving brightness last for
	 * disabled states so no effect write can turn the bar back on.
	 */
	if (globally_enabled && config.enabled) {
		ret = endpoint_write_bool(svc, "breathing_in_suspend", config.breathing);
		if (ret < 0)
			return ret;
		ret = endpoint_write_bool(svc, "rainbow_animation", config.rainbow);
		if (ret < 0)
			return ret;
	}

	pthread_rwlock_wrlock(&svc->state_lock);
	svc->last_applied_lightbar_source = source;
	pthread_rwlock_unlock(&svc->state_lock);
	return 0;
}

static void append_lightbar_state_json(struct uniwilld *svc, char *buf, size_t size,
				       size_t *pos)
{
	pthread_rwlock_rdlock(&svc->state_lock);
	appendf(buf, size, pos, "{\"global_enabled\":%s,\"active_source\":\"%s\","
		"\"sources\":{",
		svc->lightbar_enabled ? "true" : "false",
		svc->active_power_source < 0 ? "unknown" :
		power_source_name(svc->active_power_source));
	for (int source = 0; source < POWER_SOURCE_COUNT; source++) {
		struct lightbar_source_state state = svc->lightbar[source];

		appendf(buf, size, pos,
			"%s\"%s\":{\"enabled\":%s,\"brightness\":%d,"
			"\"red\":%d,\"green\":%d,\"blue\":%d,"
			"\"rainbow\":%s,\"breathing\":%s}",
			source ? "," : "", power_source_name(source),
			state.enabled ? "true" : "false", state.brightness,
			state.red, state.green, state.blue,
			state.rainbow ? "true" : "false",
			state.breathing ? "true" : "false");
	}
	appendf(buf, size, pos, "}}");
	pthread_rwlock_unlock(&svc->state_lock);
}

static int apply_keyboard_light_source(struct uniwilld *svc, int source)
{
	struct keyboard_light_source_state config;
	char color[16];
	bool globally_enabled;
	int ret;

	if (source < 0 || source >= POWER_SOURCE_COUNT)
		return -EINVAL;

	pthread_rwlock_rdlock(&svc->state_lock);
	config = svc->keyboard_light[source];
	globally_enabled = svc->keyboard_light_enabled;
	pthread_rwlock_unlock(&svc->state_lock);

	if (!globally_enabled || !config.enabled) {
		ret = endpoint_write_int(svc, "keyboard_backlight_brightness", 0);
		if (ret < 0)
			return ret;
		goto applied;
	}

	snprintf(color, sizeof(color), "%02x%02x%02x\n",
		 config.red, config.green, config.blue);
	ret = endpoint_write_string(svc, "keyboard_backlight_effect_color", color);
	if (ret < 0)
		return ret;
	ret = endpoint_write_int(svc, "keyboard_backlight_effect_speed", config.speed);
	if (ret < 0)
		return ret;
	ret = endpoint_write_string(svc, "keyboard_backlight_effect_direction",
				    keyboard_direction_name(config.direction));
	if (ret < 0)
		return ret;
	ret = endpoint_write_bool(svc, "keyboard_backlight_effect_reactive",
				  config.reactive);
	if (ret < 0)
		return ret;
	ret = endpoint_write_int(svc, "keyboard_backlight_brightness",
				 config.brightness);
	if (ret < 0)
		return ret;
	ret = endpoint_write_string(svc, "keyboard_backlight_effect",
				    keyboard_effect_name(config.effect));
	if (ret < 0)
		return ret;

applied:
	pthread_rwlock_wrlock(&svc->state_lock);
	svc->last_applied_keyboard_light_source = source;
	pthread_rwlock_unlock(&svc->state_lock);
	return 0;
}

static void append_keyboard_light_state_json(struct uniwilld *svc, char *buf,
					     size_t size, size_t *pos)
{
	char probe[64];
	bool available = endpoint_read(svc, "keyboard_backlight_effect", probe,
				       sizeof(probe)) == 0;

	pthread_rwlock_rdlock(&svc->state_lock);
	appendf(buf, size, pos,
		"{\"available\":%s,\"global_enabled\":%s,\"active_source\":\"%s\","
		"\"sources\":{",
		available ? "true" : "false",
		svc->keyboard_light_enabled ? "true" : "false",
		svc->active_power_source < 0 ? "unknown" :
		power_source_name(svc->active_power_source));
	for (int source = 0; source < POWER_SOURCE_COUNT; source++) {
		struct keyboard_light_source_state state =
			svc->keyboard_light[source];

		appendf(buf, size, pos,
			"%s\"%s\":{\"enabled\":%s,\"brightness\":%d,"
			"\"red\":%d,\"green\":%d,\"blue\":%d,"
			"\"effect\":\"%s\",\"speed\":%d,\"direction\":\"%s\","
			"\"reactive\":%s}",
			source ? "," : "", power_source_name(source),
			state.enabled ? "true" : "false", state.brightness,
			state.red, state.green, state.blue,
			keyboard_effect_name(state.effect), state.speed,
			keyboard_direction_name(state.direction),
			state.reactive ? "true" : "false");
	}
	appendf(buf, size, pos, "}}");
	pthread_rwlock_unlock(&svc->state_lock);
}

static int apply_active_profile_inner(struct uniwilld *svc, bool force)
{
	struct profile_branch branch;
	char value[64];
	int indicated_mode;
	int profile;
	int source;
	int persist_err = 0;
	int ret;
	bool apply_hardware;
	bool curve_control;
	bool curve_control_active;
	bool fan_mode_changed;
	bool apply_lightbar;
	bool apply_keyboard_light;
	bool apply_passive = false;
	bool passive_cooling = true;

	ret = refresh_endpoint_cache_by_name(svc, "performance_profile");
	if (ret < 0)
		return ret;
	ret = endpoint_read(svc, "performance_profile", value, sizeof(value));
	if (ret < 0)
		return ret;
	indicated_mode = atoi(value);
	if (indicated_mode < 1 || indicated_mode > PROFILE_COUNT)
		return -EINVAL;

	ret = read_power_source(&source);
	if (ret < 0)
		return ret;

	pthread_rwlock_wrlock(&svc->state_lock);
	/*
	 * performance_profile is the chassis mode indicator.  A change which did
	 * not come from our last applied hardware power level is a physical
	 * profile-key request; cycle the selected userspace slot independently
	 * from the indicator value because a slot may store any power level.
	 */
	if (!force && svc->last_synced_power_profile >= 1 &&
	    indicated_mode != svc->last_synced_power_profile) {
		svc->active_profile =
			svc->active_profile >= PROFILE_COUNT ? 1 : svc->active_profile + 1;
		persist_err = save_profile_state_locked(svc);
	}
	profile = svc->active_profile;
	if (profile < 1 || profile > PROFILE_COUNT) {
		pthread_rwlock_unlock(&svc->state_lock);
		return -EINVAL;
	}
	branch = svc->profiles[profile - 1].branch[source];
	if (svc->source_controls[source].fan_mode_valid &&
	    fan_mode_suspends_curve_control(svc->source_controls[source].fan_mode))
		branch.fan_mode = svc->source_controls[source].fan_mode;
	fan_mode_changed = branch.fan_mode != svc->last_applied_fan_mode;
	apply_lightbar = force || source != svc->last_applied_lightbar_source;
	apply_keyboard_light =
		force || source != svc->last_applied_keyboard_light_source;
	apply_hardware = force || profile != svc->last_applied_profile ||
		source != svc->last_applied_power_source || fan_mode_changed;
	if (!apply_hardware && branch.power_mode == svc->last_synced_power_profile) {
		pthread_rwlock_unlock(&svc->state_lock);
		return persist_err;
	}
	svc->active_power_source = source;
	curve_control = svc->fan_curve_control_enabled;
	curve_control_active = curve_control &&
		!fan_mode_suspends_curve_control(branch.fan_mode);
	if (apply_hardware) {
		if (fan_mode_changed)
			load_default_curves(svc, branch.fan_mode);
		if (svc->source_controls[source].cpu_curve_valid)
			svc->cpu_curve = svc->source_controls[source].cpu_curve;
		if (svc->source_controls[source].gpu_curve_valid)
			svc->gpu_curve = svc->source_controls[source].gpu_curve;
		svc->last_cpu_pwm = -1;
		svc->last_gpu_pwm = -1;
		apply_passive = svc->source_controls[source].passive_cooling_valid;
		passive_cooling = svc->source_controls[source].passive_cooling;
	}
	pthread_rwlock_unlock(&svc->state_lock);

	if (apply_hardware) {
		ret = endpoint_write_int(svc, "hardware_power_mode", branch.power_mode);
		if (ret < 0)
			return ret;

		/* Keep the chassis indicator aligned with the power level that was
		 * actually applied, not with the userspace preset slot number. */
		ret = endpoint_write_int(svc, "performance_profile", branch.power_mode);
		if (ret < 0)
			return ret;

		ret = apply_fan_mode_hardware(svc, branch.fan_mode, curve_control);
		if (ret < 0) {
			if (curve_control_active) {
				pthread_rwlock_wrlock(&svc->state_lock);
				svc->fan_curve_control_enabled = false;
				svc->last_cpu_pwm = -1;
				svc->last_gpu_pwm = -1;
				pthread_rwlock_unlock(&svc->state_lock);
				set_auto_fan_mode(svc);
			}
			return ret;
		}
		if (apply_passive) {
			ret = endpoint_write_bool(svc, "passive_cooling", passive_cooling);
			if (ret < 0)
				return ret;
		}

		pthread_rwlock_wrlock(&svc->state_lock);
		svc->last_applied_profile = profile;
		svc->last_applied_power_source = source;
		svc->last_applied_fan_mode = branch.fan_mode;
		pthread_rwlock_unlock(&svc->state_lock);

		if (curve_control_active) {
			ret = fan_control_tick(svc);
			if (ret < 0)
				return ret;
		}
	}

	if (apply_lightbar)
		apply_lightbar_source(svc, source);
	if (apply_keyboard_light)
		apply_keyboard_light_source(svc, source);

	ret = dbus_set_system_power_mode(profile_system_power_mode(branch.power_mode));
	if (ret < 0)
		return ret;

	ret = apply_cpu_frequency_policy(branch.power_mode);
	if (ret < 0)
		return ret;

	pthread_rwlock_wrlock(&svc->state_lock);
	svc->last_synced_power_profile = branch.power_mode;
	pthread_rwlock_unlock(&svc->state_lock);
	refresh_system_power_cache(svc);
	return persist_err;
}

static int apply_active_profile(struct uniwilld *svc, bool force)
{
	int before[5];
	int after[5];
	int ret;

	pthread_rwlock_rdlock(&svc->state_lock);
	before[0] = svc->active_profile;
	before[1] = svc->active_power_source;
	before[2] = svc->last_applied_profile;
	before[3] = svc->last_applied_fan_mode;
	before[4] = svc->last_synced_power_profile;
	pthread_rwlock_unlock(&svc->state_lock);

	pthread_mutex_lock(&svc->profile_apply_lock);
	ret = apply_active_profile_inner(svc, force);
	pthread_mutex_unlock(&svc->profile_apply_lock);

	pthread_rwlock_rdlock(&svc->state_lock);
	after[0] = svc->active_profile;
	after[1] = svc->active_power_source;
	after[2] = svc->last_applied_profile;
	after[3] = svc->last_applied_fan_mode;
	after[4] = svc->last_synced_power_profile;
	pthread_rwlock_unlock(&svc->state_lock);
	if (ret >= 0 && memcmp(before, after, sizeof(before)))
		publish_state_event(svc);
	return ret;
}

static int parse_fan_mode_value(const char *req, int *fan_mode)
{
	char mode[32];

	if (json_get_int(req, "fan_mode", fan_mode) == 0 ||
	    json_get_int(req, "value", fan_mode) == 0)
		return 0;

	if (json_get_string(req, "mode", mode, sizeof(mode)) < 0 &&
	    json_get_string(req, "value", mode, sizeof(mode)) < 0)
		return -EINVAL;

	if (!strcmp(mode, "performance") || !strcmp(mode, "perf")) {
		*fan_mode = 1;
		return 0;
	}
	if (!strcmp(mode, "standard") || !strcmp(mode, "balanced") ||
	    !strcmp(mode, "balance")) {
		*fan_mode = 2;
		return 0;
	}
	if (!strcmp(mode, "quiet") || !strcmp(mode, "silent")) {
		*fan_mode = 3;
		return 0;
	}
	if (!strcmp(mode, "whisper")) {
		*fan_mode = 4;
		return 0;
	}
	if (!strcmp(mode, "benchmark") || !strcmp(mode, "boost")) {
		*fan_mode = 5;
		return 0;
	}

	return -EINVAL;
}

static const char *fan_mode_name(const char *fan_mode)
{
	if (!strcmp(fan_mode, "1"))
		return "performance";
	if (!strcmp(fan_mode, "2"))
		return "standard";
	if (!strcmp(fan_mode, "3"))
		return "quiet";
	if (!strcmp(fan_mode, "4"))
		return "whisper";
	if (!strcmp(fan_mode, "5"))
		return "benchmark";
	return "unknown";
}

static int parse_bool_value(const char *req, const char *key, int *value)
{
	char pattern[64];
	const char *p;
	char text[16];

	if (json_get_int(req, key, value) == 0)
		return 0;
	if (json_get_string(req, key, text, sizeof(text)) < 0)
		goto literal;

	if (!strcmp(text, "true") || !strcmp(text, "on") || !strcmp(text, "1") ||
	    !strcmp(text, "enable") || !strcmp(text, "enabled")) {
		*value = 1;
		return 0;
	}
	if (!strcmp(text, "false") || !strcmp(text, "off") || !strcmp(text, "0") ||
	    !strcmp(text, "disable") || !strcmp(text, "disabled")) {
		*value = 0;
		return 0;
	}

	return -EINVAL;

literal:
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(req, pattern);
	if (!p)
		return -EINVAL;
	p = strchr(p + strlen(pattern), ':');
	if (!p)
		return -EINVAL;
	p = skip_ws(p + 1);

	if (!strncmp(p, "true", 4)) {
		*value = 1;
		return 0;
	}
	if (!strncmp(p, "false", 5)) {
		*value = 0;
		return 0;
	}

	return -EINVAL;
}

static void handle_simple_bool(struct uniwilld *svc, const char *req, char *resp,
			       size_t resp_size, const char *endpoint,
			       const char *field, bool set)
{
	char value[64];
	int bool_value;
	int err;

	if (set) {
		if (parse_bool_value(req, "value", &bool_value) < 0) {
			make_error(resp, resp_size, -EINVAL, "missing or invalid value");
			return;
		}
		err = endpoint_write_bool(svc, endpoint, bool_value);
		if (err < 0) {
			make_error(resp, resp_size, err, "write failed");
			return;
		}
	}

	err = endpoint_read(svc, endpoint, value, sizeof(value));
	if (err < 0) {
		make_error(resp, resp_size, err, "read failed");
		return;
	}

	snprintf(resp, resp_size, "{\"ok\":true,\"%s\":%s}\n",
		 field, atoi(value) ? "true" : "false");
	if (set)
		publish_state_event(svc);
}

static void handle_touchpad_toggle(struct uniwilld *svc, const char *req, char *resp,
				   size_t resp_size, bool set, int forced_value)
{
	char value[64];
	int enabled = forced_value;
	int err;

	if (set) {
		if (forced_value < 0 && parse_bool_value(req, "value", &enabled) < 0) {
			make_error(resp, resp_size, -EINVAL, "missing or invalid value");
			return;
		}

		err = endpoint_write_bool(svc, "touchpad_toggle_enable", enabled);
		if (err < 0) {
			make_error(resp, resp_size, err, "write failed");
			return;
		}
	}

	err = endpoint_read(svc, "touchpad_toggle_enable", value, sizeof(value));
	if (err < 0) {
		make_error(resp, resp_size, err, "read failed");
		return;
	}

	snprintf(resp, resp_size,
		 "{\"ok\":true,\"touchpad_toggle_enable\":%s}\n",
		 atoi(value) ? "true" : "false");
}

static int restore_saved_fan_control(struct uniwilld *svc)
{
	bool restore_curve_control;
	int err;

	pthread_rwlock_rdlock(&svc->state_lock);
	if (!svc->fan_boost_restore_valid) {
		pthread_rwlock_unlock(&svc->state_lock);
		return 0;
	}

	restore_curve_control = svc->fan_boost_restore_curve_control;
	pthread_rwlock_unlock(&svc->state_lock);

	if (restore_curve_control)
		err = set_manual_fan_mode(svc);
	else
		err = set_auto_fan_mode(svc);
	if (err < 0)
		return err;

	pthread_rwlock_wrlock(&svc->state_lock);
	svc->fan_curve_control_enabled = restore_curve_control;
	svc->fan_boost_restore_valid = false;
	pthread_rwlock_unlock(&svc->state_lock);
	return 0;
}

static void handle_fan_boost(struct uniwilld *svc, const char *req, char *resp,
			     size_t resp_size, bool set)
{
	char value[64];
	int bool_value;
	int err;

	if (set) {
		if (parse_bool_value(req, "value", &bool_value) < 0) {
			make_error(resp, resp_size, -EINVAL, "missing or invalid value");
			return;
		}

		if (bool_value) {
			err = endpoint_read(svc, "fan_boost", value, sizeof(value));
			if (err < 0) {
				make_error(resp, resp_size, err, "read failed");
				return;
			}

			if (!atoi(value)) {
				pthread_rwlock_wrlock(&svc->state_lock);
				svc->fan_boost_restore_curve_control = svc->fan_curve_control_enabled;
				svc->fan_boost_restore_valid = true;
				pthread_rwlock_unlock(&svc->state_lock);
			}

			pthread_rwlock_wrlock(&svc->state_lock);
			svc->fan_curve_control_enabled = false;
			pthread_rwlock_unlock(&svc->state_lock);
			err = set_auto_fan_mode(svc);
			if (err < 0) {
				pthread_rwlock_wrlock(&svc->state_lock);
				svc->fan_curve_control_enabled = svc->fan_boost_restore_curve_control;
				pthread_rwlock_unlock(&svc->state_lock);
				make_error(resp, resp_size, err, "failed to switch fan control to auto");
				return;
			}

			err = endpoint_write_bool(svc, "fan_boost", 1);
			if (err < 0) {
				restore_saved_fan_control(svc);
				make_error(resp, resp_size, err, "failed to enable fan boost");
				return;
			}
		} else {
			err = endpoint_write_bool(svc, "fan_boost", 0);
			if (err < 0) {
				make_error(resp, resp_size, err, "failed to disable fan boost");
				return;
			}

			err = restore_saved_fan_control(svc);
			if (err < 0) {
				make_error(resp, resp_size, err, "fan boost disabled but restore failed");
				return;
			}
		}
	}

	err = endpoint_read(svc, "fan_boost", value, sizeof(value));
	if (err < 0) {
		make_error(resp, resp_size, err, "read failed");
		return;
	}

	pthread_rwlock_rdlock(&svc->state_lock);
	snprintf(resp, resp_size,
		 "{\"ok\":true,\"fan_boost\":%s,\"fan_control\":\"%s\","
		 "\"restore_pending\":%s,\"restore_mode\":\"%s\"}\n",
		 atoi(value) ? "true" : "false",
		 svc->fan_curve_control_enabled ? "manual" : "auto",
		 svc->fan_boost_restore_valid ? "true" : "false",
		 svc->fan_boost_restore_curve_control ? "manual" : "auto");
	pthread_rwlock_unlock(&svc->state_lock);
}

static void append_battery_json(char *buf, size_t size, size_t *pos,
				const char *base, const char *battery_name)
{
	appendf(buf, size, pos, "{\"name\":\"%s\"", battery_name);
	battery_attr_to_json(buf, size, pos, base, "manufacturer", "manufacturer", false);
	battery_attr_to_json(buf, size, pos, base, "model_name", "model_name", false);
	battery_attr_to_json(buf, size, pos, base, "serial_number", "serial_number", false);
	battery_attr_to_json(buf, size, pos, base, "technology", "technology", false);
	battery_attr_to_json(buf, size, pos, base, "status", "status", false);
	battery_attr_to_json(buf, size, pos, base, "capacity", "capacity_percent", true);
	battery_attr_to_json(buf, size, pos, base, "health", "health", false);
	battery_attr_to_json(buf, size, pos, base, "cycle_count", "cycle_count", true);
	battery_attr_to_json(buf, size, pos, base, "charge_control_end_threshold",
			     "charge_control_end_threshold", true);
	battery_attr_to_json(buf, size, pos, base, "energy_now", "energy_now_uwh", true);
	battery_attr_to_json(buf, size, pos, base, "energy_full", "energy_full_uwh", true);
	battery_attr_to_json(buf, size, pos, base, "energy_full_design",
			     "energy_full_design_uwh", true);
	battery_attr_to_json(buf, size, pos, base, "charge_now", "charge_now_uah", true);
	battery_attr_to_json(buf, size, pos, base, "charge_full", "charge_full_uah", true);
	battery_attr_to_json(buf, size, pos, base, "charge_full_design",
			     "charge_full_design_uah", true);
	battery_attr_to_json(buf, size, pos, base, "voltage_now", "voltage_now_uv", true);
	battery_attr_to_json(buf, size, pos, base, "voltage_min_design",
			     "voltage_min_design_uv", true);
	battery_attr_to_json(buf, size, pos, base, "power_now", "power_now_uw", true);
	battery_attr_to_json(buf, size, pos, base, "current_now", "current_now_ua", true);
	appendf(buf, size, pos, "}");
}

static void append_dashboard_snapshot_json(struct uniwilld *svc, char *buf,
					   size_t size, size_t *pos)
{
	char temp1[64] = "";
	char temp2[64] = "";
	char fan1[64] = "";
	char fan2[64] = "";
	char pwm1[64] = "";
	char pwm2[64] = "";
	char performance_profile[64] = "";
	char passive[64] = "";
	char fn_lock[64] = "";
	char super_key[64] = "";
	char touchpad_toggle[64] = "";
	char lightbar_brightness[64] = "";
	char lightbar_max[64] = "";
	char lightbar_color[64] = "";
	char rainbow[64] = "";
	char breathing[64] = "";
	char system_power_mode[64] = "";
	int temp1_err = endpoint_read(svc, "temp1_input", temp1, sizeof(temp1));
	int temp2_err = endpoint_read(svc, "temp2_input", temp2, sizeof(temp2));
	int fan1_err = endpoint_read(svc, "fan1_input", fan1, sizeof(fan1));
	int fan2_err = endpoint_read(svc, "fan2_input", fan2, sizeof(fan2));
	int pwm1_err = endpoint_read(svc, "pwm1", pwm1, sizeof(pwm1));
	int pwm2_err = endpoint_read(svc, "pwm2", pwm2, sizeof(pwm2));
	int profile_err = endpoint_read(svc, "performance_profile",
					performance_profile, sizeof(performance_profile));
	int passive_err = endpoint_read(svc, "passive_cooling", passive,
					sizeof(passive));
	int fn_err = endpoint_read(svc, "fn_lock_toggle_enable", fn_lock,
				   sizeof(fn_lock));
	int super_err = endpoint_read(svc, "super_key_toggle_enable", super_key,
				      sizeof(super_key));
	int touchpad_err = endpoint_read(svc, "touchpad_toggle_enable",
					 touchpad_toggle, sizeof(touchpad_toggle));
	int lightbar_brightness_err = endpoint_read(svc, "lightbar_brightness",
						    lightbar_brightness,
						    sizeof(lightbar_brightness));
	int lightbar_max_err = endpoint_read(svc, "lightbar_max_brightness",
					     lightbar_max, sizeof(lightbar_max));
	int lightbar_color_err = endpoint_read(svc, "lightbar_multi_intensity",
					       lightbar_color, sizeof(lightbar_color));
	int rainbow_err = endpoint_read(svc, "rainbow_animation", rainbow,
					sizeof(rainbow));
	int breathing_err = endpoint_read(svc, "breathing_in_suspend", breathing,
					  sizeof(breathing));
	int fan_mode;
	bool curve_control;
	int active_profile;
	int active_source;

	pthread_rwlock_rdlock(&svc->state_lock);
	snprintf(system_power_mode, sizeof(system_power_mode), "%s",
		 svc->system_power_mode);
	fan_mode = active_fan_mode_locked(svc);
	curve_control = svc->fan_curve_control_enabled;
	active_profile = svc->active_profile;
	active_source = svc->active_power_source;
	pthread_rwlock_unlock(&svc->state_lock);

	appendf(buf, size, pos,
		"{\"temp1_input\":%s,\"temp2_input\":%s,"
		"\"fan1_input\":%s,\"fan2_input\":%s,"
		"\"pwm1\":%s,\"pwm2\":%s,\"power_mode\":",
		temp1_err < 0 ? "null" : temp1,
		temp2_err < 0 ? "null" : temp2,
		fan1_err < 0 ? "null" : fan1,
		fan2_err < 0 ? "null" : fan2,
		pwm1_err < 0 ? "null" : pwm1,
		pwm2_err < 0 ? "null" : pwm2);
	if (profile_err < 0)
		appendf(buf, size, pos, "null");
	else
		appendf(buf, size, pos, "\"%s\"", profile_name(performance_profile));
	appendf(buf, size, pos,
		",\"system_power_mode\":\"%s\","
		"\"fan_mode\":\"%s\",\"fan_control\":\"%s\","
		"\"passive_cooling\":%s,"
		"\"active_profile\":%d,\"active_source\":\"%s\","
		"\"fn_lock\":%s,\"super_key_enabled\":%s,"
		"\"touchpad_hotkey_enabled\":%s,\"lightbar\":{"
		"\"brightness\":%s,\"max_brightness\":%s,\"multi_intensity\":",
		system_power_mode,
		fan_level_name(fan_mode),
		fan_mode == FAN_MODE_BENCHMARK ? "benchmark" :
		fan_mode == FAN_MODE_WHISPER ? "whisper" :
		curve_control ? "manual" : "auto",
		passive_err < 0 ? "null" : atoi(passive) ? "true" : "false",
		active_profile,
		active_source < 0 ? "unknown" : power_source_name(active_source),
		fn_err < 0 ? "null" : atoi(fn_lock) ? "true" : "false",
		super_err < 0 ? "null" : atoi(super_key) ? "true" : "false",
		touchpad_err < 0 ? "null" : atoi(touchpad_toggle) ? "true" : "false",
		lightbar_brightness_err < 0 ? "null" : lightbar_brightness,
		lightbar_max_err < 0 ? "null" : lightbar_max);
	if (lightbar_color_err < 0)
		appendf(buf, size, pos, "null");
	else
		appendf(buf, size, pos, "\"%s\"", lightbar_color);
	appendf(buf, size, pos,
		",\"rainbow\":%s,\"breathing\":%s,\"settings\":",
		rainbow_err < 0 ? "null" : atoi(rainbow) ? "true" : "false",
		breathing_err < 0 ? "null" : atoi(breathing) ? "true" : "false");
	append_lightbar_state_json(svc, buf, size, pos);
	appendf(buf, size, pos, "},\"keyboard_backlight\":");
	append_keyboard_light_state_json(svc, buf, size, pos);
	appendf(buf, size, pos, ",\"batteries\":[");

	DIR *dir = opendir("/sys/class/power_supply");
	if (dir) {
		struct dirent *de;
		bool first = true;

		while ((de = readdir(dir))) {
			char battery_path[PATH_MAX];
			char battery_name[64];

			if (de->d_name[0] == '.' ||
			    find_battery_path(de->d_name, battery_path,
					      sizeof(battery_path), battery_name,
					      sizeof(battery_name)) < 0)
				continue;
			appendf(buf, size, pos, "%s", first ? "" : ",");
			append_battery_json(buf, size, pos, battery_path, battery_name);
			first = false;
		}
		closedir(dir);
	}
	appendf(buf, size, pos, "]}");
}

static void handle_request(struct uniwilld *svc, const char *req, char *resp, size_t resp_size)
{
	char cmd[64];
	size_t pos = 0;

	if (json_get_string(req, "cmd", cmd, sizeof(cmd)) < 0) {
		make_error(resp, resp_size, -EINVAL, "missing cmd");
		return;
	}

	if (!strcmp(cmd, "dashboard_snapshot")) {
		appendf(resp, resp_size, &pos, "{\"ok\":true,\"snapshot\":");
		append_dashboard_snapshot_json(svc, resp, resp_size, &pos);
		appendf(resp, resp_size, &pos, "}\n");
		return;
	}

	if (!strcmp(cmd, "get_device_details")) {
		appendf(resp, resp_size, &pos, "{\"ok\":true,\"storage\":");
		append_storage_smart_json(resp, resp_size, &pos);
		appendf(resp, resp_size, &pos, ",\"memory\":");
		append_memory_devices_json(resp, resp_size, &pos);
		appendf(resp, resp_size, &pos, "}\n");
		return;
	}

	if (!strcmp(cmd, "get_version")) {
		appendf(resp, resp_size, &pos,
			"{\"ok\":true,\"name\":\"uniwilld\","
			"\"version\":\"%s\",\"build\":\"%s\","
			"\"full\":\"%s+%s\"}\n",
			UNIWILLD_VERSION, UNIWILLD_BUILD_NUMBER,
			UNIWILLD_VERSION, UNIWILLD_BUILD_NUMBER);
		return;
	}

	if (!strcmp(cmd, "list")) {
		size_t i;

		pthread_rwlock_rdlock(&svc->state_lock);
		appendf(resp, resp_size, &pos, "{\"ok\":true,\"endpoints\":[");
		for (i = 0; i < svc->endpoint_count; i++) {
			appendf(resp, resp_size, &pos,
				"%s{\"name\":\"%s\",\"writable\":%s}",
				i ? "," : "", svc->endpoints[i].name,
				svc->endpoints[i].writable ? "true" : "false");
		}
		appendf(resp, resp_size, &pos, "]}\n");
		pthread_rwlock_unlock(&svc->state_lock);
		return;
	}

	if (!strcmp(cmd, "get") || !strcmp(cmd, "set")) {
		char name[64];
		char value[MAX_LINE];
		char escaped[MAX_LINE * 2];
		int err;

		if (json_get_string(req, "name", name, sizeof(name)) < 0) {
			make_error(resp, resp_size, -EINVAL, "missing name");
			return;
		}

		if (!strcmp(cmd, "set")) {
			int fan_mode = active_fan_mode(svc);

			if (fan_mode_suspends_curve_control(fan_mode) &&
			    is_direct_fan_speed_endpoint(name)) {
				make_error(resp, resp_size, -EBUSY,
					   fan_mode == FAN_MODE_BENCHMARK ?
					   "benchmark mode owns fan speed; use set_fan_mode to leave it" :
					   "WhisperMode owns fan speed; use set_fan_mode to leave it");
				return;
			}
			if (json_get_string(req, "value", value, sizeof(value)) < 0) {
				int int_value;

				if (json_get_int(req, "value", &int_value) < 0) {
					make_error(resp, resp_size, -EINVAL, "missing value");
					return;
				}
				snprintf(value, sizeof(value), "%d\n", int_value);
			}
			err = endpoint_write_string(svc, name, value);
			if (err < 0) {
				make_error(resp, resp_size, err, "write failed");
				return;
			}
		}

		err = endpoint_read(svc, name, value, sizeof(value));
		if (err < 0) {
			make_error(resp, resp_size, err, "read failed");
			return;
		}
		json_escape(escaped, sizeof(escaped), value);
		snprintf(resp, resp_size, "{\"ok\":true,\"name\":\"%s\",\"value\":\"%s\"}\n",
			 name, escaped);
		return;
	}

	if (!strcmp(cmd, "get_all")) {
		size_t i;
		bool first = true;

		pthread_rwlock_rdlock(&svc->state_lock);
		appendf(resp, resp_size, &pos, "{\"ok\":true,\"values\":{");
		for (i = 0; i < svc->endpoint_count; i++) {
			char escaped[MAX_LINE * 2];

			if (svc->endpoints[i].read_err < 0)
				continue;
			json_escape(escaped, sizeof(escaped), svc->endpoints[i].value);
			appendf(resp, resp_size, &pos, "%s\"%s\":\"%s\"",
				first ? "" : ",",
				svc->endpoints[i].name, escaped);
			first = false;
		}
		appendf(resp, resp_size, &pos, "}}\n");
		pthread_rwlock_unlock(&svc->state_lock);
		return;
	}

	if (!strcmp(cmd, "get_battery")) {
		char requested_name[64] = "";
		char battery_path[PATH_MAX];
		char battery_name[64];

		json_get_string(req, "name", requested_name, sizeof(requested_name));
		if (find_battery_path(requested_name, battery_path, sizeof(battery_path),
				      battery_name, sizeof(battery_name)) < 0) {
			make_error(resp, resp_size, -ENOENT, "battery not found");
			return;
		}

		appendf(resp, resp_size, &pos, "{\"ok\":true,\"battery\":");
		append_battery_json(resp, resp_size, &pos, battery_path, battery_name);
		appendf(resp, resp_size, &pos, "}\n");
		return;
	}

	if (!strcmp(cmd, "get_batteries")) {
		DIR *dir = opendir("/sys/class/power_supply");
		struct dirent *de;
		bool first = true;

		if (!dir) {
			make_error(resp, resp_size, -errno, "failed to open power_supply");
			return;
		}

		appendf(resp, resp_size, &pos, "{\"ok\":true,\"batteries\":[");
		while ((de = readdir(dir))) {
			char battery_path[PATH_MAX];
			char battery_name[64];

			if (de->d_name[0] == '.')
				continue;
			if (find_battery_path(de->d_name, battery_path, sizeof(battery_path),
					      battery_name, sizeof(battery_name)) < 0)
				continue;

			appendf(resp, resp_size, &pos, "%s", first ? "" : ",");
			append_battery_json(resp, resp_size, &pos, battery_path, battery_name);
			first = false;
		}
		closedir(dir);
		appendf(resp, resp_size, &pos, "]}\n");
		return;
	}

	if (!strcmp(cmd, "set_battery_charge_limit")) {
		char requested_name[64] = "";
		char battery_path[PATH_MAX];
		char battery_name[64];
		char threshold_path[PATH_MAX];
		int threshold;
		int err;

		if (json_get_int(req, "threshold", &threshold) < 0 &&
		    json_get_int(req, "value", &threshold) < 0) {
			make_error(resp, resp_size, -EINVAL, "missing threshold");
			return;
		}

		if (threshold < 1 || threshold > 100) {
			make_error(resp, resp_size, -EINVAL, "invalid threshold");
			return;
		}

		json_get_string(req, "name", requested_name, sizeof(requested_name));
		if (find_battery_path(requested_name, battery_path, sizeof(battery_path),
				      battery_name, sizeof(battery_name)) < 0) {
			make_error(resp, resp_size, -ENOENT, "battery not found");
			return;
		}

		join_path(threshold_path, sizeof(threshold_path), battery_path,
			  "charge_control_end_threshold");
		err = write_int(threshold_path, threshold);
		if (err < 0) {
			make_error(resp, resp_size, err, "failed to set charge limit");
			return;
		}

		appendf(resp, resp_size, &pos, "{\"ok\":true,\"battery\":");
		append_battery_json(resp, resp_size, &pos, battery_path, battery_name);
		appendf(resp, resp_size, &pos, "}\n");
		return;
	}

	if (!strcmp(cmd, "get_profiles") || !strcmp(cmd, "get_profile_config") ||
	    !strcmp(cmd, "set_profile_config")) {
		int profile;
		int source;
		int power_mode = 0;
		int fan_mode = 0;
		bool set_power;
		bool set_fan;
		bool apply_now = false;
		int err;

		if (!strcmp(cmd, "set_profile_config")) {
			set_power = json_has_key(req, "power_mode");
			set_fan = json_has_key(req, "fan_mode");
			if (parse_profile_value(req, &profile) < 0 ||
			    profile < 1 || profile > PROFILE_COUNT ||
			    parse_power_source_value(req, &source) < 0 ||
			    (!set_power && !set_fan) ||
			    (set_power && parse_level_field(req, "power_mode", &power_mode) < 0) ||
			    (set_fan && parse_level_field(req, "fan_mode", &fan_mode) < 0)) {
				make_error(resp, resp_size, -EINVAL, "invalid profile slot configuration");
				return;
			}

			pthread_rwlock_wrlock(&svc->state_lock);
			struct profile_branch old_branch = svc->profiles[profile - 1].branch[source];
			struct source_control_state old_control = svc->source_controls[source];
			if (set_power)
				svc->profiles[profile - 1].branch[source].power_mode = power_mode;
			if (set_fan) {
				svc->profiles[profile - 1].branch[source].fan_mode = fan_mode;
				/* Base fan modes belong to the selected preset branch.
				 * Retain only Whisper/benchmark as source-wide override
				 * layers with higher cooling ownership. */
				if (svc->source_controls[source].fan_mode_valid &&
				    !fan_mode_suspends_curve_control(
					    svc->source_controls[source].fan_mode))
					svc->source_controls[source].fan_mode_valid = false;
			}
			apply_now = svc->active_profile == profile &&
				svc->active_power_source == source;
			err = save_profile_state_locked(svc);
			if (err < 0) {
				svc->profiles[profile - 1].branch[source] = old_branch;
				svc->source_controls[source] = old_control;
			}
			pthread_rwlock_unlock(&svc->state_lock);
			if (err < 0) {
				make_error(resp, resp_size, err, "failed to persist profile configuration");
				return;
			}

			if (apply_now) {
				err = apply_active_profile(svc, true);
				if (err < 0) {
					make_error(resp, resp_size, err,
						   "configuration saved but failed to apply active profile");
					return;
				}
			}
		}

		appendf(resp, resp_size, &pos, "{\"ok\":true,\"state\":");
		append_profile_state_json(svc, resp, resp_size, &pos);
		appendf(resp, resp_size, &pos, "}\n");
		return;
	}

	if (!strcmp(cmd, "get_power_mode") || !strcmp(cmd, "set_power_mode")) {
		char value[64];
		int power_mode;
		int err;

		if (!strcmp(cmd, "get_power_mode")) {
			err = refresh_endpoint_cache_by_name(svc, "performance_profile");
			if (err < 0) {
				make_error(resp, resp_size, err, "read failed");
				return;
			}
		} else {
			if (parse_profile_value(req, &power_mode) < 0 ||
			    power_mode < 1 || power_mode > 3) {
				make_error(resp, resp_size, -EINVAL, "invalid power mode");
				return;
			}

			pthread_rwlock_wrlock(&svc->state_lock);
			int active_profile = svc->active_profile;
			int source = svc->active_power_source;
			if (active_profile < 1 || active_profile > PROFILE_COUNT ||
			    source < 0 || source >= POWER_SOURCE_COUNT) {
				pthread_rwlock_unlock(&svc->state_lock);
				make_error(resp, resp_size, -ENODEV,
					   "active profile source is unavailable");
				return;
			}
			struct profile_branch old_branch =
				svc->profiles[active_profile - 1].branch[source];
			svc->profiles[active_profile - 1].branch[source].power_mode =
				power_mode;
			err = save_profile_state_locked(svc);
			if (err < 0)
				svc->profiles[active_profile - 1].branch[source] = old_branch;
			pthread_rwlock_unlock(&svc->state_lock);
			if (err < 0) {
				make_error(resp, resp_size, err, "failed to persist power mode");
				return;
			}
			err = apply_active_profile(svc, true);
			if (err < 0) {
				make_error(resp, resp_size, err, "failed to apply power mode");
				return;
			}
		}

		err = endpoint_read(svc, "performance_profile", value, sizeof(value));
		if (err < 0) {
			make_error(resp, resp_size, err, "read failed");
			return;
		}

		snprintf(resp, resp_size, "{\"ok\":true,\"profile\":%d,\"mode\":\"%s\"}\n",
			 atoi(value), profile_name(value));
		return;
	}

	if (!strcmp(cmd, "activate_profile")) {
		int profile;
		int err;

		if (parse_profile_value(req, &profile) < 0 ||
		    profile < 1 || profile > PROFILE_COUNT) {
			make_error(resp, resp_size, -EINVAL, "invalid profile slot");
			return;
		}

		pthread_rwlock_wrlock(&svc->state_lock);
		int old_profile = svc->active_profile;
		svc->active_profile = profile;
		err = save_profile_state_locked(svc);
		if (err < 0)
			svc->active_profile = old_profile;
		pthread_rwlock_unlock(&svc->state_lock);
		if (err < 0) {
			make_error(resp, resp_size, err, "failed to persist active profile");
			return;
		}

		err = apply_active_profile(svc, true);
		if (err < 0) {
			make_error(resp, resp_size, err, "failed to apply profile slot");
			return;
		}

		appendf(resp, resp_size, &pos, "{\"ok\":true,\"state\":");
		append_profile_state_json(svc, resp, resp_size, &pos);
		appendf(resp, resp_size, &pos, "}\n");
		return;
	}

	if (!strcmp(cmd, "get_system_power_mode") || !strcmp(cmd, "set_system_power_mode")) {
		char mode[64];
		int err;

		if (!strcmp(cmd, "set_system_power_mode")) {
			if (parse_system_power_mode_value(req, mode, sizeof(mode)) < 0) {
				make_error(resp, resp_size, -EINVAL, "invalid system power mode");
				return;
			}

			pthread_mutex_lock(&svc->hardware_lock);
			err = dbus_set_system_power_mode(mode);
			pthread_mutex_unlock(&svc->hardware_lock);
			if (err < 0) {
				make_error(resp, resp_size, err,
					   "failed to set system power mode");
				return;
			}
			refresh_system_power_cache(svc);
		}

		pthread_rwlock_rdlock(&svc->state_lock);
		err = svc->system_power_mode_err;
		snprintf(mode, sizeof(mode), "%s", svc->system_power_mode);
		pthread_rwlock_unlock(&svc->state_lock);
		if (err < 0) {
			make_error(resp, resp_size, err, "failed to read system power mode");
			return;
		}

		snprintf(resp, resp_size, "{\"ok\":true,\"mode\":\"%s\"}\n", mode);
		return;
	}

	if (!strcmp(cmd, "get_passive_cooling") || !strcmp(cmd, "set_passive_cooling")) {
		char value[64];
		bool source_requested = json_has_key(req, "source");
		bool apply_now = true;
		int source = -1;
		int enabled = 0;
		int err;

		if (source_requested && parse_power_source_value(req, &source) < 0) {
			make_error(resp, resp_size, -EINVAL, "invalid power source");
			return;
		}
		if (!strcmp(cmd, "set_passive_cooling")) {
			if (parse_bool_value(req, "value", &enabled) < 0) {
				make_error(resp, resp_size, -EINVAL, "missing or invalid value");
				return;
			}

			pthread_rwlock_wrlock(&svc->state_lock);
			if (source < 0)
				source = svc->active_power_source;
			if (source >= 0 && source < POWER_SOURCE_COUNT) {
				struct source_control_state old = svc->source_controls[source];

				svc->source_controls[source].passive_cooling = !!enabled;
				svc->source_controls[source].passive_cooling_valid = true;
				apply_now = source == svc->active_power_source;
				err = save_profile_state_locked(svc);
				if (err < 0)
					svc->source_controls[source] = old;
			} else {
				err = 0;
			}
			pthread_rwlock_unlock(&svc->state_lock);
			if (err < 0) {
				make_error(resp, resp_size, err, "failed to persist passive cooling");
				return;
			}
			if (apply_now) {
				err = endpoint_write_bool(svc, "passive_cooling", enabled);
				if (err < 0) {
					make_error(resp, resp_size, err,
						   "setting persisted but hardware write failed");
					return;
				}
			}
			snprintf(resp, resp_size,
				 "{\"ok\":true,\"passive_cooling\":%s,\"source\":\"%s\","
				 "\"persisted\":true,\"applied\":%s}\n",
				 enabled ? "true" : "false",
				 source >= 0 ? power_source_name(source) : "active",
				 apply_now ? "true" : "false");
			return;
		}

		if (source >= 0 && source < POWER_SOURCE_COUNT) {
			pthread_rwlock_rdlock(&svc->state_lock);
			if (svc->source_controls[source].passive_cooling_valid) {
				enabled = svc->source_controls[source].passive_cooling;
				pthread_rwlock_unlock(&svc->state_lock);
				snprintf(resp, resp_size,
					 "{\"ok\":true,\"passive_cooling\":%s,\"source\":\"%s\","
					 "\"persisted\":true}\n",
					 enabled ? "true" : "false", power_source_name(source));
				return;
			}
			pthread_rwlock_unlock(&svc->state_lock);
		}
		err = endpoint_read(svc, "passive_cooling", value, sizeof(value));
		if (err < 0) {
			make_error(resp, resp_size, err, "read failed");
			return;
		}
		snprintf(resp, resp_size, "{\"ok\":true,\"passive_cooling\":%s}\n",
			 atoi(value) ? "true" : "false");
		return;
	}

	if (!strcmp(cmd, "get_fan_mode") || !strcmp(cmd, "set_fan_mode")) {
		char value[64];
		bool curve_control;
		bool curve_control_active;
		bool source_requested = json_has_key(req, "source");
		bool apply_now = true;
		int source = -1;
		int fan_mode;
		int err;

		if (source_requested && parse_power_source_value(req, &source) < 0) {
			make_error(resp, resp_size, -EINVAL, "invalid power source");
			return;
		}
		if (!strcmp(cmd, "get_fan_mode") && source >= 0) {
			pthread_rwlock_rdlock(&svc->state_lock);
			if (svc->source_controls[source].fan_mode_valid &&
			    fan_mode_suspends_curve_control(
				    svc->source_controls[source].fan_mode)) {
				fan_mode = svc->source_controls[source].fan_mode;
			} else if (svc->active_profile >= 1 &&
				   svc->active_profile <= PROFILE_COUNT) {
				fan_mode =
					svc->profiles[svc->active_profile - 1].branch[source].fan_mode;
			} else {
				fan_mode = -1;
			}
			pthread_rwlock_unlock(&svc->state_lock);
			if (fan_mode >= FAN_MODE_PERFORMANCE &&
			    fan_mode <= FAN_MODE_BENCHMARK) {
				snprintf(value, sizeof(value), "%d", fan_mode);
				snprintf(resp, resp_size,
					 "{\"ok\":true,\"fan_mode\":%d,\"mode\":\"%s\","
					 "\"source\":\"%s\",\"persisted\":true}\n",
					 fan_mode, fan_mode_name(value), power_source_name(source));
				return;
			}
		}

		if (!strcmp(cmd, "set_fan_mode")) {
			if (parse_fan_mode_value(req, &fan_mode) < 0 ||
			    fan_mode < 1 || fan_mode > 5) {
				make_error(resp, resp_size, -EINVAL, "invalid fan mode");
				return;
			}

			pthread_rwlock_wrlock(&svc->state_lock);
			if (source < 0)
				source = svc->active_power_source;
			if (source >= 0 && source < POWER_SOURCE_COUNT) {
				struct source_control_state old = svc->source_controls[source];
				int active_profile = svc->active_profile;
				struct profile_branch old_branch = { 0 };

				if (active_profile >= 1 && active_profile <= PROFILE_COUNT)
					old_branch =
						svc->profiles[active_profile - 1].branch[source];
				if (fan_mode_suspends_curve_control(fan_mode)) {
					svc->source_controls[source].fan_mode = fan_mode;
					svc->source_controls[source].fan_mode_valid = true;
				} else if (active_profile >= 1 &&
					   active_profile <= PROFILE_COUNT) {
					svc->profiles[active_profile - 1].branch[source].fan_mode =
						fan_mode;
					svc->source_controls[source].fan_mode_valid = false;
				}
				apply_now = source == svc->active_power_source;
				err = save_profile_state_locked(svc);
				if (err < 0) {
					svc->source_controls[source] = old;
					if (active_profile >= 1 &&
					    active_profile <= PROFILE_COUNT)
						svc->profiles[active_profile - 1].branch[source] =
							old_branch;
				}
			} else {
				err = 0;
			}
			if (apply_now && source >= 0 &&
			    !svc->source_controls[source].cpu_curve_valid &&
			    !svc->source_controls[source].gpu_curve_valid)
				load_default_curves(svc, fan_mode);
			curve_control = svc->fan_curve_control_enabled;
			curve_control_active = curve_control &&
				!fan_mode_suspends_curve_control(fan_mode);
			pthread_rwlock_unlock(&svc->state_lock);
			if (err < 0) {
				make_error(resp, resp_size, err, "failed to persist fan mode");
				return;
			}
			if (!apply_now) {
				snprintf(value, sizeof(value), "%d", fan_mode);
				snprintf(resp, resp_size,
					 "{\"ok\":true,\"fan_mode\":%d,\"mode\":\"%s\","
					 "\"source\":\"%s\",\"persisted\":true,\"applied\":false}\n",
					 fan_mode, fan_mode_name(value), power_source_name(source));
				return;
			}

			err = apply_fan_mode_hardware(svc, fan_mode, curve_control);
			if (err < 0) {
				make_error(resp, resp_size, err, "setting persisted but hardware write failed");
				return;
			}
			if (curve_control_active) {
				err = fan_control_tick(svc);
				if (err < 0) {
					make_error(resp, resp_size, err,
						   "fan mode selected but curve apply failed");
					return;
				}
			}

			/* The requested source mode is already persisted and the hardware
			 * operation above completed successfully. Return that logical mode
			 * directly instead of paying for another slow EC readback. */
			snprintf(value, sizeof(value), "%d", fan_mode);
			snprintf(resp, resp_size,
				 "{\"ok\":true,\"fan_mode\":%d,\"mode\":\"%s\","
				 "\"source\":\"%s\",\"persisted\":true,\"applied\":true}\n",
				 fan_mode, fan_mode_name(value), power_source_name(source));
			return;
		}

		err = endpoint_read(svc, "fan_mode", value, sizeof(value));
		if (err < 0) {
			make_error(resp, resp_size, err, "read failed");
			return;
		}

		snprintf(resp, resp_size, "{\"ok\":true,\"fan_mode\":%d,\"mode\":\"%s\"}\n",
			 atoi(value), fan_mode_name(value));
		return;
	}

	if (!strcmp(cmd, "get_fan_boost") || !strcmp(cmd, "set_fan_boost")) {
		handle_fan_boost(svc, req, resp, resp_size, !strcmp(cmd, "set_fan_boost"));
		return;
	}

	if (!strcmp(cmd, "get_fn_lock") || !strcmp(cmd, "set_fn_lock")) {
		handle_simple_bool(svc, req, resp, resp_size, "fn_lock_toggle_enable",
				   "fn_lock", !strcmp(cmd, "set_fn_lock"));
		return;
	}

	if (!strcmp(cmd, "get_touchpad_toggle") || !strcmp(cmd, "set_touchpad_toggle") ||
	    !strcmp(cmd, "enable_touchpad_toggle") ||
	    !strcmp(cmd, "disable_touchpad_toggle")) {
		if (!strcmp(cmd, "enable_touchpad_toggle")) {
			handle_touchpad_toggle(svc, req, resp, resp_size, true, 1);
			return;
		}

		if (!strcmp(cmd, "disable_touchpad_toggle")) {
			handle_touchpad_toggle(svc, req, resp, resp_size, true, 0);
			return;
		}

		handle_touchpad_toggle(svc, req, resp, resp_size,
				       !strcmp(cmd, "set_touchpad_toggle"), -1);
		return;
	}

	if (!strcmp(cmd, "set_touchpad_state") || !strcmp(cmd, "sync_touchpad_state")) {
		int enabled;
		int err;

		if (parse_bool_value(req, "enabled", &enabled) < 0 &&
		    parse_bool_value(req, "value", &enabled) < 0) {
			make_error(resp, resp_size, -EINVAL, "missing or invalid enabled state");
			return;
		}

		pthread_mutex_lock(&svc->hardware_lock);
		err = set_touchpad_hid_state(enabled);
		pthread_mutex_unlock(&svc->hardware_lock);
		if (err < 0) {
			make_error(resp, resp_size, err, "touchpad HID feature write failed");
			return;
		}

		snprintf(resp, resp_size,
			 "{\"ok\":true,\"touchpad_enabled\":%s,\"disabled_led\":%s}\n",
			 enabled ? "true" : "false", enabled ? "false" : "true");
		publish_state_event(svc);
		return;
	}

	if (!strcmp(cmd, "get_super_key") || !strcmp(cmd, "set_super_key") ||
	    !strcmp(cmd, "get_super_lock") || !strcmp(cmd, "set_super_lock")) {
		handle_simple_bool(svc, req, resp, resp_size, "super_key_toggle_enable",
				   "super_key_enabled",
				   !strcmp(cmd, "set_super_key") || !strcmp(cmd, "set_super_lock"));
		return;
	}

	if (!strcmp(cmd, "get_lightbar") || !strcmp(cmd, "set_lightbar")) {
		char brightness[64] = "";
		char max_brightness[64] = "";
		char multi_intensity[MAX_LINE] = "";
		char rainbow[64] = "";
		char breathing[64] = "";
		bool global_changed = false;
		int source = -1;
		int err;

		if (json_has_key(req, "source") &&
		    parse_power_source_value(req, &source) < 0) {
			make_error(resp, resp_size, -EINVAL, "invalid power source");
			return;
		}
		pthread_rwlock_rdlock(&svc->state_lock);
		if (source < 0)
			source = svc->active_power_source;
		pthread_rwlock_unlock(&svc->state_lock);
		if (source < 0 || source >= POWER_SOURCE_COUNT)
			source = POWER_SOURCE_AC;

		if (!strcmp(cmd, "set_lightbar")) {
			int value;
			int red;
			int green;
			int blue;
			bool has_red = json_has_key(req, "red");
			bool has_green = json_has_key(req, "green");
			bool has_blue = json_has_key(req, "blue");
			struct lightbar_source_state old_state;
			bool old_global;

			pthread_rwlock_wrlock(&svc->state_lock);
			old_state = svc->lightbar[source];
			old_global = svc->lightbar_enabled;
			if (parse_bool_value(req, "global_enabled", &value) == 0) {
				svc->lightbar_enabled = !!value;
				global_changed = svc->lightbar_enabled != old_global;
			}
			if (parse_bool_value(req, "enabled", &value) == 0)
				svc->lightbar[source].enabled = !!value;
			if (json_get_int(req, "brightness", &value) == 0) {
				if (value < 0 || value > LIGHTBAR_CONFIG_MAX)
					goto invalid_lightbar;
				svc->lightbar[source].brightness = value;
			}

			if (has_red || has_green || has_blue) {
				if (!has_red || !has_green || !has_blue ||
				    json_get_int(req, "red", &red) < 0 ||
				    json_get_int(req, "green", &green) < 0 ||
				    json_get_int(req, "blue", &blue) < 0 ||
				    red < 0 || red > 255 || green < 0 || green > 255 ||
				    blue < 0 || blue > 255)
					goto invalid_lightbar;
				svc->lightbar[source].red = red;
				svc->lightbar[source].green = green;
				svc->lightbar[source].blue = blue;
			}

			if (parse_bool_value(req, "rainbow", &value) == 0 ||
			    parse_bool_value(req, "rainbow_mode", &value) == 0)
				svc->lightbar[source].rainbow = !!value;

			if (parse_bool_value(req, "breathing", &value) == 0 ||
			    parse_bool_value(req, "breathing_in_suspend", &value) == 0 ||
			    parse_bool_value(req, "sleep_breathing", &value) == 0)
				svc->lightbar[source].breathing = !!value;

			err = save_profile_state_locked(svc);
			if (err < 0) {
				svc->lightbar[source] = old_state;
				svc->lightbar_enabled = old_global;
			}
			pthread_rwlock_unlock(&svc->state_lock);
			if (err < 0) {
				make_error(resp, resp_size, err,
					   "failed to persist lightbar configuration");
				return;
			}

			pthread_rwlock_rdlock(&svc->state_lock);
			bool apply_now = source == svc->active_power_source || global_changed;
			int active_source = svc->active_power_source;
			pthread_rwlock_unlock(&svc->state_lock);
			if (apply_now) {
				err = apply_lightbar_source(svc,
					global_changed && source != active_source ?
					active_source : source);
				if (err < 0) {
					make_error(resp, resp_size, err,
						   "configuration saved but failed to apply lightbar");
					return;
				}
			}
			goto lightbar_saved;

invalid_lightbar:
			svc->lightbar[source] = old_state;
			svc->lightbar_enabled = old_global;
			pthread_rwlock_unlock(&svc->state_lock);
			make_error(resp, resp_size, -EINVAL, "invalid lightbar configuration");
			return;
		}

lightbar_saved:
		endpoint_read(svc, "lightbar_brightness", brightness, sizeof(brightness));
		endpoint_read(svc, "lightbar_max_brightness", max_brightness, sizeof(max_brightness));
		endpoint_read(svc, "lightbar_multi_intensity", multi_intensity,
			      sizeof(multi_intensity));
		endpoint_read(svc, "rainbow_animation", rainbow, sizeof(rainbow));
		endpoint_read(svc, "breathing_in_suspend", breathing, sizeof(breathing));

		appendf(resp, resp_size, &pos,
			"{\"ok\":true,\"brightness\":\"%s\",\"max_brightness\":\"%s\","
			"\"multi_intensity\":\"%s\",\"rainbow\":%s,"
			"\"rainbow_mode\":%s,\"breathing_in_suspend\":%s,"
			"\"sleep_breathing\":%s,\"source\":\"%s\",\"state\":",
			brightness, max_brightness, multi_intensity,
			atoi(rainbow) ? "true" : "false",
			atoi(rainbow) ? "true" : "false",
			atoi(breathing) ? "true" : "false",
			atoi(breathing) ? "true" : "false",
			power_source_name(source));
		append_lightbar_state_json(svc, resp, resp_size, &pos);
		appendf(resp, resp_size, &pos, "}\n");
		return;
	}

	if (!strcmp(cmd, "get_keyboard_backlight") ||
	    !strcmp(cmd, "set_keyboard_backlight")) {
		int source = -1;
		int err;

		if (json_has_key(req, "source") &&
		    parse_power_source_value(req, &source) < 0) {
			make_error(resp, resp_size, -EINVAL, "invalid power source");
			return;
		}
		pthread_rwlock_rdlock(&svc->state_lock);
		if (source < 0)
			source = svc->active_power_source;
		pthread_rwlock_unlock(&svc->state_lock);
		if (source < 0 || source >= POWER_SOURCE_COUNT)
			source = POWER_SOURCE_AC;

		if (!strcmp(cmd, "set_keyboard_backlight")) {
			struct keyboard_light_source_state old_state;
			bool old_global;
			bool global_changed = false;
			bool has_red = json_has_key(req, "red");
			bool has_green = json_has_key(req, "green");
			bool has_blue = json_has_key(req, "blue");
			char value_text[64];
			int value;
			int red;
			int green;
			int blue;

			pthread_rwlock_wrlock(&svc->state_lock);
			old_state = svc->keyboard_light[source];
			old_global = svc->keyboard_light_enabled;
			if (parse_bool_value(req, "global_enabled", &value) == 0) {
				svc->keyboard_light_enabled = !!value;
				global_changed =
					svc->keyboard_light_enabled != old_global;
			}
			if (parse_bool_value(req, "enabled", &value) == 0)
				svc->keyboard_light[source].enabled = !!value;
			if (json_get_int(req, "brightness", &value) == 0) {
				if (value < 0 || value > KEYBOARD_BRIGHTNESS_MAX)
					goto invalid_keyboard_light;
				svc->keyboard_light[source].brightness = value;
			}
			if (json_get_string(req, "color", value_text,
					    sizeof(value_text)) == 0) {
				const char *hex = value_text;
				char *end;
				unsigned long rgb;

				if (hex[0] == '#')
					hex++;
				errno = 0;
				rgb = strtoul(hex, &end, 16);
				if (errno || end == hex || *end || strlen(hex) != 6 ||
				    rgb > 0xffffff)
					goto invalid_keyboard_light;
				svc->keyboard_light[source].red = (rgb >> 16) & 0xff;
				svc->keyboard_light[source].green = (rgb >> 8) & 0xff;
				svc->keyboard_light[source].blue = rgb & 0xff;
			} else if (has_red || has_green || has_blue) {
				if (!has_red || !has_green || !has_blue ||
				    json_get_int(req, "red", &red) < 0 ||
				    json_get_int(req, "green", &green) < 0 ||
				    json_get_int(req, "blue", &blue) < 0)
					goto invalid_keyboard_light;
				if (red < 0 || red > 255 || green < 0 || green > 255 ||
				    blue < 0 || blue > 255)
					goto invalid_keyboard_light;
				svc->keyboard_light[source].red = red;
				svc->keyboard_light[source].green = green;
				svc->keyboard_light[source].blue = blue;
			}

			if (json_get_string(req, "effect", value_text,
					    sizeof(value_text)) == 0) {
				value = keyboard_effect_value(value_text);
				if (value < 0)
					goto invalid_keyboard_light;
				svc->keyboard_light[source].effect = value;
			}
			if (json_get_int(req, "speed", &value) == 0) {
				if (value < 0 || value > KEYBOARD_EFFECT_SPEED_MAX)
					goto invalid_keyboard_light;
				svc->keyboard_light[source].speed = value;
			}
			if (json_get_string(req, "direction", value_text,
					    sizeof(value_text)) == 0) {
				value = keyboard_direction_value(value_text);
				if (value < 0)
					goto invalid_keyboard_light;
				svc->keyboard_light[source].direction = value;
			}
			if (parse_bool_value(req, "reactive", &value) == 0)
				svc->keyboard_light[source].reactive = !!value;

			err = save_profile_state_locked(svc);
			if (err < 0) {
				svc->keyboard_light[source] = old_state;
				svc->keyboard_light_enabled = old_global;
			}
			pthread_rwlock_unlock(&svc->state_lock);
			if (err < 0) {
				make_error(resp, resp_size, err,
					   "failed to persist keyboard light configuration");
				return;
			}

			pthread_rwlock_rdlock(&svc->state_lock);
			bool apply_now =
				source == svc->active_power_source || global_changed;
			int active_source = svc->active_power_source;
			pthread_rwlock_unlock(&svc->state_lock);
			if (apply_now) {
				err = apply_keyboard_light_source(
					svc, global_changed && source != active_source ?
					active_source : source);
				if (err < 0) {
					make_error(resp, resp_size, err,
						   "configuration saved but failed to apply keyboard light");
					return;
				}
			}
			publish_state_event(svc);
			goto keyboard_light_saved;

invalid_keyboard_light:
			svc->keyboard_light[source] = old_state;
			svc->keyboard_light_enabled = old_global;
			pthread_rwlock_unlock(&svc->state_lock);
			make_error(resp, resp_size, -EINVAL,
				   "invalid keyboard light configuration");
			return;
		}

keyboard_light_saved:
		appendf(resp, resp_size, &pos,
			"{\"ok\":true,\"source\":\"%s\",\"state\":",
			power_source_name(source));
		append_keyboard_light_state_json(svc, resp, resp_size, &pos);
		appendf(resp, resp_size, &pos, "}\n");
		return;
	}

	if (!strcmp(cmd, "get_curve") || !strcmp(cmd, "set_curve")) {
		char fan[32];
		struct fan_curve *curve;
		struct fan_curve parsed;
		bool *curve_valid = NULL;
		bool source_requested = json_has_key(req, "source");
		int source = -1;
		int persist_err = 0;

		if (json_get_string(req, "fan", fan, sizeof(fan)) < 0) {
			make_error(resp, resp_size, -EINVAL, "missing fan");
			return;
		}
		if (source_requested && parse_power_source_value(req, &source) < 0) {
			make_error(resp, resp_size, -EINVAL, "invalid power source");
			return;
		}
		if (!strcmp(cmd, "set_curve")) {
			snprintf(parsed.name, sizeof(parsed.name), "%s",
				 !strcmp(fan, "gpu") || !strcmp(fan, "1") ||
				 !strcmp(fan, "secondary") ? "gpu" : "cpu");
			if (parse_curve_points(req, &parsed) < 0) {
				make_error(resp, resp_size, -EINVAL, "invalid curve");
				return;
			}
		}

		pthread_rwlock_wrlock(&svc->state_lock);
		if (source < 0)
			source = svc->active_power_source;
		if (source >= 0 && source < POWER_SOURCE_COUNT)
			curve = select_source_curve(&svc->source_controls[source], fan,
						   &curve_valid);
		else
			curve = select_curve(svc, fan);
		if (!curve) {
			pthread_rwlock_unlock(&svc->state_lock);
			make_error(resp, resp_size, -EINVAL, "unknown fan");
			return;
		}

		if (!strcmp(cmd, "set_curve")) {
			struct fan_curve old_curve = *curve;
			bool old_valid = curve_valid ? *curve_valid : false;
			bool old_control = svc->fan_curve_control_enabled;

			*curve = parsed;
			if (curve_valid)
				*curve_valid = true;
			if (source == svc->active_power_source) {
				struct fan_curve *active = select_curve(svc, fan);
				if (active)
					*active = parsed;
			}
			svc->fan_curve_control_enabled = true;
			svc->active_curve_mode = 0;
			svc->last_cpu_pwm = -1;
			svc->last_gpu_pwm = -1;
			persist_err = save_profile_state_locked(svc);
			if (persist_err < 0) {
				*curve = old_curve;
				if (curve_valid)
					*curve_valid = old_valid;
				svc->fan_curve_control_enabled = old_control;
			}
		}

		if (persist_err < 0) {
			pthread_rwlock_unlock(&svc->state_lock);
			make_error(resp, resp_size, persist_err, "failed to persist curve");
			return;
		}
		appendf(resp, resp_size, &pos, "{\"ok\":true,\"curve\":");
		curve_to_json(resp, resp_size, &pos, curve);
		appendf(resp, resp_size, &pos, ",\"source\":\"%s\",\"persisted\":%s",
			source >= 0 ? power_source_name(source) : "active",
			curve_valid && *curve_valid ? "true" : "false");
		appendf(resp, resp_size, &pos, "}\n");
		pthread_rwlock_unlock(&svc->state_lock);
		return;
	}

	if (!strcmp(cmd, "get_cpu_temp") || !strcmp(cmd, "get_gpu_temp") ||
	    !strcmp(cmd, "get_temps")) {
		int cpu_mdeg = 0;
		int cpu_c = 0;
		int gpu_mdeg = 0;
		int gpu_c = 0;
		int cpu_err = read_hwmon_temp(svc, 0, &cpu_mdeg, &cpu_c);
		int gpu_err = read_hwmon_temp(svc, 1, &gpu_mdeg, &gpu_c);

		if (!strcmp(cmd, "get_cpu_temp")) {
			if (cpu_err < 0) {
				make_error(resp, resp_size, cpu_err, "failed to read CPU temperature");
				return;
			}
			snprintf(resp, resp_size,
				 "{\"ok\":true,\"sensor\":\"cpu\",\"temp_millidegree\":%d,"
				 "\"temp_c\":%d,\"curve_fan\":\"cpu\"}\n",
				 cpu_mdeg, cpu_c);
			return;
		}

		if (!strcmp(cmd, "get_gpu_temp")) {
			if (gpu_err < 0) {
				make_error(resp, resp_size, gpu_err, "failed to read GPU temperature");
				return;
			}
			snprintf(resp, resp_size,
				 "{\"ok\":true,\"sensor\":\"gpu\",\"temp_millidegree\":%d,"
				 "\"temp_c\":%d,\"curve_fan\":\"gpu\"}\n",
				 gpu_mdeg, gpu_c);
			return;
		}

		snprintf(resp, resp_size,
			 "{\"ok\":true,\"cpu\":{\"available\":%s,\"temp_millidegree\":%d,"
			 "\"temp_c\":%d,\"curve_fan\":\"cpu\"},"
			 "\"gpu\":{\"available\":%s,\"temp_millidegree\":%d,"
			 "\"temp_c\":%d,\"curve_fan\":\"gpu\"}}\n",
			 cpu_err < 0 ? "false" : "true", cpu_mdeg, cpu_c,
			 gpu_err < 0 ? "false" : "true", gpu_mdeg, gpu_c);
		return;
	}

	if (!strcmp(cmd, "get_cpu_fan_rpm") || !strcmp(cmd, "get_gpu_fan_rpm") ||
	    !strcmp(cmd, "get_fan_rpms")) {
		int cpu_rpm = 0;
		int gpu_rpm = 0;
		int cpu_err = read_hwmon_fan_rpm(svc, 0, &cpu_rpm);
		int gpu_err = read_hwmon_fan_rpm(svc, 1, &gpu_rpm);

		if (!strcmp(cmd, "get_cpu_fan_rpm")) {
			if (cpu_err < 0) {
				make_error(resp, resp_size, cpu_err, "failed to read CPU fan RPM");
				return;
			}
			snprintf(resp, resp_size,
				 "{\"ok\":true,\"fan\":\"cpu\",\"sensor\":\"fan1_input\","
				 "\"rpm\":%d}\n",
				 cpu_rpm);
			return;
		}

		if (!strcmp(cmd, "get_gpu_fan_rpm")) {
			if (gpu_err < 0) {
				make_error(resp, resp_size, gpu_err, "failed to read GPU fan RPM");
				return;
			}
			snprintf(resp, resp_size,
				 "{\"ok\":true,\"fan\":\"gpu\",\"sensor\":\"fan2_input\","
				 "\"rpm\":%d}\n",
				 gpu_rpm);
			return;
		}

		snprintf(resp, resp_size,
			 "{\"ok\":true,\"cpu\":{\"available\":%s,\"sensor\":\"fan1_input\","
			 "\"rpm\":%d},\"gpu\":{\"available\":%s,\"sensor\":\"fan2_input\","
			 "\"rpm\":%d}}\n",
			 cpu_err < 0 ? "false" : "true", cpu_rpm,
			 gpu_err < 0 ? "false" : "true", gpu_rpm);
		return;
	}

	if (!strcmp(cmd, "get_fan_control") || !strcmp(cmd, "set_fan_control")) {
		char pwm1_enable[64] = "";
		char pwm2_enable[64] = "";
		char mode[32];
		bool curve_control;
		bool curve_suspended;
		int fan_mode;
		int err;

		if (!strcmp(cmd, "set_fan_control")) {
			if (json_get_string(req, "mode", mode, sizeof(mode)) < 0 &&
			    json_get_string(req, "value", mode, sizeof(mode)) < 0) {
				make_error(resp, resp_size, -EINVAL, "missing mode");
				return;
			}

			if (!strcmp(mode, "manual") || !strcmp(mode, "curve")) {
				pthread_rwlock_wrlock(&svc->state_lock);
				svc->fan_curve_control_enabled = true;
				svc->last_cpu_pwm = -1;
				svc->last_gpu_pwm = -1;
				pthread_rwlock_unlock(&svc->state_lock);
				err = fan_control_tick(svc);
				if (err < 0) {
					pthread_rwlock_wrlock(&svc->state_lock);
					svc->fan_curve_control_enabled = false;
					svc->last_cpu_pwm = -1;
					svc->last_gpu_pwm = -1;
					pthread_rwlock_unlock(&svc->state_lock);
					set_auto_fan_mode(svc);
				}
			} else if (!strcmp(mode, "auto") || !strcmp(mode, "automatic")) {
				int current_fan_mode;

				pthread_rwlock_wrlock(&svc->state_lock);
				svc->fan_curve_control_enabled = false;
				svc->last_cpu_pwm = -1;
				svc->last_gpu_pwm = -1;
				current_fan_mode = active_fan_mode_locked(svc);
				pthread_rwlock_unlock(&svc->state_lock);
				/* This changes the saved post-mode intent only. Benchmark and
				 * Whisper own the live fan hardware until their mode is left. */
				err = fan_mode_suspends_curve_control(current_fan_mode) ? 0 :
					set_auto_fan_mode(svc);
			} else {
				make_error(resp, resp_size, -EINVAL, "invalid fan control mode");
				return;
			}

			if (err < 0) {
				make_error(resp, resp_size, err, "failed to set fan control mode");
				return;
			}
			pthread_rwlock_wrlock(&svc->state_lock);
			err = save_profile_state_locked(svc);
			pthread_rwlock_unlock(&svc->state_lock);
			if (err < 0) {
				make_error(resp, resp_size, err,
					   "fan control changed but persistence failed");
				return;
			}
		}

		endpoint_read(svc, "pwm1_enable", pwm1_enable, sizeof(pwm1_enable));
		endpoint_read(svc, "pwm2_enable", pwm2_enable, sizeof(pwm2_enable));
		pthread_rwlock_rdlock(&svc->state_lock);
		curve_control = svc->fan_curve_control_enabled;
		fan_mode = active_fan_mode_locked(svc);
		curve_suspended = fan_mode_suspends_curve_control(fan_mode);
		pthread_rwlock_unlock(&svc->state_lock);

		snprintf(resp, resp_size,
			 "{\"ok\":true,\"mode\":\"%s\",\"curve_control\":%s,"
			 "\"curve_suspended\":%s,\"suspended_by\":%s,"
			 "\"pwm1_enable\":\"%s\",\"pwm2_enable\":\"%s\"}\n",
			 curve_control ? "manual" : "auto",
			 curve_control ? "true" : "false",
			 curve_suspended ? "true" : "false",
			 fan_mode == FAN_MODE_BENCHMARK ? "\"benchmark\"" :
			 fan_mode == FAN_MODE_WHISPER ? "\"whisper\"" : "null",
			 pwm1_enable, pwm2_enable);
		return;
	}

	if (!strcmp(cmd, "status")) {
		char temp1[64] = "";
		char temp2[64] = "";
		char fan1[64] = "";
		char fan2[64] = "";
		char pwm1[64] = "";
		char pwm2[64] = "";
		char pwm1_enable[64] = "";
		char pwm2_enable[64] = "";
		char system_power_mode[64] = "";
		bool curve_control;
		bool curve_suspended;
		int fan_mode;
		int last_synced_power_profile;
		int active_profile;
		int active_power_source;

		endpoint_read(svc, "temp1_input", temp1, sizeof(temp1));
		endpoint_read(svc, "temp2_input", temp2, sizeof(temp2));
		endpoint_read(svc, "fan1_input", fan1, sizeof(fan1));
		endpoint_read(svc, "fan2_input", fan2, sizeof(fan2));
		endpoint_read(svc, "pwm1", pwm1, sizeof(pwm1));
		endpoint_read(svc, "pwm2", pwm2, sizeof(pwm2));
		endpoint_read(svc, "pwm1_enable", pwm1_enable, sizeof(pwm1_enable));
		endpoint_read(svc, "pwm2_enable", pwm2_enable, sizeof(pwm2_enable));
		pthread_rwlock_rdlock(&svc->state_lock);
		snprintf(system_power_mode, sizeof(system_power_mode), "%s", svc->system_power_mode);
		curve_control = svc->fan_curve_control_enabled;
		fan_mode = active_fan_mode_locked(svc);
		curve_suspended = fan_mode_suspends_curve_control(fan_mode);
		last_synced_power_profile = svc->last_synced_power_profile;
		active_profile = svc->active_profile;
		active_power_source = svc->active_power_source;
		pthread_rwlock_unlock(&svc->state_lock);

		snprintf(resp, resp_size,
			 "{\"ok\":true,\"hwmon\":\"%s\",\"platform\":\"%s\","
			 "\"temp1_input\":\"%s\",\"temp2_input\":\"%s\","
			 "\"fan1_input\":\"%s\",\"fan2_input\":\"%s\","
			 "\"pwm1\":\"%s\",\"pwm2\":\"%s\","
			 "\"pwm1_enable\":\"%s\",\"pwm2_enable\":\"%s\","
			 "\"fan_control\":\"%s\",\"curve_control\":%s,"
			 "\"curve_suspended\":%s,\"suspended_by\":%s,"
			 "\"system_power_mode\":\"%s\",\"power_mode_auto_sync\":true,"
			 "\"last_synced_power_profile\":%d,"
			 "\"active_profile\":%d,\"active_source\":\"%s\"}\n",
			 svc->hwmon_path, svc->platform_path, temp1, temp2, fan1, fan2, pwm1, pwm2,
			 pwm1_enable, pwm2_enable,
			 fan_mode == FAN_MODE_BENCHMARK ? "benchmark" :
			 fan_mode == FAN_MODE_WHISPER ? "whisper" :
			 curve_control ? "manual" : "auto",
			 curve_control ? "true" : "false",
			 curve_suspended ? "true" : "false",
			 fan_mode == FAN_MODE_BENCHMARK ? "\"benchmark\"" :
			 fan_mode == FAN_MODE_WHISPER ? "\"whisper\"" : "null",
			 system_power_mode, last_synced_power_profile, active_profile,
			 active_power_source < 0 ? "unknown" : power_source_name(active_power_source));
		return;
	}

	if (!strcmp(cmd, "reload")) {
		size_t endpoint_count;

		pthread_rwlock_wrlock(&svc->state_lock);
		svc->endpoint_count = 0;
		discover_hwmon(svc);
		discover_platform(svc);
		add_hwmon_endpoints(svc);
		add_platform_endpoints(svc);
		add_power_supply_endpoints(svc);
		add_lightbar_endpoints(svc);
		add_keyboard_backlight_endpoints(svc);
		pthread_rwlock_unlock(&svc->state_lock);
		refresh_endpoint_cache(svc);
		refresh_system_power_cache(svc);
		pthread_rwlock_rdlock(&svc->state_lock);
		endpoint_count = svc->endpoint_count;
		pthread_rwlock_unlock(&svc->state_lock);
		snprintf(resp, resp_size, "{\"ok\":true,\"endpoints\":%zu}\n", endpoint_count);
		return;
	}

	if (!strcmp(cmd, "tick")) {
		int err = fan_control_tick(svc);

		if (err < 0) {
			make_error(resp, resp_size, err, "fan control tick failed");
			return;
		}
		snprintf(resp, resp_size, "{\"ok\":true}\n");
		return;
	}

	make_error(resp, resp_size, -EINVAL, "unknown cmd");
}

static int parse_octal_mode(const char *text, mode_t *mode)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 8);
	if (errno || end == text || *end || value > 0777)
		return -EINVAL;

	*mode = (mode_t)value;
	return 0;
}

static int write_all_poll(int fd, const char *buf, size_t len)
{
	size_t pos = 0;

	while (pos < len) {
		ssize_t written = write(fd, buf + pos, len - pos);

		if (written > 0) {
			pos += (size_t)written;
			continue;
		}

		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pfd = {
				.fd = fd,
				.events = POLLOUT,
			};
			int ret;

			do {
				ret = poll(&pfd, 1, 1000);
			} while (ret < 0 && errno == EINTR && !stop_requested);

			if (ret > 0)
				continue;
			if (ret == 0)
				return -ETIMEDOUT;
			return -errno;
		}

		if (written < 0 && errno == EINTR)
			continue;

		if (written == 0)
			return -EPIPE;

		return -errno;
	}

	return 0;
}

static int setup_socket(const char *path, mode_t mode, const char *group)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path))
		return -ENAMETOOLONG;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -errno;

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int err = -errno;

		close(fd);
		return err;
	}

	if (group && group[0]) {
		struct group *gr = getgrnam(group);

		if (!gr) {
			int err = errno ? -errno : -ENOENT;

			close(fd);
			unlink(path);
			return err;
		}

		if (chown(path, (uid_t)-1, gr->gr_gid) < 0) {
			int err = -errno;

			close(fd);
			unlink(path);
			return err;
		}
	}

	if (chmod(path, mode) < 0) {
		int err = -errno;

		close(fd);
		unlink(path);
		return err;
	}

	if (listen(fd, SOMAXCONN) < 0) {
		int err = -errno;

		close(fd);
		unlink(path);
		return err;
	}

	return fd;
}

struct client_thread_args {
	struct uniwilld *svc;
	int fd;
};

static void *client_thread_main(void *arg)
{
	struct client_thread_args *thread_args = arg;
	struct uniwilld *svc = thread_args->svc;
	int fd = thread_args->fd;
	char input[MAX_LINE * 2];
	size_t input_len = 0;
	bool subscribed = false;
	bool initial_snapshot_pending = false;
	unsigned long long delivered_revision = 0;

	free(thread_args);
	while (!stop_requested) {
		char req[MAX_LINE];
		char resp[MAX_RESPONSE];
		bool have_line = false;

		if (subscribed) {
			unsigned long long revision = state_event_revision(svc);

			if (initial_snapshot_pending || revision != delivered_revision) {
				size_t event_pos = 0;

				appendf(resp, sizeof(resp), &event_pos,
					"{\"ok\":true,\"event\":\"state_changed\","
					"\"revision\":%llu,\"snapshot\":", revision);
				append_dashboard_snapshot_json(svc, resp, sizeof(resp),
							       &event_pos);
				appendf(resp, sizeof(resp), &event_pos, "}\n");
				if (write_all_poll(fd, resp, strlen(resp)) < 0)
					break;
				delivered_revision = revision;
				initial_snapshot_pending = false;
			}
		}

		while (!stop_requested) {
			char *newline = memchr(input, '\n', input_len);

			if (newline) {
				size_t line_len = (size_t)(newline - input);
				size_t consumed = line_len + 1;

				if (line_len >= sizeof(req))
					goto done;
				memcpy(req, input, line_len);
				req[line_len] = '\0';
				memmove(input, input + consumed, input_len - consumed);
				input_len -= consumed;
				have_line = true;
				break;
			}

			if (input_len >= sizeof(req) - 1)
				goto done;

			size_t available = sizeof(input) - input_len;
			size_t read_size = available < 1024 ? available : 1024;
			ssize_t ret = recv(fd, input + input_len, read_size, 0);

			if (ret < 0 && errno == EINTR)
				continue;
			if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				struct pollfd pfd = {
					.fd = fd,
					.events = POLLIN | POLLHUP | POLLERR,
				};
				int poll_ret = poll(&pfd, 1, 100);

				if (poll_ret > 0 && (pfd.revents & (POLLHUP | POLLERR)))
					goto done;
				if (subscribed && poll_ret == 0)
					break;
				continue;
			}
			if (ret <= 0)
				goto done;
			input_len += (size_t)ret;
		}

		if (!have_line || req[0] == '\0')
			continue;

		char cmd[64];
		if (json_get_string(req, "cmd", cmd, sizeof(cmd)) == 0 &&
		    !strcmp(cmd, "subscribe")) {
			subscribed = true;
			initial_snapshot_pending = true;
			delivered_revision = state_event_revision(svc);
			int response_len = snprintf(resp, sizeof(resp),
				"{\"ok\":true,\"event\":\"subscribed\","
				"\"revision\":%llu}\n", delivered_revision);

			if (response_len < 0 ||
			    write_all_poll(fd, resp, (size_t)response_len) < 0)
				break;
			continue;
		}

		handle_request(svc, req, resp, sizeof(resp));
		if (write_all_poll(fd, resp, strlen(resp)) < 0)
			break;
		if (!strncmp(resp, "{\"ok\":true", 10) &&
		    command_changes_state(req))
			publish_state_event(svc);
		if (!subscribed)
			break;
	}

done:
	shutdown(fd, SHUT_WR);
	close(fd);
	pthread_mutex_lock(&svc->client_lock);
	svc->client_count--;
	pthread_cond_broadcast(&svc->client_cond);
	pthread_mutex_unlock(&svc->client_lock);
	return NULL;
}

static void wait_for_clients(struct uniwilld *svc)
{
	pthread_mutex_lock(&svc->client_lock);
	while (svc->client_count > 0)
		pthread_cond_wait(&svc->client_cond, &svc->client_lock);
	pthread_mutex_unlock(&svc->client_lock);
}

static void *state_monitor_thread_main(void *arg)
{
	struct uniwilld *svc = arg;
	size_t platform_cursor = 0;

	while (!stop_requested) {
		bool changed = refresh_runtime_endpoint_cache(svc, &platform_cursor);

		changed |= refresh_system_power_cache(svc);
		if (changed)
			publish_state_event(svc);

		for (int slept = 0; slept < svc->interval_ms && !stop_requested; slept += 100)
			usleep(100000);
	}

	return NULL;
}

static void *control_thread_main(void *arg)
{
	struct uniwilld *svc = arg;
	int interval_ms = svc->interval_ms < MAX_FAN_CONTROL_INTERVAL_MS ?
		svc->interval_ms : MAX_FAN_CONTROL_INTERVAL_MS;
	char profile_path[PATH_MAX] = "";
	int profile_fd = -1;

	pthread_rwlock_rdlock(&svc->state_lock);
	struct endpoint *profile_endpoint = find_endpoint(svc, "performance_profile");
	if (profile_endpoint)
		snprintf(profile_path, sizeof(profile_path), "%s", profile_endpoint->path);
	pthread_rwlock_unlock(&svc->state_lock);

	if (profile_path[0]) {
		char discard[32];

		profile_fd = open(profile_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (profile_fd >= 0)
			read(profile_fd, discard, sizeof(discard));
	}

	while (!stop_requested) {
		apply_active_profile(svc, false);
		fan_control_tick(svc);

		if (profile_fd >= 0) {
			struct pollfd pfd = {
				.fd = profile_fd,
				.events = POLLPRI | POLLERR,
			};
			int poll_ret = poll(&pfd, 1, interval_ms);

			if (poll_ret > 0 && pfd.revents) {
				char discard[32];

				lseek(profile_fd, 0, SEEK_SET);
				read(profile_fd, discard, sizeof(discard));
			}
		} else {
			for (int slept = 0; slept < interval_ms && !stop_requested; slept += 100)
				usleep(100000);
		}
	}

	if (profile_fd >= 0)
		close(profile_fd);

	return NULL;
}

static int run_server(struct uniwilld *svc)
{
	int listen_fd;
	pthread_t state_thread;
	pthread_t control_thread;
	bool state_thread_started = false;
	bool control_thread_started = false;
	int thread_ret;

	listen_fd = setup_socket(svc->socket_path, svc->socket_mode, svc->socket_group);
	if (listen_fd < 0)
		return listen_fd;

	thread_ret = pthread_create(&state_thread, NULL, state_monitor_thread_main, svc);
	if (thread_ret == 0) {
		state_thread_started = true;
	} else {
		close(listen_fd);
		unlink(svc->socket_path);
		return -thread_ret;
	}

	thread_ret = pthread_create(&control_thread, NULL, control_thread_main, svc);
	if (thread_ret == 0) {
		control_thread_started = true;
	} else {
		stop_requested = 1;
		if (state_thread_started)
			pthread_join(state_thread, NULL);
		close(listen_fd);
		unlink(svc->socket_path);
		return -thread_ret;
	}

	while (!stop_requested) {
		struct pollfd pfd = {
			.fd = listen_fd,
			.events = POLLIN,
		};
		int ret = poll(&pfd, 1, 1000);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0) {
			int err = -errno;

			stop_requested = 1;
			if (state_thread_started)
				pthread_join(state_thread, NULL);
			if (control_thread_started)
				pthread_join(control_thread, NULL);
			close(listen_fd);
			unlink(svc->socket_path);
			wait_for_clients(svc);
			return err;
		}
		if (ret == 0)
			continue;

		for (;;) {
			struct client_thread_args *thread_args;
			pthread_t client_thread;
			int client_fd = accept4(listen_fd, NULL, NULL,
						SOCK_CLOEXEC | SOCK_NONBLOCK);

			if (client_fd < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					break;
				int err = -errno;

				stop_requested = 1;
				if (state_thread_started)
					pthread_join(state_thread, NULL);
				if (control_thread_started)
					pthread_join(control_thread, NULL);
				close(listen_fd);
				unlink(svc->socket_path);
				wait_for_clients(svc);
				return err;
			}

			thread_args = calloc(1, sizeof(*thread_args));
			if (!thread_args) {
				close(client_fd);
				continue;
			}
			thread_args->svc = svc;
			thread_args->fd = client_fd;

			pthread_mutex_lock(&svc->client_lock);
			svc->client_count++;
			pthread_mutex_unlock(&svc->client_lock);
			thread_ret = pthread_create(&client_thread, NULL, client_thread_main, thread_args);
			if (thread_ret != 0) {
				pthread_mutex_lock(&svc->client_lock);
				svc->client_count--;
				pthread_cond_broadcast(&svc->client_cond);
				pthread_mutex_unlock(&svc->client_lock);
				close(client_fd);
				free(thread_args);
				continue;
			}
			pthread_detach(client_thread);
		}
	}

	if (state_thread_started)
		pthread_join(state_thread, NULL);
	if (control_thread_started)
		pthread_join(control_thread, NULL);
	close(listen_fd);
	unlink(svc->socket_path);
	wait_for_clients(svc);
	return 0;
}

static void usage(FILE *stream, const char *argv0)
{
	fprintf(stream,
		"Usage: %s [--socket PATH] [--state-file PATH] [--socket-mode MODE] [--socket-group GROUP] [--interval-ms N] [--fan-control] [--once] [--version]\n"
		"\n"
		"Fan control remains with the EC unless --fan-control is supplied.\n"
		"\n"
		"Commands are JSON lines sent to the Unix socket, for example:\n"
		"  {\"cmd\":\"list\"}\n"
		"  {\"cmd\":\"get\",\"name\":\"temp1_input\"}\n"
		"  {\"cmd\":\"set\",\"name\":\"pwm1\",\"value\":120}\n"
		"  {\"cmd\":\"get_curve\",\"fan\":\"cpu\"}\n"
		"  {\"cmd\":\"set_curve\",\"fan\":\"gpu\",\"points\":[{\"temp\":40,\"pwm\":0},{\"temp\":92,\"pwm\":255}]}\n",
		argv0);
}

int main(int argc, char **argv)
{
	struct uniwilld svc = {
		.socket_path = DEFAULT_SOCKET_PATH,
		.state_path = DEFAULT_STATE_PATH,
		.socket_mode = 0666,
		.interval_ms = DEFAULT_INTERVAL_MS,
		.fan_curve_control_enabled = false,
		.last_synced_power_profile = -1,
	};
	int ret;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
			printf("uniwilld %s+%s\n",
			       UNIWILLD_VERSION, UNIWILLD_BUILD_NUMBER);
			return 0;
		}
	}

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
			snprintf(svc.socket_path, sizeof(svc.socket_path), "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--state-file") && i + 1 < argc) {
			snprintf(svc.state_path, sizeof(svc.state_path), "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--socket-mode") && i + 1 < argc) {
			if (parse_octal_mode(argv[++i], &svc.socket_mode) < 0) {
				fprintf(stderr, "uniwilld: invalid socket mode\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--socket-group") && i + 1 < argc) {
			snprintf(svc.socket_group, sizeof(svc.socket_group), "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) {
			svc.interval_ms = atoi(argv[++i]);
			if (svc.interval_ms < 100)
				svc.interval_ms = 100;
		} else if (!strcmp(argv[i], "--fan-control")) {
			svc.fan_curve_control_enabled = true;
		} else if (!strcmp(argv[i], "--once")) {
			svc.once = true;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(stdout, argv[0]);
			return 0;
		} else {
			usage(stderr, argv[0]);
			return 2;
		}
	}

	load_default_curves(&svc, FAN_MODE_STANDARD);
	init_default_profiles(&svc);
	if (pthread_rwlock_init(&svc.state_lock, NULL) != 0 ||
	    pthread_mutex_init(&svc.hardware_lock, NULL) != 0 ||
	    pthread_mutex_init(&svc.fan_control_lock, NULL) != 0 ||
	    pthread_mutex_init(&svc.profile_apply_lock, NULL) != 0 ||
	    pthread_mutex_init(&svc.client_lock, NULL) != 0 ||
	    pthread_cond_init(&svc.client_cond, NULL) != 0 ||
	    pthread_mutex_init(&svc.event_lock, NULL) != 0) {
		fprintf(stderr, "uniwilld: failed to initialize locks\n");
		return 1;
	}

	ret = wait_for_hwmon(&svc);
	if (ret < 0) {
		fprintf(stderr, "uniwilld: failed to find uniwill hwmon device: %s\n",
			strerror(-ret));
		return 1;
	}
	discover_platform(&svc);
	add_hwmon_endpoints(&svc);
	add_platform_endpoints(&svc);
	add_power_supply_endpoints(&svc);
	add_lightbar_endpoints(&svc);
	add_keyboard_backlight_endpoints(&svc);
	refresh_endpoint_cache(&svc);
	refresh_system_power_cache(&svc);

	ret = load_profile_state(&svc);
	if (ret < 0) {
		fprintf(stderr, "uniwilld: failed to load profile state, using defaults: %s\n",
			strerror(-ret));
		init_default_profiles(&svc);
	}

	ret = apply_active_profile(&svc, true);
	if (ret < 0)
		fprintf(stderr, "uniwilld: failed to apply active profile: %s\n", strerror(-ret));

	if (svc.once) {
		int fan_ret = fan_control_tick(&svc);
		int auto_ret = 0;

		if (svc.fan_curve_control_enabled &&
		    !fan_mode_suspends_curve_control(active_fan_mode(&svc)))
			auto_ret = set_auto_fan_mode(&svc);

		return fan_ret < 0 || auto_ret < 0 || ret < 0;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	signal(SIGPIPE, SIG_IGN);

	ret = run_server(&svc);
	if (svc.fan_curve_control_enabled &&
	    !fan_mode_suspends_curve_control(active_fan_mode(&svc))) {
		int auto_ret = set_auto_fan_mode(&svc);

		if (auto_ret < 0)
			fprintf(stderr, "uniwilld: failed to restore automatic fan mode: %s\n",
				strerror(-auto_ret));
	}
	if (ret < 0) {
		fprintf(stderr, "uniwilld: server failed: %s\n", strerror(-ret));
		return 1;
	}

	return 0;
}
