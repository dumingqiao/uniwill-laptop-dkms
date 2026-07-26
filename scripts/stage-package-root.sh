#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || "$1" != /* ]]; then
  echo "usage: $0 /absolute/package/root" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"
package_root="$1"
version="$(<"${project_root}/VERSION")"
dkms_source="${package_root}/usr/src/uniwill-laptop-${version}"

install -Dm644 "${project_root}/dkms.conf" "${dkms_source}/dkms.conf"
install -Dm644 "${project_root}/Makefile" "${dkms_source}/Makefile"
install -Dm644 "${project_root}/uniwill-acpi.c" "${dkms_source}/uniwill-acpi.c"
install -Dm644 "${project_root}/uniwill-wmi.c" "${dkms_source}/uniwill-wmi.c"
install -Dm644 "${project_root}/uniwill-wmi.h" "${dkms_source}/uniwill-wmi.h"
install -Dm644 "${project_root}/uniwill-ite8291.c" "${dkms_source}/uniwill-ite8291.c"
install -Dm644 "${project_root}/uniwill-ite8291.h" "${dkms_source}/uniwill-ite8291.h"

install -Dm755 "${project_root}/uniwilld" "${package_root}/usr/bin/uniwilld"
install -Dm755 \
  "${project_root}/uniwill-touchpad-sync" \
  "${package_root}/usr/bin/uniwill-touchpad-sync"

install -Dm644 \
  "${project_root}/packaging/arch/uniwilld.service" \
  "${package_root}/usr/lib/systemd/system/uniwilld.service"
install -Dm644 \
  "${project_root}/uniwilld-sleep.service" \
  "${package_root}/usr/lib/systemd/system/uniwilld-sleep.service"
install -Dm644 \
  "${project_root}/uniwill-laptop.conf" \
  "${package_root}/usr/lib/modules-load.d/uniwill-laptop.conf"

install -Dm755 \
  "${project_root}/uniwill-ite8291-bind" \
  "${package_root}/usr/lib/udev/uniwill-ite8291-bind"
install -Dm644 \
  "${project_root}/90-uniwill-ite8291.rules" \
  "${package_root}/usr/lib/udev/rules.d/90-uniwill-ite8291.rules"
install -Dm644 \
  "${project_root}/uniwill-touchpad-sync.desktop" \
  "${package_root}/etc/xdg/autostart/uniwill-touchpad-sync.desktop"

install -Dm644 \
  "${project_root}/LICENSE" \
  "${package_root}/usr/share/licenses/uniwill-laptop-dkms/LICENSE"
install -Dm644 \
  "${project_root}/README.md" \
  "${package_root}/usr/share/doc/uniwill-laptop-dkms/README.md"
install -Dm644 \
  "${project_root}/uniwilld-json-api.md" \
  "${package_root}/usr/share/doc/uniwill-laptop-dkms/uniwilld-json-api.md"

