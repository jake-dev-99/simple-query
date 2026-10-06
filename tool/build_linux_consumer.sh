#!/usr/bin/env bash
set -euo pipefail

# Purpose: Prove the current Linux plugin compiles through Flutter's generated
# consumer runner, registrar, and CMake integration without editing the checkout.
# Inputs: FLUTTER_ROOT optionally selects the SDK; otherwise flutter comes from
# PATH. Returns: zero only after a real Linux debug consumer build succeeds.
# Throws: exits nonzero on fixture creation, dependency, or compiler failure.

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "The Flutter Linux consumer fixture must run on Linux." >&2
  exit 1
fi

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
flutter_executable="${FLUTTER_ROOT:+${FLUTTER_ROOT}/bin/}flutter"
fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/simple-query-linux-consumer.XXXXXX")"
trap 'rm -rf "${fixture_root}"' EXIT

# Purpose: Preserve arbitrary checkout paths inside pub's YAML descriptors.
# @param path is the path to encode without shell or YAML reinterpretation.
# @returns A single-quoted YAML scalar on standard output.
# @throws Nothing; shell substitution and printf failures stop the script.
quote_yaml_path() {
  local path="$1"
  local escaped="${path//\'/\'\'}"
  printf "'%s'" "${escaped}"
}

linux_package_path="$(quote_yaml_path "${repository_root}/packages/simple_query_linux")"
interface_package_path="$(quote_yaml_path "${repository_root}/packages/simple_query_platform_interface")"
shared_package_path="$(quote_yaml_path "${repository_root}/packages/simple_query_shared")"

"${flutter_executable}" create --platforms=linux \
  --project-name=simple_query_linux_consumer --no-pub "${fixture_root}"

(
  cd "${fixture_root}"
  "${flutter_executable}" pub add \
    "simple_query_linux:{path: ${linux_package_path}}" \
    "override:simple_query_platform_interface:{path: ${interface_package_path}}" \
    "override:simple_query_shared:{path: ${shared_package_path}}"
  "${flutter_executable}" build linux --debug
  if ! grep -Fq 'simple_query_linux_plugin_register_with_registrar' \
      linux/flutter/generated_plugin_registrant.cc; then
    echo "Flutter did not register the local Simple Query Linux plugin." >&2
    exit 1
  fi
)
