// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Keep the firmware touchpad-disabled LED in sync with the desktop session.
 *
 * Precision touchpads expose a Surface Button Switch selective-reporting
 * feature report.  0x00 disables touch and click reporting (LED on), while
 * 0x03 enables both (LED off).  KDE and GNOME normally disable a touchpad in
 * the compositor only, so the firmware never sees this report without a
 * small per-session bridge like this one.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gio/gio.h>

#define TOUCHPAD_I2C_NAME "i2c-UNIW0001:00"
#define KWIN_NAME "org.kde.KWin"
#define KWIN_INTERFACE "org.kde.KWin.InputDevice"
#define KWIN_PATH_PREFIX "/org/kde/KWin/InputDevice/"
#define RETRY_INTERVAL_MS 1000
#define KWIN_CONFIG_SYNC_DELAY_MS 20
#define UNIWILLD_SOCKET "/run/uniwilld.sock"

static const unsigned char selective_reporting_marker[] = {
	0x05, 0x0d, 0x09, 0x22, 0xa1, 0x00, 0x09, 0x57, 0x09, 0x58,
};

static GMainLoop *main_loop;
static GDBusConnection *session_bus;
static GDBusConnection *system_bus;
static GSettings *gnome_touchpad_settings;
static GFileMonitor *kwin_config_monitor;
static int last_applied_state = -1;
static guint wakeup_timer_id;
static guint desktop_sync_timer_id;

static bool path_is_uniwill_touchpad(const char *hidraw_sysfs)
{
	char device_link[PATH_MAX];
	char resolved[PATH_MAX];

	if (snprintf(device_link, sizeof(device_link), "%s/device", hidraw_sysfs) >=
	    (int)sizeof(device_link))
		return false;

	if (!realpath(device_link, resolved))
		return false;

	return strstr(resolved, "/" TOUCHPAD_I2C_NAME "/") != NULL;
}

static int selective_reporting_report_id(const char *hidraw_sysfs)
{
	unsigned char descriptor[HID_MAX_DESCRIPTOR_SIZE];
	char descriptor_path[PATH_MAX];
	ssize_t descriptor_size;
	int fd;

	if (snprintf(descriptor_path, sizeof(descriptor_path),
	             "%s/device/report_descriptor", hidraw_sysfs) >=
	    (int)sizeof(descriptor_path))
		return -1;

	fd = open(descriptor_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	descriptor_size = read(fd, descriptor, sizeof(descriptor));
	close(fd);
	if (descriptor_size <= 0)
		return -1;

	for (size_t i = 0;
	     i + sizeof(selective_reporting_marker) + 1 < (size_t)descriptor_size; i++) {
		if (memcmp(&descriptor[i], selective_reporting_marker,
		           sizeof(selective_reporting_marker)))
			continue;

		for (size_t j = i + sizeof(selective_reporting_marker);
		     j + 1 < (size_t)descriptor_size; j++) {
			if (descriptor[j] == 0x85)
				return descriptor[j + 1];
			/* Do not accidentally take a report ID from another collection. */
			if (descriptor[j] == 0xc0)
				break;
		}
	}

	return -1;
}

static int visit_touchpad_hidraw(bool write_state, bool enabled, bool verbose)
{
	glob_t matches = { 0 };
	int found = 0;
	int succeeded = 0;

	if (glob("/sys/class/hidraw/hidraw*", 0, NULL, &matches) != 0)
		return -1;

	for (size_t i = 0; i < matches.gl_pathc; i++) {
		const char *base;
		char devnode[PATH_MAX];
		unsigned char report[2];
		int report_id;
		int fd;

		if (!path_is_uniwill_touchpad(matches.gl_pathv[i]))
			continue;

		found++;
		base = strrchr(matches.gl_pathv[i], '/');
		if (!base || snprintf(devnode, sizeof(devnode), "/dev/%s", base + 1) >=
		             (int)sizeof(devnode))
			continue;

		report_id = selective_reporting_report_id(matches.gl_pathv[i]);
		if (report_id < 0) {
			g_printerr("uniwill-touchpad-sync: %s has no selective-reporting feature report\n",
			           devnode);
			continue;
		}

		if (verbose)
			g_print("%s: selective-reporting feature report 0x%02x\n",
			        devnode, report_id);

		if (!write_state) {
			succeeded++;
			continue;
		}

		fd = open(devnode, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			g_printerr("uniwill-touchpad-sync: cannot open %s: %s\n",
			           devnode, g_strerror(errno));
			continue;
		}

		report[0] = report_id;
		report[1] = enabled ? 0x03 : 0x00;
		if (ioctl(fd, HIDIOCSFEATURE(sizeof(report)), report) < 0) {
			g_printerr("uniwill-touchpad-sync: HID feature write to %s failed: %s\n",
			           devnode, g_strerror(errno));
		} else {
			succeeded++;
		}
		close(fd);
	}

	globfree(&matches);
	if (!found) {
		g_printerr("uniwill-touchpad-sync: no " TOUCHPAD_I2C_NAME
		           " hidraw device found\n");
		return -1;
	}

	return succeeded > 0 ? 0 : -1;
}

static int set_touchpad_state_via_service(bool enabled)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	char request[96];
	char response[512];
	ssize_t length;
	int fd;

	if (strlen(UNIWILLD_SOCKET) >= sizeof(address.sun_path))
		return -1;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", UNIWILLD_SOCKET);

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		close(fd);
		return -1;
	}

	length = snprintf(request, sizeof(request),
	                  "{\"cmd\":\"set_touchpad_state\",\"enabled\":%s}\n",
	                  enabled ? "true" : "false");
	if (write(fd, request, (size_t)length) != length) {
		close(fd);
		return -1;
	}
	shutdown(fd, SHUT_WR);
	length = read(fd, response, sizeof(response) - 1);
	close(fd);
	if (length <= 0)
		return -1;
	response[length] = '\0';
	return strstr(response, "\"ok\":true") ? 0 : -1;
}

