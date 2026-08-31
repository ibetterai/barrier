#!/bin/bash

set -euo pipefail

test_dir=$(cd "$(dirname "$0")" && pwd)
verifier=${1:-$test_dir/../verify-macos-deployment-target.sh}
closure_helper="$test_dir/../macos-macho-closure.py"
python3_bin=$(/usr/bin/xcrun --find python3)
test_root=$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/barrier-macos-verifier-test.XXXXXX")
iconv_bin=$(command -v iconv) || {
    printf 'macOS verifier test requires iconv\n' >&2
    exit 1
}

cleanup() {
    /bin/chmod -R u+rwx "$test_root" 2>/dev/null || true
    /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

make_app() {
    local app_name=$1
    local minimum_version=$2
    local app_path="$test_root/$app_name.app"
    /bin/mkdir -p "$app_path/Contents/MacOS" "$app_path/Contents/Resources"
    /usr/bin/plutil -create xml1 "$app_path/Contents/Info.plist"
    /usr/bin/plutil -insert CFBundleExecutable -string barrier \
        "$app_path/Contents/Info.plist"
    /usr/bin/plutil -insert LSMinimumSystemVersion -string "$minimum_version" \
        "$app_path/Contents/Info.plist"
    printf '%s\n' "$app_path"
}

compile_executable() {
    local output=$1
    local target_triple=$2
    local source=$3
    printf '%s\n' "$source" | /usr/bin/xcrun clang -x c - \
        -target "$target_triple" -o "$output"
}

expect_failure() {
    local test_name=$1
    local expected_message=$2
    local log_path
    shift 2
    log_path="$test_root/$test_name.log"

    if "$@" > "$log_path" 2>&1; then
        printf 'expected verifier failure: %s\n' "$test_name" >&2
        exit 1
    fi

    /usr/bin/grep -F "$expected_message" "$log_path" >/dev/null
}

good_app=$(make_app good 11.0)
compile_executable "$good_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'int main(void) { return 0; }'
/bin/ln -s ../MacOS/barrier "$good_app/Contents/Resources/barrier-link"
good_manifest="$good_app/Contents/Resources/macho-manifest.txt"
"$verifier" --app "$good_app" --target 11.0 --arch arm64 \
    --manifest "$good_manifest" > "$test_root/good.log"
/usr/bin/grep -F $'format\tbarrier-macho-manifest-v2' \
    "$good_manifest" >/dev/null
/usr/bin/grep -F $'declared_minimum_macos\t11.0' "$good_manifest" >/dev/null
/usr/bin/grep -F $'required_architecture\tarm64' "$good_manifest" >/dev/null
/usr/bin/grep -F $'file\tMacOS/barrier\tarch=arm64\tminos=11.0' "$good_manifest" >/dev/null
if /usr/bin/grep -F "$test_root" "$good_manifest" >/dev/null; then
    printf 'manifest contains a local build path\n' >&2
    exit 1
fi

missing_minimum_app=$(make_app missing-minimum 11.0)
/usr/bin/plutil -remove LSMinimumSystemVersion \
    "$missing_minimum_app/Contents/Info.plist"
expect_failure missing-minimum 'unable to read LSMinimumSystemVersion' \
    "$verifier" --app "$missing_minimum_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$test_root" "$test_root/missing-minimum.log" >/dev/null; then
    printf 'verifier exposed the malformed plist path\n' >&2
    exit 1
fi

missing_executable_app=$(make_app missing-executable 11.0)
/usr/bin/plutil -remove CFBundleExecutable \
    "$missing_executable_app/Contents/Info.plist"
expect_failure missing-executable 'unable to read CFBundleExecutable' \
    "$verifier" --app "$missing_executable_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$test_root" "$test_root/missing-executable.log" >/dev/null; then
    printf 'verifier exposed the malformed plist path\n' >&2
    exit 1
fi

invalid_target_value="/""Users""/example/private-build"
invalid_target_root="$test_root/invalid-target-root"
/bin/mkdir -p "$invalid_target_root"
expect_failure invalid-target 'invalid macOS target' \
    "$verifier" --root "$invalid_target_root" \
        --target "$invalid_target_value" --arch arm64
if /usr/bin/grep -F "$invalid_target_value" \
    "$test_root/invalid-target.log" >/dev/null; then
    printf 'verifier exposed an invalid target value\n' >&2
    exit 1
fi

unsafe_plist_target_app=$(make_app unsafe-plist-target 11.0)
/usr/bin/plutil -replace LSMinimumSystemVersion -string "$invalid_target_value" \
    "$unsafe_plist_target_app/Contents/Info.plist"
expect_failure unsafe-plist-target \
    'requested target differs from LSMinimumSystemVersion' \
    "$verifier" --app "$unsafe_plist_target_app" \
        --target 11.0 --arch arm64
if /usr/bin/grep -F "$invalid_target_value" \
    "$test_root/unsafe-plist-target.log" >/dev/null; then
    printf 'verifier exposed an invalid plist target value\n' >&2
    exit 1
fi

whitespace_app=$(make_app whitespace 11.0)
whitespace_library='libSpace Fixture.dylib'
/bin/mkdir -p "$whitespace_app/Contents/Frameworks"
printf '%s\n' 'int whitespace_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        "-Wl,-install_name,@rpath/$whitespace_library" \
        -o "$whitespace_app/Contents/Frameworks/$whitespace_library"
printf '%s\n' \
    'int whitespace_fixture(void); int main(void) { return whitespace_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$whitespace_app/Contents/Frameworks/$whitespace_library" \
        -Wl,-rpath,@executable_path/../Frameworks \
        -o "$whitespace_app/Contents/MacOS/barrier"
whitespace_manifest="$whitespace_app/Contents/Resources/macho-manifest.txt"
"$verifier" --app "$whitespace_app" --target 11.0 --arch arm64 \
    --manifest "$whitespace_manifest" > "$test_root/whitespace.log"
/usr/bin/grep -F \
    $'\tload-dylib\t@rpath/libSpace Fixture.dylib\tbundle\tFrameworks/libSpace Fixture.dylib' \
    "$whitespace_manifest" >/dev/null

weak_fixture="$test_root/libWeakMissing.dylib"
printf '%s\n' 'int weak_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libWeakMissing.dylib \
        -o "$weak_fixture"
weak_app=$(make_app weak-missing 11.0)
/bin/mkdir -p "$weak_app/Contents/Frameworks"
printf '%s\n' \
    'int weak_fixture(void) __attribute__((weak_import)); int main(void) { return weak_fixture ? weak_fixture() : 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -Wl,-weak_library,"$weak_fixture" \
        -Wl,-rpath,@executable_path/../Frameworks \
        -o "$weak_app/Contents/MacOS/barrier"
weak_manifest="$weak_app/Contents/Resources/macho-manifest.txt"
"$verifier" --app "$weak_app" --target 11.0 --arch arm64 \
    --manifest "$weak_manifest" > "$test_root/weak-missing.log"
/usr/bin/grep -F $'\t@rpath/libWeakMissing.dylib\tweak-missing\t-' \
    "$weak_manifest" >/dev/null
printf 'not a Mach-O image\n' \
    > "$weak_app/Contents/Frameworks/libWeakMissing.dylib"
"$verifier" --app "$weak_app" --target 11.0 --arch arm64 \
    > "$test_root/weak-present-nonloadable.log"

helper_chain_app=$(make_app helper-chain 11.0)
/bin/mkdir -p "$helper_chain_app/Contents/Frameworks"
printf '%s\n' 'int helper_only_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libHelperOnly.dylib \
        -o "$helper_chain_app/Contents/Frameworks/libHelperOnly.dylib"
printf '%s\n' 'int main(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -Wl,-rpath,@executable_path/../Frameworks \
        -o "$helper_chain_app/Contents/MacOS/barrier"
printf '%s\n' \
    'int helper_only_fixture(void); int main(void) { return helper_only_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$helper_chain_app/Contents/Frameworks/libHelperOnly.dylib" \
        -o "$helper_chain_app/Contents/MacOS/helper-tool"
expect_failure helper-chain \
    'unresolved bundled dependency: MacOS/helper-tool' \
    "$verifier" --app "$helper_chain_app" --target 11.0 --arch arm64

runpath_order_app=$(make_app runpath-order 11.0)
/bin/mkdir -p "$runpath_order_app/Contents/Bad" \
    "$runpath_order_app/Contents/Good"
printf '%s\n' 'int runpath_order_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libRunpathOrder.dylib \
        -o "$runpath_order_app/Contents/Good/libRunpathOrder.dylib"
printf '%s\n' \
    'int runpath_order_fixture(void); int main(void) { return runpath_order_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$runpath_order_app/Contents/Good/libRunpathOrder.dylib" \
        -Wl,-rpath,@executable_path/../Bad \
        -Wl,-rpath,@executable_path/../Good \
        -o "$runpath_order_app/Contents/MacOS/barrier"
printf 'first runpath candidate is not a Mach-O image\n' \
    > "$runpath_order_app/Contents/Bad/libRunpathOrder.dylib"
runpath_order_manifest="$test_root/runpath-order-manifest.txt"
"$verifier" --app "$runpath_order_app" --target 11.0 --arch arm64 \
    --manifest "$runpath_order_manifest" > "$test_root/runpath-order.log"
/usr/bin/grep -F \
    $'\t@rpath/libRunpathOrder.dylib\tbundle\tGood/libRunpathOrder.dylib' \
    "$runpath_order_manifest" >/dev/null
"$runpath_order_app/Contents/MacOS/barrier"

non_dylib_target_app=$(make_app non-dylib-target 11.0)
/bin/mkdir -p "$non_dylib_target_app/Contents/Frameworks"
printf '%s\n' 'int non_dylib_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libNonDylib.dylib \
        -o "$test_root/libNonDylib.dylib"
printf '%s\n' \
    'int non_dylib_fixture(void); int main(void) { return non_dylib_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$test_root/libNonDylib.dylib" \
        -Wl,-rpath,@executable_path/../Frameworks \
        -o "$non_dylib_target_app/Contents/MacOS/barrier"
compile_executable \
    "$non_dylib_target_app/Contents/Frameworks/libNonDylib.dylib" \
    arm64-apple-macos11 'int main(void) { return 0; }'
expect_failure non-dylib-target \
    'unresolved bundled dependency: MacOS/barrier' \
    "$verifier" --app "$non_dylib_target_app" --target 11.0 --arch arm64

multiple_executables_app=$(make_app multiple-executables 11.0)
/bin/mkdir -p "$multiple_executables_app/Contents/Frameworks"
printf '%s\n' 'int multiple_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libMultiple.dylib \
        -o "$multiple_executables_app/Contents/Frameworks/libMultiple.dylib"
compile_executable "$multiple_executables_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
printf '%s\n' \
    'int multiple_fixture(void); int main(void) { return multiple_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$multiple_executables_app/Contents/Frameworks/libMultiple.dylib" \
        -Wl,-rpath,@executable_path/../Frameworks \
        -o "$multiple_executables_app/Contents/MacOS/helper-tool"
multiple_manifest="$multiple_executables_app/Contents/Resources/macho-manifest.txt"
"$verifier" --app "$multiple_executables_app" --target 11.0 --arch arm64 \
    --manifest "$multiple_manifest" > "$test_root/multiple-executables.log"
/usr/bin/grep -F \
    $'dependency\tMacOS/helper-tool\t' "$multiple_manifest" >/dev/null
/usr/bin/grep -F \
    $'\t@rpath/libMultiple.dylib\tbundle\tFrameworks/libMultiple.dylib' \
    "$multiple_manifest" >/dev/null

cycle_app=$(make_app dependency-cycle 11.0)
/bin/mkdir -p "$cycle_app/Contents/Frameworks"
printf '%s\n' 'int cycle_b(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libCycleB.dylib \
        -o "$cycle_app/Contents/Frameworks/libCycleB.dylib"
printf '%s\n' 'int cycle_b(void); int cycle_a(void) { return cycle_b(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libCycleA.dylib -Wl,-rpath,@loader_path \
        -x none "$cycle_app/Contents/Frameworks/libCycleB.dylib" \
        -o "$cycle_app/Contents/Frameworks/libCycleA.dylib"
printf '%s\n' 'int cycle_a(void); int cycle_b(void) { return cycle_a(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libCycleB.dylib -Wl,-rpath,@loader_path \
        -x none "$cycle_app/Contents/Frameworks/libCycleA.dylib" \
        -o "$cycle_app/Contents/Frameworks/libCycleB.dylib"
compile_executable "$cycle_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'int main(void) { return 0; }'
"$verifier" --app "$cycle_app" --target 11.0 --arch arm64 \
    > "$test_root/dependency-cycle.log"

directory_alias_app=$(make_app directory-alias 11.0)
/bin/mkdir -p "$directory_alias_app/Contents/Frameworks/Nested" \
    "$directory_alias_app/Contents/Libraries" \
    "$directory_alias_app/Contents/PlugIns"
printf '%s\n' 'int directory_alias_target(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@loader_path/../../Libraries/libAliasTarget.dylib \
        -o "$directory_alias_app/Contents/Libraries/libAliasTarget.dylib"
printf '%s\n' \
    'int directory_alias_target(void); int alias_plugin(void) { return directory_alias_target(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libAliasPlugin.dylib \
        -x none "$directory_alias_app/Contents/Libraries/libAliasTarget.dylib" \
        -o "$directory_alias_app/Contents/Frameworks/Nested/libAliasPlugin.dylib"
compile_executable "$directory_alias_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
/bin/ln -s ../Frameworks \
    "$directory_alias_app/Contents/PlugIns/AliasFrameworks"
/bin/ln -s AliasFrameworks \
    "$directory_alias_app/Contents/PlugIns/ComposedAliasFrameworks"
"$verifier" --app "$directory_alias_app" --target 11.0 --arch arm64 \
    > "$test_root/directory-alias.log"

reentry_alias_app=$(make_app reentry-alias 11.0)
/bin/mkdir -p "$reentry_alias_app/Contents/Frameworks" \
    "$reentry_alias_app/Contents/Real" \
    "$test_root/reentry-external"
printf '%s\n' 'int reentry_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libReentry.dylib \
        -o "$reentry_alias_app/Contents/Real/libReentry.dylib"
/bin/ln -s ../reentry-alias.app/Contents/Real \
    "$test_root/reentry-external/Back"
/bin/ln -s ../../../reentry-external/Back \
    "$reentry_alias_app/Contents/Frameworks/Alias"
printf '%s\n' \
    'int reentry_fixture(void); int main(void) { return reentry_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$reentry_alias_app/Contents/Real/libReentry.dylib" \
        -Wl,-rpath,@executable_path/../Frameworks/Alias \
        -o "$reentry_alias_app/Contents/MacOS/barrier"
test -f "$reentry_alias_app/Contents/Frameworks/Alias/libReentry.dylib"
reentry_isolated_app="$test_root/reentry-isolated/reentry-alias.app"
/bin/mkdir -p "$test_root/reentry-isolated"
/usr/bin/ditto "$reentry_alias_app" "$reentry_isolated_app"
if test -e "$reentry_isolated_app/Contents/Frameworks/Alias/libReentry.dylib"; then
    printf 'isolated app retained an external symlink intermediary\n' >&2
    exit 1
fi
expect_failure reentry-alias \
    'symlink target escapes verification root: Frameworks/Alias' \
    "$verifier" --app "$reentry_alias_app" --target 11.0 --arch arm64
expect_failure reentry-alias-helper \
    'unsafe bundle symlink: Frameworks/Alias' \
    "$python3_bin" "$closure_helper" \
        --root "$reentry_alias_app/Contents" \
        --main MacOS/barrier \
        --arch arm64 \
        --target 11.0 \
        --output "$test_root/reentry-helper-manifest.txt"

canonical_alias_app=$(make_app canonical-alias 11.0)
/bin/mkdir -p "$canonical_alias_app/Contents/Frameworks" \
    "$canonical_alias_app/Contents/Libraries"
printf '%s\n' 'int canonical_alias_b(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@loader_path/libCanonicalAliasB.dylib \
        -o "$canonical_alias_app/Contents/Frameworks/libCanonicalAliasB.dylib"
printf '%s\n' \
    'int canonical_alias_b(void); int canonical_alias_a(void) { return canonical_alias_b(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libCanonicalAliasA.dylib \
        -x none "$canonical_alias_app/Contents/Frameworks/libCanonicalAliasB.dylib" \
        -o "$canonical_alias_app/Contents/Libraries/libCanonicalAliasA.dylib"
/bin/ln -s ../Libraries/libCanonicalAliasA.dylib \
    "$canonical_alias_app/Contents/Frameworks/libCanonicalAliasA.dylib"
printf '%s\n' \
    'int canonical_alias_a(void); int main(void) { return canonical_alias_a(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$canonical_alias_app/Contents/Libraries/libCanonicalAliasA.dylib" \
        -Wl,-rpath,@executable_path/../Frameworks \
        -o "$canonical_alias_app/Contents/MacOS/barrier"
expect_failure canonical-alias \
    'unresolved bundled dependency: Libraries/libCanonicalAliasA.dylib' \
    "$verifier" --app "$canonical_alias_app" --target 11.0 --arch arm64

system_traversal_fixture="$test_root/libSystemTraversal.dylib"
printf '%s\n' 'int system_traversal(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,/System/Library/../Frameworks/libTraversal.dylib \
        -o "$system_traversal_fixture"
system_traversal_app=$(make_app system-traversal 11.0)
printf '%s\n' \
    'int system_traversal(void); int main(void) { return system_traversal(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$system_traversal_fixture" \
        -o "$system_traversal_app/Contents/MacOS/barrier"
expect_failure system-traversal \
    'forbidden external dependency: MacOS/barrier' \
    "$verifier" --app "$system_traversal_app" --target 11.0 --arch arm64
if /usr/bin/grep -F '/System/Library/../' \
    "$test_root/system-traversal.log" >/dev/null; then
    printf 'verifier exposed a malformed system dependency\n' >&2
    exit 1
fi

"$python3_bin" - "$closure_helper" "$test_root" <<'PY'
import importlib.util
import os
import struct
import sys

spec = importlib.util.spec_from_file_location("macho_closure_fixture", sys.argv[1])
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
if module.normalize_absolute("/usr/lib/libSystem.B.dylib") != \
        "/usr/lib/libSystem.B.dylib":
    raise SystemExit("valid system dependency was not preserved")
for malformed in (
        "/usr/lib//libFixture.dylib",
        "/usr/lib/./libFixture.dylib",
        "/usr/lib/../libFixture.dylib",
        "/usr/lib\\libFixture.dylib",
        "/usr/lib/\u200blibFixture.dylib"):
    if module.normalize_absolute(malformed) is not None:
        raise SystemExit("malformed system dependency was accepted")
if module.safe_field("visible\u200bformat"):
    raise SystemExit("Unicode format character was accepted")
for separator in ("\u2028", "\u2029"):
    if module.safe_field("visible" + separator + "separator"):
        raise SystemExit("Unicode line separator was accepted")

fixture_root = sys.argv[2]

def write_macho(name, command_count, command_bytes):
    path = os.path.join(fixture_root, name)
    header = struct.pack(
        "<IIIIIIII",
        module.MH_MAGIC_64,
        module.CPU_TYPE_ARM64,
        0,
        module.MH_BUNDLE,
        command_count,
        len(command_bytes),
        0,
        0,
    )
    with open(path, "wb") as stream:
        stream.write(header + command_bytes)
    return path

valid_unknown = write_macho(
    "valid-unknown-command", 1, struct.pack("<II", 0x7FFFFFFF, 8))
module.parse_macho(valid_unknown, "PlugIns/valid-unknown-command", 11)

rejected_commands = {
    "dyld-environment": module.LC_DYLD_ENVIRONMENT,
    "load-fvmlib": module.LC_LOADFVMLIB,
    "id-fvmlib": module.LC_IDFVMLIB,
    "unknown-required": module.LC_REQ_DYLD | 0x7FFF,
}
for name, command_type in rejected_commands.items():
    path = write_macho(name, 1, struct.pack("<II", command_type, 8))
    try:
        module.parse_macho(path, "PlugIns/" + name, 11)
    except module.AuditError:
        continue
    raise SystemExit("unsupported Mach-O loader command was accepted")

malformed_commands = {
    "command-count": (
        2,
        struct.pack("<II", 0x7FFFFFFF, 8),
    ),
    "command-size": (
        1,
        struct.pack("<II", module.LC_LOAD_DYLIB, 8),
    ),
    "missing-string-nul": (
        1,
        struct.pack("<IIIIII", module.LC_LOAD_DYLIB, 32, 24, 0, 0, 0)
        + b"ABCDEFGH",
    ),
    "trailing-command-bytes": (
        1,
        struct.pack("<II", 0x7FFFFFFF, 8) + (b"X" * 8),
    ),
    "short-dyld-info": (
        1,
        struct.pack("<II", module.LC_DYLD_INFO_ONLY, 8),
    ),
    "short-main": (
        1,
        struct.pack("<II", module.LC_MAIN, 8),
    ),
    "short-exports-trie": (
        1,
        struct.pack("<II", module.LC_DYLD_EXPORTS_TRIE, 8),
    ),
    "short-chained-fixups": (
        1,
        struct.pack("<II", module.LC_DYLD_CHAINED_FIXUPS, 8),
    ),
}
for name, (command_count, command_bytes) in malformed_commands.items():
    path = write_macho(name, command_count, command_bytes)
    try:
        module.parse_macho(path, "PlugIns/" + name, 11)
    except module.AuditError:
        continue
    raise SystemExit("malformed Mach-O parser fixture was accepted")
PY

line_separator_app=$(make_app line-separator-command 11.0)
printf '%s\n' 'int main(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -Wl,-headerpad,128 \
        -o "$line_separator_app/Contents/MacOS/barrier"
"$python3_bin" - "$line_separator_app/Contents/MacOS/barrier" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
command_count, command_bytes = struct.unpack_from("<II", data, 16)
dependency_path = b"/usr/lib/libLine\xe2\x80\xa8Break.dylib\0"
command_size = (24 + len(dependency_path) + 7) & ~7
command = struct.pack(
    "<IIIIII", 0x80000018, command_size, 24, 0, 0, 0)
command += dependency_path
command += b"\0" * (command_size - len(command))
command_start = 32 + command_bytes
if command_start + command_size > len(data) or \
        any(data[command_start:command_start + command_size]):
    raise SystemExit("fixture Mach-O header padding is unavailable")
data[command_start:command_start + command_size] = command
struct.pack_into(
    "<II", data, 16, command_count + 1, command_bytes + command_size)
with open(path, "wb") as stream:
    stream.write(data)
PY
line_separator_manifest="$test_root/line-separator-manifest.txt"
expect_failure line-separator-command \
    'malformed Mach-O load commands: MacOS/barrier' \
    "$verifier" --app "$line_separator_app" --target 11.0 --arch arm64 \
        --manifest "$line_separator_manifest"
if test -e "$line_separator_manifest"; then
    printf 'verifier wrote a manifest for an unsafe load-command field\n' >&2
    exit 1
fi

invalid_dylinker_app=$(make_app invalid-dylinker 11.0)
compile_executable "$invalid_dylinker_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
"$python3_bin" - "$invalid_dylinker_app/Contents/MacOS/barrier" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
command_count = struct.unpack_from("<I", data, 16)[0]
offset = 32
for _index in range(command_count):
    command, command_size = struct.unpack_from("<II", data, offset)
    if command == 0xE:
        name_offset = struct.unpack_from("<I", data, offset + 8)[0]
        name_start = offset + name_offset
        name_end = data.index(0, name_start, offset + command_size)
        old_size = name_end - name_start + 1
        replacement = b"/opt/x/dyld\0"
        if len(replacement) > old_size:
            raise SystemExit("fixture dynamic-linker command is too small")
        data[name_start:name_start + old_size] = \
            replacement + (b"\0" * (old_size - len(replacement)))
        break
    offset += command_size
else:
    raise SystemExit("fixture dynamic-linker command was not found")
with open(path, "wb") as stream:
    stream.write(data)
PY
expect_failure invalid-dylinker \
    'invalid executable loader metadata: MacOS/barrier' \
    "$verifier" --app "$invalid_dylinker_app" --target 11.0 --arch arm64

dyld_environment_app=$(make_app dyld-environment 11.0)
printf '%s\n' 'int main(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -Wl,-headerpad,128 \
        -o "$dyld_environment_app/Contents/MacOS/barrier"
"$python3_bin" - "$dyld_environment_app/Contents/MacOS/barrier" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
command_count, command_bytes = struct.unpack_from("<II", data, 16)
value = b"DYLD_LIBRARY_PATH=/opt/example\0"
command_size = (12 + len(value) + 7) & ~7
command = struct.pack("<III", 0x27, command_size, 12) + value
command += b"\0" * (command_size - len(command))
command_offset = 32 + command_bytes
if any(data[command_offset:command_offset + command_size]):
    raise SystemExit("fixture executable has insufficient header padding")
data[command_offset:command_offset + command_size] = command
struct.pack_into(
    "<II", data, 16, command_count + 1, command_bytes + command_size)
with open(path, "wb") as stream:
    stream.write(data)
PY
expect_failure dyld-environment \
    'unsupported Mach-O loader command: MacOS/barrier' \
    "$verifier" --app "$dyld_environment_app" --target 11.0 --arch arm64

alternate_app=$(make_app alternate-command 11.0)
/bin/mkdir -p "$alternate_app/Contents/Frameworks"
printf '%s\n' 'int alternate_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libA.dylib \
        -o "$alternate_app/Contents/Frameworks/libA.dylib"
printf '%s\n' \
    'int alternate_fixture(void); int main(void) { return alternate_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -x none "$alternate_app/Contents/Frameworks/libA.dylib" \
        -Wl,-rpath,@executable_path/../Frameworks \
        -o "$alternate_app/Contents/MacOS/barrier"
"$python3_bin" - "$alternate_app/Contents/MacOS/barrier" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
_magic, _cpu, _subtype, _filetype, command_count, _size, _flags, _reserved = \
    struct.unpack_from("<IIIIIIII", data, 0)
offset = 32
converted = False
for _index in range(command_count):
    command, command_size = struct.unpack_from("<II", data, offset)
    if command == 0xC:
        name_offset = struct.unpack_from("<I", data, offset + 8)[0]
        name_start = offset + name_offset
        name_end = data.index(0, name_start, offset + command_size)
        name = bytes(data[name_start:name_end + 1])
        if name == b"@rpath/libA.dylib\0":
            if name_offset != 24 or 28 + len(name) > command_size:
                raise SystemExit("fixture load command has insufficient padding")
            struct.pack_into("<I", data, offset + 8, 28)
            struct.pack_into("<I", data, offset + 12, 0x1A741800)
            struct.pack_into("<I", data, offset + 24, 0x0A)
            data[offset + 28:offset + 28 + len(name)] = name
            converted = True
            break
    offset += command_size
if not converted:
    raise SystemExit("fixture dependency load command was not found")
with open(path, "wb") as stream:
    stream.write(data)
PY
expect_failure alternate-before-supported-floor \
    'unsupported Mach-O dependency command: MacOS/barrier' \
    "$python3_bin" "$closure_helper" \
        --root "$alternate_app/Contents" \
        --main MacOS/barrier \
        --arch arm64 \
        --target 11.0 \
        --output "$test_root/alternate-before-floor-manifest.txt"
alternate_manifest="$test_root/alternate-command-manifest.txt"
"$python3_bin" "$closure_helper" \
    --root "$alternate_app/Contents" \
    --main MacOS/barrier \
    --arch arm64 \
    --target 15.0 \
    --output "$alternate_manifest"
/usr/bin/grep -F \
    $'\tload-dylib+alternate+reexport+delayed-init\t@rpath/libA.dylib\tbundle\tFrameworks/libA.dylib' \
    "$alternate_manifest" >/dev/null

"$python3_bin" - "$alternate_app/Contents/MacOS/barrier" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
command_count = struct.unpack_from("<I", data, 16)[0]
offset = 32
corrupted = False
for _index in range(command_count):
    _command, command_size = struct.unpack_from("<II", data, offset)
    if command_size >= 28 and \
            struct.unpack_from("<I", data, offset + 12)[0] == 0x1A741800:
        struct.pack_into("<I", data, offset + 24, 0x10)
        corrupted = True
        break
    offset += command_size
if not corrupted:
    raise SystemExit("fixture alternate load command was not found")
with open(path, "wb") as stream:
    stream.write(data)
PY
expect_failure malformed-alternate \
    'malformed Mach-O load commands: MacOS/barrier' \
    "$python3_bin" "$closure_helper" \
        --root "$alternate_app/Contents" \
        --main MacOS/barrier \
        --arch arm64 \
        --target 15.0 \
        --output "$test_root/malformed-alternate-manifest.txt"
if /usr/bin/grep -F "$test_root" \
    "$test_root/malformed-alternate.log" >/dev/null; then
    printf 'closure helper exposed a local malformed-image path\n' >&2
    exit 1
fi

legacy_variants_app=$(make_app legacy-load-variants 11.0)
/bin/mkdir -p "$legacy_variants_app/Contents/Frameworks"
printf '%s\n' 'int legacy_reexport(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libR.dylib \
        -o "$legacy_variants_app/Contents/Frameworks/libR.dylib"
printf '%s\n' 'int legacy_upward(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libU.dylib \
        -o "$legacy_variants_app/Contents/Frameworks/libU.dylib"
printf '%s\n' \
    'int legacy_reexport(void); int legacy_upward(void); int carrier(void) { return legacy_reexport() + legacy_upward(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libCarrier.dylib -Wl,-rpath,@loader_path \
        -x none "$legacy_variants_app/Contents/Frameworks/libR.dylib" \
        "$legacy_variants_app/Contents/Frameworks/libU.dylib" \
        -o "$legacy_variants_app/Contents/Frameworks/libCarrier.dylib"
compile_executable "$legacy_variants_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
"$python3_bin" - \
    "$legacy_variants_app/Contents/Frameworks/libCarrier.dylib" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
command_count = struct.unpack_from("<I", data, 16)[0]
offset = 32
converted = set()
for _index in range(command_count):
    command, command_size = struct.unpack_from("<II", data, offset)
    if command == 0xC:
        name_offset = struct.unpack_from("<I", data, offset + 8)[0]
        name_start = offset + name_offset
        name_end = data.index(0, name_start, offset + command_size)
        name = bytes(data[name_start:name_end])
        if name == b"@rpath/libR.dylib":
            struct.pack_into("<I", data, offset, 0x8000001F)
            converted.add("reexport")
        elif name == b"@rpath/libU.dylib":
            struct.pack_into("<I", data, offset, 0x80000023)
            converted.add("upward")
    offset += command_size
if converted != {"reexport", "upward"}:
    raise SystemExit("fixture legacy load commands were not found")
with open(path, "wb") as stream:
    stream.write(data)
PY
legacy_variants_manifest="$test_root/legacy-load-variants-manifest.txt"
"$python3_bin" "$closure_helper" \
    --root "$legacy_variants_app/Contents" \
    --main MacOS/barrier \
    --arch arm64 \
    --target 11.0 \
    --output "$legacy_variants_manifest"
/usr/bin/grep -F $'\treexport-dylib\t@rpath/libR.dylib\tbundle\tFrameworks/libR.dylib' \
    "$legacy_variants_manifest" >/dev/null
/usr/bin/grep -F $'\tload-upward-dylib\t@rpath/libU.dylib\tbundle\tFrameworks/libU.dylib' \
    "$legacy_variants_manifest" >/dev/null

wrong_arch_app=$(make_app wrong-arch 11.0)
compile_executable "$wrong_arch_app/Contents/MacOS/barrier" x86_64-apple-macos11 \
    'int main(void) { return 0; }'
expect_failure wrong-arch 'unexpected architectures (x86_64): MacOS/barrier' \
    "$verifier" --app "$wrong_arch_app" --target 11.0 --arch arm64

high_target_app=$(make_app high-target 11.0)
compile_executable "$high_target_app/Contents/MacOS/barrier" arm64-apple-macos12 \
    'int main(void) { return 0; }'
expect_failure high-target 'deployment target 12.0 exceeds 11.0: MacOS/barrier' \
    "$verifier" --app "$high_target_app" --target 11.0 --arch arm64

plugin_id_app=$(make_app plugin-id 11.0)
compile_executable "$plugin_id_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'int main(void) { return 0; }'
/bin/mkdir -p "$plugin_id_app/Contents/PlugIns/bearer"
printf '%s\n' 'int plugin_fixture(void) { return 0; }' | /usr/bin/xcrun clang -x c - \
    -target arm64-apple-macos11 -dynamiclib \
    -Wl,-install_name,libValidPlugin.dylib \
    -o "$plugin_id_app/Contents/PlugIns/bearer/libValidPlugin.dylib"
"$verifier" --app "$plugin_id_app" --target 11.0 --arch arm64 \
    > "$test_root/plugin-id.log"

unsafe_prefix=/opt/example
unsafe_install_id="$unsafe_prefix/lib/libUnsafeIdentity.dylib"
unsafe_id_app=$(make_app unsafe-id 11.0)
compile_executable "$unsafe_id_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'int main(void) { return 0; }'
printf '%s\n' 'int unsafe_identity(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,"$unsafe_install_id" \
        -o "$unsafe_id_app/Contents/MacOS/libUnsafeIdentity.dylib"
expect_failure unsafe-id 'unsafe install identity: MacOS/libUnsafeIdentity.dylib' \
    "$verifier" --app "$unsafe_id_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$unsafe_install_id" "$test_root/unsafe-id.log" >/dev/null; then
    printf 'verifier exposed an unsafe install identity\n' >&2
    exit 1
fi

unsafe_runpath="$unsafe_prefix/lib"
unsafe_rpath_app=$(make_app unsafe-rpath 11.0)
printf '%s\n' 'int main(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -Wl,-rpath,"$unsafe_runpath" \
        -o "$unsafe_rpath_app/Contents/MacOS/barrier"
expect_failure unsafe-rpath 'unsafe runpath: MacOS/barrier' \
    "$verifier" --app "$unsafe_rpath_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$unsafe_runpath" "$test_root/unsafe-rpath.log" >/dev/null; then
    printf 'verifier exposed an unsafe runpath\n' >&2
    exit 1
fi

escaping_runpath='@loader_path/../../../outside'
escaping_rpath_app=$(make_app escaping-rpath 11.0)
printf '%s\n' 'int main(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -Wl,-rpath,"$escaping_runpath" \
        -o "$escaping_rpath_app/Contents/MacOS/barrier"
expect_failure escaping-rpath 'unsafe runpath: MacOS/barrier' \
    "$verifier" --app "$escaping_rpath_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$escaping_runpath" \
    "$test_root/escaping-rpath.log" >/dev/null; then
    printf 'verifier exposed an escaping runpath\n' >&2
    exit 1
fi

unrelated_rpath_app=$(make_app unrelated-rpath 11.0)
compile_executable "$unrelated_rpath_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
/bin/mkdir -p "$unrelated_rpath_app/Contents/Frameworks" \
    "$unrelated_rpath_app/Contents/PlugIns"
printf '%s\n' 'int context_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libContextFixture.dylib \
        -o "$unrelated_rpath_app/Contents/Frameworks/libContextFixture.dylib"
printf '%s\n' \
    '#include <dlfcn.h>' \
    'typedef int (*plugin_fn)(void);' \
    'int main(void) { void *handle = dlopen("@executable_path/../PlugIns/libContextPlugin.dylib", RTLD_NOW); if (!handle) return 90; plugin_fn function = (plugin_fn)dlsym(handle, "plugin"); return function ? function() : 91; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -Wl,-rpath,@executable_path/../Frameworks \
        -o "$unrelated_rpath_app/Contents/MacOS/unrelated-helper"
printf '%s\n' \
    'int context_fixture(void); int plugin(void) { return context_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,libContextPlugin.dylib \
        -L"$unrelated_rpath_app/Contents/Frameworks" -lContextFixture \
        -o "$unrelated_rpath_app/Contents/PlugIns/libContextPlugin.dylib"
unrelated_rpath_manifest="$test_root/unrelated-rpath-manifest.txt"
"$verifier" --app "$unrelated_rpath_app" --target 11.0 --arch arm64 \
    --manifest "$unrelated_rpath_manifest" > "$test_root/unrelated-rpath.log"
/usr/bin/grep -F \
    $'dependency\tPlugIns/libContextPlugin.dylib\t' \
    "$unrelated_rpath_manifest" >/dev/null
"$unrelated_rpath_app/Contents/MacOS/unrelated-helper"

no_dynamic_context_app=$(make_app no-dynamic-context 11.0)
compile_executable "$no_dynamic_context_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
/bin/mkdir -p "$no_dynamic_context_app/Contents/Frameworks" \
    "$no_dynamic_context_app/Contents/PlugIns"
printf '%s\n' 'int no_context_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@rpath/libNoContext.dylib \
        -o "$no_dynamic_context_app/Contents/Frameworks/libNoContext.dylib"
printf '%s\n' \
    'int no_context_fixture(void); int plugin(void) { return no_context_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,libNoContextPlugin.dylib \
        -L"$no_dynamic_context_app/Contents/Frameworks" -lNoContext \
        -o "$no_dynamic_context_app/Contents/PlugIns/libNoContextPlugin.dylib"
expect_failure no-dynamic-context \
    'unresolved bundled dependency: PlugIns/libNoContextPlugin.dylib' \
    "$verifier" --app "$no_dynamic_context_app" --target 11.0 --arch arm64

dynamic_external_app=$(make_app dynamic-external 11.0)
compile_executable "$dynamic_external_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
/bin/mkdir -p "$dynamic_external_app/Contents/PlugIns"
dynamic_external_path=/opt/example/libDynamicExternal.dylib
printf '%s\n' 'int dynamic_external(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,"$dynamic_external_path" \
        -o "$test_root/libDynamicExternal.dylib"
printf '%s\n' \
    'int dynamic_external(void); int plugin(void) { return dynamic_external(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,libDynamicExternalPlugin.dylib \
        -x none "$test_root/libDynamicExternal.dylib" \
        -o "$dynamic_external_app/Contents/PlugIns/libDynamicExternalPlugin.dylib"
expect_failure dynamic-external \
    'forbidden external dependency: PlugIns/libDynamicExternalPlugin.dylib' \
    "$verifier" --app "$dynamic_external_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$dynamic_external_path" \
    "$test_root/dynamic-external.log" >/dev/null; then
    printf 'verifier exposed a dynamic external dependency\n' >&2
    exit 1
fi

printf '%s\n' 'int bare_fixture(void) { return 0; }' | /usr/bin/xcrun clang -x c - \
    -target arm64-apple-macos11 -dynamiclib \
    -Wl,-install_name,libBareExternal.dylib \
    -o "$test_root/libBareExternal.dylib"
bare_external_app=$(make_app bare-external 11.0)
printf '%s\n' 'int bare_fixture(void); int main(void) { return bare_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -L"$test_root" -lBareExternal \
        -o "$bare_external_app/Contents/MacOS/barrier"
expect_failure bare-external 'forbidden external dependency: MacOS/barrier' \
    "$verifier" --app "$bare_external_app" --target 11.0 --arch arm64
if /usr/bin/grep -F 'libBareExternal.dylib' "$test_root/bare-external.log" >/dev/null; then
    printf 'verifier exposed a bare dependency value\n' >&2
    exit 1
fi

missing_link_app=$(make_app missing-link 11.0)
printf '%s\n' 'int missing_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -dynamiclib \
        -Wl,-install_name,@loader_path/libMissingFixture.dylib \
        -o "$test_root/libMissingFixture.dylib"
printf '%s\n' 'int missing_fixture(void); int main(void) { return missing_fixture(); }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 \
        -L"$test_root" -lMissingFixture \
        -o "$missing_link_app/Contents/MacOS/barrier"
expect_failure missing-link 'unresolved bundled dependency: MacOS/barrier' \
    "$verifier" --app "$missing_link_app" --target 11.0 --arch arm64
if /usr/bin/grep -F 'libMissingFixture.dylib' "$test_root/missing-link.log" >/dev/null; then
    printf 'verifier exposed a missing dependency value\n' >&2
    exit 1
fi

printf 'not a library\n' > "$missing_link_app/Contents/MacOS/libMissingFixture.dylib"
expect_failure placeholder-link 'unresolved bundled dependency: MacOS/barrier' \
    "$verifier" --app "$missing_link_app" --target 11.0 --arch arm64
if /usr/bin/grep -F 'libMissingFixture.dylib' "$test_root/placeholder-link.log" >/dev/null; then
    printf 'verifier exposed a placeholder dependency value\n' >&2
    exit 1
fi

privacy_app=$(make_app privacy 11.0)
compile_executable "$privacy_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'const char *path = "/Users" "/local-user/private-source"; int main(void) { return path[0] == 0; }'
expect_failure privacy-string 'local-environment or private-network string found: MacOS/barrier' \
    "$verifier" --app "$privacy_app" --target 11.0 --arch arm64
user_path_prefix=/Users
if /usr/bin/grep -F "$user_path_prefix/" "$test_root/privacy-string.log" >/dev/null; then
    printf 'verifier exposed a local privacy string\n' >&2
    exit 1
fi

unix_home_app=$(make_app unix-home 11.0)
compile_executable "$unix_home_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'const char *path = "/home" "/local-user/private-source"; int main(void) { return path[0] == 0; }'
expect_failure unix-home 'local-environment or private-network string found: MacOS/barrier' \
    "$verifier" --app "$unix_home_app" --target 11.0 --arch arm64
unix_home_prefix=/home
if /usr/bin/grep -F "$unix_home_prefix/" "$test_root/unix-home.log" >/dev/null; then
    printf 'verifier exposed a local privacy string\n' >&2
    exit 1
fi

windows_home_app=$(make_app windows-home 11.0)
compile_executable "$windows_home_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'const char *path = "C:" "\\\\Users\\\\local-user\\\\private-source"; int main(void) { return path[0] == 0; }'
expect_failure windows-home 'local-environment or private-network string found: MacOS/barrier' \
    "$verifier" --app "$windows_home_app" --target 11.0 --arch arm64
windows_home_prefix='C:\\Users'
if /usr/bin/grep -F "$windows_home_prefix" "$test_root/windows-home.log" >/dev/null; then
    printf 'verifier exposed a local privacy string\n' >&2
    exit 1
fi

private_network_app=$(make_app private-network 11.0)
compile_executable "$private_network_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'const char *endpoint = "192." "168." "0." "42"; int main(void) { return endpoint[0] == 0; }'
expect_failure private-network 'local-environment or private-network string found: MacOS/barrier' \
    "$verifier" --app "$private_network_app" --target 11.0 --arch arm64
private_network_prefix=192.168.
if /usr/bin/grep -F "$private_network_prefix" "$test_root/private-network.log" >/dev/null; then
    printf 'verifier exposed a private-network string\n' >&2
    exit 1
fi

protected_resource_app=$(make_app protected-resource-path 11.0)
compile_executable "$protected_resource_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
protected_resource_name=$(printf '%s.%s.%s.%s' 192 168 30 41)
printf 'public resource\n' \
    > "$protected_resource_app/Contents/Resources/$protected_resource_name"
expect_failure protected-resource-path \
    'protected or unsafe app pathname found' \
    "$verifier" --app "$protected_resource_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$protected_resource_name" \
    "$test_root/protected-resource-path.log" >/dev/null; then
    printf 'verifier exposed a protected resource pathname\n' >&2
    exit 1
fi

protected_directory_app=$(make_app protected-directory-path 11.0)
compile_executable "$protected_directory_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
protected_directory_name=$(printf '%s.%s.%s.%s' 172 20 30 41)
/bin/mkdir \
    "$protected_directory_app/Contents/Resources/$protected_directory_name"
expect_failure protected-directory-path \
    'protected or unsafe app pathname found' \
    "$verifier" --app "$protected_directory_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$protected_directory_name" \
    "$test_root/protected-directory-path.log" >/dev/null; then
    printf 'verifier exposed a protected directory pathname\n' >&2
    exit 1
fi

unsafe_resource_app=$(make_app unsafe-resource-path 11.0)
compile_executable "$unsafe_resource_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
forged_resource_marker=forged-app-verifier-diagnostic
unsafe_resource_name=$(printf 'unsafe\n%s' "$forged_resource_marker")
printf 'public resource\n' \
    > "$unsafe_resource_app/Contents/Resources/$unsafe_resource_name"
expect_failure unsafe-resource-path \
    'protected or unsafe app pathname found' \
    "$verifier" --app "$unsafe_resource_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$forged_resource_marker" \
    "$test_root/unsafe-resource-path.log" >/dev/null; then
    printf 'verifier allowed a pathname to forge a diagnostic\n' >&2
    exit 1
fi

private_link_target_app=$(make_app private-link-target 11.0)
compile_executable "$private_link_target_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
private_link_target=$(printf '%s.%s.%s.%s' 192 168 20 31)
/bin/ln -s "$private_link_target" \
    "$private_link_target_app/Contents/Resources/private-link"
expect_failure private-link-target \
    'protected or unsafe symlink target found' \
    "$verifier" --app "$private_link_target_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$private_link_target" \
    "$test_root/private-link-target.log" >/dev/null; then
    printf 'verifier exposed a protected symlink target\n' >&2
    exit 1
fi

unsafe_link_target_app=$(make_app unsafe-link-target 11.0)
compile_executable "$unsafe_link_target_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
forged_link_marker=forged-symlink-target-diagnostic
unsafe_link_target=$(printf 'safe-target\n%s' "$forged_link_marker")
/bin/ln -s "$unsafe_link_target" \
    "$unsafe_link_target_app/Contents/Resources/unsafe-link"
expect_failure unsafe-link-target \
    'protected or unsafe symlink target found' \
    "$verifier" --app "$unsafe_link_target_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$forged_link_marker" \
    "$test_root/unsafe-link-target.log" >/dev/null; then
    printf 'verifier exposed an unsafe symlink target\n' >&2
    exit 1
fi

wide_private_root="/""Users""/"
wide_private_path="${wide_private_root}example/private-build"
for wide_encoding in UTF-16LE UTF-16BE UTF-32LE UTF-32BE; do
    case "$wide_encoding" in
        UTF-16LE) wide_fixture_name=utf16le ;;
        UTF-16BE) wide_fixture_name=utf16be ;;
        UTF-32LE) wide_fixture_name=utf32le ;;
        UTF-32BE) wide_fixture_name=utf32be ;;
        *) printf 'unsupported wide fixture encoding\n' >&2; exit 1 ;;
    esac
    wide_app=$(make_app "privacy-$wide_fixture_name" 11.0)
    compile_executable "$wide_app/Contents/MacOS/barrier" \
        arm64-apple-macos11 'int main(void) { return 0; }'
    wide_resource="$wide_app/Contents/Resources/protected-metadata.bin"
    printf '\177' > "$wide_resource"
    if ! printf '%s' "$wide_private_path" \
        | "$iconv_bin" -f UTF-8 -t "$wide_encoding" >> "$wide_resource"; then
        printf 'unable to encode wide verifier fixture\n' >&2
        exit 1
    fi
    expect_failure "$wide_fixture_name" \
        'local-environment or private-network string found: Resources/protected-metadata.bin' \
        "$verifier" --app "$wide_app" --target 11.0 --arch arm64
    if /usr/bin/grep -F "$wide_private_root" \
        "$test_root/$wide_fixture_name.log" >/dev/null; then
        printf 'verifier exposed an encoded privacy string\n' >&2
        exit 1
    fi
