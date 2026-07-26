#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"
version="$(<"${project_root}/VERSION")"
build_number="$(<"${project_root}/BUILD_NUMBER")"
package_version="${version}-${build_number}"
bundle_dir="${project_root}/dist/deb"
staging_root="$(mktemp -d "${TMPDIR:-/tmp}/uniwill-deb.XXXXXX")"

cleanup() {
  rm -rf -- "${staging_root}"
}
trap cleanup EXIT

if [[ "$(dpkg --print-architecture)" != "amd64" ]]; then
  echo "build-deb-package.sh: x86_64/amd64 build host required" >&2
  exit 1
fi

make -B -C "${project_root}" service
make -B -C "${project_root}" test
"${project_root}/scripts/stage-package-root.sh" "${staging_root}"

install -d "${staging_root}/DEBIAN"
sed "s/@PACKAGE_VERSION@/${package_version}/g" \
  "${project_root}/packaging/deb/control.in" \
  > "${staging_root}/DEBIAN/control"
sed "s/@DRIVER_VERSION@/${version}/g" \
  "${project_root}/packaging/deb/postinst.in" \
  > "${staging_root}/DEBIAN/postinst"
sed "s/@DRIVER_VERSION@/${version}/g" \
  "${project_root}/packaging/deb/prerm.in" \
  > "${staging_root}/DEBIAN/prerm"
install -m755 "${project_root}/packaging/deb/postrm" "${staging_root}/DEBIAN/postrm"
chmod 0755 "${staging_root}/DEBIAN/postinst" "${staging_root}/DEBIAN/prerm"

mkdir -p "${bundle_dir}"
dpkg-deb --build --root-owner-group \
  "${staging_root}" \
  "${bundle_dir}/uniwill-laptop-dkms_${package_version}_amd64.deb"
