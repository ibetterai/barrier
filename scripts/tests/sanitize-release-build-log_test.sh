#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
sanitizer="$repo_root/scripts/sanitize-release-build-log.py"
emitter="$repo_root/scripts/emit-sanitized-release-diagnostics.sh"
metadata_scanner="$repo_root/scripts/scan-protected-metadata.pl"
release_workflow="$repo_root/.github/workflows/release-macos-arm64.yml"
test_root=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-release-log-test.XXXXXX")

cleanup() {
    /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

source_root=/opt/barrier-diagnostic/source-root
build_root=/opt/barrier-diagnostic/build-root
prefix_root=/opt/barrier-diagnostic/prefix-root
runner_temp=/opt/barrier-diagnostic/runner-temp
root_path="/""root""/build-cache/private.cpp"
var_temp_path="/""var""/tmp/build-cache/private.cpp"
unknown_path="/""opt""/unlisted/build/private.cpp"
windows_separator=$'\\'
windows_path="C:${windows_separator}Users${windows_separator}runner${windows_separator}build${windows_separator}private.cpp"
private_ipv4="192.168"".44.18"
link_local_ipv4="169.254"".23.7"
private_ipv6="fd12""::44"
link_local_ipv6="fe80""::beef"
synthetic_url="https""://example.invalid/build?trace=1"
credential_name="GH_""TOKEN"
authorization_name="Authori""zation"
api_key_name="API_""KEY"
synthetic_value="synthetic""-credential-value"
short_synthetic_value="short""-value"
spaced_home_path="/""Users""/synthetic user/private source.cpp"
pem_begin="-----BE""GIN PRIVATE KEY-----"
pem_body="c3ludGhldGljLWtleS1ib2R5LXZhbHVl"
pem_end="-----E""ND PRIVATE KEY-----"
opaque_value="abcdefghijklmnopqrstuvwxyz""ABCDEFGH1234567890"
log_path="$test_root/build.log"
output_path="$test_root/output.log"

{
    printf '%s\n' \
        "$source_root/src/example.cpp:42:7: error: expected expression" \
        "$build_root/generated.cpp:8:2: fatal error: header missing" \
        "CMake Error at $source_root/CMakeLists.txt:81 (message):" \
        'Undefined symbols for architecture arm64:' \
        "$source_root/src/credential.cpp:50:2: error: $credential_name=$synthetic_value" \
        "$source_root/src/auth.cpp:51:2: error: $authorization_name: Bearer $synthetic_value" \
        "$source_root/src/api.cpp:51:3: error: $api_key_name=$short_synthetic_value" \
        "$source_root/src/spaced.cpp:51:4: error: from $spaced_home_path" \
        "$spaced_home_path:51:5: error: spaced location" \
        "$source_root/src/path.cpp:52:2: error: from $root_path $var_temp_path $unknown_path $windows_path" \
        "$source_root/src/network.cpp:53:2: error: contacted $private_ipv4 $link_local_ipv4 $private_ipv6 $link_local_ipv6 $synthetic_url" \
        "$source_root/src/opaque.cpp:54:2: error: value $opaque_value" \
        "$pem_begin" \
        "$source_root/src/key.cpp:55:2: error: $pem_body" \
        "$pem_end" \
        "$credential_name=$synthetic_value" \
        "$authorization_name: Bearer $synthetic_value" \
        $'\033[31mclang: error: colored failure\033[0m'
    for sequence in $(/usr/bin/seq 1 121); do
        printf 'ordinary build tail line %s\n' "$sequence"
    done
    printf '%s' "$source_root/src/oversized.cpp:88:2: error: "
    /usr/bin/perl -e 'print "word " x 40000'
    printf '\n'
} > "$log_path"

/usr/bin/python3 "$sanitizer" "$log_path" \
    --source-root "$source_root" \
    --build-root "$build_root" \
    --prefix-root "$prefix_root" \
    --temp-root "$runner_temp" \
    --max-lines 20 > "$output_path"

/usr/bin/grep -F \
    '| compiler error at barrier-source/src/example.cpp:42:7' \
    "$output_path" >/dev/null
/usr/bin/grep -F \
    '| compiler fatal error at barrier-build/generated.cpp:8:2' \
    "$output_path" >/dev/null
/usr/bin/grep -F \
    '| cmake error at barrier-source/CMakeLists.txt:81' \
    "$output_path" >/dev/null
/usr/bin/grep -F \
    '| linker error: undefined symbols for architecture arm64' \
    "$output_path" >/dev/null
/usr/bin/grep -F '<external-path>' "$output_path" >/dev/null
/usr/bin/grep -F 'compiler driver error: command failed' "$output_path" >/dev/null
/usr/bin/grep -F \
    '| compiler error at barrier-source/src/oversized.cpp:88:2' \
    "$output_path" >/dev/null

for protected_value in \
    "$root_path" "$var_temp_path" "$unknown_path" "$windows_path" \
    "$private_ipv4" "$link_local_ipv4" "$private_ipv6" "$link_local_ipv6" \
    "$synthetic_url" "$credential_name" "$authorization_name" \
    "$api_key_name" "$synthetic_value" "$short_synthetic_value" \
    "$spaced_home_path" "$pem_begin" "$pem_body" "$pem_end" "$opaque_value" \
    'expected expression' 'header missing' 'colored failure'; do
    if /usr/bin/grep -F -- "$protected_value" "$output_path" >/dev/null; then
        echo 'sanitized build diagnostic exposed protected input' >&2
        exit 1
    fi
done

if LC_ALL=C /usr/bin/grep $'\033' "$output_path" >/dev/null; then
    echo 'sanitized build diagnostic retained terminal control characters' >&2
    exit 1
fi
if /usr/bin/awk 'length($0) > 768 { exit 1 }' "$output_path"; then
    :
else
    echo 'sanitized build diagnostic exceeded its per-line bound' >&2
    exit 1
fi
if [ "$(/usr/bin/wc -c < "$output_path")" -gt 49152 ]; then
    echo 'sanitized build diagnostic exceeded its total byte bound' >&2
    exit 1
fi
/usr/bin/perl "$metadata_scanner" --source "$output_path" >/dev/null

hosted_runner_temp="$test_root/""Users""/runner/work/_temp"
/bin/mkdir -p "$hosted_runner_temp"
emitted_output="$test_root/emitted.log"
"$emitter" \
    "$log_path" \
    "$source_root" \
    "$build_root" \
    "$prefix_root" \
    "$hosted_runner_temp" > "$emitted_output"
/usr/bin/grep -F \
    '| compiler error at barrier-source/src/example.cpp:42:7' \
    "$emitted_output" >/dev/null
/usr/bin/perl "$metadata_scanner" --source "$emitted_output" >/dev/null

emitter_sandbox="$test_root/emitter-sandbox"
/bin/mkdir -p "$emitter_sandbox"
/bin/cp "$emitter" "$emitter_sandbox/emit-sanitized-release-diagnostics.sh"
/bin/cp "$metadata_scanner" "$emitter_sandbox/scan-protected-metadata.pl"
printf '%s\n' \
    '#!/usr/bin/env python3' \
    'import os' \
    'print(os.environ["SYNTHETIC_DIAGNOSTIC"])' \
    > "$emitter_sandbox/sanitize-release-build-log.py"
hostile_candidate="compiler error at barrier-source/src/example.cpp:1:1: $root_path"
hostile_emission="$test_root/hostile-emission.log"
if SYNTHETIC_DIAGNOSTIC="$hostile_candidate" \
    "$emitter_sandbox/emit-sanitized-release-diagnostics.sh" \
        "$log_path" \
        "$source_root" \
        "$build_root" \
        "$prefix_root" \
        "$hosted_runner_temp" > "$hostile_emission"; then
    echo 'diagnostic emitter accepted hostile generated content' >&2
    exit 1
fi
if [ -s "$hostile_emission" ]; then
    echo 'diagnostic emitter exposed hostile generated content' >&2
    exit 1
fi

unstructured_log="$test_root/unstructured.log"
unstructured_output="$test_root/unstructured.stdout"
printf '%s\n' \
    "$credential_name=$synthetic_value" \
    "$pem_begin" "$pem_body" "$pem_end" \
    > "$unstructured_log"
if /usr/bin/python3 "$sanitizer" "$unstructured_log" \
    > "$unstructured_output" 2> "$test_root/unstructured.stderr"; then
    echo 'sanitizer accepted an unstructured hostile log' >&2
    exit 1
fi
if [ -s "$unstructured_output" ]; then
    echo 'sanitizer emitted an unvalidated partial candidate' >&2
    exit 1
fi
/usr/bin/grep -F 'no structured diagnostics' \
    "$test_root/unstructured.stderr" >/dev/null

missing_path="$root_path/missing-build.log"
if /usr/bin/python3 "$sanitizer" "$missing_path" \
    > "$test_root/missing.stdout" 2> "$test_root/missing.stderr"; then
    echo 'sanitizer accepted a missing log' >&2
    exit 1
fi
if [ -s "$test_root/missing.stdout" ] \
    || /usr/bin/grep -F "$missing_path" "$test_root/missing.stderr" >/dev/null; then
    echo 'missing-log diagnostic exposed its input path' >&2
    exit 1
fi
/usr/bin/grep -F 'log unavailable' "$test_root/missing.stderr" >/dev/null

/usr/bin/grep -F 'scripts/emit-sanitized-release-diagnostics.sh' \
    "$release_workflow" >/dev/null
# This is intentionally a literal helper source fragment.
# shellcheck disable=SC2016
/usr/bin/grep -F -- '--source "$diagnostic_output"' "$emitter" >/dev/null

printf 'release build log sanitizer tests passed\n'
