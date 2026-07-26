#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"
"${project_root}/scripts/check-version.sh"
version="$(<"${project_root}/VERSION")"
build_number="$(<"${project_root}/BUILD_NUMBER")"
bundle_dir="${project_root}/dist/rpm"
rpm_root="$(mktemp -d "${TMPDIR:-/tmp}/uniwill-rpm.XXXXXX")"

cleanup() {
  rm -rf -- "${rpm_root}"
}
trap cleanup EXIT

if [[ "$(uname -m)" != "x86_64" ]]; then
  echo "build-rpm-package.sh: x86_64 build host required" >&2
  exit 1
fi

mkdir -p \
  "${rpm_root}/BUILD" \
  "${rpm_root}/BUILDROOT" \
  "${rpm_root}/RPMS" \
  "${rpm_root}/SOURCES" \
  "${rpm_root}/SPECS" \
  "${rpm_root}/SRPMS"

rpmbuild -bb "${project_root}/packaging/rpm/uniwill-laptop-dkms.spec" \
  --define "_topdir ${rpm_root}" \
  --define "project_root ${project_root}" \
  --define "driver_version ${version}" \
  --define "driver_release ${build_number}"

mkdir -p "${bundle_dir}"
find "${rpm_root}/RPMS/x86_64" -maxdepth 1 -type f -name '*.rpm' \
  -exec cp -f -- '{}' "${bundle_dir}/" ';'
