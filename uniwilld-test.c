// SPDX-License-Identifier: GPL-2.0-or-later

#define main uniwilld_program_main
#include "uniwilld.c"
#undef main

#include <assert.h>

static void test_touchpad_report_descriptor_parser(void)
{
	static const unsigned char descriptor[] = {
		0x05, 0x01, 0x09, 0x02,
		0x05, 0x0d, 0x09, 0x22, 0xa1, 0x00, 0x09, 0x57, 0x09, 0x58,
		0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
		0x85, 0x07, 0xb1, 0x02, 0xc0,
	};
	static const unsigned char unrelated[] = {
		0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x07, 0xc0,
	};

	assert(touchpad_report_id_from_descriptor(descriptor, sizeof(descriptor)) == 7);
	assert(touchpad_report_id_from_descriptor(unrelated, sizeof(unrelated)) == -ENOENT);
}

static void test_dmi_memory_capacity_and_slots(void)
{
	unsigned char type16[0x0f] = { 0 };
	unsigned char type17[0x20] = { 0 };

	type16[0] = 16;
	type16[1] = sizeof(type16);
	type16[0x05] = 0x03;
	type16[0x0d] = 2;
	assert(dmi_system_memory_slots_from_raw(type16, sizeof(type16)) == 2);

	type17[0] = 17;
	type17[1] = sizeof(type17);
	type17[0x0c] = 0xff;
	type17[0x0d] = 0x7f;
	type17[0x1d] = 0x80;
	assert(dmi_memory_size(type17, sizeof(type17)) ==
	       32ULL * 1024 * 1024 * 1024);
}

