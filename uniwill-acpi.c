// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for Uniwill notebooks.
 *
 * Special thanks go to Pőcze Barnabás, Christoffer Sandberg and Werner Sembach
 * for supporting the development of this driver either through prior work or
 * by answering questions regarding the underlying ACPI and WMI interfaces.
 *
 * Copyright (C) 2025 Armin Wolf <W_Armin@gmx.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/fixp-arith.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i8042.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/leds.h>
#include <linux/led-class-multicolor.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/units.h>
#include <linux/wmi.h>
#include <linux/workqueue.h>

#include <acpi/battery.h>

#include "uniwill-wmi.h"
#include "uniwill-ite8291.h"

/* WMI GUID for EC read/write (MGMT_GUID_BC in Tuxedo terminology) */
#define UNIWILL_WMI_EC_GUID	"ABBC0F6F-8EA1-11D1-00A0-C90629100000"
#define UNIWILL_WMI_EC_INSTANCE	0x00
#define UNIWILL_WMI_EC_METHOD	0x04
#define UNIWILL_WMI_FUNC_WRITE	0
#define UNIWILL_WMI_FUNC_READ	1
#define UNIWILL_WMI_EC_RETRIES	3

#define EC_ADDR_BAT_POWER_UNIT_1	0x0400

#define EC_ADDR_BAT_POWER_UNIT_2	0x0401

#define EC_ADDR_BAT_DESIGN_CAPACITY_1	0x0402

#define EC_ADDR_BAT_DESIGN_CAPACITY_2	0x0403

#define EC_ADDR_BAT_FULL_CAPACITY_1	0x0404

#define EC_ADDR_BAT_FULL_CAPACITY_2	0x0405

#define EC_ADDR_BAT_DESIGN_VOLTAGE_1	0x0408

#define EC_ADDR_BAT_DESIGN_VOLTAGE_2	0x0409

#define EC_ADDR_BAT_STATUS_1		0x0432
#define BAT_DISCHARGING			BIT(0)

#define EC_ADDR_BAT_STATUS_2		0x0433

#define EC_ADDR_BAT_CURRENT_1		0x0434

#define EC_ADDR_BAT_CURRENT_2		0x0435

#define EC_ADDR_BAT_REMAIN_CAPACITY_1	0x0436

#define EC_ADDR_BAT_REMAIN_CAPACITY_2	0x0437

#define EC_ADDR_BAT_VOLTAGE_1		0x0438

#define EC_ADDR_BAT_VOLTAGE_2		0x0439

#define EC_ADDR_CPU_TEMP		0x043E

#define EC_ADDR_GPU_TEMP		0x044F

#define EC_ADDR_SYSTEM_ID		0x0456
#define HAS_GPU				BIT(7)

#define EC_ADDR_MAIN_FAN_RPM_1		0x0464

#define EC_ADDR_MAIN_FAN_RPM_2		0x0465

#define EC_ADDR_SECOND_FAN_RPM_1	0x046C

#define EC_ADDR_SECOND_FAN_RPM_2	0x046D

#define EC_ADDR_DEVICE_STATUS		0x047B
#define WIFI_STATUS_ON			BIT(7)
/* BIT(5) is also unset depending on the rfkill state (bluetooth?) */

#define EC_ADDR_BAT_ALERT		0x0494

#define EC_ADDR_BAT_CYCLE_COUNT_1	0x04A6

#define EC_ADDR_BAT_CYCLE_COUNT_2	0x04A7

#define EC_ADDR_PROJECT_ID		0x0740

#define EC_ADDR_AP_OEM			0x0741
#define	ENABLE_MANUAL_CTRL		BIT(0)
#define ITE_KBD_EFFECT_REACTIVE		BIT(3)
#define FAN_ABNORMAL			BIT(5)

#define EC_ADDR_SUPPORT_5		0x0742
#define FAN_TURBO_SUPPORTED		BIT(4)
#define FAN_SUPPORT			BIT(5)

#define EC_ADDR_CTGP_DB_CTRL		0x0743
#define CTGP_DB_GENERAL_ENABLE		BIT(0)
#define CTGP_DB_DB_ENABLE		BIT(1)
#define CTGP_DB_CTGP_ENABLE		BIT(2)

#define EC_ADDR_CTGP_DB_CTGP_OFFSET	0x0744

#define EC_ADDR_CTGP_DB_TPP_OFFSET	0x0745

#define EC_ADDR_CTGP_DB_DB_OFFSET	0x0746

#define EC_ADDR_LIGHTBAR_AC_CTRL	0x0748
#define LIGHTBAR_APP_EXISTS		BIT(0)
#define LIGHTBAR_POWER_SAVE		BIT(1)
#define LIGHTBAR_S0_OFF			BIT(2)
#define LIGHTBAR_S3_OFF			BIT(3)	// Breathing animation when suspended
#define LIGHTBAR_WELCOME		BIT(7)	// Rainbow animation
#define LIGHTBAR_MODE_MASK		(LIGHTBAR_APP_EXISTS | LIGHTBAR_S0_OFF | \
					 LIGHTBAR_S3_OFF | LIGHTBAR_WELCOME)
#define LIGHTBAR_MODE_PRESERVE_MASK	0x72
#define LIGHTBAR_MODE_SOLID		0x09
#define LIGHTBAR_MODE_SOLID_BREATHING	0x01
#define LIGHTBAR_MODE_RAINBOW		0x89
#define LIGHTBAR_MODE_RAINBOW_BREATHING	0x81
#define LIGHTBAR_MODE_OFF		0x05

#define EC_ADDR_LIGHTBAR_AC_RED		0x0749

#define EC_ADDR_LIGHTBAR_AC_GREEN	0x074A

#define EC_ADDR_LIGHTBAR_AC_BLUE	0x074B

#define EC_ADDR_BIOS_OEM		0x074E
#define FN_LOCK_STATUS			BIT(4)

#define EC_ADDR_MANUAL_FAN_CTRL		0x0751
#define FAN_LEVEL_MASK			GENMASK(2, 0)
#define FAN_MODE_TURBO			BIT(4)
#define FAN_MODE_HIGH			BIT(5)
#define FAN_MODE_BOOST			BIT(6)
#define FAN_MODE_USER			BIT(7)

#define EC_ADDR_PWM_1			0x075B

#define EC_ADDR_PWM_2			0x075C

/* Unreliable */
#define EC_ADDR_SUPPORT_1		0x0765
#define AIRPLANE_MODE			BIT(0)
#define GPS_SWITCH			BIT(1)
#define OVERCLOCK			BIT(2)
#define MACRO_KEY			BIT(3)
#define SHORTCUT_KEY			BIT(4)
#define SUPER_KEY_LOCK			BIT(5)
#define LIGHTBAR			BIT(6)
#define FAN_BOOST			BIT(7)

#define EC_ADDR_SUPPORT_2		0x0766
#define SILENT_MODE			BIT(0)
#define USB_CHARGING			BIT(1)
#define RGB_KEYBOARD			BIT(2)
#define CHINA_MODE			BIT(5)
#define MY_BATTERY			BIT(6)

#define EC_ADDR_TRIGGER			0x0767
#define TRIGGER_SUPER_KEY_LOCK		BIT(0)
#define TRIGGER_LIGHTBAR		BIT(1)
#define TRIGGER_FAN_BOOST		BIT(2)
#define TRIGGER_SILENT_MODE		BIT(3)
#define TRIGGER_USB_CHARGING		BIT(4)
#define RGB_APPLY_COLOR			BIT(5)
#define RGB_LOGO_EFFECT			BIT(6)
#define RGB_RAINBOW_EFFECT		BIT(7)

#define EC_ADDR_SWITCH_STATUS		0x0768
#define SUPER_KEY_LOCK_STATUS		BIT(0)
#define LIGHTBAR_STATUS			BIT(1)
#define FAN_BOOST_STATUS		BIT(2)
#define MACRO_KEY_STATUS		BIT(3)
#define MY_BAT_POWER_BAT_STATUS		BIT(4)

#define EC_ADDR_RGB_RED			0x0769

#define EC_ADDR_RGB_GREEN		0x076A

#define EC_ADDR_RGB_BLUE		0x076B

#define EC_ADDR_ROMID_START		0x0770
#define ROMID_LENGTH			14

#define EC_ADDR_ROMID_EXTRA_1		0x077E

#define EC_ADDR_ROMID_EXTRA_2		0x077F

#define EC_ADDR_BIOS_OEM_2		0x0782
#define FAN_V2_NEW			BIT(0)
#define FAN_QKEY			BIT(1)
#define FAN_TABLE_OFFICE_MODE		BIT(2)
#define FAN_V3				BIT(3)
#define DEFAULT_MODE			BIT(4)

#define EC_ADDR_PL1_SETTING		0x0783

#define EC_ADDR_PL2_SETTING		0x0784

#define EC_ADDR_PL4_SETTING		0x0785

#define EC_ADDR_GPU_DSTATE		0x078B
#define GPU_DSTATE_MASK			GENMASK(2, 0)

#define EC_ADDR_PERFORMANCE_PL1_DEFAULT	0x07A7
#define EC_ADDR_PERFORMANCE_PL2_DEFAULT	0x07A8
#define EC_ADDR_PERFORMANCE_DSTATE_DEFAULT 0x07AA
#define EC_ADDR_BALANCED_PL1_DEFAULT	0x0730
#define EC_ADDR_BALANCED_PL2_DEFAULT	0x0731
#define EC_ADDR_BALANCED_DSTATE_DEFAULT	0x0733
#define EC_ADDR_SAVER_PL1_DEFAULT	0x0734
#define EC_ADDR_SAVER_PL2_DEFAULT	0x0735
#define EC_ADDR_SAVER_DSTATE_DEFAULT	0x0737

#define EC_ADDR_FAN_DEFAULT		0x0786
#define FAN_CURVE_LENGTH		5

#define EC_ADDR_KBD_STATUS		0x078C
#define KBD_WHITE_ONLY			BIT(0)	// ~single color
#define KBD_SINGLE_COLOR_OFF		BIT(1)
#define KBD_TURBO_LEVEL_MASK		GENMASK(3, 2)
#define KBD_APPLY			BIT(4)
#define KBD_BRIGHTNESS			GENMASK(7, 5)

#define EC_ADDR_FAN_CTRL		0x078E
#define FAN3P5				BIT(1)
#define CHARGING_PROFILE		BIT(3)
#define UNIVERSAL_FAN_CTRL		BIT(6)

#define EC_ADDR_BIOS_OEM_3		0x07A3
#define FAN_REDUCED_DURY_CYCLE		BIT(5)
#define FAN_ALWAYS_ON			BIT(6)
#define PASSIVE_COOLING_DISABLED	BIT(6)

#define EC_ADDR_BIOS_BYTE		0x07A4
#define FN_LOCK_SWITCH			BIT(3)

#define EC_ADDR_OEM_3			0x07A5
#define POWER_LED_MASK			GENMASK(1, 0)
#define POWER_LED_LEFT			0x00
#define POWER_LED_BOTH			0x01
#define POWER_LED_NONE			0x02
#define FAN_QUIET			BIT(2)
#define OVERBOOST			BIT(4)
#define HIGH_POWER			BIT(7)

#define EC_ADDR_OEM_4			0x07A6
#define OVERBOOST_DYN_TEMP_OFF		BIT(1)
#define TOUCHPAD_TOGGLE_OFF		BIT(6)

#define EC_ADDR_CHARGE_CTRL		0x07B9
#define CHARGE_CTRL_MASK		GENMASK(6, 0)
#define CHARGE_CTRL_REACHED		BIT(7)

#define EC_ADDR_UNIVERSAL_FAN_CTRL	0x07C5
#define SPLIT_TABLES			BIT(7)

#define EC_ADDR_AP_OEM_6		0x07C6
#define ENABLE_UNIVERSAL_FAN_CTRL	BIT(2)
#define BATTERY_CHARGE_FULL_OVER_24H	BIT(3)
#define BATTERY_ERM_STATUS_REACHED	BIT(4)

#define EC_ADDR_PERFORMANCE_MODE	0x07AB
#define EC_FAN_MODE_MASK		GENMASK(3, 0)
#define EC_FAN_MODE_WHISPER		0x00
#define EC_FAN_MODE_PERFORMANCE		0x01
#define EC_FAN_MODE_STANDARD		0x02
#define EC_FAN_MODE_QUIET		0x03
#define UNIWILL_FAN_MODE_PERFORMANCE	1
#define UNIWILL_FAN_MODE_STANDARD	2
#define UNIWILL_FAN_MODE_QUIET		3
#define UNIWILL_FAN_MODE_WHISPER	4
#define UNIWILL_FAN_MODE_BENCHMARK	5