done

absolute_link_app=$(make_app absolute-link 11.0)
compile_executable "$absolute_link_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'int main(void) { return 0; }'
absolute_link_root=/Users
/bin/ln -s "$absolute_link_root/local-user/private-source" \
    "$absolute_link_app/Contents/Resources/private-link"
expect_failure absolute-link 'absolute symlink target found: Resources/private-link' \
    "$verifier" --app "$absolute_link_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$absolute_link_root/" "$test_root/absolute-link.log" >/dev/null; then
    printf 'verifier exposed an absolute symlink target\n' >&2
    exit 1
fi

relative_link_app=$(make_app relative-link 11.0)
compile_executable "$relative_link_app/Contents/MacOS/barrier" arm64-apple-macos11 \
    'int main(void) { return 0; }'
printf 'outside app contents\n' > "$test_root/outside-target"
/bin/ln -s ../../../outside-target \
    "$relative_link_app/Contents/Resources/private-link"
expect_failure relative-link \
    'symlink target escapes verification root: Resources/private-link' \
    "$verifier" --app "$relative_link_app" --target 11.0 --arch arm64
if /usr/bin/grep -F "$test_root" "$test_root/relative-link.log" >/dev/null; then
    printf 'verifier exposed an escaping symlink target\n' >&2
    exit 1
