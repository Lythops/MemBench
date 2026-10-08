#!/usr/bin/env bash
# Run the ROM in Ares for N seconds and keep its MB, lines. Ares reports 0 for
# the RDP counters and its CPU timings are not the console's: this is a crash
# and format check, never a measurement.
#   bash tools/ares.sh [secs] [rom]     -> raw/ares-<date>.log, MB lines on stdout
set -u
SECS="${1:-45}"
ROM="${2:-/c/Nintendo64/MemBench/membench.z64}"
ARES_DIR="${ARES_DIR:-/c/Nintendo64/tools/ares-64}"
ARES="$ARES_DIR/ares.exe"
OUT="/c/Nintendo64/MemBench/raw/ares-$(date +%Y-%m-%d-%H%M).log"
[ -x "$ARES" ] || { echo "ares not found at $ARES" >&2; exit 1; }
[ -f "$ROM" ] || { echo "no ROM $ROM" >&2; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
sed 's/^  HomebrewMode: false/  HomebrewMode: true/' "$ARES_DIR/settings.bml" > "$WORK/settings.bml" 2>/dev/null \
    || printf 'General\n  HomebrewMode: true\n' > "$WORK/settings.bml"
"$ARES" --no-file-prompt --settings-file "$(cygpath -w "$WORK/settings.bml")" \
        --system "Nintendo 64" "$(cygpath -w "$ROM")" > "$WORK/ares.log" 2>&1 &
ARES_PID=$!
ARES_WPID=$(ps -W | awk -v p="$ARES_PID" '$1==p {print $4}')
sleep "$SECS"
kill "$ARES_PID" > /dev/null 2>&1
[ -n "$ARES_WPID" ] && taskkill //F //PID "$ARES_WPID" > /dev/null 2>&1
cp "$WORK/ares.log" "$OUT"
grep '^MB,' "$OUT"
echo "full log: $OUT" >&2