#define EC_ADDR_CHARGE_PRIO		0x07CC
#define CHARGING_PERFORMANCE		BIT(7)

/* Same bits as EC_ADDR_LIGHTBAR_AC_CTRL except LIGHTBAR_S3_OFF */
#define EC_ADDR_LIGHTBAR_BAT_CTRL	0x07E2

#define EC_ADDR_LIGHTBAR_BAT_RED	0x07E3

#define EC_ADDR_LIGHTBAR_BAT_GREEN	0x07E4

#define EC_ADDR_LIGHTBAR_BAT_BLUE	0x07E5

#define EC_ADDR_CPU_TEMP_END_TABLE	0x0F00

#define EC_ADDR_CPU_TEMP_START_TABLE	0x0F10

#define EC_ADDR_CPU_FAN_SPEED_TABLE	0x0F20

#define EC_ADDR_GPU_TEMP_END_TABLE	0x0F30

#define EC_ADDR_GPU_TEMP_START_TABLE	0x0F40

#define EC_ADDR_GPU_FAN_SPEED_TABLE	0x0F50

#define EC_ADDR_FAN_TABLE_MAGIC_1	0x0F5D
#define EC_ADDR_FAN_TABLE_MAGIC_2	0x0F5E
#define EC_ADDR_FAN_TABLE_SELECT	0x0F5F
#define FAN_TABLE_MAGIC_1		0xFD
#define FAN_TABLE_MAGIC_2		0xC9

/*
 * Those two registers technically allow for manual fan control,
 * but are unstable on some models and are likely not meant to
 * be used by applications as they are only accessible when using
 * the WMI interface.
 */
#define EC_ADDR_PWM_1_WRITEABLE		0x1804

#define EC_ADDR_PWM_2_WRITEABLE		0x1809

#define UNIWILL_OSD_TOUCHPADWORKAROUND	0xFFF

#define DRIVER_NAME	"uniwill"

/*
 * The OEM software always sleeps 6 ms after reading/writing EC
 * registers, so we emulate this behaviour for maximum compatibility.
 */
#define UNIWILL_EC_DELAY_US	6000
#define UNIWILL_FAN_WATCHDOG_MS	5000

#define PWM_MAX			200
#define FAN_TABLE_LENGTH	16

#define PL_SETTING_CLEAR	0
#define PL_SETTING_MIN		5
#define PL1_SETTING_MAX		80
#define PL2_SETTING_MAX		100
#define PL4_SETTING_MAX		120

#define LED_CHANNELS		3
#define LED_MAX_BRIGHTNESS	200

#define UNIWILL_FEATURE_FN_LOCK_TOGGLE		BIT(0)
#define UNIWILL_FEATURE_SUPER_KEY_TOGGLE	BIT(1)
#define UNIWILL_FEATURE_TOUCHPAD_TOGGLE		BIT(2)
#define UNIWILL_FEATURE_LIGHTBAR		BIT(3)
#define UNIWILL_FEATURE_BATTERY			BIT(4)
#define UNIWILL_FEATURE_HWMON			BIT(5)
#define UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL	BIT(6)
#define UNIWILL_FEATURE_FAN_CTRL		BIT(7)
#define UNIWILL_FEATURE_TDP_CTRL		BIT(8)
#define UNIWILL_FEATURE_PERF_PROFILE		BIT(9)

#define FAN_ON_MIN_SPEED_PERCENT	25

/*
 * The firmware presets on the NUC X15 do not start the CPU fan until about
 * 70 C.  Keep the EC in charge of the curve, but bring the first transition
 * forward so heat does not build up in the chassis while the machine is
 * lightly loaded.  Quieter modes still start later than performance mode.
 */
#define FAN_START_TEMP_PERFORMANCE	45
#define FAN_START_TEMP_STANDARD		50
#define FAN_START_TEMP_QUIET		53
#define FAN_START_TEMP_WHISPER		55
#define FAN_START_TEMP_HYSTERESIS	5

struct uniwill_data {
	struct device *dev;
	acpi_handle handle;
	struct regmap *regmap;
	unsigned int features;
	struct acpi_battery_hook hook;
	unsigned int last_charge_ctrl;
	bool fans_initialized;
	struct mutex fan_lock;		/* Protects multi-register fan control sequences */
	struct delayed_work fan_watchdog_work;
	u8 active_performance_profile;	/* Selected preset slot, independent of fan mode */
	u8 active_fan_mode;		/* Selected fan policy, independent of the mode LED */
	u8 active_hardware_power_mode;
	struct mutex power_lock;		/* Protects multi-register power mode sequences */
	struct mutex battery_lock;	/* Protects the list of currently registered batteries */
	unsigned int last_switch_status;
	struct mutex super_key_lock;	/* Protects the toggling of the super key lock state */
	struct list_head batteries;
	struct mutex led_lock;		/* Protects writes to the lightbar registers */
	struct led_classdev_mc led_mc_cdev;
	struct mc_subled led_mc_subled_info[LED_CHANNELS];
	struct mutex input_lock;	/* Protects input sequence during notify */
	struct input_dev *input_device;
	struct notifier_block nb;
	struct work_struct touchpad_work;
	struct work_struct performance_mode_work;
};

struct uniwill_battery_entry {
	struct list_head head;
	struct power_supply *battery;
};

struct uniwill_device_descriptor {
	unsigned int features;
	bool adjust_fan_start_temp;
	/* Executed during driver probing */
	int (*probe)(struct uniwill_data *data);
};

static bool force;
module_param_unsafe(force, bool, 0);
MODULE_PARM_DESC(force, "Force loading without checking for supported devices\n");

/*
 * Contains device specific data like the feature bitmap since
 * the associated registers are not always reliable.
 */
static struct uniwill_device_descriptor device_descriptor __ro_after_init;
static bool uniwill_ite8291_registered;

static const char * const uniwill_temp_labels[] = {
	"CPU",
	"GPU",
};

static const char * const uniwill_fan_labels[] = {
	"Main",
	"Secondary",
};

static const struct key_entry uniwill_keymap[] = {
	/* Reported via keyboard controller */
	{ KE_IGNORE,    UNIWILL_OSD_CAPSLOCK,                   { KEY_CAPSLOCK }},
	{ KE_IGNORE,    UNIWILL_OSD_NUMLOCK,                    { KEY_NUMLOCK }},

	/* Reported when the user locks/unlocks the super key */
	{ KE_IGNORE,    UNIWILL_OSD_SUPER_KEY_LOCK_ENABLE,      { KEY_UNKNOWN }},
	{ KE_IGNORE,    UNIWILL_OSD_SUPER_KEY_LOCK_DISABLE,     { KEY_UNKNOWN }},
	/* Optional, might not be reported by all devices */
	{ KE_IGNORE,	UNIWILL_OSD_SUPER_KEY_LOCK_CHANGED,	{ KEY_UNKNOWN }},

	/* Reported in manual mode when toggling the airplane mode status */
	{ KE_KEY,       UNIWILL_OSD_RFKILL,                     { KEY_RFKILL }},
	{ KE_IGNORE,    UNIWILL_OSD_RADIOON,                    { KEY_UNKNOWN }},
	{ KE_IGNORE,    UNIWILL_OSD_RADIOOFF,                   { KEY_UNKNOWN }},

	/* Reported directly by WMI on devices with firmware touchpad handling. */
	{ KE_KEY,       UNIWILL_OSD_TOUCHPAD_ON,                 { KEY_TOUCHPAD_ON }},
	{ KE_KEY,       UNIWILL_OSD_TOUCHPAD_OFF,                { KEY_TOUCHPAD_OFF }},

	/* Reported through i8042 when toggling the touchpad on some devices */
	{ KE_KEY,       UNIWILL_OSD_TOUCHPADWORKAROUND,          { KEY_TOUCHPAD_TOGGLE }},

	/* Handled by firmware unless manual control mode is active */
	{ KE_IGNORE,    UNIWILL_OSD_PERFORMANCE_MODE_TOGGLE,    { KEY_UNKNOWN }},

	/* Reported when the user wants to adjust the brightness of the keyboard */
	{ KE_KEY,       UNIWILL_OSD_KBDILLUMDOWN,               { KEY_KBDILLUMDOWN }},
	{ KE_KEY,       UNIWILL_OSD_KBDILLUMUP,                 { KEY_KBDILLUMUP }},

	/* Reported when the user wants to toggle the microphone mute status */
	{ KE_KEY,       UNIWILL_OSD_MIC_MUTE,                   { KEY_MICMUTE }},

	/* Reported when the user wants to toggle the mute status */
	{ KE_IGNORE,    UNIWILL_OSD_MUTE,                       { KEY_MUTE }},

	/* Reported when the user locks/unlocks the Fn key */
	{ KE_IGNORE,    UNIWILL_OSD_FN_LOCK,                    { KEY_FN_ESC }},

	/* Reported when the user wants to toggle the brightness of the keyboard */
	{ KE_KEY,       UNIWILL_OSD_KBDILLUMTOGGLE,             { KEY_KBDILLUMTOGGLE }},
	{ KE_KEY,       UNIWILL_OSD_KB_LED_LEVEL0,              { KEY_KBDILLUMTOGGLE }},
	{ KE_KEY,       UNIWILL_OSD_KB_LED_LEVEL1,              { KEY_KBDILLUMTOGGLE }},
	{ KE_KEY,       UNIWILL_OSD_KB_LED_LEVEL2,              { KEY_KBDILLUMTOGGLE }},
	{ KE_KEY,       UNIWILL_OSD_KB_LED_LEVEL3,              { KEY_KBDILLUMTOGGLE }},
	{ KE_KEY,       UNIWILL_OSD_KB_LED_LEVEL4,              { KEY_KBDILLUMTOGGLE }},

	/* FIXME: find out the exact meaning of those events */
	{ KE_IGNORE,    UNIWILL_OSD_BAT_CHARGE_FULL_24_H,       { KEY_UNKNOWN }},
	{ KE_IGNORE,    UNIWILL_OSD_BAT_ERM_UPDATE,             { KEY_UNKNOWN }},

	/* Reported when the user wants to toggle the benchmark mode status */
	{ KE_IGNORE,    UNIWILL_OSD_BENCHMARK_MODE_TOGGLE,      { KEY_UNKNOWN }},

	/* Reported when the user wants to toggle the webcam */
	{ KE_IGNORE,    UNIWILL_OSD_WEBCAM_TOGGLE,              { KEY_UNKNOWN }},

	{ KE_END }
};

static inline bool uniwill_device_supports(struct uniwill_data *data,
					   unsigned int features)
{
	return (data->features & features) == features;
}

/* ECRR/ECRW and the WMI method access the same physical EC interface. */
static DEFINE_MUTEX(uniwill_ec_lock);

static int uniwill_ec_reg_write(void *context, unsigned int reg, unsigned int val)
{
	union acpi_object params[2] = {
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = reg,
			},
		},
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = val,
			},
		},
	};
	struct uniwill_data *data = context;
	struct acpi_object_list input = {
		.count = ARRAY_SIZE(params),
		.pointer = params,
	};
	acpi_status status;

	mutex_lock(&uniwill_ec_lock);
	status = acpi_evaluate_object(data->handle, "ECRW", &input, NULL);
	usleep_range(UNIWILL_EC_DELAY_US, UNIWILL_EC_DELAY_US * 2);
	mutex_unlock(&uniwill_ec_lock);

	return ACPI_FAILURE(status) ? -EIO : 0;
}

static int uniwill_ec_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	union acpi_object params[1] = {
		{
			.integer = {
				.type = ACPI_TYPE_INTEGER,
				.value = reg,
			},
		},
	};
	struct uniwill_data *data = context;
	struct acpi_object_list input = {
		.count = ARRAY_SIZE(params),
		.pointer = params,
	};
	unsigned long long output;
	acpi_status status;

	mutex_lock(&uniwill_ec_lock);
	status = acpi_evaluate_integer(data->handle, "ECRR", &input, &output);
	usleep_range(UNIWILL_EC_DELAY_US, UNIWILL_EC_DELAY_US * 2);
	mutex_unlock(&uniwill_ec_lock);

	if (ACPI_FAILURE(status))
		return -EIO;

	if (output > U8_MAX)
		return -ENXIO;

	*val = output;

	return 0;
}

static const struct regmap_bus uniwill_ec_bus = {
	.reg_write = uniwill_ec_reg_write,
	.reg_read = uniwill_ec_reg_read,
	.reg_format_endian_default = REGMAP_ENDIAN_LITTLE,
	.val_format_endian_default = REGMAP_ENDIAN_LITTLE,
};

