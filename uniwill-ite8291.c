// SPDX-License-Identifier: GPL-2.0
/*
 * ITE 8291 revision 0.03 RGB keyboard backlight support.
 *
 * Based on the standalone hid-ite8291r3 driver by Barnabas Pocze. This
 * version is registered as a sub-driver of uniwill-laptop.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <dt-bindings/leds/common.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/usb.h>

#include "uniwill-ite8291.h"
#include "uniwill-wmi.h"

#define ITE8291R3_NUM_ROWS		6
#define ITE8291R3_NUM_COLS		21
#define ITE8291R3_MAX_BRIGHTNESS	50

#define ITE8291R3_ROW_COLOR_OFFSET	2
#define ITE8291R3_ROW_RED_OFFSET \
	(ITE8291R3_ROW_COLOR_OFFSET + 2 * ITE8291R3_NUM_COLS)
#define ITE8291R3_ROW_GREEN_OFFSET	(ITE8291R3_ROW_COLOR_OFFSET + 1 * ITE8291R3_NUM_COLS)
#define ITE8291R3_ROW_BLUE_OFFSET	(ITE8291R3_ROW_COLOR_OFFSET + 0 * ITE8291R3_NUM_COLS)

#define ITE8291R3_HID_REPORT_LENGTH	9
#define ITE8291R3_ROW_REPORT_LENGTH	(2 + 3 * ITE8291R3_NUM_COLS)

#define ITE8291R3_SET_EFFECT		8
#define ITE8291R3_SET_BRIGHTNESS	9
#define ITE8291R3_SET_COLOR		20
#define ITE8291R3_SET_ROW_INDEX		22
#define ITE8291R3_GET_FW_VERSION	128
#define ITE8291R3_GET_EFFECT		136

#define ITE8291R3_REP_BRIGHTNESS_OFFSET	5
#define ITE8291R3_FW_VERSION_LENGTH	4
#define ITE8291R3_USB_REVISION		0x0003

#define USB_VENDOR_ID_ITE		0x048d

#define ITE8291R3_EFFECT_BREATHING	0x02
#define ITE8291R3_EFFECT_WAVE		0x03
#define ITE8291R3_EFFECT_RANDOM		0x04
#define ITE8291R3_EFFECT_RAINBOW		0x05
#define ITE8291R3_EFFECT_RIPPLE		0x06
#define ITE8291R3_EFFECT_MARQUEE		0x09
#define ITE8291R3_EFFECT_RAINDROP	0x0a
#define ITE8291R3_EFFECT_AURORA		0x0e
#define ITE8291R3_EFFECT_FIREWORKS	0x11
#define ITE8291R3_EFFECT_USER		0x33

#define ITE8291R3_DIRECTION_RIGHT	1
#define ITE8291R3_DIRECTION_LEFT		2
#define ITE8291R3_DIRECTION_UP		3
#define ITE8291R3_DIRECTION_DOWN		4

struct ite8291r3_effect_name {
	const char *name;
	u8 value;
	bool supports_color;
	bool supports_reactive;
	bool supports_direction;
};

static const struct ite8291r3_effect_name ite8291r3_effects[] = {
	{ "solid", ITE8291R3_EFFECT_USER, true, false, false },
	{ "breathing", ITE8291R3_EFFECT_BREATHING, true, false, false },
	{ "wave", ITE8291R3_EFFECT_WAVE, false, false, true },
	{ "random", ITE8291R3_EFFECT_RANDOM, true, true, false },
	{ "rainbow", ITE8291R3_EFFECT_RAINBOW, false, false, false },
	{ "ripple", ITE8291R3_EFFECT_RIPPLE, true, true, false },
	{ "marquee", ITE8291R3_EFFECT_MARQUEE, false, false, false },
	{ "raindrop", ITE8291R3_EFFECT_RAINDROP, true, false, false },
	{ "aurora", ITE8291R3_EFFECT_AURORA, true, true, false },
	{ "fireworks", ITE8291R3_EFFECT_FIREWORKS, true, true, false },
};

struct ite8291r3_priv {
	struct hid_device *hdev;
	struct led_classdev_mc mcled;
	struct mc_subled subleds[3];
	struct mutex lock;	/* Serializes HID reports and cached state. */
	char name[64];
	u32 color;
	u32 effect_color;
	u32 subled_state;
	u8 brightness;
	u8 effect;
	u8 effect_speed;
	u8 effect_direction;
	bool effect_reactive;
	u8 transfer_buf[ITE8291R3_HID_REPORT_LENGTH];
	u8 row_color_buf[ITE8291R3_ROW_REPORT_LENGTH];
};

