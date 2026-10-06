#!/usr/bin/env bash
set -euo pipefail

# Purpose: Compile and run Linux native tests against Flutter's cached embedder.
# Inputs: FLUTTER_ROOT optionally selects the SDK; CXX optionally selects the
# compiler. GTK3/GIO/GLib are resolved with pkg-config. If all EDS development
# packages are present, the script also compiles and runs the EDS-enabled path.

package_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
flutter_root="${FLUTTER_ROOT:-}"
if [[ -z "${flutter_root}" ]]; then
  flutter_executable="$(command -v flutter)"
  flutter_root="$(cd "$(dirname "${flutter_executable}")/.." && pwd)"
fi

case "$(uname -m)" in
  x86_64|amd64) engine_platform="linux-x64" ;;
  arm64|aarch64) engine_platform="linux-arm64" ;;
  *)
    echo "Unsupported Linux architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

engine_root="${flutter_root}/bin/cache/artifacts/engine/${engine_platform}"
flutter_library="${engine_root}/libflutter_linux_gtk.so"
flutter_headers="${engine_root}"
if [[ ! -f "${flutter_library}" || ! -d "${flutter_headers}/flutter_linux" ]]; then
  echo "Flutter Linux engine artifacts are missing under ${engine_root}." >&2
  echo "Run 'flutter precache --linux' first." >&2
  exit 1
fi

build_root="$(mktemp -d "${TMPDIR:-/tmp}/simple-query-linux-native.XXXXXX")"
trap 'rm -rf "${build_root}"' EXIT

cxx="${CXX:-clang++}"
read -r -a gtk_flags <<<"$(pkg-config --cflags --libs gtk+-3.0 gio-2.0 glib-2.0)"
common_flags=(
  -std=c++17
  -Wall
  -Wextra
  -Werror
  -Wno-unused-parameter
  -pthread
  -I"${flutter_headers}"
  -I"${package_root}/linux"
  "${package_root}/linux/test/simple_query_linux_plugin_test.cc"
  -L"${engine_root}"
  -Wl,-rpath,"${engine_root}"
  -lflutter_linux_gtk
  "${gtk_flags[@]}"
)

# Purpose: Build and execute one native configuration.
# Parameters: name selects the output; remaining arguments add compiler/linker
# flags. Returns: zero only when compilation and every native test pass.
run_variant() {
  local name="$1"
  shift
  local binary="${build_root}/${name}"
  "${cxx}" "${common_flags[@]}" "$@" -o "${binary}"
  "${binary}"
}

run_variant no_eds

if pkg-config --exists libebook-1.2 libecal-2.0 libedataserver-1.2; then
  read -r -a eds_flags <<<"$(pkg-config --cflags --libs \
    libebook-1.2 libecal-2.0 libedataserver-1.2)"
  run_variant with_eds -DHAS_LIBEBOOK=1 -DHAS_LIBECAL=1 "${eds_flags[@]}"
else
  echo "EDS development packages not found; skipped EDS-enabled native variant."
fi