static void test_profile_state_round_trip(void)
{
	char directory[] = "/tmp/uniwilld-test-XXXXXX";
	char state_path[PATH_MAX];
	struct uniwilld saved = { 0 };
	struct uniwilld loaded = { 0 };

	assert(mkdtemp(directory));
	assert(join_path(state_path, sizeof(state_path), directory, "state.conf") == 0);

	init_default_profiles(&saved);
	assert(pthread_rwlock_init(&saved.state_lock, NULL) == 0);
	snprintf(saved.state_path, sizeof(saved.state_path), "%s", state_path);
	saved.active_profile = 3;
	saved.profiles[1].branch[POWER_SOURCE_AC].power_mode = 1;
	saved.profiles[1].branch[POWER_SOURCE_AC].fan_mode = 3;
	saved.profiles[1].branch[POWER_SOURCE_BATTERY].power_mode = 3;
	saved.profiles[1].branch[POWER_SOURCE_BATTERY].fan_mode = 2;
	saved.fan_curve_control_enabled = true;
	saved.source_controls[POWER_SOURCE_AC].fan_mode = FAN_MODE_BENCHMARK;
	saved.source_controls[POWER_SOURCE_AC].fan_mode_valid = true;
	saved.source_controls[POWER_SOURCE_AC].passive_cooling = false;
	saved.source_controls[POWER_SOURCE_AC].passive_cooling_valid = true;
	saved.source_controls[POWER_SOURCE_AC].cpu_curve = (struct fan_curve) {
		.name = "cpu",
		.points = { { 30, 0 }, { 60, 128 }, { 100, 255 } },
		.count = 3,
	};
	saved.source_controls[POWER_SOURCE_AC].cpu_curve_valid = true;
	saved.lightbar_enabled = false;
	saved.lightbar[POWER_SOURCE_AC] = (struct lightbar_source_state) {
		.enabled = true,
		.brightness = 175,
		.red = 240,
		.green = 90,
		.blue = 30,
		.rainbow = true,
		.breathing = false,
	};
	saved.lightbar[POWER_SOURCE_BATTERY] = (struct lightbar_source_state) {
		.enabled = false,
		.brightness = 72,
		.red = 40,
		.green = 80,
		.blue = 220,
		.rainbow = false,
		.breathing = true,
	};
	saved.keyboard_light_enabled = false;
	saved.keyboard_light[POWER_SOURCE_AC] =
		(struct keyboard_light_source_state) {
			.enabled = true,
			.brightness = 42,
			.red = 190,
			.green = 50,
			.blue = 240,
			.effect = KEYBOARD_EFFECT_AURORA,
			.speed = 3,
			.direction = 1,
			.reactive = true,
		};
	pthread_rwlock_wrlock(&saved.state_lock);
	assert(save_profile_state_locked(&saved) == 0);
	pthread_rwlock_unlock(&saved.state_lock);

	init_default_profiles(&loaded);
	snprintf(loaded.state_path, sizeof(loaded.state_path), "%s", state_path);
	assert(load_profile_state(&loaded) == 0);
	assert(loaded.active_profile == 3);
	assert(loaded.profiles[1].branch[POWER_SOURCE_AC].power_mode == 1);
	assert(loaded.profiles[1].branch[POWER_SOURCE_AC].fan_mode == 3);
	assert(loaded.profiles[1].branch[POWER_SOURCE_BATTERY].power_mode == 3);
	assert(loaded.profiles[1].branch[POWER_SOURCE_BATTERY].fan_mode == 2);
	assert(loaded.profiles[0].branch[POWER_SOURCE_AC].power_mode == 1);
	assert(loaded.profiles[2].branch[POWER_SOURCE_BATTERY].fan_mode == 3);
	assert(loaded.fan_curve_control_enabled);
	assert(loaded.source_controls[POWER_SOURCE_AC].fan_mode_valid);
	assert(loaded.source_controls[POWER_SOURCE_AC].fan_mode == FAN_MODE_BENCHMARK);
	assert(loaded.source_controls[POWER_SOURCE_AC].passive_cooling_valid);
	assert(!loaded.source_controls[POWER_SOURCE_AC].passive_cooling);
	assert(loaded.source_controls[POWER_SOURCE_AC].cpu_curve_valid);
	assert(loaded.source_controls[POWER_SOURCE_AC].cpu_curve.count == 3);
	assert(loaded.source_controls[POWER_SOURCE_AC].cpu_curve.points[1].pwm == 128);
	assert(!loaded.lightbar_enabled);
	assert(loaded.lightbar[POWER_SOURCE_AC].enabled);
	assert(loaded.lightbar[POWER_SOURCE_AC].brightness == 175);
	assert(loaded.lightbar[POWER_SOURCE_AC].red == 240);
	assert(loaded.lightbar[POWER_SOURCE_AC].rainbow);
	assert(!loaded.lightbar[POWER_SOURCE_BATTERY].enabled);
	assert(loaded.lightbar[POWER_SOURCE_BATTERY].brightness == 72);
	assert(loaded.lightbar[POWER_SOURCE_BATTERY].blue == 220);
	assert(loaded.lightbar[POWER_SOURCE_BATTERY].breathing);
	assert(!loaded.keyboard_light_enabled);
	assert(loaded.keyboard_light[POWER_SOURCE_AC].brightness == 42);
	assert(loaded.keyboard_light[POWER_SOURCE_AC].red == 190);
	assert(loaded.keyboard_light[POWER_SOURCE_AC].effect ==
	       KEYBOARD_EFFECT_AURORA);
	assert(loaded.keyboard_light[POWER_SOURCE_AC].speed == 3);
	assert(loaded.keyboard_light[POWER_SOURCE_AC].direction == 1);
	assert(loaded.keyboard_light[POWER_SOURCE_AC].reactive);

	unlink(state_path);
	rmdir(directory);
	pthread_rwlock_destroy(&saved.state_lock);
}

static void test_profile_request_parsing(void)
{
	const char *request =
		"{\"cmd\":\"set_profile_config\",\"profile\":2,\"source\":\"battery\","
		"\"power_mode\":\"battery_saver\",\"fan_mode\":\"standard\"}";
	int profile;
	int source;
	int power_mode;
	int fan_mode;

	assert(parse_profile_value(request, &profile) == 0 && profile == 2);
	assert(parse_power_source_value(request, &source) == 0 &&
	       source == POWER_SOURCE_BATTERY);
	assert(parse_level_field(request, "power_mode", &power_mode) == 0 && power_mode == 3);
	assert(parse_level_field(request, "fan_mode", &fan_mode) == 0 && fan_mode == 2);
}