static DEFINE_MUTEX(ite8291r3_active_lock);
static struct ite8291r3_priv *ite8291r3_active;

static const u8 ite8291r3_brightness_steps[] = { 0, 13, 25, 38, 50 };

static int ite8291r3_set_color(struct ite8291r3_priv *priv, u32 color,
			      u8 brightness);

static const struct ite8291r3_effect_name *
ite8291r3_find_effect_by_value(u8 value)
{
	for (size_t i = 0; i < ARRAY_SIZE(ite8291r3_effects); i++)
		if (ite8291r3_effects[i].value == value)
			return &ite8291r3_effects[i];

	return NULL;
}

static const struct ite8291r3_effect_name *
ite8291r3_find_effect_by_name(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(ite8291r3_effects); i++)
		if (sysfs_streq(name, ite8291r3_effects[i].name))
			return &ite8291r3_effects[i];

	return NULL;
}

static struct ite8291r3_priv *ite8291r3_priv_from_led(struct led_classdev *led_cdev)
{
	struct led_classdev_mc *mcled_cdev = lcdev_to_mccdev(led_cdev);

	return container_of(mcled_cdev, struct ite8291r3_priv, mcled);
}

static u32 ite8291r3_get_subled_state(struct ite8291r3_priv *priv)
{
	u32 red = READ_ONCE(priv->subleds[0].intensity);
	u32 green = READ_ONCE(priv->subleds[1].intensity);
	u32 blue = READ_ONCE(priv->subleds[2].intensity);

	return (red << 16) | (green << 8) | blue;
}

static u32 ite8291r3_color_from_subleds(struct ite8291r3_priv *priv)
{
	u32 state = ite8291r3_get_subled_state(priv);
	u32 red = state >> 16;
	u32 green = (state >> 8) & 0xff;
	u32 blue = state & 0xff;

	/* multi_intensity uses max_brightness (50); the HID channels use 255. */
	red = DIV_ROUND_CLOSEST(red * 255, ITE8291R3_MAX_BRIGHTNESS);
	green = DIV_ROUND_CLOSEST(green * 255, ITE8291R3_MAX_BRIGHTNESS);
	blue = DIV_ROUND_CLOSEST(blue * 255, ITE8291R3_MAX_BRIGHTNESS);

	return (red << 16) | (green << 8) | blue;
}

static void ite8291r3_update_subleds(struct ite8291r3_priv *priv, u32 color)
{
	u32 red = color >> 16;
	u32 green = (color >> 8) & 0xff;
	u32 blue = color & 0xff;

	WRITE_ONCE(priv->subleds[0].intensity,
		   DIV_ROUND_CLOSEST(red * ITE8291R3_MAX_BRIGHTNESS, 255));
	WRITE_ONCE(priv->subleds[1].intensity,
		   DIV_ROUND_CLOSEST(green * ITE8291R3_MAX_BRIGHTNESS, 255));
	WRITE_ONCE(priv->subleds[2].intensity,
		   DIV_ROUND_CLOSEST(blue * ITE8291R3_MAX_BRIGHTNESS, 255));
}

static int ite8291r3_lock_and_get(struct ite8291r3_priv *priv)
{
	struct usb_interface *intf = to_usb_interface(priv->hdev->dev.parent);
	int ret;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret < 0)
		return ret;

	ret = usb_autopm_get_interface(intf);
	if (ret < 0)
		mutex_unlock(&priv->lock);

	return ret;
}