static bool uniwill_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EC_ADDR_AP_OEM:
	case EC_ADDR_LIGHTBAR_AC_CTRL:
	case EC_ADDR_LIGHTBAR_AC_RED:
	case EC_ADDR_LIGHTBAR_AC_GREEN:
	case EC_ADDR_LIGHTBAR_AC_BLUE:
	case EC_ADDR_BIOS_OEM:
	case EC_ADDR_TRIGGER:
	case EC_ADDR_OEM_4:
	case EC_ADDR_CHARGE_CTRL:
	case EC_ADDR_LIGHTBAR_BAT_CTRL:
	case EC_ADDR_LIGHTBAR_BAT_RED:
	case EC_ADDR_LIGHTBAR_BAT_GREEN:
	case EC_ADDR_LIGHTBAR_BAT_BLUE:
	case EC_ADDR_CTGP_DB_CTRL:
	case EC_ADDR_CTGP_DB_CTGP_OFFSET:
	case EC_ADDR_CTGP_DB_TPP_OFFSET:
	case EC_ADDR_CTGP_DB_DB_OFFSET:
	case EC_ADDR_MANUAL_FAN_CTRL:
	case EC_ADDR_PL1_SETTING:
	case EC_ADDR_PL2_SETTING:
	case EC_ADDR_PL4_SETTING:
	case EC_ADDR_GPU_DSTATE:
	case EC_ADDR_UNIVERSAL_FAN_CTRL:
	case EC_ADDR_AP_OEM_6:
	case EC_ADDR_BIOS_OEM_3:
	case EC_ADDR_PERFORMANCE_MODE:
	case EC_ADDR_CPU_TEMP_END_TABLE ... (EC_ADDR_GPU_FAN_SPEED_TABLE + FAN_TABLE_LENGTH - 1):
		return true;
	default:
		return false;
	}
}

static bool uniwill_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EC_ADDR_CPU_TEMP:
	case EC_ADDR_GPU_TEMP:
	case EC_ADDR_MAIN_FAN_RPM_1:
	case EC_ADDR_MAIN_FAN_RPM_2:
	case EC_ADDR_SECOND_FAN_RPM_1:
	case EC_ADDR_SECOND_FAN_RPM_2:
	case EC_ADDR_BAT_ALERT:
	case EC_ADDR_PROJECT_ID:
	case EC_ADDR_AP_OEM:
	case EC_ADDR_LIGHTBAR_AC_CTRL:
	case EC_ADDR_LIGHTBAR_AC_RED:
	case EC_ADDR_LIGHTBAR_AC_GREEN:
	case EC_ADDR_LIGHTBAR_AC_BLUE:
	case EC_ADDR_BIOS_OEM:
	case EC_ADDR_PWM_1:
	case EC_ADDR_PWM_2:
	case EC_ADDR_TRIGGER:
	case EC_ADDR_SWITCH_STATUS:
	case EC_ADDR_OEM_4:
	case EC_ADDR_CHARGE_CTRL:
	case EC_ADDR_LIGHTBAR_BAT_CTRL:
	case EC_ADDR_LIGHTBAR_BAT_RED:
	case EC_ADDR_LIGHTBAR_BAT_GREEN:
	case EC_ADDR_LIGHTBAR_BAT_BLUE:
	case EC_ADDR_SYSTEM_ID:
	case EC_ADDR_CTGP_DB_CTRL:
	case EC_ADDR_CTGP_DB_CTGP_OFFSET:
	case EC_ADDR_CTGP_DB_TPP_OFFSET:
	case EC_ADDR_CTGP_DB_DB_OFFSET:
	case EC_ADDR_MANUAL_FAN_CTRL:
	case EC_ADDR_BIOS_OEM_2:
	case EC_ADDR_PL1_SETTING:
	case EC_ADDR_PL2_SETTING:
	case EC_ADDR_PL4_SETTING:
	case EC_ADDR_GPU_DSTATE:
	case EC_ADDR_PERFORMANCE_PL1_DEFAULT:
	case EC_ADDR_PERFORMANCE_PL2_DEFAULT:
	case EC_ADDR_PERFORMANCE_DSTATE_DEFAULT:
	case EC_ADDR_BALANCED_PL1_DEFAULT:
	case EC_ADDR_BALANCED_PL2_DEFAULT:
	case EC_ADDR_BALANCED_DSTATE_DEFAULT:
	case EC_ADDR_SAVER_PL1_DEFAULT:
	case EC_ADDR_SAVER_PL2_DEFAULT:
	case EC_ADDR_SAVER_DSTATE_DEFAULT:
	case EC_ADDR_FAN_CTRL:
	case EC_ADDR_UNIVERSAL_FAN_CTRL:
	case EC_ADDR_AP_OEM_6:
	case EC_ADDR_OEM_3:
	case EC_ADDR_PERFORMANCE_MODE:
		return true;
	default:
		return false;
	}
}

static bool uniwill_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EC_ADDR_CPU_TEMP:
	case EC_ADDR_GPU_TEMP:
	case EC_ADDR_MAIN_FAN_RPM_1:
	case EC_ADDR_MAIN_FAN_RPM_2:
	case EC_ADDR_SECOND_FAN_RPM_1:
	case EC_ADDR_SECOND_FAN_RPM_2:
	case EC_ADDR_BAT_ALERT:
	case EC_ADDR_AP_OEM:
	case EC_ADDR_BIOS_OEM:
	case EC_ADDR_PWM_1:
	case EC_ADDR_PWM_2:
	case EC_ADDR_TRIGGER:
	case EC_ADDR_SWITCH_STATUS:
	case EC_ADDR_CHARGE_CTRL:
	case EC_ADDR_MANUAL_FAN_CTRL:
	case EC_ADDR_UNIVERSAL_FAN_CTRL:
	case EC_ADDR_AP_OEM_6:
	case EC_ADDR_BIOS_OEM_3:
	case EC_ADDR_PERFORMANCE_MODE:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config uniwill_ec_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.writeable_reg = uniwill_writeable_reg,
	.readable_reg = uniwill_readable_reg,
	.volatile_reg = uniwill_volatile_reg,
	.can_sleep = true,
	.max_register = 0xFFF,
	.cache_type = REGCACHE_MAPLE,
	.use_single_read = true,
	.use_single_write = true,
};

static int uniwill_wmi_ec_read_retry(u16 addr, u8 *val);
static int uniwill_wmi_ec_write_retry(u16 addr, u8 val);
static int uniwill_get_performance_profile(struct uniwill_data *data, u8 *profile);
static int uniwill_set_performance_profile(struct uniwill_data *data, unsigned int profile);

static ssize_t fn_lock_toggle_enable_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t count)
{
	u8 value;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_BIOS_OEM, &value);
	if (ret < 0)
		return ret;
	if (enable == !!(value & FN_LOCK_STATUS))
		return count;

	if (enable)
		value |= FN_LOCK_STATUS;
	else
		value &= ~FN_LOCK_STATUS;

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_BIOS_OEM, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t fn_lock_toggle_enable_show(struct device *dev, struct device_attribute *attr,
					  char *buf)
{
	u8 value;
	int ret;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_BIOS_OEM, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(value & FN_LOCK_STATUS));
}

static DEVICE_ATTR_RW(fn_lock_toggle_enable);

static ssize_t super_key_toggle_enable_store(struct device *dev, struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u8 value;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	guard(mutex)(&data->super_key_lock);

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_SWITCH_STATUS, &value);
	if (ret < 0)
		return ret;

	/*
	 * We can only toggle the super key lock, so we return early if the setting
	 * is already in the correct state.
	 */
	if (enable == !(value & SUPER_KEY_LOCK_STATUS))
		return count;

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_TRIGGER, TRIGGER_SUPER_KEY_LOCK);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t super_key_toggle_enable_show(struct device *dev, struct device_attribute *attr,
					    char *buf)
{
	u8 value;
	int ret;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_SWITCH_STATUS, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !(value & SUPER_KEY_LOCK_STATUS));
}

static DEVICE_ATTR_RW(super_key_toggle_enable);

static ssize_t touchpad_toggle_enable_store(struct device *dev, struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	if (enable)
		value = 0;
	else
		value = TOUCHPAD_TOGGLE_OFF;

	ret = regmap_update_bits(data->regmap, EC_ADDR_OEM_4, TOUCHPAD_TOGGLE_OFF, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t touchpad_toggle_enable_show(struct device *dev, struct device_attribute *attr,
					   char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_OEM_4, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !(value & TOUCHPAD_TOGGLE_OFF));
}

static DEVICE_ATTR_RW(touchpad_toggle_enable);

static int uniwill_lightbar_get_state(struct uniwill_data *data, bool *rainbow, bool *breathing);
static int uniwill_lightbar_apply_effect(struct uniwill_data *data, bool rainbow, bool breathing);

static ssize_t rainbow_animation_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	bool breathing;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	guard(mutex)(&data->led_lock);

	ret = uniwill_lightbar_get_state(data, NULL, &breathing);
	if (ret < 0)
		return ret;

	ret = uniwill_lightbar_apply_effect(data, enable, breathing);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t rainbow_animation_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(value & LIGHTBAR_WELCOME));
}

static DEVICE_ATTR_RW(rainbow_animation);

static ssize_t breathing_in_suspend_store(struct device *dev, struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	bool rainbow;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	guard(mutex)(&data->led_lock);

	ret = uniwill_lightbar_get_state(data, &rainbow, NULL);
	if (ret < 0)
		return ret;

	ret = uniwill_lightbar_apply_effect(data, rainbow, enable);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t breathing_in_suspend_show(struct device *dev, struct device_attribute *attr,
					 char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", !(value & LIGHTBAR_S3_OFF));
}

static DEVICE_ATTR_RW(breathing_in_suspend);

static const unsigned int uniwill_led_channel_to_bat_reg[LED_CHANNELS] = {
	EC_ADDR_LIGHTBAR_BAT_RED,
	EC_ADDR_LIGHTBAR_BAT_GREEN,
	EC_ADDR_LIGHTBAR_BAT_BLUE,
};

static const unsigned int uniwill_led_channel_to_ac_reg[LED_CHANNELS] = {
	EC_ADDR_LIGHTBAR_AC_RED,
	EC_ADDR_LIGHTBAR_AC_GREEN,
	EC_ADDR_LIGHTBAR_AC_BLUE,
};

static unsigned int uniwill_lightbar_mode_value(bool rainbow, bool breathing)
{
	if (rainbow)
		return breathing ? LIGHTBAR_MODE_RAINBOW_BREATHING : LIGHTBAR_MODE_RAINBOW;

	return breathing ? LIGHTBAR_MODE_SOLID_BREATHING : LIGHTBAR_MODE_SOLID;
}

static int uniwill_lightbar_update_ctrl(struct uniwill_data *data, unsigned int mode)
{
	unsigned int value;
	int ret;

	mode &= LIGHTBAR_MODE_MASK;

	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, &value);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL,
			   (value & LIGHTBAR_MODE_PRESERVE_MASK) | mode);
	if (ret < 0)
		return ret;

	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_BAT_CTRL, &value);
	if (ret < 0)
		return ret;

	return regmap_write(data->regmap, EC_ADDR_LIGHTBAR_BAT_CTRL,
			    (value & LIGHTBAR_MODE_PRESERVE_MASK) | mode);
}

static int uniwill_lightbar_get_state(struct uniwill_data *data, bool *rainbow, bool *breathing)
{
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, &value);
	if (ret < 0)
		return ret;

	if (rainbow)
		*rainbow = value & LIGHTBAR_WELCOME;
	if (breathing)
		*breathing = !(value & LIGHTBAR_S3_OFF);

	return 0;
}

static int uniwill_lightbar_apply_effect(struct uniwill_data *data, bool rainbow, bool breathing)
{
	return uniwill_lightbar_update_ctrl(data, uniwill_lightbar_mode_value(rainbow, breathing));
}

static int uniwill_lightbar_apply_solid_mode(struct uniwill_data *data)
{
	bool breathing;
	int ret;

	ret = uniwill_lightbar_get_state(data, NULL, &breathing);
	if (ret < 0)
		return ret;

	return uniwill_lightbar_apply_effect(data, false, breathing);
}

static int uniwill_lightbar_write_color_value(struct uniwill_data *data,
					      int channel, unsigned int value)
{
	int ret;

	if (channel < 0 || channel >= LED_CHANNELS)
		return -EINVAL;

	if (value > LED_MAX_BRIGHTNESS)
		return -EINVAL;

	ret = regmap_write(data->regmap, uniwill_led_channel_to_ac_reg[channel], value);
	if (ret < 0)
		return ret;

	return regmap_write(data->regmap, uniwill_led_channel_to_bat_reg[channel], value);
}

