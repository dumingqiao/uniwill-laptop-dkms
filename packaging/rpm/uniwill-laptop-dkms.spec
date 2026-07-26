Name:           uniwill-laptop-dkms
Version:        %{driver_version}
Release:        %{driver_release}%{?dist}
Summary:        Uniwill laptop DKMS driver and hardware control service
License:        GPL-2.0-or-later
BuildArch:      x86_64

BuildRequires:  gcc
BuildRequires:  glib2-devel
BuildRequires:  libblockdev-devel
BuildRequires:  libblockdev-fs-devel
BuildRequires:  libblockdev-nvme-devel
BuildRequires:  libblockdev-smart-devel
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  systemd-devel

Requires:       btrfs-progs
Requires:       dkms
Requires:       dosfstools
Requires:       glib2
Requires:       kmod
Requires:       libblockdev
Requires:       libblockdev-fs
Requires:       libblockdev-nvme
Requires:       libblockdev-smart
Requires:       ntfs-3g
Requires:       systemd
Requires:       systemd-libs
Requires:       systemd-udev
Requires:       xfsprogs
Provides:       uniwill-laptop
Provides:       uniwilld
Conflicts:      uniwill-laptop
Conflicts:      uniwilld

%description
Installs the out-of-tree Uniwill laptop kernel driver through DKMS together
with the uniwilld hardware-control service, systemd integration, udev rules,
and the desktop-session touchpad synchronization helper.

%prep

%build
make -B -C "%{project_root}" service

%check
make -B -C "%{project_root}" test

%install
"%{project_root}/scripts/stage-package-root.sh" "%{buildroot}"

%post
systemctl stop uniwilld.service >/dev/null 2>&1 || true
if grep -q '^uniwill_laptop ' /proc/modules; then
  modprobe -r uniwill-laptop >/dev/null 2>&1 || \
    echo "The running Uniwill module could not be replaced; reboot to load the updated driver."
fi
dkms remove -m uniwill-laptop -v %{version} --all >/dev/null 2>&1 || true
dkms add -m uniwill-laptop -v %{version} >/dev/null 2>&1 || true
dkms autoinstall -m uniwill-laptop -v %{version} || \
  echo "DKMS build deferred: install matching kernel headers, then run 'dkms autoinstall'."
systemctl daemon-reload
systemctl enable uniwilld.service uniwilld-sleep.service >/dev/null 2>&1 || true
udevadm control --reload-rules || true
udevadm trigger --subsystem-match=hid --action=add || true
modprobe uniwill-laptop >/dev/null 2>&1 || true
systemctl restart uniwilld.service >/dev/null 2>&1 || true

%preun
if [ "$1" -eq 0 ]; then
  systemctl disable --now uniwilld-sleep.service uniwilld.service >/dev/null 2>&1 || true
  dkms remove -m uniwill-laptop -v %{version} --all >/dev/null 2>&1 || true
fi

%postun
if [ "$1" -eq 0 ]; then
  systemctl daemon-reload
  systemctl reset-failed uniwilld-sleep.service uniwilld.service >/dev/null 2>&1 || true
  udevadm control --reload-rules || true
  rm -f /run/uniwilld.sock
fi

%files
%license /usr/share/licenses/uniwill-laptop-dkms/LICENSE
%doc /usr/share/doc/uniwill-laptop-dkms/README.md
%doc /usr/share/doc/uniwill-laptop-dkms/uniwilld-json-api.md
%config(noreplace) /etc/xdg/autostart/uniwill-touchpad-sync.desktop
/usr/bin/uniwill-touchpad-sync
/usr/bin/uniwilld
/usr/lib/modules-load.d/uniwill-laptop.conf
/usr/lib/systemd/system/uniwilld-sleep.service
/usr/lib/systemd/system/uniwilld.service
/usr/lib/udev/rules.d/90-uniwill-ite8291.rules
/usr/lib/udev/uniwill-ite8291-bind
/usr/src/uniwill-laptop-%{version}/