static void ite8291r3_put_and_unlock(struct ite8291r3_priv *priv)
{
	struct usb_interface *intf = to_usb_interface(priv->hdev->dev.parent);

	lockdep_assert_held(&priv->lock);
	usb_autopm_put_interface(intf);
	mutex_unlock(&priv->lock);
}

/* priv->lock must be held and the USB interface runtime-PM reference held. */
static int ite8291r3_receive(struct ite8291r3_priv *priv)
{
	int ret;

	lockdep_assert_held(&priv->lock);
	memset(priv->transfer_buf, 0, sizeof(priv->transfer_buf));

	ret = hid_hw_raw_request(priv->hdev, 0, priv->transfer_buf,
				 sizeof(priv->transfer_buf), HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_err(priv->hdev, "get feature report failed: %d\n", ret);
		return ret;
	}
	if (ret != sizeof(priv->transfer_buf)) {
		hid_err(priv->hdev, "short feature report: got %d, expected %zu\n",
			ret, sizeof(priv->transfer_buf));
		return -ENODATA;
	}

	return ret;
}

/* priv->lock must be held and the USB interface runtime-PM reference held. */
static int ite8291r3_send(struct ite8291r3_priv *priv)
{
	int ret;

	lockdep_assert_held(&priv->lock);
	ret = hid_hw_raw_request(priv->hdev, 0, priv->transfer_buf,
				 sizeof(priv->transfer_buf), HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);
	if (ret < 0) {
		hid_err(priv->hdev, "set feature report failed: %d\n", ret);
		return ret;
	}
	if (ret != sizeof(priv->transfer_buf)) {
		hid_err(priv->hdev, "short feature write: sent %d, expected %zu\n",
			ret, sizeof(priv->transfer_buf));
		return -ENOSPC;
	}

	return 0;
}

static int ite8291r3_get_brightness(struct ite8291r3_priv *priv)
{
	int ret;

	lockdep_assert_held(&priv->lock);
	memset(priv->transfer_buf, 0, sizeof(priv->transfer_buf));
	priv->transfer_buf[1] = ITE8291R3_GET_EFFECT;

	ret = ite8291r3_send(priv);
	if (ret < 0)
		return ret;
	usleep_range(1000, 2000);

	ret = ite8291r3_receive(priv);
	if (ret < 0)
		return ret;

	return priv->transfer_buf[ITE8291R3_REP_BRIGHTNESS_OFFSET];
}

static int ite8291r3_set_brightness(struct ite8291r3_priv *priv, u8 brightness)
{
	int ret;

	lockdep_assert_held(&priv->lock);
	memset(priv->transfer_buf, 0, sizeof(priv->transfer_buf));
	priv->transfer_buf[1] = ITE8291R3_SET_BRIGHTNESS;
	priv->transfer_buf[2] = 0x02;
	priv->transfer_buf[3] = brightness;

	ret = ite8291r3_send(priv);
	if (ret == 0)
		priv->brightness = brightness;

	return ret;
}

/* priv->lock must be held and the USB interface runtime-PM reference held. */
static int ite8291r3_set_palette_color(struct ite8291r3_priv *priv, u8 index,
				       u32 color)
{
	lockdep_assert_held(&priv->lock);
	memset(priv->transfer_buf, 0, sizeof(priv->transfer_buf));
	priv->transfer_buf[1] = ITE8291R3_SET_COLOR;
	priv->transfer_buf[3] = index;
	priv->transfer_buf[4] = color >> 16;
	priv->transfer_buf[5] = color >> 8;
	priv->transfer_buf[6] = color;
	return ite8291r3_send(priv);
}