static bool pl_setting_value_valid(unsigned int value, unsigned int max)
{
	if (value == PL_SETTING_CLEAR)
		return true;

	return value >= PL_SETTING_MIN && value <= max;
}

static ssize_t pl_setting_store(struct device *dev, unsigned int addr,
				unsigned int max, const char *buf, size_t count)
{
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value > U8_MAX || !pl_setting_value_valid(value, max))
		return -EINVAL;

	ret = uniwill_wmi_ec_write_retry(addr, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t pl_setting_show(struct device *dev, unsigned int addr, char *buf)
{
	u8 value;
	int ret;

	ret = uniwill_wmi_ec_read_retry(addr, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}

static ssize_t pl1_setting_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	return pl_setting_store(dev, EC_ADDR_PL1_SETTING, PL1_SETTING_MAX, buf, count);
}

static ssize_t pl1_setting_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return pl_setting_show(dev, EC_ADDR_PL1_SETTING, buf);
}

static DEVICE_ATTR_RW(pl1_setting);

static ssize_t pl2_setting_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	return pl_setting_store(dev, EC_ADDR_PL2_SETTING, PL2_SETTING_MAX, buf, count);
}

static ssize_t pl2_setting_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return pl_setting_show(dev, EC_ADDR_PL2_SETTING, buf);
}

static DEVICE_ATTR_RW(pl2_setting);

static ssize_t pl4_setting_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	return pl_setting_store(dev, EC_ADDR_PL4_SETTING, PL4_SETTING_MAX, buf, count);
}

static ssize_t pl4_setting_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return pl_setting_show(dev, EC_ADDR_PL4_SETTING, buf);
}

static DEVICE_ATTR_RW(pl4_setting);

static int uniwill_set_fan_table_active(void)
{
	u8 value;
	int ret;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_AP_OEM_6, &value);
	if (ret < 0)
		return ret;

	if (value & ENABLE_UNIVERSAL_FAN_CTRL)
		return 0;

	return uniwill_wmi_ec_write_retry(EC_ADDR_AP_OEM_6,
					  value | ENABLE_UNIVERSAL_FAN_CTRL);
}

static int uniwill_get_ec_fan_mode(struct uniwill_data *data, u8 *mode)
{
	u8 value;
	int ret;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_PERFORMANCE_MODE, &value);
	if (ret < 0)
		return ret;

	*mode = value & EC_FAN_MODE_MASK;
	return 0;
}

/*
 * EC 0x07ab is the chassis performance-mode indicator, not persistent fan
 * policy state.  The OEM service writes the selected profile (1..3) here and
 * temporarily forces performance (1) while benchmark mode is active.
 */
static int uniwill_set_performance_mode_led(unsigned int profile)
{
	u8 value;
	int ret;

	if (profile < 1 || profile > 3)
		return -EINVAL;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_PERFORMANCE_MODE, &value);
	if (ret < 0)
		return ret;

	value = (value & ~EC_FAN_MODE_MASK) | profile;
	return uniwill_wmi_ec_write_retry(EC_ADDR_PERFORMANCE_MODE, value);
}

static int uniwill_apply_default_fan_table(u8 fan_mode)
{
	u8 table_mode;
	u8 start_temp;
	int ret;

	switch (fan_mode) {
	case UNIWILL_FAN_MODE_PERFORMANCE:
	case UNIWILL_FAN_MODE_BENCHMARK:
		table_mode = EC_FAN_MODE_PERFORMANCE;
		start_temp = FAN_START_TEMP_PERFORMANCE;
		break;
	case UNIWILL_FAN_MODE_STANDARD:
		table_mode = EC_FAN_MODE_STANDARD;
		start_temp = FAN_START_TEMP_STANDARD;
		break;
	case UNIWILL_FAN_MODE_QUIET:
		table_mode = EC_FAN_MODE_QUIET;
		start_temp = FAN_START_TEMP_QUIET;
		break;
	case UNIWILL_FAN_MODE_WHISPER:
		/* LAPKC71F firmware rejects selector 0; its quiet preset is the
		 * available EC-side fallback for NVIDIA WhisperMode on Linux. */
		table_mode = EC_FAN_MODE_QUIET;
		start_temp = FAN_START_TEMP_WHISPER;
		break;
	default:
		return -EINVAL;
	}

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_FAN_TABLE_MAGIC_1, FAN_TABLE_MAGIC_1);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_FAN_TABLE_MAGIC_2, FAN_TABLE_MAGIC_2);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_FAN_TABLE_SELECT, table_mode);
	if (ret < 0)
		return ret;

	for (int i = 0; i < 10; i++) {
		u8 magic1;
		u8 magic2;

		usleep_range(10000, 20000);

		ret = uniwill_wmi_ec_read_retry(EC_ADDR_FAN_TABLE_MAGIC_1, &magic1);
		if (ret < 0)
			return ret;

		ret = uniwill_wmi_ec_read_retry(EC_ADDR_FAN_TABLE_MAGIC_2, &magic2);
		if (ret < 0)
			return ret;

		/* Firmware consumes the request by changing at least one handshake byte. */
		if (magic1 != FAN_TABLE_MAGIC_1 || magic2 != FAN_TABLE_MAGIC_2)
			break;

		if (i == 9)
			return -EIO;
	}

	if (!device_descriptor.adjust_fan_start_temp)
		return uniwill_set_fan_table_active();

	/*
	 * Only replace the first transition and its stop temperature.  The
	 * firmware-provided speeds and all medium/high-temperature safeguards
	 * remain intact.  This applies to both fans and to every AC/DC profile
	 * because the selected EC table is installed whenever a profile branch
	 * becomes active.
	 */
	ret = uniwill_wmi_ec_write_retry(EC_ADDR_CPU_TEMP_END_TABLE, start_temp);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_CPU_TEMP_START_TABLE + 1,
					start_temp - FAN_START_TEMP_HYSTERESIS);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_GPU_TEMP_END_TABLE, start_temp);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_GPU_TEMP_START_TABLE + 1,
					start_temp - FAN_START_TEMP_HYSTERESIS);
	if (ret < 0)
		return ret;

	/* The selector has already copied the firmware preset into the active table. */
	return uniwill_set_fan_table_active();
}

static int __uniwill_set_fan_boost(struct uniwill_data *data, bool enabled)
{
	int ret;

	ret = regmap_set_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
	if (ret < 0)
		return ret;

	return uniwill_wmi_ec_write_retry(EC_ADDR_MANUAL_FAN_CTRL,
					  enabled ? FAN_MODE_BOOST : 0);
}

static int uniwill_set_fan_boost(struct uniwill_data *data, bool enabled)
{
	guard(mutex)(&data->fan_lock);

	return __uniwill_set_fan_boost(data, enabled);
}

static int uniwill_fan_mode_to_ec_mode(unsigned int fan_mode, u8 *ec_mode)
{
	switch (fan_mode) {
	case UNIWILL_FAN_MODE_PERFORMANCE:
	case UNIWILL_FAN_MODE_BENCHMARK:
		*ec_mode = EC_FAN_MODE_PERFORMANCE;
		return 0;
	case UNIWILL_FAN_MODE_STANDARD:
		*ec_mode = EC_FAN_MODE_STANDARD;
		return 0;
	case UNIWILL_FAN_MODE_QUIET:
		*ec_mode = EC_FAN_MODE_QUIET;
		return 0;
	case UNIWILL_FAN_MODE_WHISPER:
		*ec_mode = EC_FAN_MODE_QUIET;
		return 0;
	default:
		return -EINVAL;
	}
}

static int uniwill_ec_mode_to_fan_mode(u8 ec_mode, u8 *fan_mode)
{
	switch (ec_mode) {
	case EC_FAN_MODE_WHISPER:
		*fan_mode = UNIWILL_FAN_MODE_WHISPER;
		return 0;
	case EC_FAN_MODE_PERFORMANCE:
		*fan_mode = UNIWILL_FAN_MODE_PERFORMANCE;
		return 0;
	case EC_FAN_MODE_STANDARD:
		*fan_mode = UNIWILL_FAN_MODE_STANDARD;
		return 0;
	case EC_FAN_MODE_QUIET:
		*fan_mode = UNIWILL_FAN_MODE_QUIET;
		return 0;
	default:
		return -EIO;
	}
}

static int uniwill_set_fan_mode(struct uniwill_data *data, unsigned int fan_mode)
{
	u8 ec_mode;
	u8 profile;
	int ret;

	guard(mutex)(&data->fan_lock);

	ret = uniwill_fan_mode_to_ec_mode(fan_mode, &ec_mode);
	if (ret < 0)
		return ret;

	ret = regmap_set_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
	if (ret < 0)
		return ret;

	ret = __uniwill_set_fan_boost(data, fan_mode == UNIWILL_FAN_MODE_BENCHMARK);
	if (ret < 0)
		return ret;

	/*
	 * Manual curve control owns the active fan table. Copying a firmware
	 * preset here would overwrite that table and wait for a slow EC
	 * handshake, only for userspace to restore the curve immediately.
	 */
	if (!data->fans_initialized) {
		ret = uniwill_apply_default_fan_table(fan_mode);
		if (ret < 0)
			return ret;
	}

	/* Benchmark owns both fans and the performance indicator until disabled. */
	profile = READ_ONCE(data->active_performance_profile);
	if (fan_mode == UNIWILL_FAN_MODE_BENCHMARK)
		profile = 1;
	else if (profile < 1 || profile > 3)
		profile = ec_mode;

	ret = uniwill_set_performance_mode_led(profile);
	if (ret < 0)
		return ret;

	WRITE_ONCE(data->active_fan_mode, fan_mode);
	return 0;
}

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int mode;
	int ret;

	ret = kstrtouint(buf, 0, &mode);
	if (ret < 0)
		return ret;

	ret = uniwill_set_fan_mode(data, mode);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t fan_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u8 ec_mode;
	u8 fan_mode;
	int ret;
	u8 cached_mode = READ_ONCE(data->active_fan_mode);

	if (cached_mode >= UNIWILL_FAN_MODE_PERFORMANCE &&
	    cached_mode <= UNIWILL_FAN_MODE_BENCHMARK)
		return sysfs_emit(buf, "%u\n", cached_mode);

	ret = uniwill_get_ec_fan_mode(data, &ec_mode);
	if (ret < 0)
		return ret;

	ret = uniwill_ec_mode_to_fan_mode(ec_mode, &fan_mode);
	if (ret < 0)
		return ret;

	if (fan_mode == UNIWILL_FAN_MODE_PERFORMANCE) {
		u8 boost;

		ret = uniwill_wmi_ec_read_retry(EC_ADDR_MANUAL_FAN_CTRL, &boost);
		if (ret < 0)
			return ret;

		if (boost & FAN_MODE_BOOST)
			fan_mode = UNIWILL_FAN_MODE_BENCHMARK;
	}

	WRITE_ONCE(data->active_fan_mode, fan_mode);

	return sysfs_emit(buf, "%u\n", fan_mode);
}

static DEVICE_ATTR_RW(fan_mode);

static ssize_t passive_cooling_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	bool enabled;
	u8 value;
	int ret;

	ret = kstrtobool(buf, &enabled);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_BIOS_OEM_3, &value);
	if (ret < 0)
		return ret;
	if (enabled == !(value & PASSIVE_COOLING_DISABLED))
		return count;

	if (enabled)
		value &= ~PASSIVE_COOLING_DISABLED;
	else
		value |= PASSIVE_COOLING_DISABLED;

	ret = uniwill_wmi_ec_write_retry(EC_ADDR_BIOS_OEM_3, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t passive_cooling_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	u8 value;
	int ret;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_BIOS_OEM_3, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", !(value & PASSIVE_COOLING_DISABLED));
}

static DEVICE_ATTR_RW(passive_cooling);

static ssize_t performance_profile_store(struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int profile;
	int ret;

	ret = kstrtouint(buf, 0, &profile);
	if (ret < 0)
		return ret;

	ret = uniwill_set_performance_profile(data, profile);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t performance_profile_show(struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u8 profile;
	int ret;

	ret = uniwill_get_performance_profile(data, &profile);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", profile);
}

static DEVICE_ATTR_RW(performance_profile);

static ssize_t ec_debug_dump_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	u8 value;
	int ret;

	for (int reg = 0x0700; reg <= 0x07ff; reg++) {
		ret = uniwill_wmi_ec_read_retry(reg, &value);
		if (ret < 0)
			len += sysfs_emit_at(buf, len, "0x%04x=ERR%d\n", reg, ret);
		else
			len += sysfs_emit_at(buf, len, "0x%04x=0x%02x\n", reg, value);
	}

	for (int reg = 0x1800; reg <= 0x1810; reg++) {
		ret = uniwill_wmi_ec_read_retry(reg, &value);
		if (ret < 0)
			len += sysfs_emit_at(buf, len, "0x%04x=ERR%d\n", reg, ret);
		else
			len += sysfs_emit_at(buf, len, "0x%04x=0x%02x\n", reg, value);
	}

	for (int reg = 0x0f00; reg <= 0x0f5f; reg++) {
		ret = uniwill_wmi_ec_read_retry(reg, &value);
		if (ret < 0)
			len += sysfs_emit_at(buf, len, "0x%04x=ERR%d\n", reg, ret);
		else
			len += sysfs_emit_at(buf, len, "0x%04x=0x%02x\n", reg, value);
	}

	return len;
}