fi

enumeration_failure_app=$(make_app enumeration-failure 11.0)
compile_executable "$enumeration_failure_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
/bin/mkdir -p \
    "$enumeration_failure_app/Contents/Resources/unreadable-subtree"
printf 'unreadable enumeration fixture\n' \
    > "$enumeration_failure_app/Contents/Resources/unreadable-subtree/file.txt"
/bin/chmod 000 \
    "$enumeration_failure_app/Contents/Resources/unreadable-subtree"
expect_failure enumeration-failure 'unable to enumerate verification pathnames' \
    "$verifier" --app "$enumeration_failure_app" --target 11.0 --arch arm64
/bin/chmod 700 \
    "$enumeration_failure_app/Contents/Resources/unreadable-subtree"
if /usr/bin/grep -F "$test_root" \
    "$test_root/enumeration-failure.log" >/dev/null; then
    printf 'verifier exposed an enumeration failure path\n' >&2
    exit 1
fi

unreadable_file_app=$(make_app unreadable-file 11.0)
compile_executable "$unreadable_file_app/Contents/MacOS/barrier" \
    arm64-apple-macos11 'int main(void) { return 0; }'
printf 'unreadable regular-file fixture\n' \
    > "$unreadable_file_app/Contents/Resources/unreadable.bin"
