#!/usr/bin/env bash
#
# Publishes simple_query packages to pub.dev in dependency order.
#
# Usage:
#   tool/publish.sh            Dry run — validates all packages, publishes nothing
#   tool/publish.sh --live     Publishes to pub.dev (with 15s pause between packages)
#   tool/publish.sh --help     Shows publish order and current versions
#
# Prerequisites:
#   - `dart pub login` (authenticated with pub.dev)
#   - All tests pass (`dart run melos run verify`)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Publish order — topological sort of internal dependency graph.
# A package is listed only after all its simple_query_* dependencies.
PACKAGES=(
  simple_query_platform_interface
  simple_query_shared
  simple_query_android
  simple_query_ios
  simple_query_macos
  simple_query_linux
  simple_query_windows
  simple_query
)

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

get_version() {
  grep '^version:' "$1/pubspec.yaml" | head -1 | awk '{print $2}'
}

print_order() {
  echo ""
  echo -e "${BOLD}Publish order (${#PACKAGES[@]} packages):${RESET}"
  echo ""
  local step=1
  for pkg in "${PACKAGES[@]}"; do
    local version
    version=$(get_version "$REPO_ROOT/packages/$pkg")
    printf "  ${CYAN}%2d.${RESET} %-40s ${YELLOW}%s${RESET}\n" "$step" "$pkg" "$version"
    step=$((step + 1))
  done
  echo ""
}

convert_path_to_version() {
  # Replaces simple_query_* path deps with ^VERSION version constraints.
  local pubspec="$1"
  local version="$2"
  local tmpfile
  tmpfile=$(mktemp)

  awk -v ver="$version" '
  /^  simple_query_/ {
    pkg_line = $0
    getline
    if ($0 ~ /path:/) {
      sub(/:$/, "", pkg_line)
      pkg_name = pkg_line
      gsub(/^  /, "", pkg_name)
      gsub(/:/, "", pkg_name)
      print "  " pkg_name ": ^" ver
      next
    } else {
      print pkg_line
      print $0
      next
    }
  }
  { print }
  ' "$pubspec" > "$tmpfile"

  mv "$tmpfile" "$pubspec"
}

validate_package() {
  # Local validation — checks structure without hitting pub.dev.
  local pkg_dir="$1"
  local pkg_name="$2"
  local version="$3"
  local errors=0

  # Required files
  for required_file in pubspec.yaml LICENSE README.md CHANGELOG.md; do
    if [[ ! -f "$pkg_dir/$required_file" ]]; then
      echo -e "    ${RED}Missing $required_file${RESET}"
      errors=$((errors + 1))
    fi
  done

  # CHANGELOG mentions current version
  if [[ -f "$pkg_dir/CHANGELOG.md" ]] && ! grep -q "## $version" "$pkg_dir/CHANGELOG.md"; then
    echo -e "    ${RED}CHANGELOG.md does not mention version $version${RESET}"
    errors=$((errors + 1))
  fi

  # No path dependencies remain after conversion
  if grep -q 'path: \.\./' "$pkg_dir/pubspec.yaml"; then
    echo -e "    ${RED}Path dependencies still present after conversion${RESET}"
    errors=$((errors + 1))
  fi

  # pubspec has description
  if ! grep -q '^description:' "$pkg_dir/pubspec.yaml"; then
    echo -e "    ${RED}Missing description in pubspec.yaml${RESET}"
    errors=$((errors + 1))
  fi

  # pubspec has homepage or repository
  if ! grep -qE '^(homepage|repository):' "$pkg_dir/pubspec.yaml"; then
    echo -e "    ${RED}Missing homepage/repository in pubspec.yaml${RESET}"
    errors=$((errors + 1))
  fi

  # lib/ directory exists
  if [[ ! -d "$pkg_dir/lib" ]]; then
    echo -e "    ${RED}Missing lib/ directory${RESET}"
    errors=$((errors + 1))
  fi

  return $errors
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

case "${1:-}" in
  --help|-h)
    echo -e "${BOLD}simple_query publish tool${RESET}"
    echo ""
    echo "Usage:"
    echo "  tool/publish.sh            Dry run (validates structure, publishes nothing)"
    echo "  tool/publish.sh --live     Publish to pub.dev"
    echo "  tool/publish.sh --help     Show this help"
    print_order
    echo "Before publishing:"
    echo "  1. dart run melos run verify   (analysis + tests)"
    echo "  2. dart pub login              (authenticate with pub.dev)"
    echo "  3. git status is clean         (no uncommitted changes)"
    echo ""
    exit 0
    ;;
  --live)
    MODE="live"
    ;;
  "")
    MODE="dry-run"
    ;;
  *)
    echo -e "${RED}Unknown option: $1${RESET}" >&2
    echo "Run 'tool/publish.sh --help' for usage." >&2
    exit 1
    ;;