static DEVICE_ATTR_RO(ec_debug_dump);

static int uniwill_get_performance_profile(struct uniwill_data *data, u8 *profile)
{
	u8 mode;
	int ret;
	u8 cached_profile = READ_ONCE(data->active_performance_profile);

	if (cached_profile >= 1 && cached_profile <= 3) {
		*profile = cached_profile;
		return 0;
	}

	ret = uniwill_get_ec_fan_mode(data, &mode);
	if (ret < 0)
		return ret;

	switch (mode) {
	case EC_FAN_MODE_PERFORMANCE:
		*profile = 1;
		break;
	case EC_FAN_MODE_STANDARD:
		*profile = 2;
		break;
	case EC_FAN_MODE_WHISPER:
	case EC_FAN_MODE_QUIET:
		*profile = 3;
		break;
	default:
		return -EIO;
	}

	WRITE_ONCE(data->active_performance_profile, *profile);
	return 0;
}

static int uniwill_set_performance_profile(struct uniwill_data *data, unsigned int profile)
{
	int ret;

	if (profile < 1 || profile > 3)
		return -EINVAL;

	guard(mutex)(&data->fan_lock);

	/* Benchmark has priority over profile-key indicator changes. */
	if (READ_ONCE(data->active_fan_mode) != UNIWILL_FAN_MODE_BENCHMARK) {
		ret = uniwill_set_performance_mode_led(profile);
		if (ret < 0)
			return ret;
	}

	WRITE_ONCE(data->active_performance_profile, profile);
	sysfs_notify(&data->dev->kobj, NULL, "performance_profile");
	return 0;
}

static int uniwill_cycle_performance_profile(struct uniwill_data *data)
{
	u8 profile;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_PERF_PROFILE))
		return -EOPNOTSUPP;

	ret = uniwill_get_performance_profile(data, &profile);
	if (ret < 0)
		return ret;

	if (profile >= 3)
		profile = 1;
	else
		profile++;

	return uniwill_set_performance_profile(data, profile);
}

static int uniwill_set_hardware_power_mode(struct uniwill_data *data, unsigned int mode)
{
	static const u16 pl1_default_regs[] = {
		EC_ADDR_PERFORMANCE_PL1_DEFAULT,
		EC_ADDR_BALANCED_PL1_DEFAULT,
		EC_ADDR_SAVER_PL1_DEFAULT,
	};
	static const u16 pl2_default_regs[] = {
		EC_ADDR_PERFORMANCE_PL2_DEFAULT,
		EC_ADDR_BALANCED_PL2_DEFAULT,
		EC_ADDR_SAVER_PL2_DEFAULT,
	};
	static const u16 dstate_default_regs[] = {
		EC_ADDR_PERFORMANCE_DSTATE_DEFAULT,
		EC_ADDR_BALANCED_DSTATE_DEFAULT,
		EC_ADDR_SAVER_DSTATE_DEFAULT,
	};
	unsigned int pl1;
	unsigned int pl2;
	unsigned int dstate;
	unsigned int ctrl_bits;
	int ret;

	if (mode < 1 || mode > 3)
		return -EINVAL;

	mutex_lock(&data->power_lock);
	ret = regmap_read(data->regmap, pl1_default_regs[mode - 1], &pl1);
	if (ret < 0)
		goto out;
	ret = regmap_read(data->regmap, pl2_default_regs[mode - 1], &pl2);
	if (ret < 0)
		goto out;
	if (uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL)) {
		ret = regmap_read(data->regmap, dstate_default_regs[mode - 1], &dstate);
		if (ret < 0)
			goto out;
	}

	ret = regmap_write(data->regmap, EC_ADDR_PL1_SETTING, pl1);
	if (ret < 0)
		goto out;
	ret = regmap_write(data->regmap, EC_ADDR_PL2_SETTING, pl2);
	if (ret < 0)
		goto out;
	if (uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL)) {
		ret = regmap_update_bits(data->regmap, EC_ADDR_GPU_DSTATE,
					 GPU_DSTATE_MASK, dstate & GPU_DSTATE_MASK);
		if (ret < 0)
			goto out;

	switch (mode) {
	case 1:
		ctrl_bits = CTGP_DB_GENERAL_ENABLE | CTGP_DB_DB_ENABLE |
			CTGP_DB_CTGP_ENABLE;
		break;
	case 2:
		ctrl_bits = CTGP_DB_GENERAL_ENABLE | CTGP_DB_DB_ENABLE;
		break;
	default:
		ctrl_bits = 0;
		break;
	}
		ret = regmap_update_bits(data->regmap, EC_ADDR_CTGP_DB_CTRL,
					 CTGP_DB_GENERAL_ENABLE | CTGP_DB_DB_ENABLE |
					 CTGP_DB_CTGP_ENABLE, ctrl_bits);
		if (ret < 0)
			goto out;
	}

	WRITE_ONCE(data->active_hardware_power_mode, mode);
out:
	mutex_unlock(&data->power_lock);
	return ret;
}

static ssize_t hardware_power_mode_store(struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int mode;
	int ret;

	ret = kstrtouint(buf, 0, &mode);
	if (ret < 0)
		return ret;
	ret = uniwill_set_hardware_power_mode(data, mode);
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t hardware_power_mode_show(struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	u8 mode = READ_ONCE(data->active_hardware_power_mode);

	return mode >= 1 && mode <= 3 ? sysfs_emit(buf, "%u\n", mode) : -ENODATA;
}

static DEVICE_ATTR_RW(hardware_power_mode);

static ssize_t db_offset_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value > U8_MAX)
		return -EINVAL;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_DB_OFFSET, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t db_offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_CTGP_DB_DB_OFFSET, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}

static DEVICE_ATTR_RW(db_offset);

static ssize_t ctgp_offset_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value > U8_MAX)
		return -EINVAL;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_CTGP_OFFSET, value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t ctgp_offset_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_CTGP_DB_CTGP_OFFSET, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}

static DEVICE_ATTR_RW(ctgp_offset);

static ssize_t fan_boost_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	bool enabled;
	int ret;

	ret = kstrtobool(buf, &enabled);
	if (ret < 0)
		return ret;

	ret = uniwill_set_fan_boost(data, enabled);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t fan_boost_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 value;
	int ret;

	ret = uniwill_wmi_ec_read_retry(EC_ADDR_MANUAL_FAN_CTRL, &value);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", !!(value & FAN_MODE_BOOST));
}

static DEVICE_ATTR_RW(fan_boost);

static int uniwill_nvidia_ctgp_init(struct uniwill_data *data)
{
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
		return 0;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_CTGP_OFFSET, 0);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_TPP_OFFSET, 255);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, EC_ADDR_CTGP_DB_DB_OFFSET, 25);
	if (ret < 0)
		return ret;

	/* Balanced is the safe boot default; userspace applies the persisted slot shortly after. */
	ret = regmap_update_bits(data->regmap, EC_ADDR_CTGP_DB_CTRL,
				 CTGP_DB_GENERAL_ENABLE | CTGP_DB_DB_ENABLE |
				 CTGP_DB_CTGP_ENABLE,
				 CTGP_DB_GENERAL_ENABLE | CTGP_DB_DB_ENABLE);
	if (ret < 0)
		return ret;

	return 0;
}

static struct attribute *uniwill_attrs[] = {
	/* Keyboard-related */
	&dev_attr_fn_lock_toggle_enable.attr,
	&dev_attr_super_key_toggle_enable.attr,
	&dev_attr_touchpad_toggle_enable.attr,
	/* Lightbar-related */
	&dev_attr_rainbow_animation.attr,
	&dev_attr_breathing_in_suspend.attr,
	/* Power-management-related */
	&dev_attr_ctgp_offset.attr,
	&dev_attr_db_offset.attr,
	/* TDP control */
	&dev_attr_pl1_setting.attr,
	&dev_attr_pl2_setting.attr,
	&dev_attr_pl4_setting.attr,
	/* Performance profile */
	&dev_attr_performance_profile.attr,
	&dev_attr_hardware_power_mode.attr,
	&dev_attr_fan_mode.attr,
	&dev_attr_fan_boost.attr,
	&dev_attr_passive_cooling.attr,
	&dev_attr_ec_debug_dump.attr,
	NULL
};

static umode_t uniwill_attr_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct uniwill_data *data = dev_get_drvdata(dev);

	if (attr == &dev_attr_fn_lock_toggle_enable.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_FN_LOCK_TOGGLE))
			return attr->mode;
	}

	if (attr == &dev_attr_super_key_toggle_enable.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_SUPER_KEY_TOGGLE))
			return attr->mode;
	}

	if (attr == &dev_attr_touchpad_toggle_enable.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_TOUCHPAD_TOGGLE))
			return attr->mode;
	}

	if (attr == &dev_attr_rainbow_animation.attr ||
	    attr == &dev_attr_breathing_in_suspend.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_LIGHTBAR))
			return attr->mode;
	}

	if (attr == &dev_attr_ctgp_offset.attr ||
	    attr == &dev_attr_db_offset.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
			return attr->mode;
	}

	if (attr == &dev_attr_pl1_setting.attr ||
	    attr == &dev_attr_pl2_setting.attr ||
	    attr == &dev_attr_pl4_setting.attr ||
	    attr == &dev_attr_hardware_power_mode.attr) {
		if (uniwill_device_supports(data, UNIWILL_FEATURE_TDP_CTRL))
			return attr->mode;
	}

	if (attr == &dev_attr_performance_profile.attr ||
	    attr == &dev_attr_fan_mode.attr ||
	    attr == &dev_attr_fan_boost.attr ||
	    attr == &dev_attr_passive_cooling.attr)
		return attr->mode;

	if (attr == &dev_attr_ec_debug_dump.attr)
		return attr->mode;

	return 0;
}

static const struct attribute_group uniwill_group = {
	.is_visible = uniwill_attr_is_visible,
	.attrs = uniwill_attrs,
};

static const struct attribute_group *uniwill_groups[] = {
	&uniwill_group,
	NULL
};