/* priv->lock must be held and the USB interface runtime-PM reference held. */
static int ite8291r3_apply_effect(struct ite8291r3_priv *priv)
{
	const struct ite8291r3_effect_name *effect;
	u8 modifier = 0;
	u8 color_index = 0;
	int ret;

	lockdep_assert_held(&priv->lock);
	effect = ite8291r3_find_effect_by_value(priv->effect);
	if (!effect)
		return -EINVAL;

	if (priv->effect == ITE8291R3_EFFECT_USER)
		return ite8291r3_set_color(priv, priv->effect_color,
					   priv->brightness);

	if (effect->supports_color) {
		ret = ite8291r3_set_palette_color(priv, 1, priv->effect_color);
		if (ret < 0)
			return ret;
		color_index = 1;
	}
	if (effect->supports_direction)
		modifier = priv->effect_direction;
	else if (effect->supports_reactive)
		modifier = priv->effect_reactive ? 1 : 0;

	memset(priv->transfer_buf, 0, sizeof(priv->transfer_buf));
	priv->transfer_buf[1] = ITE8291R3_SET_EFFECT;
	priv->transfer_buf[2] = 0x02;
	priv->transfer_buf[3] = priv->effect;
	priv->transfer_buf[4] = priv->effect_speed;
	priv->transfer_buf[5] = priv->brightness;
	priv->transfer_buf[6] = color_index;
	priv->transfer_buf[7] = modifier;
	return ite8291r3_send(priv);
}

static u8 ite8291r3_next_brightness(u8 brightness)
{
	for (size_t i = 0; i < ARRAY_SIZE(ite8291r3_brightness_steps); i++) {
		if (ite8291r3_brightness_steps[i] > brightness)
			return ite8291r3_brightness_steps[i];
	}

	return 0;
}

static u8 ite8291r3_increase_brightness(u8 brightness)
{
	for (size_t i = 0; i < ARRAY_SIZE(ite8291r3_brightness_steps); i++) {
		if (ite8291r3_brightness_steps[i] > brightness)
			return ite8291r3_brightness_steps[i];
	}

	return ITE8291R3_MAX_BRIGHTNESS;
}

static u8 ite8291r3_decrease_brightness(u8 brightness)
{
	for (size_t i = ARRAY_SIZE(ite8291r3_brightness_steps); i > 0; i--) {
		if (ite8291r3_brightness_steps[i - 1] < brightness)
			return ite8291r3_brightness_steps[i - 1];
	}

	return 0;
}

int uniwill_ite8291_handle_brightness_event(unsigned long event)
{
	struct ite8291r3_priv *priv;
	struct led_classdev *led_cdev;
	u8 brightness;
	u8 target;
	int ret;

	mutex_lock(&ite8291r3_active_lock);
	priv = ite8291r3_active;
	if (!priv) {
		ret = -ENODEV;
		goto out_active;
	}
	led_cdev = &priv->mcled.led_cdev;

	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		goto out_active;

	/*
	 * GET_EFFECT takes roughly 200 ms on some ITE 8291 firmware.  Most WMI
	 * events describe the requested level, so use the cache and only query
	 * the controller for the generic "hardware changed" notification.
	 */
	if (event == UNIWILL_OSD_KBD_BACKLIGHT_CHANGED) {
		ret = ite8291r3_get_brightness(priv);
		if (ret < 0)
			goto out_device;
		if (ret > ITE8291R3_MAX_BRIGHTNESS) {
			hid_warn_ratelimited(priv->hdev,
					     "ignoring invalid brightness %d\n", ret);
			ret = 0;
			goto out_device;
		}

		brightness = ret;
		WRITE_ONCE(priv->brightness, brightness);
		WRITE_ONCE(led_cdev->brightness, brightness);
		led_classdev_notify_brightness_hw_changed(led_cdev, brightness);
		ret = 0;
		goto out_device;
	}

	brightness = READ_ONCE(priv->brightness);
	target = brightness;

	switch (event) {
	case UNIWILL_OSD_KBDILLUMDOWN:
		target = ite8291r3_decrease_brightness(brightness);
		break;
	case UNIWILL_OSD_KBDILLUMUP:
		target = ite8291r3_increase_brightness(brightness);
		break;
	case UNIWILL_OSD_KBDILLUMTOGGLE:
		target = ite8291r3_next_brightness(brightness);
		break;
	case UNIWILL_OSD_KB_LED_LEVEL0:
		target = ite8291r3_brightness_steps[0];
		break;
	case UNIWILL_OSD_KB_LED_LEVEL1:
		target = ite8291r3_brightness_steps[1];
		break;
	case UNIWILL_OSD_KB_LED_LEVEL2:
		target = ite8291r3_brightness_steps[2];
		break;
	case UNIWILL_OSD_KB_LED_LEVEL3:
		target = ite8291r3_brightness_steps[3];
		break;
	case UNIWILL_OSD_KB_LED_LEVEL4:
		target = ite8291r3_brightness_steps[4];
		break;
	default:
		ret = -EOPNOTSUPP;
		goto out_device;
	}

	ret = ite8291r3_set_brightness(priv, target);
	if (ret < 0)
		goto out_device;

	WRITE_ONCE(led_cdev->brightness, target);
	led_classdev_notify_brightness_hw_changed(led_cdev, target);

out_device:
	ite8291r3_put_and_unlock(priv);
out_active:
	mutex_unlock(&ite8291r3_active_lock);
	return ret;
}