static void test_curve_failsafe_validation(void)
{
	struct fan_curve curve = { .name = "cpu" };

	assert(parse_curve_points(
		"{\"points\":[{\"temp\":40,\"pwm\":0},{\"temp\":92,\"pwm\":255}]}",
		&curve) == 0);
	assert(parse_curve_points(
		"{\"points\":[{\"pwm\":0,\"temp\":40},{\"pwm\":255,\"temp\":92}]}",
		&curve) == 0);
	assert(curve.count == 2 && curve.points[0].temp_c == 40 &&
	       curve.points[1].pwm == 255);
	assert(parse_curve_points(
		"{\"points\":[{\"temp\":40,\"pwm\":0},{\"temp\":92,\"pwm\":180}]}",
		&curve) == -EINVAL);
	assert(parse_curve_points(
		"{\"points\":[{\"temp\":40,\"pwm\":0},{\"temp\":105,\"pwm\":255}]}",
		&curve) == -EINVAL);
}

static void test_fan_mode_priority(void)
{
	struct uniwilld svc = { 0 };

	init_default_profiles(&svc);
	assert(pthread_rwlock_init(&svc.state_lock, NULL) == 0);
	svc.active_profile = 2;
	svc.active_power_source = POWER_SOURCE_AC;
	svc.fan_curve_control_enabled = true;

	assert(active_fan_mode(&svc) == FAN_MODE_STANDARD);
	assert(!fan_mode_suspends_curve_control(active_fan_mode(&svc)));

	svc.source_controls[POWER_SOURCE_AC].fan_mode = FAN_MODE_WHISPER;
	svc.source_controls[POWER_SOURCE_AC].fan_mode_valid = true;
	assert(active_fan_mode(&svc) == FAN_MODE_WHISPER);
	assert(fan_mode_suspends_curve_control(active_fan_mode(&svc)));

	svc.source_controls[POWER_SOURCE_AC].fan_mode = FAN_MODE_BENCHMARK;
	assert(active_fan_mode(&svc) == FAN_MODE_BENCHMARK);
	assert(fan_mode_suspends_curve_control(active_fan_mode(&svc)));
	assert(svc.fan_curve_control_enabled);

	svc.source_controls[POWER_SOURCE_AC].fan_mode = FAN_MODE_STANDARD;
	assert(active_fan_mode(&svc) == FAN_MODE_STANDARD);
	assert(!fan_mode_suspends_curve_control(active_fan_mode(&svc)));
	assert(svc.fan_curve_control_enabled);

	svc.profiles[1].branch[POWER_SOURCE_AC].fan_mode = FAN_MODE_PERFORMANCE;
	assert(active_fan_mode(&svc) == FAN_MODE_PERFORMANCE);

	pthread_rwlock_destroy(&svc.state_lock);
}

static void test_default_curve_modes_and_hysteresis(void)
{
	struct uniwilld svc = { 0 };

	for (int mode = FAN_MODE_PERFORMANCE; mode <= FAN_MODE_BENCHMARK; mode++) {
		load_default_curves(&svc, mode);
		assert(svc.cpu_curve.count >= 2);
		assert(svc.gpu_curve.count >= 2);
		assert(svc.cpu_curve.points[svc.cpu_curve.count - 1].pwm == PWM_MAX);
		assert(svc.gpu_curve.points[svc.gpu_curve.count - 1].pwm == PWM_MAX);
		assert(svc.cpu_curve.points[svc.cpu_curve.count - 1].temp_c <=
		       CURVE_FAILSAFE_TEMP_C);
		assert(svc.gpu_curve.points[svc.gpu_curve.count - 1].temp_c <=
		       CURVE_FAILSAFE_TEMP_C);
	}

	load_default_curves(&svc, FAN_MODE_STANDARD);
	assert(svc.active_curve_mode == FAN_MODE_STANDARD);
	assert(svc.cpu_curve.count == 7);
	assert(svc.gpu_curve.count == 7);
	assert(curve_pwm(&svc.cpu_curve, 46, 0) == 0);
	assert(curve_pwm(&svc.cpu_curve, 47, 0) == PWM_MIN_ON);
	assert(curve_pwm(&svc.cpu_curve, 45, PWM_MIN_ON) == PWM_MIN_ON);
	assert(curve_pwm(&svc.cpu_curve, 42, PWM_MIN_ON) == 0);
	assert(curve_pwm(&svc.cpu_curve, 65, PWM_MIN_ON) == 107);
	assert(curve_pwm(&svc.gpu_curve, 68, PWM_MIN_ON) == 107);

	load_default_curves(&svc, FAN_MODE_PERFORMANCE);
	assert(svc.active_curve_mode == FAN_MODE_PERFORMANCE);
	assert(svc.cpu_curve.points[1].temp_c == 43);
	assert(svc.last_cpu_pwm == -1);
	assert(svc.last_gpu_pwm == -1);

	load_default_curves(&svc, FAN_MODE_QUIET);
	assert(svc.active_curve_mode == FAN_MODE_QUIET);
	assert(svc.cpu_curve.points[1].temp_c == 51);
}

