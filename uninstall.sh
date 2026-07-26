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
REMOVE_DKMS="${REMOVE_DKMS:-0}"
DKMS_VERSION="${DKMS_VERSION:-0.1.0}"

need_root()
{
	if [ "$(id -u)" -ne 0 ]; then
		echo "uninstall.sh: please run as root" >&2
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

need_root

if have_cmd systemctl; then
	run systemctl stop uniwilld-sleep.service || true
	run systemctl disable uniwilld-sleep.service || true
	run systemctl stop uniwilld.service || true
	run systemctl disable uniwilld.service || true
fi

run rm -f "$SYSTEMD_UNIT_DIR/uniwilld-sleep.service"
run rm -f "$SYSTEMD_UNIT_DIR/uniwilld.service"
run rm -f "$SBINDIR/uniwilld"
run rm -f "$BINDIR/uniwill-touchpad-sync"
run rm -f "$UDEV_HELPER_DIR/uniwill-ite8291-bind"
run rm -f "$MODULES_LOAD_DIR/uniwill-laptop.conf"
run rm -f "$UDEV_RULES_DIR/90-uniwill-ite8291.rules"
run rm -f "$UDEV_RULES_DIR/70-uniwill-touchpad.rules"
run rm -f "$XDG_AUTOSTART_DIR/uniwill-touchpad-sync.desktop"
run rm -f /run/uniwilld.sock

if have_cmd systemctl; then
	run systemctl daemon-reload
	run systemctl reset-failed uniwilld-sleep.service || true
	run systemctl reset-failed uniwilld.service || true
fi

if have_cmd udevadm; then
	run udevadm control --reload-rules
fi

if [ "$REMOVE_DKMS" = "1" ]; then
	if ! have_cmd dkms; then
		echo "uninstall.sh: dkms not found, skipping DKMS removal" >&2
	else
		run dkms remove -m uniwill-laptop -v "$DKMS_VERSION" --all || true
	fi
fi

echo "uninstall.sh: removed uniwilld service files"