static int apply_touchpad_state(bool enabled)
{
	return set_touchpad_state_via_service(enabled);
}

static bool read_text_file(const char *path, char *buffer, size_t buffer_size)
{
	FILE *file;

	file = fopen(path, "re");
	if (!file)
		return false;

	if (!fgets(buffer, buffer_size, file)) {
		fclose(file);
		return false;
	}
	fclose(file);
	buffer[strcspn(buffer, "\r\n")] = '\0';
	return true;
}

static bool input_is_touchpad(const char *input_path)
{
	char name_path[PATH_MAX];
	char name[256];

	if (snprintf(name_path, sizeof(name_path), "%s/name", input_path) >=
	    (int)sizeof(name_path) || !read_text_file(name_path, name, sizeof(name)))
		return false;

	return strcasestr(name, "touchpad") != NULL;
}

static int read_kwin_touchpad_state(void)
{
	glob_t inputs = { 0 };
	bool found = false;
	bool any_enabled = false;

	if (!session_bus || glob("/sys/class/input/input*", 0, NULL, &inputs) != 0)
		return -1;

	for (size_t i = 0; i < inputs.gl_pathc; i++) {
		glob_t events = { 0 };
		char event_glob[PATH_MAX];

		if (!input_is_touchpad(inputs.gl_pathv[i]) ||
		    snprintf(event_glob, sizeof(event_glob), "%s/event*",
		             inputs.gl_pathv[i]) >= (int)sizeof(event_glob) ||
		    glob(event_glob, 0, NULL, &events) != 0)
			continue;

		for (size_t j = 0; j < events.gl_pathc; j++) {
			const char *event_name = strrchr(events.gl_pathv[j], '/');
			char object_path[PATH_MAX];
			GVariant *reply;
			GVariant *boxed = NULL;
			GVariant *value = NULL;
			GError *error = NULL;
			gboolean enabled;

			if (!event_name ||
			    snprintf(object_path, sizeof(object_path), KWIN_PATH_PREFIX "%s",
			             event_name + 1) >= (int)sizeof(object_path))
				continue;

			reply = g_dbus_connection_call_sync(
				session_bus, KWIN_NAME, object_path,
				"org.freedesktop.DBus.Properties", "Get",
				g_variant_new("(ss)", KWIN_INTERFACE, "enabled"),
				G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500,
				NULL, &error);
			if (!reply) {
				g_clear_error(&error);
				continue;
			}

			g_variant_get(reply, "(@v)", &boxed);
			value = g_variant_get_variant(boxed);
			if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
				g_variant_unref(value);
				g_variant_unref(boxed);
				g_variant_unref(reply);
				continue;
			}
			enabled = g_variant_get_boolean(value);
			found = true;
			any_enabled |= enabled;
			g_variant_unref(value);
			g_variant_unref(boxed);
			g_variant_unref(reply);
		}
		globfree(&events);
	}

	globfree(&inputs);
	return found ? any_enabled : -1;
}

