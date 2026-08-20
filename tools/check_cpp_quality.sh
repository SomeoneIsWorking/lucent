#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

build_dir=${1:-scratch/build}
format=${CLANG_FORMAT:-$(command -v clang-format || true)}
tidy=${CLANG_TIDY:-$(command -v clang-tidy || true)}

[[ -n $format ]] || { echo "REFUSING: clang-format is not installed" >&2; exit 1; }
[[ -n $tidy ]] || { echo "REFUSING: clang-tidy is not installed" >&2; exit 1; }
[[ -f $build_dir/compile_commands.json ]] || {
  echo "REFUSING: $build_dir/compile_commands.json is missing" >&2
  exit 1
}

mapfile -t formatted < <(find include/lucent src tests -type f \
  \( -name '*.h' -o -name '*.cpp' \) | sort)
"$format" --dry-run --Werror "${formatted[@]}"

mapfile -t translation_units < <(find src tests -type f -name '*.cpp' | sort)
resource_dir=$(clang++ -print-resource-dir)
"$tidy" -p "$build_dir" "${translation_units[@]}" \
  --extra-arg="-resource-dir=$resource_dir" --quiet