static void test_keyboard_backlight_api(void)
{
	char directory[] = "/tmp/uniwilld-kbd-test-XXXXXX";
	char brightness_path[PATH_MAX];
	char max_path[PATH_MAX];
	char color_path[PATH_MAX];
	char multi_path[PATH_MAX];
	char effect_path[PATH_MAX];
	char speed_path[PATH_MAX];
	char direction_path[PATH_MAX];
	char reactive_path[PATH_MAX];
	char effect_color_path[PATH_MAX];
	char state_path[PATH_MAX];
	char response[MAX_RESPONSE];
	char value[64];
	struct uniwilld svc = { 0 };
	int fd;

	assert(mkdtemp(directory));
	assert(join_path(brightness_path, sizeof(brightness_path), directory, "brightness") == 0);
	assert(join_path(max_path, sizeof(max_path), directory, "max_brightness") == 0);
	assert(join_path(color_path, sizeof(color_path), directory, "color") == 0);
	assert(join_path(multi_path, sizeof(multi_path), directory, "multi_intensity") == 0);
	assert(join_path(effect_path, sizeof(effect_path), directory, "effect") == 0);
	assert(join_path(speed_path, sizeof(speed_path), directory, "effect_speed") == 0);
	assert(join_path(direction_path, sizeof(direction_path), directory, "effect_direction") == 0);
	assert(join_path(reactive_path, sizeof(reactive_path), directory, "effect_reactive") == 0);
	assert(join_path(effect_color_path, sizeof(effect_color_path), directory, "effect_color") == 0);
	assert(join_path(state_path, sizeof(state_path), directory, "state.conf") == 0);

	fd = open(brightness_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	fd = open(max_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	fd = open(color_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	fd = open(multi_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	fd = open(effect_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	fd = open(speed_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	fd = open(direction_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	fd = open(reactive_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	fd = open(effect_color_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	assert(write_text(brightness_path, "10\n") == 0);
	assert(write_text(max_path, "50\n") == 0);
	assert(write_text(color_path, "000000\n") == 0);
	assert(write_text(multi_path, "0 0 0\n") == 0);
	assert(write_text(effect_path, "solid\n") == 0);
	assert(write_text(speed_path, "5\n") == 0);
	assert(write_text(direction_path, "left\n") == 0);
	assert(write_text(reactive_path, "0\n") == 0);
	assert(write_text(effect_color_path, "000000\n") == 0);

	init_default_profiles(&svc);
	assert(pthread_rwlock_init(&svc.state_lock, NULL) == 0);
	assert(pthread_mutex_init(&svc.hardware_lock, NULL) == 0);
	assert(pthread_mutex_init(&svc.event_lock, NULL) == 0);
	snprintf(svc.state_path, sizeof(svc.state_path), "%s", state_path);
	svc.active_power_source = POWER_SOURCE_AC;
	add_endpoint(&svc, "keyboard_backlight_brightness", brightness_path);
	add_endpoint(&svc, "keyboard_backlight_max_brightness", max_path);
	add_endpoint(&svc, "keyboard_backlight_color", color_path);
	add_endpoint(&svc, "keyboard_backlight_multi_intensity", multi_path);
	add_endpoint(&svc, "keyboard_backlight_effect", effect_path);
	add_endpoint(&svc, "keyboard_backlight_effect_speed", speed_path);
	add_endpoint(&svc, "keyboard_backlight_effect_direction", direction_path);
	add_endpoint(&svc, "keyboard_backlight_effect_reactive", reactive_path);
	add_endpoint(&svc, "keyboard_backlight_effect_color", effect_color_path);
	refresh_endpoint_cache(&svc);

	handle_request(&svc,
		       "{\"cmd\":\"set_keyboard_backlight\",\"brightness\":25,"
		       "\"color\":\"#12abef\",\"effect\":\"aurora\","
		       "\"speed\":3,\"direction\":\"right\",\"reactive\":true}",
		       response, sizeof(response));
	assert(strstr(response, "\"ok\":true"));
	assert(strstr(response, "\"brightness\":25"));
	assert(strstr(response, "\"effect\":\"aurora\""));
	assert(read_text(brightness_path, value, sizeof(value)) == 0 && !strcmp(value, "25"));
	assert(read_text(effect_color_path, value, sizeof(value)) == 0 && !strcmp(value, "12abef"));
	assert(read_text(effect_path, value, sizeof(value)) == 0 && !strcmp(value, "aurora"));
	assert(read_text(speed_path, value, sizeof(value)) == 0 && !strcmp(value, "3"));
	assert(read_text(reactive_path, value, sizeof(value)) == 0 && !strcmp(value, "1"));
	assert(read_text(multi_path, value, sizeof(value)) == 0 && !strcmp(value, "0 0 0"));

	handle_request(&svc,
		       "{\"cmd\":\"set_keyboard_backlight\",\"brightness\":51}",
		       response, sizeof(response));
	assert(strstr(response, "\"ok\":false"));

	pthread_mutex_destroy(&svc.hardware_lock);
	pthread_mutex_destroy(&svc.event_lock);
	pthread_rwlock_destroy(&svc.state_lock);
	unlink(state_path);
	unlink(brightness_path);
	unlink(max_path);
	unlink(color_path);
	unlink(multi_path);
	unlink(effect_path);
	unlink(speed_path);
	unlink(direction_path);
	unlink(reactive_path);
	unlink(effect_color_path);
	rmdir(directory);
}

static void test_cpu_frequency_profiles(void)
{
	char directory[] = "/tmp/uniwilld-cpufreq-test-XXXXXX";
	char policy0[PATH_MAX];
	char policy1[PATH_MAX];
	char path[PATH_MAX];
	char value[64];
	const char *policies[2];

	assert(mkdtemp(directory));
	assert(join_path(policy0, sizeof(policy0), directory, "policy0") == 0);
	assert(join_path(policy1, sizeof(policy1), directory, "policy1") == 0);
	assert(mkdir(policy0, 0700) == 0);
	assert(mkdir(policy1, 0700) == 0);
	policies[0] = policy0;
	policies[1] = policy1;

	for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); i++) {
		assert(join_path(path, sizeof(path), policies[i], "cpuinfo_min_freq") == 0);
		assert(write_text(path, "800000\n") == -ENOENT);
		int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		assert(fd >= 0 && close(fd) == 0);
		assert(write_text(path, "800000\n") == 0);
		assert(join_path(path, sizeof(path), policies[i], "scaling_min_freq") == 0);
		fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		assert(fd >= 0 && close(fd) == 0);
		assert(write_text(path, "1200000\n") == 0);
		assert(join_path(path, sizeof(path), policies[i], "scaling_max_freq") == 0);
		fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		assert(fd >= 0 && close(fd) == 0);
		assert(write_text(path, "4600000\n") == 0);
		assert(join_path(path, sizeof(path), policies[i],
				 "energy_performance_preference") == 0);
		fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		assert(fd >= 0 && close(fd) == 0);
		assert(write_text(path, "performance\n") == 0);
	}

	assert(apply_cpu_frequency_policy_at(directory, 1) == 0);
	assert(join_path(path, sizeof(path), policy0, "scaling_min_freq") == 0);
	assert(read_text(path, value, sizeof(value)) == 0 && !strcmp(value, "800000"));
	assert(join_path(path, sizeof(path), policy0,
			 "energy_performance_preference") == 0);
	assert(read_text(path, value, sizeof(value)) == 0 &&
	       !strcmp(value, "balance_performance"));

	assert(apply_cpu_frequency_policy_at(directory, 2) == 0);
	assert(read_text(path, value, sizeof(value)) == 0 && !strcmp(value, "balance_power"));
	assert(apply_cpu_frequency_policy_at(directory, 3) == 0);
	assert(read_text(path, value, sizeof(value)) == 0 && !strcmp(value, "power"));

	for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); i++) {
		const char *files[] = { "cpuinfo_min_freq", "scaling_min_freq",
			"scaling_max_freq", "energy_performance_preference" };

		for (size_t j = 0; j < sizeof(files) / sizeof(files[0]); j++) {
			assert(join_path(path, sizeof(path), policies[i], files[j]) == 0);
			unlink(path);
		}
		rmdir(policies[i]);
	}
	rmdir(directory);
}

static void test_persisted_lightbar_profiles(void)
{
	char directory[] = "/tmp/uniwilld-lightbar-test-XXXXXX";
	char state_path[PATH_MAX];
	char brightness_path[PATH_MAX];
	char max_path[PATH_MAX];
	char color_path[PATH_MAX];
	char rainbow_path[PATH_MAX];
	char breathing_path[PATH_MAX];
	char response[MAX_RESPONSE];
	char value[64];
	struct uniwilld svc = { 0 };
	const char *paths[] = {
		brightness_path, max_path, color_path, rainbow_path, breathing_path,
	};

	assert(mkdtemp(directory));
	assert(join_path(state_path, sizeof(state_path), directory, "state.conf") == 0);
	assert(join_path(brightness_path, sizeof(brightness_path), directory,
			 "brightness") == 0);
	assert(join_path(max_path, sizeof(max_path), directory, "max_brightness") == 0);
	assert(join_path(color_path, sizeof(color_path), directory,
			 "multi_intensity") == 0);
	assert(join_path(rainbow_path, sizeof(rainbow_path), directory,
			 "rainbow_animation") == 0);
	assert(join_path(breathing_path, sizeof(breathing_path), directory,
			 "breathing_in_suspend") == 0);
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int fd = open(paths[i], O_CREAT | O_RDWR | O_CLOEXEC, 0600);

		assert(fd >= 0 && close(fd) == 0);
	}
	assert(write_text(brightness_path, "160\n") == 0);
	assert(write_text(max_path, "255\n") == 0);
	assert(write_text(color_path, "255 132 48\n") == 0);
	assert(write_text(rainbow_path, "0\n") == 0);
	assert(write_text(breathing_path, "0\n") == 0);

	init_default_profiles(&svc);
	svc.active_power_source = POWER_SOURCE_AC;
	snprintf(svc.state_path, sizeof(svc.state_path), "%s", state_path);
	assert(pthread_rwlock_init(&svc.state_lock, NULL) == 0);
	assert(pthread_mutex_init(&svc.hardware_lock, NULL) == 0);
	add_endpoint(&svc, "lightbar_brightness", brightness_path);
	add_endpoint(&svc, "lightbar_max_brightness", max_path);
	add_endpoint(&svc, "lightbar_multi_intensity", color_path);
	add_endpoint(&svc, "rainbow_animation", rainbow_path);
	add_endpoint(&svc, "breathing_in_suspend", breathing_path);
	refresh_endpoint_cache(&svc);

	handle_request(&svc,
		       "{\"cmd\":\"set_lightbar\",\"source\":\"ac\","
		       "\"global_enabled\":true,\"enabled\":true,\"brightness\":180,"
		       "\"red\":12,\"green\":120,\"blue\":240,"
		       "\"rainbow\":true,\"breathing\":true}",
		       response, sizeof(response));
	assert(strstr(response, "\"ok\":true"));
	assert(strstr(response, "\"global_enabled\":true"));
	assert(strstr(response, "\"brightness\":180"));
	assert(strstr(response, "\"rainbow\":true"));
	assert(strstr(response, "\"breathing\":true"));
	assert(read_text(brightness_path, value, sizeof(value)) == 0 &&
	       !strcmp(value, "180"));
	assert(read_text(color_path, value, sizeof(value)) == 0 &&
	       !strcmp(value, "12 120 240"));
	assert(read_text(rainbow_path, value, sizeof(value)) == 0 &&
	       !strcmp(value, "1"));
	assert(read_text(breathing_path, value, sizeof(value)) == 0 &&
	       !strcmp(value, "1"));

	pthread_mutex_destroy(&svc.hardware_lock);
	pthread_rwlock_destroy(&svc.state_lock);
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
		unlink(paths[i]);
	unlink(state_path);
	rmdir(directory);
}

static void read_socket_line(int fd, char *line, size_t size)
{
	size_t pos = 0;

	while (pos + 1 < size) {
		struct pollfd pfd = {
			.fd = fd,
			.events = POLLIN,
		};
		char byte;

		assert(poll(&pfd, 1, 1000) == 1);
		assert(recv(fd, &byte, 1, 0) == 1);
		line[pos++] = byte;
		if (byte == '\n')
			break;
	}
	line[pos] = '\0';
}

static void test_persistent_duplex_connection(void)
{
	struct uniwilld svc = { 0 };
	struct client_thread_args *args;
	pthread_t thread;
	char line[MAX_RESPONSE];
	int sockets[2];

	init_default_profiles(&svc);
	svc.active_profile = 2;
	svc.active_power_source = POWER_SOURCE_AC;
	assert(pthread_rwlock_init(&svc.state_lock, NULL) == 0);
	assert(pthread_mutex_init(&svc.client_lock, NULL) == 0);
	assert(pthread_cond_init(&svc.client_cond, NULL) == 0);
	assert(pthread_mutex_init(&svc.event_lock, NULL) == 0);
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
	assert(fcntl(sockets[0], F_SETFL,
		     fcntl(sockets[0], F_GETFL) & ~O_NONBLOCK) == 0);

	args = calloc(1, sizeof(*args));
	assert(args);
	args->svc = &svc;
	args->fd = sockets[1];
	svc.client_count = 1;
	assert(stop_requested == 0);

	/*
	 * Queue two protocol lines in one write before the server thread starts.
	 * This deterministically presents both lines to recv(), so the server must
	 * preserve everything after the first newline.
	 */
	assert(dprintf(sockets[0],
		       "{\"cmd\":\"subscribe\"}\n"
		       "{\"cmd\":\"get_profiles\"}\n") > 0);
	assert(pthread_create(&thread, NULL, client_thread_main, args) == 0);
	read_socket_line(sockets[0], line, sizeof(line));
	assert(strstr(line, "\"event\":\"subscribed\""));
	read_socket_line(sockets[0], line, sizeof(line));
	assert(strstr(line, "\"event\":\"state_changed\""));
	assert(strstr(line, "\"snapshot\""));
	assert(strstr(line, "\"active_profile\":2"));
	read_socket_line(sockets[0], line, sizeof(line));
	assert(strstr(line, "\"ok\":true"));
	assert(strstr(line, "\"active_profile\":2"));

	publish_state_event(&svc);
	read_socket_line(sockets[0], line, sizeof(line));
	assert(strstr(line, "\"event\":\"state_changed\""));
	assert(strstr(line, "\"revision\":1"));

	close(sockets[0]);
	assert(pthread_join(thread, NULL) == 0);
	pthread_mutex_destroy(&svc.event_lock);
	pthread_cond_destroy(&svc.client_cond);
	pthread_mutex_destroy(&svc.client_lock);
	pthread_rwlock_destroy(&svc.state_lock);
}

static void test_version_api(void)
{
	struct uniwilld svc = { 0 };
	char response[MAX_RESPONSE];

	handle_request(&svc, "{\"cmd\":\"get_version\"}",
		       response, sizeof(response));
	assert(strstr(response, "\"ok\":true"));
	assert(strstr(response, "\"version\":\"" UNIWILLD_VERSION "\""));
	assert(strstr(response, "\"build\":\"" UNIWILLD_BUILD_NUMBER "\""));
	assert(strstr(response, "\"full\":\"" UNIWILLD_VERSION "+" UNIWILLD_BUILD_NUMBER "\""));
}

int main(void)
{
	test_touchpad_report_descriptor_parser();
	test_dmi_memory_capacity_and_slots();
	test_profile_state_round_trip();
	test_profile_request_parsing();
	test_curve_failsafe_validation();
	test_fan_mode_priority();
	test_default_curve_modes_and_hysteresis();
	test_keyboard_backlight_api();
	test_cpu_frequency_profiles();
	test_persisted_lightbar_profiles();
	test_persistent_duplex_connection();
	test_version_api();
	puts("uniwilld tests passed");
	return 0;
}
