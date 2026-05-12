#!/usr/bin/env bash
# Regenerates the Pigeon glue for every platform package that has a
# pigeon.dart in its root. Idempotent — safe to run anytime.
#
# Used by:
# - Developers: run after editing any pigeon.dart.
# - CI: see .github/workflows/ci.yml `pigeon-drift` job — runs this and
#   fails the build if `git diff --exit-code` shows changes (i.e. the
#   committed generated code is out of sync with pigeon.dart).
set -euo pipefail

cd "$(dirname "$0")/.."

PACKAGES=(
  packages/simple_query_android
  packages/simple_query_ios
  packages/simple_query_macos
  packages/simple_query_linux
  packages/simple_query_windows
)

for pkg in "${PACKAGES[@]}"; do
  if [[ ! -f "$pkg/pigeon.dart" ]]; then
    echo "skip: $pkg (no pigeon.dart)"
    continue
  fi
  echo "regen: $pkg"
  (
    cd "$pkg"
    flutter pub get >/dev/null
    dart run pigeon --input pigeon.dart
    # Pigeon emits unformatted Dart. Run dart format so the committed
    # output is stable across pigeon point releases and the CI drift
    # check stays meaningful.
    if [[ -d lib/src/generated ]]; then
      dart format lib/src/generated >/dev/null
    fi
  )
done

echo "done."