static int read_gnome_touchpad_state(void)
{
	char *send_events;
	int enabled = -1;

	if (!gnome_touchpad_settings)
		return -1;

	send_events = g_settings_get_string(gnome_touchpad_settings, "send-events");
	if (!strcmp(send_events, "disabled"))
		enabled = 0;
	else if (!strcmp(send_events, "enabled") ||
	         !strcmp(send_events, "disabled-on-external-mouse"))
		enabled = 1;
	g_free(send_events);
	return enabled;
}

static int read_sysfs_touchpad_state(void)
{
	glob_t inputs = { 0 };
	bool found = false;
	bool any_enabled = false;

	if (glob("/sys/class/input/input*", 0, NULL, &inputs) != 0)
		return -1;

	for (size_t i = 0; i < inputs.gl_pathc; i++) {
		char inhibited_path[PATH_MAX];
		char value[32];

		if (!input_is_touchpad(inputs.gl_pathv[i]) ||
		    snprintf(inhibited_path, sizeof(inhibited_path), "%s/inhibited",
		             inputs.gl_pathv[i]) >= (int)sizeof(inhibited_path) ||
		    !read_text_file(inhibited_path, value, sizeof(value)))
			continue;

		if (strcmp(value, "0") && strcmp(value, "1"))
			continue;
		found = true;
		any_enabled |= !strcmp(value, "0");
	}

	globfree(&inputs);
	return found ? any_enabled : -1;
}

static int read_desktop_touchpad_state(void)
{
	int enabled;

	enabled = read_kwin_touchpad_state();
	if (enabled >= 0)
		return enabled;
	enabled = read_gnome_touchpad_state();
	if (enabled >= 0)
		return enabled;
	return read_sysfs_touchpad_state();
}

static void sync_touchpad_state(bool force)
{
	int enabled = read_desktop_touchpad_state();

	if (enabled < 0 || (!force && enabled == last_applied_state))
		return;

	if (apply_touchpad_state(enabled) == 0) {
		last_applied_state = enabled;
		g_message("touchpad %s; firmware disabled LED %s",
		          enabled ? "enabled" : "disabled",
		          enabled ? "off" : "on");
	}
}

static gboolean retry_sync(gpointer user_data)
{
	(void)user_data;
	sync_touchpad_state(false);
	return G_SOURCE_CONTINUE;
}

static gboolean delayed_desktop_sync(gpointer user_data)
{
	(void)user_data;
	desktop_sync_timer_id = 0;
	sync_touchpad_state(false);
	return G_SOURCE_REMOVE;
}

static void schedule_desktop_sync(void)
{
	if (desktop_sync_timer_id)
		g_source_remove(desktop_sync_timer_id);
	desktop_sync_timer_id = g_timeout_add(
		KWIN_CONFIG_SYNC_DELAY_MS, delayed_desktop_sync, NULL);
}

static gboolean wakeup_sync(gpointer user_data)
{
	(void)user_data;
	wakeup_timer_id = 0;
	last_applied_state = -1;
	sync_touchpad_state(true);
	return G_SOURCE_REMOVE;
}