static int ite8291r3_get_firmware_version(struct ite8291r3_priv *priv, u8 *version)
{
	int ret;

	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		return ret;

	memset(priv->transfer_buf, 0, sizeof(priv->transfer_buf));
	priv->transfer_buf[1] = ITE8291R3_GET_FW_VERSION;

	ret = ite8291r3_send(priv);
	if (ret < 0)
		goto out;
	usleep_range(1000, 2000);

	ret = ite8291r3_receive(priv);
	if (ret < ITE8291R3_FW_VERSION_LENGTH + 2) {
		if (ret >= 0)
			ret = -ENODATA;
		goto out;
	}

	memcpy(version, &priv->transfer_buf[2], ITE8291R3_FW_VERSION_LENGTH);
	ret = 0;

out:
	ite8291r3_put_and_unlock(priv);
	return ret;
}

static enum led_brightness
ite8291r3_led_get_brightness(struct led_classdev *led_cdev)
{
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);

	/* Sysfs reads must be cheap; explicit hardware-change events refresh it. */
	return READ_ONCE(priv->brightness);
}

static int ite8291r3_led_set_brightness(struct led_classdev *led_cdev,
					enum led_brightness value)
{
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	u32 color;
	u32 subled_state;
	int ret;

	if (led_cdev->flags & LED_UNREGISTERING)
		return 0;
	if (value > ITE8291R3_MAX_BRIGHTNESS)
		return -EINVAL;

	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		return ret;

	subled_state = ite8291r3_get_subled_state(priv);
	color = ite8291r3_color_from_subleds(priv);
	if (subled_state != priv->subled_state)
		ret = ite8291r3_set_color(priv, color, value);
	else if (value == priv->brightness)
		ret = 0;
	else
		ret = ite8291r3_set_brightness(priv, value);
	ite8291r3_put_and_unlock(priv);
	return ret;
}

