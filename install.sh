#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

PREFIX="${PREFIX:-/usr/local}"
SBINDIR="${SBINDIR:-$PREFIX/sbin}"
BINDIR="${BINDIR:-$PREFIX/bin}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"
MODULES_LOAD_DIR="${MODULES_LOAD_DIR:-/etc/modules-load.d}"
UDEV_RULES_DIR="${UDEV_RULES_DIR:-/etc/udev/rules.d}"
UDEV_HELPER_DIR="${UDEV_HELPER_DIR:-/usr/lib/udev}"
XDG_AUTOSTART_DIR="${XDG_AUTOSTART_DIR:-/etc/xdg/autostart}"
INSTALL_DKMS="${INSTALL_DKMS:-0}"
DKMS_VERSION="${DKMS_VERSION:-0.1.0}"
ENABLE_SERVICE="${ENABLE_SERVICE:-1}"
START_SERVICE="${START_SERVICE:-1}"

need_root()
{
	if [ "$(id -u)" -ne 0 ]; then
		echo "install.sh: please run as root" >&2
		exit 1
	fi
}

run()
{
	echo "+ $*"
	"$@"
}

have_cmd()
{
	command -v "$1" >/dev/null 2>&1
}

dkms_registered()
{
	module_name="$1"
	module_version="$2"

	dkms status -m "$module_name" -v "$module_version" 2>/dev/null |
		awk -F, -v key="$module_name/$module_version" '
			$1 == key { found = 1 }
			END { exit found ? 0 : 1 }
		'
}

need_root

if ! have_cmd make; then
	echo "install.sh: make is required" >&2
	exit 1
fi
if ! have_cmd pkg-config || ! pkg-config --exists gio-2.0; then
	echo "install.sh: pkg-config and the gio-2.0 development files are required" >&2
	exit 1
fi

run make service

run install -d "$SBINDIR"
run install -m 0755 uniwilld "$SBINDIR/uniwilld"
run install -d "$BINDIR"
run install -m 0755 uniwill-touchpad-sync "$BINDIR/uniwill-touchpad-sync"

run install -d "$SYSTEMD_UNIT_DIR"
run install -m 0644 uniwilld.service "$SYSTEMD_UNIT_DIR/uniwilld.service"
run install -m 0644 uniwilld-sleep.service "$SYSTEMD_UNIT_DIR/uniwilld-sleep.service"

run install -d "$MODULES_LOAD_DIR"
run install -m 0644 uniwill-laptop.conf "$MODULES_LOAD_DIR/uniwill-laptop.conf"

run install -d "$UDEV_HELPER_DIR"
run install -m 0755 uniwill-ite8291-bind "$UDEV_HELPER_DIR/uniwill-ite8291-bind"
run install -d "$UDEV_RULES_DIR"
run install -m 0644 90-uniwill-ite8291.rules "$UDEV_RULES_DIR/90-uniwill-ite8291.rules"
# Older builds granted the desktop direct hidraw access. State writes now go
# through the privileged uniwilld socket, so remove that obsolete rule.
run rm -f "$UDEV_RULES_DIR/70-uniwill-touchpad.rules"
run install -d "$XDG_AUTOSTART_DIR"
run install -m 0644 uniwill-touchpad-sync.desktop \
	"$XDG_AUTOSTART_DIR/uniwill-touchpad-sync.desktop"

if [ "$INSTALL_DKMS" = "1" ]; then
	if ! have_cmd dkms; then
		echo "install.sh: dkms is required when INSTALL_DKMS=1" >&2
		exit 1
	fi

	if dkms_registered uniwill-laptop "$DKMS_VERSION"; then
		run dkms remove -m uniwill-laptop -v "$DKMS_VERSION" --all
	fi

	run dkms add "$PWD"
	run dkms build -m uniwill-laptop -v "$DKMS_VERSION"
	run dkms install -m uniwill-laptop -v "$DKMS_VERSION"
fi

if have_cmd udevadm; then
	run udevadm control --reload-rules
	run udevadm trigger --subsystem-match=hid --action=add
	run udevadm settle
fi

if have_cmd systemctl; then
	run systemctl daemon-reload
	if [ "$ENABLE_SERVICE" = "1" ]; then
		run systemctl enable uniwilld.service uniwilld-sleep.service
	fi
	if [ "$START_SERVICE" = "1" ]; then
		run systemctl restart uniwilld.service
	fi
else
	echo "install.sh: systemctl not found, installed unit but did not enable/start it" >&2
fi

echo "install.sh: installed uniwilld to $SBINDIR/uniwilld"
echo "install.sh: installed systemd unit to $SYSTEMD_UNIT_DIR/uniwilld.service"
echo "install.sh: installed sleep/resume unit to $SYSTEMD_UNIT_DIR/uniwilld-sleep.service"
echo "install.sh: installed module load configuration to $MODULES_LOAD_DIR/uniwill-laptop.conf"
echo "install.sh: installed ITE 8291 HID binding rule to $UDEV_RULES_DIR/90-uniwill-ite8291.rules"
echo "install.sh: installed touchpad LED sync to $BINDIR/uniwill-touchpad-sync"
echo "install.sh: installed touchpad session autostart to $XDG_AUTOSTART_DIR/uniwill-touchpad-sync.desktop"