/bin/chmod 000 "$unreadable_file_app/Contents/Resources/unreadable.bin"
expect_failure unreadable-file \
    'unable to inspect file privacy: Resources/unreadable.bin' \
    "$verifier" --app "$unreadable_file_app" --target 11.0 --arch arm64
/bin/chmod 600 "$unreadable_file_app/Contents/Resources/unreadable.bin"
if /usr/bin/grep -F "$test_root" "$test_root/unreadable-file.log" >/dev/null; then
    printf 'verifier exposed an unreadable file path\n' >&2
    exit 1
fi

external_archive_root="$test_root/external-archive-root"
/bin/mkdir -p "$external_archive_root/bin" "$external_archive_root/lib"
compile_executable "$external_archive_root/bin/probe" arm64-apple-macos11 \
    'int main(void) { return 0; }'
printf '%s\n' 'int external_archive_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -c \
        -o "$test_root/external-archive.o"
/usr/bin/ar rcs "$test_root/libExternalArchive.a" \
    "$test_root/external-archive.o"
/bin/ln -s ../../libExternalArchive.a \
    "$external_archive_root/lib/libExternalArchive.a"
expect_failure external-archive \
    'symlinked static archive is not allowed: lib/libExternalArchive.a' \
    "$verifier" --root "$external_archive_root" --target 11.0 --arch arm64 \
        --include-static-archives