static void session_signal(GDBusConnection *connection, const gchar *sender_name,
			   const gchar *object_path, const gchar *interface_name,
			   const gchar *signal_name, GVariant *parameters,
			   gpointer user_data)
{
	(void)connection;
	(void)sender_name;
	(void)interface_name;
	(void)signal_name;
	(void)parameters;
	(void)user_data;

	if (object_path && g_str_has_prefix(object_path, KWIN_PATH_PREFIX))
		sync_touchpad_state(false);
}

static void name_owner_changed(GDBusConnection *connection, const gchar *sender_name,
			       const gchar *object_path, const gchar *interface_name,
			       const gchar *signal_name, GVariant *parameters,
			       gpointer user_data)
{
	const gchar *name;
	const gchar *old_owner;
	const gchar *new_owner;

	(void)connection;
	(void)sender_name;
	(void)object_path;
	(void)interface_name;
	(void)signal_name;
	(void)user_data;

	g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
	if (!strcmp(name, KWIN_NAME)) {
		(void)old_owner;
		last_applied_state = -1;
		if (new_owner[0])
			sync_touchpad_state(true);
	}
}

static void prepare_for_sleep(GDBusConnection *connection, const gchar *sender_name,
			      const gchar *object_path, const gchar *interface_name,
			      const gchar *signal_name, GVariant *parameters,
			      gpointer user_data)
{
	gboolean sleeping;

	(void)connection;
	(void)sender_name;
	(void)object_path;
	(void)interface_name;
	(void)signal_name;
	(void)user_data;

	g_variant_get(parameters, "(b)", &sleeping);
	if (!sleeping) {
		if (wakeup_timer_id)
			g_source_remove(wakeup_timer_id);
		wakeup_timer_id = g_timeout_add_seconds(1, wakeup_sync, NULL);
	}
}

static void gnome_settings_changed(GSettings *settings, gchar *key, gpointer user_data)
{
	(void)settings;
	(void)key;
	(void)user_data;
	sync_touchpad_state(false);
}

static void kwin_config_changed(GFileMonitor *monitor, GFile *file,
				GFile *other_file, GFileMonitorEvent event,
				gpointer user_data)
{
	(void)monitor;
	(void)file;
	(void)other_file;
	(void)user_data;

	switch (event) {
	case G_FILE_MONITOR_EVENT_CHANGED:
	case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
	case G_FILE_MONITOR_EVENT_CREATED:
	case G_FILE_MONITOR_EVENT_MOVED_IN:
	case G_FILE_MONITOR_EVENT_RENAMED:
		schedule_desktop_sync();
		break;
	default:
		break;
	}
}