static int uniwill_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			long *val)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	unsigned int value;
	__be16 rpm;
	int ret;

	switch (type) {
	case hwmon_temp:
		switch (channel) {
		case 0:
			ret = regmap_read(data->regmap, EC_ADDR_CPU_TEMP, &value);
			break;
		case 1:
			ret = regmap_read(data->regmap, EC_ADDR_GPU_TEMP, &value);
			break;
		default:
			return -EOPNOTSUPP;
		}

		if (ret < 0)
			return ret;

		*val = value * MILLIDEGREE_PER_DEGREE;
		return 0;
	case hwmon_fan:
		switch (channel) {
		case 0:
			ret = regmap_bulk_read(data->regmap, EC_ADDR_MAIN_FAN_RPM_1, &rpm,
					       sizeof(rpm));
			break;
		case 1:
			ret = regmap_bulk_read(data->regmap, EC_ADDR_SECOND_FAN_RPM_1, &rpm,
					       sizeof(rpm));
			break;
		default:
			return -EOPNOTSUPP;
		}

		if (ret < 0)
			return ret;

		*val = be16_to_cpu(rpm);
		return 0;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			*val = READ_ONCE(data->fans_initialized) ? 1 : 2;
			return 0;
		case hwmon_pwm_input:
			switch (channel) {
			case 0:
				ret = regmap_read(data->regmap, EC_ADDR_PWM_1, &value);
				break;
			case 1:
				ret = regmap_read(data->regmap, EC_ADDR_PWM_2, &value);
				break;
			default:
				return -EOPNOTSUPP;
			}

			if (ret < 0)
				return ret;

			*val = fixp_linear_interpolate(0, 0, PWM_MAX, U8_MAX, value);
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int uniwill_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			       int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = uniwill_temp_labels[channel];
		return 0;
	case hwmon_fan:
		*str = uniwill_fan_labels[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int uniwill_wmi_ec_read(u16 addr, u8 *val)
{
	u32 wmi_arg[10] = {};
	u8 *bytes = (u8 *)wmi_arg;
	u32 arg = addr;
	struct acpi_buffer wmi_in = { sizeof(wmi_arg), wmi_arg };
	struct acpi_buffer wmi_out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *out_obj;
	u32 result;
	acpi_status status;
	int ret = 0;

	memcpy(bytes, &arg, sizeof(arg));
	bytes[5] = UNIWILL_WMI_FUNC_READ;

	mutex_lock(&uniwill_ec_lock);

	status = wmi_evaluate_method(UNIWILL_WMI_EC_GUID, UNIWILL_WMI_EC_INSTANCE,
				     UNIWILL_WMI_EC_METHOD, &wmi_in, &wmi_out);
	if (ACPI_FAILURE(status)) {
		ret = -EIO;
		goto out;
	}

	out_obj = wmi_out.pointer;
	if (!out_obj || out_obj->type != ACPI_TYPE_BUFFER || out_obj->buffer.length < 4) {
		ret = -EIO;
		goto out;
	}

	memcpy(&result, out_obj->buffer.pointer, sizeof(result));
	if (result == 0xfefefefe) {
		ret = -EIO;
		goto out;
	}

	*val = out_obj->buffer.pointer[0];

out:
	usleep_range(UNIWILL_EC_DELAY_US, UNIWILL_EC_DELAY_US * 2);
	mutex_unlock(&uniwill_ec_lock);
	kfree(wmi_out.pointer);
	return ret;
}

static int uniwill_wmi_ec_write(u16 addr, u8 val)
{
	u32 wmi_arg[10] = {};
	u8 *bytes = (u8 *)wmi_arg;
	u32 arg = ((u32)val << 16) | ((u32)(addr >> 8) << 8) | (addr & 0xff);
	struct acpi_buffer wmi_in = { sizeof(wmi_arg), wmi_arg };
	struct acpi_buffer wmi_out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *out_obj;
	u32 result;
	acpi_status status;
	int ret = 0;

	memcpy(bytes, &arg, sizeof(arg));
	bytes[5] = UNIWILL_WMI_FUNC_WRITE;

	mutex_lock(&uniwill_ec_lock);

	status = wmi_evaluate_method(UNIWILL_WMI_EC_GUID, UNIWILL_WMI_EC_INSTANCE,
				     UNIWILL_WMI_EC_METHOD, &wmi_in, &wmi_out);
	if (ACPI_FAILURE(status)) {
		ret = -EIO;
		goto out;
	}

	out_obj = wmi_out.pointer;
	if (!out_obj || out_obj->type != ACPI_TYPE_BUFFER || out_obj->buffer.length < 4) {
		ret = -EIO;
		goto out;
	}

	memcpy(&result, out_obj->buffer.pointer, sizeof(result));
	if (result == 0xfefefefe)
		ret = -EIO;

out:
	usleep_range(UNIWILL_EC_DELAY_US, UNIWILL_EC_DELAY_US * 2);
	mutex_unlock(&uniwill_ec_lock);
	kfree(wmi_out.pointer);
	return ret;
}

static int uniwill_wmi_ec_read_retry(u16 addr, u8 *val)
{
	int ret, i;

	for (i = 0; i < UNIWILL_WMI_EC_RETRIES; i++) {
		ret = uniwill_wmi_ec_read(addr, val);
		if (ret == 0)
			return 0;
	}

	return ret;
}

static int uniwill_wmi_ec_write_retry(u16 addr, u8 val)
{
	int ret, i;

	for (i = 0; i < UNIWILL_WMI_EC_RETRIES; i++) {
		ret = uniwill_wmi_ec_write(addr, val);
		if (ret == 0)
			return 0;
	}

	return ret;
}

static int uwill_fan_ec_write(struct uniwill_data *data, u16 addr, u8 val)
{
	int ret;

	ret = uniwill_wmi_ec_write_retry(addr, val);
	if (ret < 0)
		dev_err(data->dev, "Failed to write fan EC register 0x%04x\n", addr);

	return ret;
}

static int uwill_fan_set_auto(struct uniwill_data *data);
static int uwill_fan_init(struct uniwill_data *data);
static int uwill_fan_set_speed(struct uniwill_data *data, int channel, u8 speed);

static int uniwill_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			 int channel, long val)
{
	struct uniwill_data *data = dev_get_drvdata(dev);

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_FAN_CTRL))
		return -EOPNOTSUPP;

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			if (val == 1)
				return uwill_fan_init(data);
			if (val == 2)
				return uwill_fan_set_auto(data);
			return -EINVAL;
		case hwmon_pwm_input:
			if (val < 0 || val > U8_MAX)
				return -EINVAL;
			return uwill_fan_set_speed(data, channel,
				fixp_linear_interpolate(0, 0, U8_MAX, PWM_MAX, val));
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t uniwill_hwmon_is_visible(const void *drvdata,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	switch (type) {
	case hwmon_chip:
	case hwmon_temp:
	case hwmon_fan:
		return 0444;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
		case hwmon_pwm_input:
			return 0644;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static const struct hwmon_ops uniwill_ops = {
	.is_visible = uniwill_hwmon_is_visible,
	.read = uniwill_read,
	.read_string = uniwill_read_string,
	.write = uniwill_write,
};

static const struct hwmon_channel_info * const uniwill_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info uniwill_chip_info = {
	.ops = &uniwill_ops,
	.info = uniwill_info,
};

static int __uwill_fan_set_auto(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_AP_OEM_6, &value);
	if (ret < 0)
		return ret;
	if (value & ENABLE_UNIVERSAL_FAN_CTRL) {
		ret = regmap_write(data->regmap, EC_ADDR_AP_OEM_6,
				   value & ~ENABLE_UNIVERSAL_FAN_CTRL);
		if (ret < 0)
			return ret;
	}

	ret = regmap_read(data->regmap, EC_ADDR_UNIVERSAL_FAN_CTRL, &value);
	if (ret < 0)
		return ret;
	if (value & SPLIT_TABLES) {
		ret = regmap_write(data->regmap, EC_ADDR_UNIVERSAL_FAN_CTRL,
				   value & ~SPLIT_TABLES);
		if (ret < 0)
			return ret;
	}

	ret = regmap_clear_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
	if (ret < 0)
		return ret;

	data->fans_initialized = false;
	return 0;
}

static int uwill_fan_set_auto(struct uniwill_data *data)
{
	cancel_delayed_work_sync(&data->fan_watchdog_work);
	guard(mutex)(&data->fan_lock);

	return __uwill_fan_set_auto(data);
}

static void uniwill_fan_watchdog_work(struct work_struct *work)
{
	struct uniwill_data *data =
		container_of(to_delayed_work(work), struct uniwill_data, fan_watchdog_work);
	int ret;

	mutex_lock(&data->fan_lock);
	ret = __uwill_fan_set_auto(data);
	mutex_unlock(&data->fan_lock);
	if (ret < 0) {
		dev_warn(data->dev, "Failed to restore automatic fan control: %d\n", ret);
		mod_delayed_work(system_wq, &data->fan_watchdog_work,
				 msecs_to_jiffies(UNIWILL_FAN_WATCHDOG_MS));
	} else {
		dev_warn(data->dev, "Fan control watchdog restored automatic mode\n");
	}
}

static int __uwill_fan_init(struct uniwill_data *data)
{
	unsigned int value;
	int ret, i;

	if (data->fans_initialized)
		return 0;

	ret = regmap_set_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
	if (ret < 0)
		return ret;

	ret = regmap_clear_bits(data->regmap, EC_ADDR_MANUAL_FAN_CTRL, FAN_MODE_BOOST);
	if (ret < 0)
		return ret;

	ret = regmap_read(data->regmap, EC_ADDR_UNIVERSAL_FAN_CTRL, &value);
	if (ret < 0)
		return ret;
	if (!(value & SPLIT_TABLES)) {
		ret = regmap_write(data->regmap, EC_ADDR_UNIVERSAL_FAN_CTRL,
				   value | SPLIT_TABLES);
		if (ret < 0)
			return ret;
	}

	/*
	 * One controllable zone 0–115 °C for CPU, 0–120 °C for GPU;
	 * remaining dummy zones ramp to max to keep EC safe.
	 * Fan table registers require WMI access — ECRW cannot reach them.
	 */
	ret = uwill_fan_ec_write(data, EC_ADDR_CPU_TEMP_END_TABLE, 115);
	if (ret < 0)
		return ret;
	ret = uwill_fan_ec_write(data, EC_ADDR_CPU_TEMP_START_TABLE, 0);
	if (ret < 0)
		return ret;
	ret = uwill_fan_ec_write(data, EC_ADDR_CPU_FAN_SPEED_TABLE, 0x01);
	if (ret < 0)
		return ret;
	ret = uwill_fan_ec_write(data, EC_ADDR_GPU_TEMP_END_TABLE, 120);
	if (ret < 0)
		return ret;
	ret = uwill_fan_ec_write(data, EC_ADDR_GPU_TEMP_START_TABLE, 0);
	if (ret < 0)
		return ret;
	ret = uwill_fan_ec_write(data, EC_ADDR_GPU_FAN_SPEED_TABLE, 0x01);
	if (ret < 0)
		return ret;
	for (i = 1; i < FAN_TABLE_LENGTH; i++) {
		ret = uwill_fan_ec_write(data, EC_ADDR_CPU_TEMP_END_TABLE + i,
					 115 + i + 1);
		if (ret < 0)
			return ret;
		ret = uwill_fan_ec_write(data, EC_ADDR_CPU_TEMP_START_TABLE + i,
					 115 + i);
		if (ret < 0)
			return ret;
		ret = uwill_fan_ec_write(data, EC_ADDR_CPU_FAN_SPEED_TABLE + i,
					 PWM_MAX);
		if (ret < 0)
			return ret;
		ret = uwill_fan_ec_write(data, EC_ADDR_GPU_TEMP_END_TABLE + i,
					 120 + i + 1);
		if (ret < 0)
			return ret;
		ret = uwill_fan_ec_write(data, EC_ADDR_GPU_TEMP_START_TABLE + i,
					 120 + i);
		if (ret < 0)
			return ret;
		ret = uwill_fan_ec_write(data, EC_ADDR_GPU_FAN_SPEED_TABLE + i,
					 PWM_MAX);
		if (ret < 0)
			return ret;
	}

	ret = regmap_read(data->regmap, EC_ADDR_AP_OEM_6, &value);
	if (ret < 0)
		return ret;
	if (!(value & ENABLE_UNIVERSAL_FAN_CTRL)) {
		ret = regmap_write(data->regmap, EC_ADDR_AP_OEM_6,
				   value | ENABLE_UNIVERSAL_FAN_CTRL);
		if (ret < 0)
			return ret;
	}

	data->fans_initialized = true;
	return 0;
}

static int uwill_fan_init(struct uniwill_data *data)
{
	int auto_ret = 0;
	int ret;

	mutex_lock(&data->fan_lock);
	ret = __uwill_fan_init(data);
	if (ret < 0)
		auto_ret = __uwill_fan_set_auto(data);
	mutex_unlock(&data->fan_lock);

	if (ret == 0 || auto_ret < 0)
		mod_delayed_work(system_wq, &data->fan_watchdog_work,
				 msecs_to_jiffies(UNIWILL_FAN_WATCHDOG_MS));

	return ret;
}

static int uwill_fan_set_speed(struct uniwill_data *data, int channel, u8 speed)
{
	unsigned int fan_table_addr;
	unsigned int fan_pwm_addr;
	u8 ec_speed;
	int auto_ret;
	int ret;

	if (speed > PWM_MAX)
		return -EINVAL;

	if (speed > 0 && speed < PWM_MAX * FAN_ON_MIN_SPEED_PERCENT / 100)
		speed = PWM_MAX * FAN_ON_MIN_SPEED_PERCENT / 100;

	mutex_lock(&data->fan_lock);
	ret = __uwill_fan_init(data);
	if (ret < 0)
		goto out_restore_auto;

	if (channel == 0) {
		fan_table_addr = EC_ADDR_CPU_FAN_SPEED_TABLE;
		fan_pwm_addr = EC_ADDR_PWM_1_WRITEABLE;
	} else if (channel == 1) {
		fan_table_addr = EC_ADDR_GPU_FAN_SPEED_TABLE;
		fan_pwm_addr = EC_ADDR_PWM_2_WRITEABLE;
	} else {
		ret = -EINVAL;
		goto out_restore_auto;
	}

	ec_speed = speed == 0 ? 1 : speed;

	ret = uwill_fan_ec_write(data, fan_table_addr, ec_speed);
	if (ret < 0)
		goto out_restore_auto;

	ret = uwill_fan_ec_write(data, fan_pwm_addr, ec_speed);
	if (ret < 0)
		goto out_restore_auto;

	mutex_unlock(&data->fan_lock);
	mod_delayed_work(system_wq, &data->fan_watchdog_work,
			 msecs_to_jiffies(UNIWILL_FAN_WATCHDOG_MS));
	return 0;

out_restore_auto:
	auto_ret = __uwill_fan_set_auto(data);
	mutex_unlock(&data->fan_lock);
	if (auto_ret < 0)
		mod_delayed_work(system_wq, &data->fan_watchdog_work,
				 msecs_to_jiffies(UNIWILL_FAN_WATCHDOG_MS));
	return ret;
}

static int uniwill_hwmon_init(struct uniwill_data *data)
{
	struct device *hdev;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_HWMON))
		return 0;

	hdev = devm_hwmon_device_register_with_info(data->dev, "uniwill", data,
						    &uniwill_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hdev);
}