if /usr/bin/grep -F "$test_root" "$test_root/external-archive.log" >/dev/null; then
    printf 'verifier exposed an external archive path\n' >&2
    exit 1
fi

valid_archive_root="$test_root/valid-archive-root"
/bin/mkdir -p "$valid_archive_root/bin" "$valid_archive_root/lib" \
    "$valid_archive_root/objects"
compile_executable "$valid_archive_root/bin/probe" arm64-apple-macos11 \
    'int main(void) { return 0; }'
printf '%s\n' 'int valid_archive_fixture(void) { return 0; }' \
    | /usr/bin/xcrun clang -x c - -target arm64-apple-macos11 -c \
        -o "$valid_archive_root/objects/valid-archive.o"
/usr/bin/ar rcs "$valid_archive_root/lib/libValidArchive.a" \
    "$valid_archive_root/objects/valid-archive.o"
/bin/ln -s probe "$valid_archive_root/bin/probe-alias"
valid_archive_manifest="$test_root/valid-archive-manifest.txt"
"$verifier" --root "$valid_archive_root" --target 11.0 --arch arm64 \
    --include-static-archives --manifest "$valid_archive_manifest" \
    > "$test_root/valid-archive.log"
/usr/bin/grep -F $'static_archive_count\t1' \
    "$valid_archive_manifest" >/dev/null