static void setup_kwin_config_monitor(void)
{
	GError *error = NULL;
	GFile *file;
	char *path;

	path = g_build_filename(g_get_user_config_dir(), "kcminputrc", NULL);
	file = g_file_new_for_path(path);
	kwin_config_monitor = g_file_monitor_file(
		file, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
	if (!kwin_config_monitor) {
		g_warning("cannot monitor %s for touchpad changes: %s", path,
		          error ? error->message : "unknown error");
		g_clear_error(&error);
	} else {
		g_signal_connect(kwin_config_monitor, "changed",
		                 G_CALLBACK(kwin_config_changed), NULL);
	}

	g_object_unref(file);
	g_free(path);
}

static void setup_gnome_settings(void)
{
	GSettingsSchemaSource *source = g_settings_schema_source_get_default();
	GSettingsSchema *schema;

	if (!source)
		return;
	schema = g_settings_schema_source_lookup(
		source, "org.gnome.desktop.peripherals.touchpad", true);
	if (!schema)
		return;

	gnome_touchpad_settings = g_settings_new_full(schema, NULL, NULL);
	g_settings_schema_unref(schema);
	g_signal_connect(gnome_touchpad_settings, "changed::send-events",
	                 G_CALLBACK(gnome_settings_changed), NULL);
}

static int run_daemon(void)
{
	GError *error = NULL;

	session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (!session_bus) {
		g_printerr("uniwill-touchpad-sync: cannot connect to session D-Bus: %s\n",
		           error ? error->message : "unknown error");
		g_clear_error(&error);
		return EXIT_FAILURE;
	}

	setup_gnome_settings();
	setup_kwin_config_monitor();
	g_dbus_connection_signal_subscribe(
		session_bus, KWIN_NAME, "org.freedesktop.DBus.Properties",
		"PropertiesChanged", NULL, KWIN_INTERFACE,
		G_DBUS_SIGNAL_FLAGS_NONE, session_signal, NULL, NULL);
	g_dbus_connection_signal_subscribe(
		session_bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
		"NameOwnerChanged", "/org/freedesktop/DBus", KWIN_NAME,
		G_DBUS_SIGNAL_FLAGS_NONE, name_owner_changed, NULL, NULL);

	system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (system_bus) {
		g_dbus_connection_signal_subscribe(
			system_bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
			"PrepareForSleep", "/org/freedesktop/login1", NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, prepare_for_sleep, NULL, NULL);
	}

	main_loop = g_main_loop_new(NULL, false);
	sync_touchpad_state(true);
	g_timeout_add(RETRY_INTERVAL_MS, retry_sync, NULL);
	g_main_loop_run(main_loop);

	if (desktop_sync_timer_id)
		g_source_remove(desktop_sync_timer_id);
	if (wakeup_timer_id)
		g_source_remove(wakeup_timer_id);
	g_clear_object(&kwin_config_monitor);
	g_clear_object(&gnome_touchpad_settings);
	g_clear_object(&system_bus);
	g_clear_object(&session_bus);
	g_clear_pointer(&main_loop, g_main_loop_unref);
	return EXIT_SUCCESS;
}

static void usage(const char *program)
{
	fprintf(stderr,
	        "Usage: %s [--probe | --status | --set-state enabled|disabled | --once]\n",
	        program);
}

static int connect_session_and_read_state(void)
{
	GError *error = NULL;
	int enabled;

	session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (!session_bus) {
		g_printerr("uniwill-touchpad-sync: cannot connect to session D-Bus: %s\n",
		           error ? error->message : "unknown error");
		g_clear_error(&error);
		return -1;
	}
	setup_gnome_settings();
	enabled = read_desktop_touchpad_state();
	if (enabled < 0)
		g_printerr("uniwill-touchpad-sync: cannot determine desktop touchpad state\n");
	return enabled;
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return run_daemon();

	if (argc == 2 && !strcmp(argv[1], "--probe"))
		return visit_touchpad_hidraw(false, false, true) == 0 ?
		       EXIT_SUCCESS : EXIT_FAILURE;

	if (argc == 2 && !strcmp(argv[1], "--status")) {
		int enabled = connect_session_and_read_state();

		if (enabled < 0 || visit_touchpad_hidraw(false, false, true) < 0)
			return EXIT_FAILURE;
		g_print("desktop touchpad state: %s; expected firmware LED: %s\n",
		        enabled ? "enabled" : "disabled",
		        enabled ? "off" : "on");
		return EXIT_SUCCESS;
	}

	if (argc == 2 && !strcmp(argv[1], "--once")) {
		int enabled = connect_session_and_read_state();

		if (enabled < 0)
			return EXIT_FAILURE;
		return apply_touchpad_state(enabled) == 0 ?
		       EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (argc == 3 && !strcmp(argv[1], "--set-state")) {
		if (!strcmp(argv[2], "enabled"))
			return apply_touchpad_state(true) == 0 ?
			       EXIT_SUCCESS : EXIT_FAILURE;
		if (!strcmp(argv[2], "disabled"))
			return apply_touchpad_state(false) == 0 ?
			       EXIT_SUCCESS : EXIT_FAILURE;
	}

	usage(argv[0]);
	return EXIT_FAILURE;
}