static int uniwill_led_brightness_set(struct led_classdev *led_cdev, enum led_brightness brightness)
{
	struct led_classdev_mc *led_mc_cdev = lcdev_to_mccdev(led_cdev);
	struct uniwill_data *data = container_of(led_mc_cdev, struct uniwill_data, led_mc_cdev);
	unsigned int value;
	int ret;

	ret = led_mc_calc_color_components(led_mc_cdev, brightness);
	if (ret < 0)
		return ret;

	guard(mutex)(&data->led_lock);

	for (int i = 0; i < LED_CHANNELS; i++) {
		/* Prevent the brightness values from overflowing */
		value = min_t(unsigned int, LED_MAX_BRIGHTNESS,
			      data->led_mc_subled_info[i].brightness);
		ret = uniwill_lightbar_write_color_value(data, i, value);
		if (ret < 0)
			return ret;
	}

	if (brightness)
		ret = uniwill_lightbar_apply_solid_mode(data);
	else
		ret = uniwill_lightbar_update_ctrl(data, LIGHTBAR_MODE_OFF);
	if (ret < 0)
		return ret;

	return 0;
}

static int uniwill_led_init(struct uniwill_data *data)
{
	struct led_init_data init_data = {
		.devicename = DRIVER_NAME,
		.default_label = "multicolor:" LED_FUNCTION_STATUS,
		.devname_mandatory = true,
	};
	unsigned int color_indices[3] = {
		LED_COLOR_ID_RED,
		LED_COLOR_ID_GREEN,
		LED_COLOR_ID_BLUE,
	};
	unsigned int value;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_LIGHTBAR))
		return 0;

	ret = devm_mutex_init(data->dev, &data->led_lock);
	if (ret < 0)
		return ret;

	/*
	 * The EC has separate lightbar settings for AC and battery mode,
	 * so we have to ensure that both settings are the same.
	 */
	ret = regmap_read(data->regmap, EC_ADDR_LIGHTBAR_AC_CTRL, &value);
	if (ret < 0)
		return ret;

	value = (value & LIGHTBAR_MODE_MASK) | LIGHTBAR_APP_EXISTS;
	ret = uniwill_lightbar_update_ctrl(data, value);
	if (ret < 0)
		return ret;

	data->led_mc_cdev.led_cdev.color = LED_COLOR_ID_MULTI;
	data->led_mc_cdev.led_cdev.max_brightness = LED_MAX_BRIGHTNESS;
	data->led_mc_cdev.led_cdev.flags = LED_REJECT_NAME_CONFLICT;
	data->led_mc_cdev.led_cdev.brightness_set_blocking = uniwill_led_brightness_set;

	if (value & LIGHTBAR_S0_OFF)
		data->led_mc_cdev.led_cdev.brightness = 0;
	else
		data->led_mc_cdev.led_cdev.brightness = LED_MAX_BRIGHTNESS;

	for (int i = 0; i < LED_CHANNELS; i++) {
		data->led_mc_subled_info[i].color_index = color_indices[i];

		ret = regmap_read(data->regmap, uniwill_led_channel_to_ac_reg[i], &value);
		if (ret < 0)
			return ret;

		/*
		 * Make sure that the initial intensity value is not greater than
		 * the maximum brightness.
		 */
		value = min_t(unsigned int, LED_MAX_BRIGHTNESS, value);
		ret = uniwill_lightbar_write_color_value(data, i, value);
		if (ret < 0)
			return ret;

		data->led_mc_subled_info[i].intensity = value;
		data->led_mc_subled_info[i].channel = i;
	}

	data->led_mc_cdev.subled_info = data->led_mc_subled_info;
	data->led_mc_cdev.num_colors = LED_CHANNELS;

	return devm_led_classdev_multicolor_register_ext(data->dev, &data->led_mc_cdev,
							 &init_data);
}

static int uniwill_get_property(struct power_supply *psy, const struct power_supply_ext *ext,
				void *drvdata, enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct uniwill_data *data = drvdata;
	union power_supply_propval prop;
	unsigned int regval;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_HEALTH:
		ret = power_supply_get_property_direct(psy, POWER_SUPPLY_PROP_PRESENT, &prop);
		if (ret < 0)
			return ret;

		if (!prop.intval) {
			val->intval = POWER_SUPPLY_HEALTH_NO_BATTERY;
			return 0;
		}

		ret = power_supply_get_property_direct(psy, POWER_SUPPLY_PROP_STATUS, &prop);
		if (ret < 0)
			return ret;

		if (prop.intval == POWER_SUPPLY_STATUS_UNKNOWN) {
			val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
			return 0;
		}

		ret = regmap_read(data->regmap, EC_ADDR_BAT_ALERT, &regval);
		if (ret < 0)
			return ret;

		if (regval) {
			/* Charging issue */
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			return 0;
		}

		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		ret = regmap_read(data->regmap, EC_ADDR_CHARGE_CTRL, &regval);
		if (ret < 0)
			return ret;

		val->intval = clamp_val(FIELD_GET(CHARGE_CTRL_MASK, regval), 0, 100);
		return 0;
	default:
		return -EINVAL;
	}
}

static int uniwill_set_property(struct power_supply *psy, const struct power_supply_ext *ext,
				void *drvdata, enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct uniwill_data *data = drvdata;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		if (val->intval < 1 || val->intval > 100)
			return -EINVAL;

		return regmap_update_bits(data->regmap, EC_ADDR_CHARGE_CTRL, CHARGE_CTRL_MASK,
					  val->intval);
	default:
		return -EINVAL;
	}
}

static int uniwill_property_is_writeable(struct power_supply *psy,
					 const struct power_supply_ext *ext, void *drvdata,
					 enum power_supply_property psp)
{
	if (psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD)
		return true;

	return false;
}

static const enum power_supply_property uniwill_properties[] = {
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_ext uniwill_extension = {
	.name = DRIVER_NAME,
	.properties = uniwill_properties,
	.num_properties = ARRAY_SIZE(uniwill_properties),
	.get_property = uniwill_get_property,
	.set_property = uniwill_set_property,
	.property_is_writeable = uniwill_property_is_writeable,
};

static int uniwill_add_battery(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	struct uniwill_data *data = container_of(hook, struct uniwill_data, hook);
	struct uniwill_battery_entry *entry;
	int ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	ret = power_supply_register_extension(battery, &uniwill_extension, data->dev, data);
	if (ret < 0) {
		kfree(entry);
		return ret;
	}

	guard(mutex)(&data->battery_lock);

	entry->battery = battery;
	list_add(&entry->head, &data->batteries);

	return 0;
}

static int uniwill_remove_battery(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	struct uniwill_data *data = container_of(hook, struct uniwill_data, hook);
	struct uniwill_battery_entry *entry, *tmp;

	scoped_guard(mutex, &data->battery_lock) {
		list_for_each_entry_safe(entry, tmp, &data->batteries, head) {
			if (entry->battery == battery) {
				list_del(&entry->head);
				kfree(entry);
				break;
			}
		}
	}

	power_supply_unregister_extension(battery, &uniwill_extension);

	return 0;
}

static int uniwill_battery_init(struct uniwill_data *data)
{
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_BATTERY))
		return 0;

	ret = devm_mutex_init(data->dev, &data->battery_lock);
	if (ret < 0)
		return ret;

	INIT_LIST_HEAD(&data->batteries);
	data->hook.name = "Uniwill Battery Extension";
	data->hook.add_battery = uniwill_add_battery;
	data->hook.remove_battery = uniwill_remove_battery;

	return devm_battery_hook_register(data->dev, &data->hook);
}

static const u8 uniwill_touchpad_toggle_seq[] = {
	0xe0, 0x5b, /* Super down */
	0x1d,       /* Control down */
	0x76,       /* Zenkaku/Hankaku down */
	0xf6,       /* Zenkaku/Hankaku up */
	0x9d,       /* Control up */
	0xe0, 0xdb, /* Super up */
};

static void uniwill_touchpad_work(struct work_struct *work)
{
	struct uniwill_data *data = container_of(work, struct uniwill_data, touchpad_work);

	msleep(50);

	mutex_lock(&data->input_lock);
	sparse_keymap_report_event(data->input_device,
				   UNIWILL_OSD_TOUCHPADWORKAROUND,
				   1, true);
	mutex_unlock(&data->input_lock);
}

static void uniwill_performance_mode_work(struct work_struct *work)
{
	struct uniwill_data *data =
		container_of(work, struct uniwill_data, performance_mode_work);
	int ret;

	ret = regmap_test_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
	if (ret < 0)
		return;

	if (ret) {
		ret = uniwill_cycle_performance_profile(data);
		if (ret < 0 && ret != -EOPNOTSUPP)
			dev_dbg(data->dev, "Failed to cycle performance profile: %d\n", ret);
	}
}

static void uniwill_cancel_performance_mode_work(void *context)
{
	struct uniwill_data *data = context;

	cancel_work_sync(&data->performance_mode_work);
}

static bool uniwill_i8042_filter(unsigned char data, unsigned char str,
				 struct serio *port __always_unused, void *context)
{
	struct uniwill_data *uw_data = context;
	static u8 seq_pos;

	if (unlikely(str & I8042_STR_AUXDATA))
		return false;

	if (unlikely(data == uniwill_touchpad_toggle_seq[seq_pos])) {
		seq_pos++;
		if (unlikely(data == 0x76 || data == 0xf6))
			return true;

		if (unlikely(seq_pos == ARRAY_SIZE(uniwill_touchpad_toggle_seq))) {
			if (uw_data)
				schedule_work(&uw_data->touchpad_work);

			seq_pos = 0;
		}

		return false;
	}

	seq_pos = 0;
	return false;
}

static void uniwill_remove_i8042_filter(void *context)
{
	struct uniwill_data *data = context;

	i8042_remove_filter(uniwill_i8042_filter);
	cancel_work_sync(&data->touchpad_work);
}

static int uniwill_notifier_call(struct notifier_block *nb, unsigned long action, void *dummy)
{
	struct uniwill_data *data = container_of(nb, struct uniwill_data, nb);
	struct uniwill_battery_entry *entry;

	switch (action) {
	case UNIWILL_OSD_BATTERY_ALERT:
		mutex_lock(&data->battery_lock);
		list_for_each_entry(entry, &data->batteries, head) {
			power_supply_changed(entry->battery);
		}
		mutex_unlock(&data->battery_lock);

		return NOTIFY_OK;
	case UNIWILL_OSD_DC_ADAPTER_CHANGED:
		/* noop for the time being, will change once charging priority
		 * gets implemented.
		 */

		return NOTIFY_OK;
	case UNIWILL_OSD_PERFORMANCE_MODE_TOGGLE:
		schedule_work(&data->performance_mode_work);
		return NOTIFY_OK;
	case UNIWILL_OSD_KBDILLUMDOWN:
	case UNIWILL_OSD_KBDILLUMUP:
	case UNIWILL_OSD_KBDILLUMTOGGLE:
	case UNIWILL_OSD_KB_LED_LEVEL0:
	case UNIWILL_OSD_KB_LED_LEVEL1:
	case UNIWILL_OSD_KB_LED_LEVEL2:
	case UNIWILL_OSD_KB_LED_LEVEL3:
	case UNIWILL_OSD_KB_LED_LEVEL4:
	case UNIWILL_OSD_KBD_BACKLIGHT_CHANGED:
		if (uniwill_ite8291_handle_brightness_event(action) == 0)
			return NOTIFY_OK;
		fallthrough;
	default:
		mutex_lock(&data->input_lock);
		sparse_keymap_report_event(data->input_device, action, 1, true);
		mutex_unlock(&data->input_lock);

		return NOTIFY_OK;
	}
}

