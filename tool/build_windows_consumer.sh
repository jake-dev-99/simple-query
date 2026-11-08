#!/usr/bin/env bash
set -euo pipefail

# Purpose: Prove the current Windows plugin compiles through Flutter's generated
# consumer runner, registrar, and CMake integration without editing the checkout,
# then build and run the plugin's native tests in that consumer.
# Inputs: FLUTTER_ROOT optionally selects the SDK; otherwise flutter comes from
# PATH. Returns: zero only after a real Windows debug consumer build succeeds
# and every native test passes.
# Throws: exits nonzero on fixture creation, dependency, compiler, or native
# test failure.

case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*) ;;
  *)
    echo "The Flutter Windows consumer fixture must run on Windows." >&2
    exit 1
    ;;
esac

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# FLUTTER_ROOT arrives as a Windows path; cygpath gives Git Bash a POSIX one.
flutter_executable="flutter"
if [[ -n "${FLUTTER_ROOT:-}" ]]; then
  flutter_executable="$(cygpath -u "${FLUTTER_ROOT}")/bin/flutter"
fi
fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/simple-query-windows-consumer.XXXXXX")"
trap 'rm -rf "${fixture_root}"' EXIT

# Purpose: Give pub a native Windows path for a Git Bash path.
# @param path is the POSIX-style path Git Bash reports.
# @returns The same path as a single-quoted YAML scalar with forward slashes.
# @throws Nothing; cygpath and printf failures stop the script.
windows_yaml_path() {
  local native
  native="$(cygpath -m "$1")"
  local escaped="${native//\'/\'\'}"
  printf "'%s'" "${escaped}"
}

windows_package_path="$(windows_yaml_path "${repository_root}/packages/simple_query_windows")"
interface_package_path="$(windows_yaml_path "${repository_root}/packages/simple_query_platform_interface")"
shared_package_path="$(windows_yaml_path "${repository_root}/packages/simple_query_shared")"

"${flutter_executable}" create --platforms=windows \
  --project-name=simple_query_windows_consumer --no-pub "$(cygpath -m "${fixture_root}")"

(
  cd "${fixture_root}"
  "${flutter_executable}" pub add \
    "simple_query_windows:{path: ${windows_package_path}}" \
    "override:simple_query_platform_interface:{path: ${interface_package_path}}" \
    "override:simple_query_shared:{path: ${shared_package_path}}"
  # The plugin builds its GoogleTest runner only for a host app that asks.
  # The flag must precede the include that adds the plugin's CMake project.
  # No end anchor: the generated file may use CRLF line endings.
  sed -i 's|^include(flutter/generated_plugins\.cmake)|set(include_simple_query_windows_tests TRUE)\n&|' \
    windows/CMakeLists.txt
  if ! grep -Fxq 'set(include_simple_query_windows_tests TRUE)' windows/CMakeLists.txt; then
    echo "Could not enable the Simple Query Windows native tests in the fixture." >&2
    exit 1
  fi

  "${flutter_executable}" build windows --debug
  if ! grep -Fq 'SimpleQueryWindowsPluginCApiRegisterWithRegistrar' \
      windows/flutter/generated_plugin_registrant.cc; then
    echo "Flutter did not register the local Simple Query Windows plugin." >&2
    exit 1
  fi

  test_runner="$(find build/windows -type f -name 'simple_query_windows_test.exe' -print -quit)"
  if [[ -z "${test_runner}" ]]; then
    echo "The build did not produce the Simple Query Windows native test runner." >&2
    exit 1
  fi
  # An inherited filter or shard setting could select no tests, and
  # GoogleTest exits 0 for an empty run, so clear them and fail a run that
  # links no tests.
  env -u GTEST_FILTER -u GTEST_TOTAL_SHARDS -u GTEST_SHARD_INDEX \
    "${test_runner}" --gtest_filter='*' --gtest_fail_if_no_test_linked
)
