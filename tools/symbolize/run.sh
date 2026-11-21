#!/usr/bin/env bash
# Regenerate profiles/spongebob/analysis/symbols/ from your decrypted EBOOT.
# usage: tools/symbolize/run.sh [build-dir]   (default: out/crab)
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$root/out/crab}"
elf="$root/profiles/spongebob/original/ULUS10478_EBOOT.ELF"
analysis="$root/profiles/spongebob/analysis"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cmake --build "$build" --target dump_image >/dev/null
"$build/dump_image" "$elf" "$work/image.bin" "$work/sections.csv"
python3 -I "$root/tools/symbolize/symbolize.py" "$work/image.bin" "$work/sections.csv" \
    "$analysis/ULUS10478_report_functions_auto.csv" "$analysis/symbols"