static int uniwill_input_init(struct uniwill_data *data)
{
	int ret;

	ret = devm_mutex_init(data->dev, &data->input_lock);
	if (ret < 0)
		return ret;

	data->input_device = devm_input_allocate_device(data->dev);
	if (!data->input_device)
		return -ENOMEM;

	ret = sparse_keymap_setup(data->input_device, uniwill_keymap, NULL);
	if (ret < 0)
		return ret;

	data->input_device->name = "Uniwill WMI hotkeys";
	data->input_device->phys = "wmi/input0";
	data->input_device->id.bustype = BUS_HOST;
	ret = input_register_device(data->input_device);
	if (ret < 0)
		return ret;

	INIT_WORK(&data->touchpad_work, uniwill_touchpad_work);
	INIT_WORK(&data->performance_mode_work, uniwill_performance_mode_work);
	ret = devm_add_action_or_reset(data->dev, uniwill_cancel_performance_mode_work, data);
	if (ret < 0)
		return ret;

	ret = i8042_install_filter(uniwill_i8042_filter, data);
	if (ret < 0) {
		dev_dbg(data->dev, "Could not install i8042 filter: %d\n", ret);
	} else {
		ret = devm_add_action_or_reset(data->dev, uniwill_remove_i8042_filter, data);
		if (ret < 0)
			return ret;
	}

	data->nb.notifier_call = uniwill_notifier_call;

	return devm_uniwill_wmi_register_notifier(data->dev, &data->nb);
}

static void uniwill_disable_manual_control(void *context)
{
	struct uniwill_data *data = context;

	uwill_fan_set_auto(data);
	regmap_clear_bits(data->regmap, EC_ADDR_AP_OEM, ENABLE_MANUAL_CTRL);
}

static int uniwill_ec_init(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, EC_ADDR_PROJECT_ID, &value);
	if (ret < 0)
		return ret;

	dev_dbg(data->dev, "Project ID: %u\n", value);

	return devm_add_action_or_reset(data->dev, uniwill_disable_manual_control, data);
}

static int uniwill_probe(struct platform_device *pdev)
{
	struct uniwill_data *data;
	struct regmap *regmap;
	acpi_handle handle;
	int ret;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return -ENODEV;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = &pdev->dev;
	data->handle = handle;
	platform_set_drvdata(pdev, data);

	regmap = devm_regmap_init(&pdev->dev, &uniwill_ec_bus, data, &uniwill_ec_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	data->regmap = regmap;
	ret = devm_mutex_init(&pdev->dev, &data->super_key_lock);
	if (ret < 0)
		return ret;
	ret = devm_mutex_init(&pdev->dev, &data->fan_lock);
	if (ret < 0)
		return ret;
	ret = devm_mutex_init(&pdev->dev, &data->power_lock);
	if (ret < 0)
		return ret;
	INIT_DELAYED_WORK(&data->fan_watchdog_work, uniwill_fan_watchdog_work);

	ret = uniwill_ec_init(data);
	if (ret < 0)
		return ret;

	data->features = device_descriptor.features;

	/*
	 * Detect optional features from EC registers.
	 */
	{
		unsigned int fan_ctrl_val;
		u8 pl1_wmi_val;

		if (regmap_read(data->regmap, EC_ADDR_FAN_CTRL, &fan_ctrl_val) == 0 &&
		    (fan_ctrl_val & UNIVERSAL_FAN_CTRL))
			data->features |= UNIWILL_FEATURE_FAN_CTRL;

		/*
		 * TUXEDO exposes TDP controls based on platform support, not on the
		 * current PL1 value. Some firmware leaves PL1 at 0 until userspace
		 * writes a custom value.
		 */
		if (uniwill_wmi_ec_read_retry(EC_ADDR_PL1_SETTING, &pl1_wmi_val) == 0)
			data->features |= UNIWILL_FEATURE_TDP_CTRL | UNIWILL_FEATURE_PERF_PROFILE;
	}

	/*
	 * Ensure EC is in automatic fan control mode on load, clearing any
	 * manual state left over from a previous driver instance.
	 */
	if (uniwill_device_supports(data, UNIWILL_FEATURE_FAN_CTRL)) {
		if (uwill_fan_set_auto(data) < 0)
			dev_warn(data->dev, "Could not restore automatic fan control during probe\n");
	}

	/*
	 * Some devices might need to perform some device-specific initialization steps
	 * before the supported features are initialized. Because of this we have to call
	 * this callback just after the EC itself was initialized.
	 */
	if (device_descriptor.probe) {
		ret = device_descriptor.probe(data);
		if (ret < 0)
			return ret;
	}

	ret = uniwill_battery_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_led_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_hwmon_init(data);
	if (ret < 0)
		return ret;

	ret = uniwill_nvidia_ctgp_init(data);
	if (ret < 0)
		return ret;

	return uniwill_input_init(data);
}

static void uniwill_shutdown(struct platform_device *pdev)
{
	struct uniwill_data *data = platform_get_drvdata(pdev);

	cancel_work_sync(&data->performance_mode_work);
	uwill_fan_set_auto(data);
}

static int uniwill_suspend_keyboard(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_SUPER_KEY_TOGGLE))
		return 0;

	/*
	 * The EC_ADDR_SWITCH_STATUS is marked as volatile, so we have to restore it
	 * ourselves.
	 */
	return regmap_read(data->regmap, EC_ADDR_SWITCH_STATUS, &data->last_switch_status);
}

static int uniwill_suspend_battery(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_BATTERY))
		return 0;

	/*
	 * Save the current charge limit in order to restore it during resume.
	 * We cannot use the regmap code for that since this register needs to
	 * be declared as volatile due to CHARGE_CTRL_REACHED.
	 */
	return regmap_read(data->regmap, EC_ADDR_CHARGE_CTRL, &data->last_charge_ctrl);
}

static int uniwill_suspend_nvidia_ctgp(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
		return 0;

	return regmap_clear_bits(data->regmap, EC_ADDR_CTGP_DB_CTRL,
				 CTGP_DB_DB_ENABLE | CTGP_DB_CTGP_ENABLE);
}

static int uniwill_suspend(struct device *dev)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	int ret;

	cancel_work_sync(&data->performance_mode_work);

	ret = uniwill_suspend_keyboard(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_battery(data);
	if (ret < 0)
		return ret;

	ret = uniwill_suspend_nvidia_ctgp(data);
	if (ret < 0)
		return ret;

	if (uniwill_device_supports(data, UNIWILL_FEATURE_FAN_CTRL)) {
		ret = uwill_fan_set_auto(data);
		if (ret < 0)
			return ret;
	}

	regcache_cache_only(data->regmap, true);
	regcache_mark_dirty(data->regmap);

	return 0;
}

static int uniwill_resume_keyboard(struct uniwill_data *data)
{
	unsigned int value;
	int ret;

	if (!uniwill_device_supports(data, UNIWILL_FEATURE_SUPER_KEY_TOGGLE))
		return 0;

	ret = regmap_read(data->regmap, EC_ADDR_SWITCH_STATUS, &value);
	if (ret < 0)
		return ret;

	if ((data->last_switch_status & SUPER_KEY_LOCK_STATUS) == (value & SUPER_KEY_LOCK_STATUS))
		return 0;

	return regmap_write(data->regmap, EC_ADDR_TRIGGER, TRIGGER_SUPER_KEY_LOCK);
}

static int uniwill_resume_battery(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_BATTERY))
		return 0;

	return regmap_update_bits(data->regmap, EC_ADDR_CHARGE_CTRL, CHARGE_CTRL_MASK,
				  data->last_charge_ctrl);
}

static int uniwill_resume_nvidia_ctgp(struct uniwill_data *data)
{
	if (!uniwill_device_supports(data, UNIWILL_FEATURE_NVIDIA_CTGP_CONTROL))
		return 0;

	return regmap_set_bits(data->regmap, EC_ADDR_CTGP_DB_CTRL,
			       CTGP_DB_DB_ENABLE | CTGP_DB_CTGP_ENABLE);
}

static int uniwill_resume(struct device *dev)
{
	struct uniwill_data *data = dev_get_drvdata(dev);
	int ret;

	regcache_cache_only(data->regmap, false);

	ret = regcache_sync(data->regmap);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_keyboard(data);
	if (ret < 0)
		return ret;

	ret = uniwill_resume_battery(data);
	if (ret < 0)
		return ret;

	return uniwill_resume_nvidia_ctgp(data);
}

static DEFINE_SIMPLE_DEV_PM_OPS(uniwill_pm_ops, uniwill_suspend, uniwill_resume);

/*
 * We only use the DMI table for auoloading because the ACPI device itself
 * does not guarantee that the underlying EC implementation is supported.
 */
static const struct acpi_device_id uniwill_id_table[] = {
	{ "INOU0000" },
	{ },
};

static struct platform_driver uniwill_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.dev_groups = uniwill_groups,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.acpi_match_table = uniwill_id_table,
		.pm = pm_sleep_ptr(&uniwill_pm_ops),
	},
	.probe = uniwill_probe,
	.shutdown = uniwill_shutdown,
};

static struct uniwill_device_descriptor lapkc71e_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK_TOGGLE |
		    UNIWILL_FEATURE_SUPER_KEY_TOGGLE |
		    UNIWILL_FEATURE_TOUCHPAD_TOGGLE |
		    UNIWILL_FEATURE_LIGHTBAR |
		    UNIWILL_FEATURE_BATTERY |
		    UNIWILL_FEATURE_HWMON,
	.adjust_fan_start_temp = true,
};


static struct uniwill_device_descriptor lapkc71f_descriptor __initdata = {
	.features = UNIWILL_FEATURE_FN_LOCK_TOGGLE |
		    UNIWILL_FEATURE_SUPER_KEY_TOGGLE |
		    UNIWILL_FEATURE_TOUCHPAD_TOGGLE |
		    UNIWILL_FEATURE_LIGHTBAR |
		    UNIWILL_FEATURE_BATTERY |
		    UNIWILL_FEATURE_HWMON,
	.adjust_fan_start_temp = true,
};

static struct uniwill_device_descriptor empty_descriptor __initdata = {};

static const struct dmi_system_id uniwill_dmi_table[] __initconst = {
	{
		.ident = "XMG FUSION 15",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SchenkerTechnologiesGmbH"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "LAPQC71A"),
		},
		.driver_data = &empty_descriptor,
	},
	{
		.ident = "XMG FUSION 15",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SchenkerTechnologiesGmbH"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "LAPQC71B"),
		},
		.driver_data = &empty_descriptor,
	},
	{
		.ident = "Intel NUC x15",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel(R) Client Systems"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "LAPKC71E"),
		},
		.driver_data = &lapkc71e_descriptor,
	},
	{
		.ident = "Intel NUC x15",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel(R) Client Systems"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "LAPKC71F"),
		},
		.driver_data = &lapkc71f_descriptor,
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, uniwill_dmi_table);

static int __init uniwill_init(void)
{
	const struct uniwill_device_descriptor *descriptor;
	const struct dmi_system_id *id;
	int ret;

	id = dmi_first_match(uniwill_dmi_table);
	if (!id) {
		if (!force)
			return -ENODEV;

		/* Assume that the device supports all features */
		device_descriptor.features = UINT_MAX;
		pr_warn("Loading on a potentially unsupported device\n");
	} else {
		/*
		 * Some devices might support additional features depending on
		 * the BIOS version/date, so we call this callback to let them
		 * modify their device descriptor accordingly.
		 */
		if (id->callback) {
			ret = id->callback(id);
			if (ret < 0)
				return ret;
		}

		descriptor = id->driver_data;
		device_descriptor = *descriptor;
	}

	ret = platform_driver_register(&uniwill_driver);
	if (ret < 0)
		return ret;

	ret = uniwill_wmi_register_driver();
	if (ret < 0) {
		platform_driver_unregister(&uniwill_driver);
		return ret;
	}

	ret = uniwill_ite8291_register_driver();
	if (ret < 0) {
		pr_warn("Could not register ITE 8291 keyboard backlight driver: %d\n", ret);
	} else {
		uniwill_ite8291_registered = true;
	}

	return 0;
}
module_init(uniwill_init);

static void __exit uniwill_exit(void)
{
	if (uniwill_ite8291_registered)
		uniwill_ite8291_unregister_driver();
	uniwill_wmi_unregister_driver();
	platform_driver_unregister(&uniwill_driver);
}
module_exit(uniwill_exit);

MODULE_AUTHOR("Armin Wolf <W_Armin@gmx.de>");
MODULE_DESCRIPTION("Uniwill notebook and ITE 8291 keyboard backlight driver");
MODULE_LICENSE("GPL");