/* priv->lock must be held and the USB interface runtime-PM reference held. */
static int ite8291r3_set_color(struct ite8291r3_priv *priv, u32 color,
			      u8 brightness)
{
	u8 red = color >> 16;
	u8 green = color >> 8;
	u8 blue = color;
	unsigned int row;
	unsigned int col;
	int ret;

	lockdep_assert_held(&priv->lock);

	memset(priv->transfer_buf, 0, sizeof(priv->transfer_buf));
	priv->transfer_buf[1] = ITE8291R3_SET_EFFECT;
	priv->transfer_buf[2] = 0x02;
	priv->transfer_buf[3] = 0x33;
	priv->transfer_buf[5] = brightness;
	ret = ite8291r3_send(priv);
	if (ret < 0)
		goto out;

	memset(priv->row_color_buf, 0, sizeof(priv->row_color_buf));
	for (col = 0; col < ITE8291R3_NUM_COLS; col++) {
		priv->row_color_buf[ITE8291R3_ROW_RED_OFFSET + col] = red;
		priv->row_color_buf[ITE8291R3_ROW_GREEN_OFFSET + col] = green;
		priv->row_color_buf[ITE8291R3_ROW_BLUE_OFFSET + col] = blue;
	}

	for (row = 0; row < ITE8291R3_NUM_ROWS; row++) {
		memset(priv->transfer_buf, 0, sizeof(priv->transfer_buf));
		priv->transfer_buf[1] = ITE8291R3_SET_ROW_INDEX;
		priv->transfer_buf[3] = row;

		ret = ite8291r3_send(priv);
		if (ret < 0)
			goto out;

		ret = hid_hw_output_report(priv->hdev, priv->row_color_buf,
					   sizeof(priv->row_color_buf));
		if (ret < 0)
			goto out;
	}

	priv->color = color;
	priv->effect_color = color;
	priv->effect = ITE8291R3_EFFECT_USER;
	priv->brightness = brightness;
	ite8291r3_update_subleds(priv, color);
	priv->subled_state = ite8291r3_get_subled_state(priv);
	ret = 0;

out:
	return ret;
}

static ssize_t color_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	u32 color;

	mutex_lock(&priv->lock);
	color = priv->color;
	mutex_unlock(&priv->lock);

	if (color == U32_MAX)
		return -ENODATA;

	return sysfs_emit(buf, "%06x\n", color);
}

static ssize_t color_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	unsigned long value;
	int ret;

	ret = kstrtoul(buf, 16, &value);
	if (ret < 0)
		return ret;
	if (value > 0xffffff)
		return -EINVAL;

	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		return ret;

	ret = ite8291r3_set_color(priv, value, priv->brightness);
	ite8291r3_put_and_unlock(priv);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(color);

static ssize_t effect_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	const struct ite8291r3_effect_name *effect;

	mutex_lock(&priv->lock);
	effect = ite8291r3_find_effect_by_value(priv->effect);
	mutex_unlock(&priv->lock);
	return sysfs_emit(buf, "%s\n", effect ? effect->name : "unknown");
}

static ssize_t effect_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	const struct ite8291r3_effect_name *effect;
	int ret;

	effect = ite8291r3_find_effect_by_name(buf);
	if (!effect)
		return -EINVAL;
	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		return ret;
	priv->effect = effect->value;
	ret = ite8291r3_apply_effect(priv);
	ite8291r3_put_and_unlock(priv);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(effect);

static ssize_t effect_speed_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(priv->effect_speed));
}

static ssize_t effect_speed_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 10, &value);
	if (ret < 0)
		return ret;
	if (value > 10)
		return -EINVAL;
	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		return ret;
	priv->effect_speed = value;
	ret = ite8291r3_apply_effect(priv);
	ite8291r3_put_and_unlock(priv);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(effect_speed);

static const char *ite8291r3_direction_name(u8 direction)
{
	switch (direction) {
	case ITE8291R3_DIRECTION_RIGHT:
		return "right";
	case ITE8291R3_DIRECTION_LEFT:
		return "left";
	case ITE8291R3_DIRECTION_UP:
		return "up";
	case ITE8291R3_DIRECTION_DOWN:
		return "down";
	default:
		return "left";
	}
}

static int ite8291r3_parse_direction(const char *buf, u8 *direction)
{
	if (sysfs_streq(buf, "right"))
		*direction = ITE8291R3_DIRECTION_RIGHT;
	else if (sysfs_streq(buf, "left"))
		*direction = ITE8291R3_DIRECTION_LEFT;
	else if (sysfs_streq(buf, "up"))
		*direction = ITE8291R3_DIRECTION_UP;
	else if (sysfs_streq(buf, "down"))
		*direction = ITE8291R3_DIRECTION_DOWN;
	else
		return -EINVAL;
	return 0;
}

