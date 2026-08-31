#!/bin/bash
set -euo pipefail

test_dir=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(unset CDPATH; cd -- "$test_dir/../.." && pwd)
builder="$repo_root/scripts/build-macos-release-prefix.sh"
lock_file="$repo_root/dist/macos/release-dependencies.lock"
test_root=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-prefix-diagnostics-test.XXXXXX")

cleanup() {
    /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

# shellcheck disable=SC1090
source "$lock_file"

expect_rejection_without_absolute_paths() {
    local expected_category=$2
    local log_path="$test_root/$1.log"
    shift 2

    if "$@" > "$log_path" 2>&1; then
        echo "expected dependency-prefix builder rejection" >&2
        exit 1
    fi
    /usr/bin/grep -Fx "$expected_category" "$log_path" >/dev/null
    if /usr/bin/grep -F "$test_root" "$log_path" >/dev/null \
        || /usr/bin/grep -F "$repo_root" "$log_path" >/dev/null; then
        echo "dependency-prefix diagnostic exposed an absolute path" >&2
        exit 1
    fi
}

make_builder_sandbox() {
    local sandbox="$test_root/$1"

    /bin/mkdir -p "$sandbox/scripts"
    /bin/cp "$builder" "$sandbox/scripts/build-macos-release-prefix.sh"
    printf '%s\n' "$sandbox"
}

missing_lock_sandbox=$(make_builder_sandbox missing-lock)
expect_rejection_without_absolute_paths missing-lock \
    "required source component is missing: dist/macos/release-dependencies.lock" \
    /bin/bash "$missing_lock_sandbox/scripts/build-macos-release-prefix.sh" \
        --prefix "$test_root/missing-lock-prefix" \
        --work-dir "$test_root/missing-lock-work"

missing_verifier_sandbox=$(make_builder_sandbox missing-verifier)
/bin/mkdir -p "$missing_verifier_sandbox/dist/macos"
/bin/cp "$lock_file" \
    "$missing_verifier_sandbox/dist/macos/release-dependencies.lock"
expect_rejection_without_absolute_paths missing-verifier \
    "required source component is missing: scripts/verify-macos-deployment-target.sh" \
    /bin/bash "$missing_verifier_sandbox/scripts/build-macos-release-prefix.sh" \
        --prefix "$test_root/missing-verifier-prefix" \
        --work-dir "$test_root/missing-verifier-work"

missing_closure_sandbox=$(make_builder_sandbox missing-closure)
/bin/mkdir -p "$missing_closure_sandbox/dist/macos"
/bin/cp "$lock_file" \
    "$missing_closure_sandbox/dist/macos/release-dependencies.lock"
/bin/cp "$repo_root/scripts/verify-macos-deployment-target.sh" \
    "$missing_closure_sandbox/scripts/verify-macos-deployment-target.sh"
expect_rejection_without_absolute_paths missing-closure \
    "required source component is missing: scripts/macos-macho-closure.py" \
    /bin/bash "$missing_closure_sandbox/scripts/build-macos-release-prefix.sh" \
        --prefix "$test_root/missing-closure-prefix" \
        --work-dir "$test_root/missing-closure-work"

missing_patch_sandbox=$(make_builder_sandbox missing-patch)
/bin/mkdir -p "$missing_patch_sandbox/dist/macos"
/bin/cp "$lock_file" \
    "$missing_patch_sandbox/dist/macos/release-dependencies.lock"
/bin/cp "$repo_root/scripts/verify-macos-deployment-target.sh" \
    "$missing_patch_sandbox/scripts/verify-macos-deployment-target.sh"
/bin/cp "$repo_root/scripts/macos-macho-closure.py" \
    "$missing_patch_sandbox/scripts/macos-macho-closure.py"
/bin/cp "$repo_root/scripts/scan-protected-metadata.pl" \
    "$missing_patch_sandbox/scripts/scan-protected-metadata.pl"
expect_rejection_without_absolute_paths missing-patch \
    "required source component is missing: $QTBASE_PATCH" \
    /bin/bash "$missing_patch_sandbox/scripts/build-macos-release-prefix.sh" \
        --prefix "$test_root/missing-patch-prefix" \
        --work-dir "$test_root/missing-patch-work"

supplied_prefix="$test_root/supplied-prefix"
/bin/mkdir -p "$supplied_prefix"
printf 'unexpected cache content\n' > "$supplied_prefix/unexpected"
expect_rejection_without_absolute_paths prefix-mismatch \
    "release prefix cache does not match the locked recipe" \
    /bin/bash "$builder" \
        --prefix "$supplied_prefix" \
        --work-dir "$test_root/prefix-mismatch-work"

supplied_work_dir="$test_root/supplied-work"
/bin/mkdir -p "$supplied_work_dir"
printf 'unexpected work content\n' > "$supplied_work_dir/unexpected"
expect_rejection_without_absolute_paths work-mismatch \
    "release prefix work directory must be absent or empty" \
    /bin/bash "$builder" \
        --prefix "$test_root/work-mismatch-prefix" \
        --work-dir "$supplied_work_dir"

if /usr/bin/grep -E \
    "echo .*\\\$(lock_file|verify_script|closure_helper|patch_file|prefix|work_dir|archive_path)" \
    "$builder" >/dev/null; then
    echo "dependency-prefix error diagnostic interpolates an absolute path" >&2
    exit 1
fi
/usr/bin/grep -F \
    "echo \"cached source checksum mismatch: \$archive_name\"" \
    "$builder" >/dev/null
/usr/bin/grep -F \
    'echo "pinned macdeployqt was not produced: bin/macdeployqt"' \
    "$builder" >/dev/null

printf 'dependency-prefix diagnostic tests passed\n'