/usr/bin/grep -F $'file\tbin/probe\tarch=arm64\tminos=11.0' \
    "$valid_archive_manifest" >/dev/null
/usr/bin/grep -F $'archive\tlib/libValidArchive.a\tarch=arm64\tminos=11.0' \
    "$valid_archive_manifest" >/dev/null
valid_archive_root_alias="$test_root/valid-archive-root-alias"
/bin/ln -s valid-archive-root "$valid_archive_root_alias"
"$verifier" --root "$valid_archive_root_alias" --target 11.0 --arch arm64 \
    --include-static-archives > "$test_root/valid-archive-root-alias.log"

builder="$test_dir/../build-macos-release-prefix.sh"
builder_verifier="$test_dir/../verify-macos-deployment-target.sh"
builder_closure="$test_dir/../macos-macho-closure.py"
builder_scanner="$test_dir/../scan-protected-metadata.pl"
builder_repo_root=$(cd "$test_dir/../.." && pwd)
builder_lock="$builder_repo_root/dist/macos/release-dependencies.lock"
# shellcheck disable=SC1090
source "$builder_lock"
builder_patch="$builder_repo_root/$QTBASE_PATCH"
builder_lock_sha=$(/usr/bin/shasum -a 256 "$builder_lock" \
    | /usr/bin/awk '{print $1}')