static ssize_t effect_direction_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);

	return sysfs_emit(buf, "%s\n",
			  ite8291r3_direction_name(READ_ONCE(priv->effect_direction)));
}

static ssize_t effect_direction_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	u8 direction;
	int ret;

	ret = ite8291r3_parse_direction(buf, &direction);
	if (ret < 0)
		return ret;
	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		return ret;
	priv->effect_direction = direction;
	ret = ite8291r3_apply_effect(priv);
	ite8291r3_put_and_unlock(priv);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(effect_direction);

static ssize_t effect_reactive_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(priv->effect_reactive));
}

static ssize_t effect_reactive_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	bool value;
	int ret;

	ret = kstrtobool(buf, &value);
	if (ret < 0)
		return ret;
	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		return ret;
	priv->effect_reactive = value;
	ret = ite8291r3_apply_effect(priv);
	ite8291r3_put_and_unlock(priv);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(effect_reactive);

static ssize_t effect_color_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);

	return sysfs_emit(buf, "%06x\n", READ_ONCE(priv->effect_color));
}

static ssize_t effect_color_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ite8291r3_priv *priv = ite8291r3_priv_from_led(led_cdev);
	unsigned long value;
	int ret;

	ret = kstrtoul(buf, 16, &value);
	if (ret < 0)
		return ret;
	if (value > 0xffffff)
		return -EINVAL;
	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0)
		return ret;
	priv->effect_color = value;
	ret = ite8291r3_apply_effect(priv);
	ite8291r3_put_and_unlock(priv);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(effect_color);

static struct attribute *ite8291r3_attrs[] = {
	&dev_attr_color.attr,
	&dev_attr_effect.attr,
	&dev_attr_effect_speed.attr,
	&dev_attr_effect_direction.attr,
	&dev_attr_effect_reactive.attr,
	&dev_attr_effect_color.attr,
	NULL,
};

static const struct attribute_group ite8291r3_attr_group = {
	.attrs = ite8291r3_attrs,
};

