#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 5 ]; then
    exit 2
fi

build_log=$1
source_root=$2
build_root=$3
prefix_root=$4
runner_temp=$5

if ! script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd); then
    exit 2
fi
sanitizer="$script_dir/sanitize-release-build-log.py"
metadata_scanner="$script_dir/scan-protected-metadata.pl"
if [ ! -f "$sanitizer" ] || [ ! -f "$metadata_scanner" ] \
    || [ ! -d "$runner_temp" ]; then
    exit 2
fi

diagnostic_output=
cleanup() {
    if [ -n "$diagnostic_output" ]; then
        /bin/rm -f "$diagnostic_output"
    fi
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

if ! diagnostic_output=$(/usr/bin/mktemp \
    "$runner_temp/barrier-release-diagnostics.XXXXXX" 2>/dev/null); then
    exit 2
fi

if ! /usr/bin/python3 "$sanitizer" \
    "$build_log" \
    --source-root "$source_root" \
    --build-root "$build_root" \
    --prefix-root "$prefix_root" \
    --temp-root "$runner_temp" \
    >"$diagnostic_output" 2>/dev/null; then
    exit 1
fi
if ! /usr/bin/perl "$metadata_scanner" \
    --source "$diagnostic_output" >/dev/null 2>&1; then
    exit 1
fi

/bin/cat "$diagnostic_output"
