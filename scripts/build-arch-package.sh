#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"
arch_package_dir="${project_root}/packaging/arch"
bundle_dir="${project_root}/dist/arch"

mkdir -p "${bundle_dir}"
PKGDEST="${bundle_dir}" makepkg \
  --dir "${arch_package_dir}" \
  --force \
  --clean \
  --cleanbuild \
  --nosign