static int ite8291r3_probe(struct hid_device *hdev,
			   const struct hid_device_id *id)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct ite8291r3_priv *priv;
	struct led_classdev_mc *mcled_cdev;
	struct led_classdev *led_cdev;
	u8 version[ITE8291R3_FW_VERSION_LENGTH];
	int brightness;
	int ret;

	if (le16_to_cpu(usb_dev->descriptor.bcdDevice) != ITE8291R3_USB_REVISION)
		return -ENODEV;

	ret = hid_parse(hdev);
	if (ret < 0)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret < 0)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret < 0)
		goto err_stop;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_close;
	}

	priv->hdev = hdev;
	priv->color = U32_MAX;
	priv->effect_color = 0xffffff;
	priv->effect = ITE8291R3_EFFECT_USER;
	priv->effect_speed = 5;
	priv->effect_direction = ITE8291R3_DIRECTION_LEFT;
	priv->subled_state = U32_MAX;
	ite8291r3_update_subleds(priv, 0xffffff);
	mcled_cdev = &priv->mcled;
	led_cdev = &mcled_cdev->led_cdev;
	mutex_init(&priv->lock);

	ret = ite8291r3_get_firmware_version(priv, version);
	if (ret < 0)
		goto err_free;

	/*
	 * Query the slow controller once; later sysfs reads use this cache.
	 * Some firmware returns a transient out-of-range value while the USB
	 * interface is still settling during boot.  That must not prevent the
	 * LED device from registering because subsequent writes still work.
	 */
	brightness = ITE8291R3_MAX_BRIGHTNESS;
	ret = ite8291r3_lock_and_get(priv);
	if (ret < 0) {
		hid_warn(hdev, "could not read initial brightness: %d\n", ret);
	} else {
		ret = ite8291r3_get_brightness(priv);
		ite8291r3_put_and_unlock(priv);
		if (ret < 0)
			hid_warn(hdev, "could not read initial brightness: %d\n", ret);
		else if (ret > ITE8291R3_MAX_BRIGHTNESS)
			hid_warn(hdev, "ignoring invalid initial brightness %d\n", ret);
		else
			brightness = ret;
	}
	priv->brightness = brightness;

	snprintf(priv->name, sizeof(priv->name),
		 "usb-%d-%d-%d-%d::" LED_FUNCTION_KBD_BACKLIGHT,
		 usb_dev->bus->busnum, usb_dev->portnum, usb_dev->devnum,
		 intf->cur_altsetting->desc.bInterfaceNumber);
	led_cdev->name = priv->name;
	led_cdev->max_brightness = ITE8291R3_MAX_BRIGHTNESS;
	led_cdev->brightness = brightness;
	led_cdev->brightness_get = ite8291r3_led_get_brightness;
	led_cdev->brightness_set_blocking = ite8291r3_led_set_brightness;
	led_cdev->flags = LED_CORE_SUSPENDRESUME | LED_BRIGHT_HW_CHANGED;
	mcled_cdev->num_colors = ARRAY_SIZE(priv->subleds);
	mcled_cdev->subled_info = priv->subleds;
	priv->subleds[0].color_index = LED_COLOR_ID_RED;
	priv->subleds[1].color_index = LED_COLOR_ID_GREEN;
	priv->subleds[2].color_index = LED_COLOR_ID_BLUE;

	ret = led_classdev_multicolor_register(&hdev->dev, mcled_cdev);
	if (ret < 0)
		goto err_free;

	/*
	 * led_classdev_multicolor_register() installs the multicolor group's
	 * attributes by replacing led_cdev->groups. Add the controller-specific
	 * effect attributes after registration so both groups are exposed.
	 */
	ret = sysfs_create_group(&led_cdev->dev->kobj, &ite8291r3_attr_group);
	if (ret < 0)
		goto err_unregister_led;

	hid_set_drvdata(hdev, priv);
	mutex_lock(&ite8291r3_active_lock);
	if (!ite8291r3_active)
		ite8291r3_active = priv;
	mutex_unlock(&ite8291r3_active_lock);
	hid_info(hdev, "keyboard backlight firmware %*ph registered as %s\n",
		 (int)sizeof(version), version, priv->name);
	return 0;

err_unregister_led:
	led_classdev_multicolor_unregister(mcled_cdev);
err_free:
	mutex_destroy(&priv->lock);
	kfree(priv);
err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void ite8291r3_remove(struct hid_device *hdev)
{
	struct ite8291r3_priv *priv = hid_get_drvdata(hdev);

	mutex_lock(&ite8291r3_active_lock);
	if (ite8291r3_active == priv)
		ite8291r3_active = NULL;
	mutex_unlock(&ite8291r3_active_lock);
	sysfs_remove_group(&priv->mcled.led_cdev.dev->kobj,
			   &ite8291r3_attr_group);
	led_classdev_multicolor_unregister(&priv->mcled);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	mutex_destroy(&priv->lock);
	kfree(priv);
}

static const struct hid_device_id ite8291r3_device_ids[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, 0x6004) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, 0x6006) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, 0xce00) },
	{ }
};
MODULE_DEVICE_TABLE(hid, ite8291r3_device_ids);

static struct hid_driver ite8291r3_driver = {
	.name = "uniwill-ite8291",
	.id_table = ite8291r3_device_ids,
	.probe = ite8291r3_probe,
	.remove = ite8291r3_remove,
};

int __init uniwill_ite8291_register_driver(void)
{
	return hid_register_driver(&ite8291r3_driver);
}

void __exit uniwill_ite8291_unregister_driver(void)
{
	hid_unregister_driver(&ite8291r3_driver);
}

MODULE_AUTHOR("Barnabas Pocze <pobrn@protonmail.com>");
