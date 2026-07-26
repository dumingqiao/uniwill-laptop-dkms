#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"
version="$(<"${project_root}/VERSION")"
build_number="$(<"${project_root}/BUILD_NUMBER")"
expected_tag="${1:-}"

semver_pattern='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
build_pattern='^[1-9][0-9]*$'

if [[ ! "${version}" =~ ${semver_pattern} ]]; then
  echo "check-version.sh: VERSION must use MAJOR.MINOR.PATCH without leading zeros" >&2
  exit 1
fi

if [[ ! "${build_number}" =~ ${build_pattern} ]]; then
  echo "check-version.sh: BUILD_NUMBER must be a positive integer" >&2
  exit 1
fi

if [[ -n "${expected_tag}" && "${expected_tag}" != "v${version}" ]]; then
  echo "check-version.sh: tag ${expected_tag} does not match VERSION v${version}" >&2
  exit 1
fi

echo "version=${version}"
echo "build_number=${build_number}"
echo "full_version=${version}+${build_number}"