esac

echo ""
echo -e "${BOLD}simple_query publish — ${MODE}${RESET}"
print_order

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------

echo -e "${BOLD}Pre-flight checks...${RESET}"

if ! command -v dart &>/dev/null; then
  echo -e "${RED}Error: 'dart' not found in PATH.${RESET}" >&2
  exit 1
fi

if ! git -C "$REPO_ROOT" diff --quiet HEAD 2>/dev/null; then
  echo -e "  ${YELLOW}Warning: uncommitted changes detected.${RESET}"
  if [[ "$MODE" == "live" ]]; then
    echo -e "  ${RED}Refusing to publish with dirty working tree. Commit or stash first.${RESET}" >&2
    exit 1
  fi
fi

for pkg in "${PACKAGES[@]}"; do
  if [[ ! -f "$REPO_ROOT/packages/$pkg/pubspec.yaml" ]]; then
    echo -e "  ${RED}Error: missing package — packages/$pkg${RESET}" >&2
    exit 1
  fi
done

EXPECTED_VERSION=$(get_version "$REPO_ROOT/packages/simple_query")
for pkg in "${PACKAGES[@]}"; do
  pkg_version=$(get_version "$REPO_ROOT/packages/$pkg")
  if [[ "$pkg_version" != "$EXPECTED_VERSION" ]]; then
    echo -e "  ${RED}Version mismatch: $pkg is $pkg_version, expected $EXPECTED_VERSION${RESET}" >&2
    exit 1
  fi
done
echo -e "  Versions: ${GREEN}$EXPECTED_VERSION${RESET} (all ${#PACKAGES[@]} packages)"
echo -e "  ${GREEN}Pre-flight passed.${RESET}"
echo ""

# ---------------------------------------------------------------------------
# Prepare workspace (convert path deps to version constraints)
# ---------------------------------------------------------------------------

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

echo -e "${BOLD}Preparing workspace...${RESET}"
cp -R "$REPO_ROOT/packages" "$WORK_DIR/packages"

CONVERTED=0
for pkg in "${PACKAGES[@]}"; do
  pubspec="$WORK_DIR/packages/$pkg/pubspec.yaml"
  if grep -q 'path: \.\./' "$pubspec"; then
    convert_path_to_version "$pubspec" "$EXPECTED_VERSION"
    CONVERTED=$((CONVERTED + 1))
  fi
done
echo -e "  Converted path deps in $CONVERTED packages."
echo ""

# ---------------------------------------------------------------------------
# Publish / validate loop
# ---------------------------------------------------------------------------

PASSED=0
FAILED=0
TOTAL=${#PACKAGES[@]}

for pkg in "${PACKAGES[@]}"; do
  pkg_dir="$WORK_DIR/packages/$pkg"
  step=$((PASSED + FAILED + 1))

  echo -e "${BOLD}[$step/$TOTAL] $pkg${RESET}"

  if [[ "$MODE" == "dry-run" ]]; then
    if validate_package "$pkg_dir" "$pkg" "$EXPECTED_VERSION"; then
      echo -e "    ${GREEN}Valid.${RESET}"
      PASSED=$((PASSED + 1))
    else
      FAILED=$((FAILED + 1))
    fi

  else
    echo -e "    Publishing to pub.dev..."
    if (cd "$pkg_dir" && dart pub publish --force) 2>&1 | sed 's/^/    /'; then
      echo -e "    ${GREEN}Published.${RESET}"
      PASSED=$((PASSED + 1))

      if [[ $step -lt $TOTAL ]]; then
        echo -e "    ${CYAN}Waiting 15s for pub.dev indexing...${RESET}"
        sleep 15
      fi
    else
      echo ""
      echo -e "    ${RED}FAILED. $PASSED package(s) published before this failure.${RESET}" >&2
      echo -e "    ${RED}Fix the issue and re-run. Already-published packages are fine.${RESET}" >&2
      exit 1
    fi
  fi

  echo ""
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo -e "${BOLD}─── Summary ───${RESET}"
echo ""

if [[ "$MODE" == "dry-run" ]]; then
  if [[ $FAILED -eq 0 ]]; then
    echo -e "${GREEN}All $PASSED packages validated.${RESET}"
    echo ""
    echo "Ready to publish. Run:"
    echo "  tool/publish.sh --live"
  else
    echo -e "${RED}$FAILED package(s) failed validation.${RESET}"
    exit 1
  fi
else
  echo -e "${GREEN}All $PASSED packages published to pub.dev.${RESET}"
fi

echo ""