builder_sha=$(/usr/bin/shasum -a 256 "$builder" \
    | /usr/bin/awk '{print $1}')
builder_verifier_sha=$(/usr/bin/shasum -a 256 "$builder_verifier" \
    | /usr/bin/awk '{print $1}')
builder_closure_sha=$(/usr/bin/shasum -a 256 "$builder_closure" \
    | /usr/bin/awk '{print $1}')
builder_scanner_sha=$(/usr/bin/shasum -a 256 "$builder_scanner" \
    | /usr/bin/awk '{print $1}')
builder_patch_sha=$(/usr/bin/shasum -a 256 "$builder_patch" \
    | /usr/bin/awk '{print $1}')
builder_recipe_sha=$(printf '%s\n%s\n%s\n%s\n%s\n' \
    "$builder_sha" "$builder_verifier_sha" "$builder_closure_sha" \
    "$builder_scanner_sha" "$builder_patch_sha" \
    | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}')
builder_cache_id="macos-${MACOS_RELEASE_ARCH}-prefix-v${BARRIER_RELEASE_PREFIX_SCHEMA}-${builder_lock_sha:0:16}-${builder_recipe_sha:0:16}"

malicious_prefix="$test_root/malicious-cached-prefix"
malicious_work_dir="$test_root/malicious-cached-work"
malicious_qmake="$test_root/malicious-qmake"
qmake_execution_marker="$test_root/qmake-executed"
/bin/mkdir -p "$malicious_prefix/bin" \
    "$malicious_prefix/lib/QtSvg.framework" \
    "$malicious_prefix/plugins/iconengines"
