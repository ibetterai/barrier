#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
sanitizer="$repo_root/scripts/sanitize-release-build-log.py"
test_root=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-release-log-test.XXXXXX")

cleanup() {
    /bin/rm -rf "$test_root"
}
trap cleanup EXIT

source_root=/opt/barrier-diagnostic/source-root
build_root=/opt/barrier-diagnostic/build-root
prefix_root=/opt/barrier-diagnostic/prefix-root
runner_temp=/opt/barrier-diagnostic/runner-temp
private_home="/""Users""/diagnostic-user/private-source"
private_address="192.168"".44.18"
synthetic_token="synthetic""-token-value"
log_path="$test_root/build.log"
output_path="$test_root/output.log"

printf '%s\n' \
    "$source_root/src/example.cpp:42:7: error: expected expression" \
    "$build_root/generated.cpp:8:2: fatal error: header missing" \
    "$prefix_root/lib/example.dylib: undefined symbol" \
    "$runner_temp/intermediate.o: failed" \
    "$private_home/file.cpp:4:1: error: private path" \
    "connection to $private_address failed" \
    "token=$synthetic_token" \
    $'\033[31mclang: error: colored failure\033[0m' \
    > "$log_path"

/usr/bin/python3 "$sanitizer" "$log_path" \
    --source-root "$source_root" \
    --build-root "$build_root" \
    --prefix-root "$prefix_root" \
    --temp-root "$runner_temp" \
    --max-lines 20 > "$output_path"

/usr/bin/grep -F 'barrier-source/src/example.cpp:42:7: error:' "$output_path" >/dev/null
/usr/bin/grep -F 'barrier-build/generated.cpp:8:2: fatal error:' "$output_path" >/dev/null
/usr/bin/grep -F 'barrier-prefix/lib/example.dylib: undefined symbol' "$output_path" >/dev/null
/usr/bin/grep -F 'runner-temp/intermediate.o: failed' "$output_path" >/dev/null
/usr/bin/grep -F 'token=<redacted>' "$output_path" >/dev/null
/usr/bin/grep -F 'clang: error: colored failure' "$output_path" >/dev/null

for protected_value in \
    "$source_root" "$build_root" "$prefix_root" "$runner_temp" \
    "$private_home" "$private_address" "$synthetic_token"; do
    if /usr/bin/grep -F "$protected_value" "$output_path" >/dev/null; then
        echo 'sanitized build diagnostic exposed protected input' >&2
        exit 1
    fi
done

if LC_ALL=C /usr/bin/grep $'\033' "$output_path" >/dev/null; then
    echo 'sanitized build diagnostic retained terminal control characters' >&2
    exit 1
fi

/usr/bin/perl "$repo_root/scripts/scan-protected-metadata.pl" \
    --source-path "$output_path" >/dev/null

missing_path="$private_home/missing-build.log"
if /usr/bin/python3 "$sanitizer" "$missing_path" \
    >"$test_root/missing.stdout" 2>"$test_root/missing.stderr"; then
    echo 'sanitizer accepted a missing log' >&2
    exit 1
fi
if /usr/bin/grep -F "$missing_path" "$test_root/missing.stderr" >/dev/null; then
    echo 'missing-log diagnostic exposed its input path' >&2
    exit 1
fi
/usr/bin/grep -F 'log unavailable' "$test_root/missing.stderr" >/dev/null

printf 'release build log sanitizer tests passed\n'