printf '%s\n' '#!/bin/bash' 'exit 0' \
    > "$malicious_prefix/bin/macdeployqt"
/bin/chmod +x "$malicious_prefix/bin/macdeployqt"
/usr/bin/touch "$malicious_prefix/lib/libssl.a" \
    "$malicious_prefix/lib/libcrypto.a" \
    "$malicious_prefix/lib/QtSvg.framework/QtSvg" \
    "$malicious_prefix/plugins/iconengines/libqsvgicon.dylib"
printf '%s\n' \
    '#!/bin/bash' \
    "sentinel_dir=\$(unset CDPATH; cd -- \"\$(dirname -- \"\$0\")\" && pwd)" \
    ": > \"\$sentinel_dir/qmake-executed\"" \
    'exit 99' > "$malicious_qmake"
/bin/chmod +x "$malicious_qmake"
/bin/ln -s "$malicious_qmake" "$malicious_prefix/bin/qmake"
cat > "$malicious_prefix/.barrier-release-prefix-manifest" <<EOF
LOCK_SHA256=$builder_lock_sha
RECIPE_SHA256=$builder_recipe_sha
CACHE_ID=$builder_cache_id
MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET
MACOS_RELEASE_ARCH=$MACOS_RELEASE_ARCH
EOF
malicious_cache_log="$test_root/malicious-cache.log"
if "$builder" --prefix "$malicious_prefix" --work-dir "$malicious_work_dir" \
    --jobs 1 > "$malicious_cache_log" 2>&1; then
    printf 'expected malicious cached prefix rejection\n' >&2
    exit 1
fi
/usr/bin/grep -F 'absolute symlink target found: bin/qmake' \
    "$malicious_cache_log" >/dev/null
if test -e "$qmake_execution_marker"; then
    printf 'cached qmake executed before structural verification\n' >&2
    exit 1
fi
if /usr/bin/grep -F "$test_root" "$malicious_cache_log" >/dev/null; then
    printf 'cached-prefix verification exposed a local path\n' >&2
    exit 1
fi

printf 'macOS deployment-target verifier tests passed\n'
