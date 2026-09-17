#!/usr/bin/env bash
# shellcheck disable=SC2129
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

usage() {
    cat >&2 <<'EOF'
usage: notarize-macos-release.sh \
  --archive FILE --archive-sha256 SHA256 \
  --version X.Y.Z --revision 8_HEX \
  --output-root DIR [--timeout SECONDS]

Builds a signed, notarized, stapled, and verified macOS arm64 release DMG.
The output root must be an absolute path that does not already exist.
EOF
    exit 2
}

script_dir=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
script_path="$script_dir/$(basename -- "$0")"
metadata_scanner="$script_dir/scan-protected-metadata.pl"
deployment_verifier="$script_dir/verify-macos-deployment-target.sh"
notary_profile=notarytool
maximum_timeout=21600

worker_active=0
fail() {
    message=$1
    code=${2:-1}
    echo "$message" >&2
    if test "$worker_active" = 1; then
        status_write "FAILED:$stage:$code"
    fi
    exit "$code"
}

validate_inputs() {
    case "$archive" in
        /*) ;;
        *) fail 'release archive path must be absolute' 2 ;;
    esac
    case "$output_root" in
        /) fail 'release output root cannot be /' 2 ;;
        /*) ;;
        *) fail 'release output root must be absolute' 2 ;;
    esac
    case "$archive$output_root" in
        *$'\n'*|*$'\r'*) fail 'release paths contain unsupported characters' 2 ;;
    esac
    [[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] \
        || fail 'release version is invalid' 2
    [[ "$revision" =~ ^[0-9a-f]{8}$ ]] \
        || fail 'release revision is invalid' 2
    [[ "$archive_sha256" =~ ^[0-9a-f]{64}$ ]] \
        || fail 'release archive SHA-256 is invalid' 2
    case "$timeout_seconds" in
        ''|*[!0-9]*|0) fail 'release timeout is invalid' 2 ;;
    esac
    test "$timeout_seconds" -le "$maximum_timeout" \
        || fail 'release timeout exceeds the supported maximum' 2
    test -f "$archive" || fail 'release archive is unavailable'
    test ! -e "$output_root" || fail 'release output root already exists'
    actual_archive_sha=$(/usr/bin/shasum -a 256 "$archive" \
        | /usr/bin/awk '{print $1}')
    test "$actual_archive_sha" = "$archive_sha256" \
        || fail 'release archive SHA-256 does not match'
}

status_write() {
    status_value=$1
    status_tmp="$status_file.tmp.$$"
    /usr/bin/printf '%s\n' "$status_value" > "$status_tmp"
    /bin/mv "$status_tmp" "$status_file"
}

run_worker() {
    test "$#" -eq 1 || fail 'notarization worker request is invalid' 2
    bridge=$1
    case "$bridge" in
        /) fail 'notarization bridge path is invalid' 2 ;;
        /*) ;;
        *) fail 'notarization bridge path is invalid' 2 ;;
    esac
    request_file="$bridge/request"
    status_file="$bridge/status"
    pid_file="$bridge/pid"
    test -f "$request_file" || fail 'notarization worker request is unavailable'
    test ! -e "$status_file" || fail 'notarization worker status already exists'

    stage=request_validation
    mount_dir=
    # Invoked by the ERR/HUP/INT/TERM trap below.
    # shellcheck disable=SC2329
    worker_error() {
        rc=$?
        if test -n "$mount_dir"; then
            /usr/bin/hdiutil detach "$mount_dir" >/dev/null 2>&1 || true
        fi
        status_write "FAILED:$stage:$rc"
        exit "$rc"
    }
    trap worker_error ERR HUP INT TERM
    worker_active=1
    /usr/bin/printf '%s\n' "$$" > "$pid_file"
    status_write RUNNING

    exec 3<"$request_file"
    IFS= read -r -d '' archive <&3 \
        || fail 'notarization worker archive is missing' 2
    IFS= read -r -d '' archive_sha256 <&3 \
        || fail 'notarization worker SHA-256 is missing' 2
    IFS= read -r -d '' version <&3 \
        || fail 'notarization worker version is missing' 2
    IFS= read -r -d '' revision <&3 \
        || fail 'notarization worker revision is missing' 2
    IFS= read -r -d '' output_root <&3 \
        || fail 'notarization worker output root is missing' 2
    IFS= read -r -d '' timeout_seconds <&3 \
        || fail 'notarization worker timeout is missing' 2
    exec 3<&-

    validate_inputs
    umask 077
    stage=initializing

    /bin/mkdir -m 700 "$output_root"
    stage_file="$output_root/release.stage"
    sign_log="$output_root/sign.log"
    app="$output_root/Barrier.app"
    app_zip="$output_root/Barrier-$version-macos-arm64-notary-input.zip"
    app_result="$output_root/app-notary-result.json"
    app_log="$output_root/app-notary-log.json"
    dmg_root="$output_root/dmg-root"
    dmg="$output_root/Barrier-$version-release-arm64.dmg"
    dmg_result="$output_root/dmg-notary-result.json"
    dmg_log="$output_root/dmg-notary-log.json"
    checksum="$output_root/Barrier-$version-release-arm64.dmg.sha256"
    worker_archive="$output_root/unsigned-release-input.zip"
    : > "$sign_log"

    set_stage() {
        stage=$1
        /usr/bin/printf '%s\n' "$stage" > "$stage_file"
    }

    set_stage extracting
    /usr/bin/ditto "$archive" "$worker_archive"
    worker_archive_sha=$(/usr/bin/shasum -a 256 "$worker_archive" \
        | /usr/bin/awk '{print $1}')
    test "$worker_archive_sha" = "$archive_sha256"
    archive=$worker_archive
    /usr/bin/ditto -x -k "$archive" "$output_root"
    test -d "$app/Contents"
    plist="$app/Contents/Info.plist"
    macos_dir="$app/Contents/MacOS"
    test "$(/usr/bin/plutil -extract CFBundleShortVersionString raw "$plist")" \
        = "$version"
    test "$(/usr/bin/plutil -extract CFBundleVersion raw "$plist")" \
        = "$version"
    test "$(/usr/bin/lipo -archs "$macos_dir/barrier")" = arm64
    /usr/bin/python3 -c \
        'import pathlib,sys; sys.exit(0 if sys.argv[2].encode() in pathlib.Path(sys.argv[1]).read_bytes() else 1)' \
        "$macos_dir/barrier" "$revision"
    /bin/bash "$deployment_verifier" \
        --app "$app" --target 12.0 --arch arm64 >>"$sign_log" 2>&1

    set_stage credential_preflight
    /usr/bin/xcrun notarytool history \
        --keychain-profile "$notary_profile" --output-format json >/dev/null
    identity_hashes=$(/usr/bin/security find-identity -v -p codesigning \
        | /usr/bin/sed -n \
            's/^[[:space:]]*[0-9][0-9]*) \([0-9A-F]\{40\}\) "Developer ID Application:.*$/\1/p')
    identity_count=$(/usr/bin/printf '%s\n' "$identity_hashes" \
        | /usr/bin/awk 'NF { count++ } END { print count + 0 }')
    test "$identity_count" -eq 1 \
        || fail 'exactly one valid Developer ID Application identity is required'
    identity=$identity_hashes
    /usr/bin/xattr -cr "$app"

    set_stage signing_app
    while IFS= read -r -d '' code; do
        if [[ "$(/usr/bin/file -b "$code")" == *Mach-O* ]]; then
            identifier=$(/usr/bin/codesign -d --verbose=4 "$code" 2>&1 \
                | /usr/bin/sed -n 's/^Identifier=//p')
            test -n "$identifier"
            /usr/bin/codesign --sign "$identity" \
                --identifier "$identifier" --force --options runtime \
                --timestamp "$code" >>"$sign_log" 2>&1
        fi
    done < <(/usr/bin/find "$app/Contents" -type f -print0)
    while IFS= read -r -d '' bundle; do
        /usr/bin/codesign --sign "$identity" --force --options runtime \
            --timestamp "$bundle" >>"$sign_log" 2>&1
    done < <(/usr/bin/find "$app/Contents" -depth -type d \
        \( -name '*.framework' -o -name '*.app' -o -name '*.xpc' \
        -o -name '*.appex' -o -name '*.bundle' \) -print0)
    /usr/bin/codesign --sign "$identity" --force --options runtime \
        --timestamp "$app" >>"$sign_log" 2>&1
    /usr/bin/codesign --verify --deep --strict "$app" >>"$sign_log" 2>&1

    set_stage notarizing_app
    /usr/bin/ditto -c -k --keepParent "$app" "$app_zip"
    /usr/bin/xcrun notarytool submit "$app_zip" \
        --keychain-profile "$notary_profile" --wait --timeout 45m \
        --no-progress --output-format json > "$app_result"
    test "$(/usr/bin/plutil -extract status raw -o - "$app_result")" \
        = Accepted
    app_notary_id=$(/usr/bin/plutil -extract id raw -o - "$app_result")
    /usr/bin/xcrun notarytool log "$app_notary_id" \
        --keychain-profile "$notary_profile" "$app_log" >/dev/null
    /usr/bin/xcrun stapler staple "$app" >>"$sign_log" 2>&1
    /usr/bin/xcrun stapler validate "$app" >>"$sign_log" 2>&1
    /usr/bin/codesign --verify --deep --strict "$app" >>"$sign_log" 2>&1
    /usr/sbin/spctl --assess --type execute "$app" >>"$sign_log" 2>&1
    /usr/bin/xcrun syspolicy_check distribution "$app" \
        >>"$sign_log" 2>&1

    set_stage creating_dmg
    /bin/mkdir "$dmg_root"
    /usr/bin/ditto "$app" "$dmg_root/Barrier.app"
    /bin/ln -s /Applications "$dmg_root/Applications"
    /usr/bin/hdiutil create -fs HFS+ -format UDZO \
        -volname "Barrier $version" -srcfolder "$dmg_root" "$dmg" \
        >>"$sign_log" 2>&1
    /usr/bin/codesign --sign "$identity" --force --timestamp "$dmg" \
        >>"$sign_log" 2>&1
    /usr/bin/codesign --verify "$dmg" >>"$sign_log" 2>&1

    set_stage notarizing_dmg
    /usr/bin/xcrun notarytool submit "$dmg" \
        --keychain-profile "$notary_profile" --wait --timeout 45m \
        --no-progress --output-format json > "$dmg_result"
    test "$(/usr/bin/plutil -extract status raw -o - "$dmg_result")" \
        = Accepted
    dmg_notary_id=$(/usr/bin/plutil -extract id raw -o - "$dmg_result")
    /usr/bin/xcrun notarytool log "$dmg_notary_id" \
        --keychain-profile "$notary_profile" "$dmg_log" >/dev/null
    /usr/bin/xcrun stapler staple "$dmg" >>"$sign_log" 2>&1
    /usr/bin/xcrun stapler validate "$dmg" >>"$sign_log" 2>&1
    /usr/bin/codesign --verify "$dmg" >>"$sign_log" 2>&1
    /usr/bin/hdiutil verify "$dmg" >>"$sign_log" 2>&1
    /usr/sbin/spctl --assess --type open \
        --context context:primary-signature "$dmg" >>"$sign_log" 2>&1

    set_stage verifying_dmg
    mount_dir=$(/usr/bin/mktemp -d \
        "${TMPDIR:-/tmp}/barrier-release-mount.XXXXXX")
    /usr/bin/hdiutil attach -readonly -nobrowse \
        -mountpoint "$mount_dir" "$dmg" >>"$sign_log" 2>&1
    mounted_app="$mount_dir/Barrier.app"
    mounted_macos="$mounted_app/Contents/MacOS"
    test "$(/usr/bin/readlink "$mount_dir/Applications")" = /Applications
    test "$(/usr/bin/plutil -extract CFBundleShortVersionString raw \
        "$mounted_app/Contents/Info.plist")" = "$version"
    test "$(/usr/bin/plutil -extract CFBundleVersion raw \
        "$mounted_app/Contents/Info.plist")" = "$version"
    test "$(/usr/bin/lipo -archs "$mounted_macos/barrier")" = arm64
    /usr/bin/python3 -c \
        'import pathlib,sys; sys.exit(0 if sys.argv[2].encode() in pathlib.Path(sys.argv[1]).read_bytes() else 1)' \
        "$mounted_macos/barrier" "$revision"
    /usr/bin/codesign --verify --deep --strict "$mounted_app" \
        >>"$sign_log" 2>&1
    /usr/bin/xcrun stapler validate "$mounted_app" >>"$sign_log" 2>&1
    /usr/sbin/spctl --assess --type execute "$mounted_app" \
        >>"$sign_log" 2>&1
    /usr/bin/xcrun syspolicy_check distribution "$mounted_app" \
        >>"$sign_log" 2>&1
    /bin/bash "$deployment_verifier" \
        --app "$mounted_app" --target 12.0 --arch arm64 \
        >>"$sign_log" 2>&1
    /usr/bin/hdiutil detach "$mount_dir" >>"$sign_log" 2>&1
    mount_dir=

    dmg_sha=$(/usr/bin/shasum -a 256 "$dmg" \
        | /usr/bin/awk '{print $1}')
    /usr/bin/printf '%s  %s\n' "$dmg_sha" "${dmg##*/}" > "$checksum"
    set_stage complete
    status_write "OK:$dmg_sha"
    trap - ERR HUP INT TERM
}

if test "${1:-}" = --worker; then
    shift
    run_worker "$@"
    exit 0
fi

archive=
archive_sha256=
version=
revision=
output_root=
timeout_seconds=7200
while test "$#" -gt 0; do
    case "$1" in
        --archive)
            test "$#" -ge 2 || usage
            archive=$2
            shift 2
            ;;
        --archive-sha256)
            test "$#" -ge 2 || usage
            archive_sha256=$2
            shift 2
            ;;
        --version)
            test "$#" -ge 2 || usage
            version=$2
            shift 2
            ;;
        --revision)
            test "$#" -ge 2 || usage
            revision=$2
            shift 2
            ;;
        --output-root)
            test "$#" -ge 2 || usage
            output_root=$2
            shift 2
            ;;
        --timeout)
            test "$#" -ge 2 || usage
            timeout_seconds=$2
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            usage
            ;;
    esac
done

test -n "$archive" && test -n "$archive_sha256" \
    && test -n "$version" && test -n "$revision" \
    && test -n "$output_root" || usage
validate_inputs
test "$(/usr/bin/uname -s)" = Darwin \
    || fail 'release notarization requires macOS'
test -x /usr/bin/osascript && test -x /usr/bin/ditto \
    && test -x /usr/bin/codesign && test -x /usr/bin/xcrun \
    || fail 'required macOS release tools are unavailable'
test -f "$metadata_scanner" && test -f "$deployment_verifier" \
    || fail 'release verification helpers are unavailable'

bridge=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-notary-bridge.XXXXXX")
/bin/chmod 700 "$bridge"
staged_archive="$bridge/unsigned-release-input.zip"
/usr/bin/ditto "$archive" "$staged_archive"
/bin/chmod 600 "$staged_archive"
staged_archive_sha=$(/usr/bin/shasum -a 256 "$staged_archive" \
    | /usr/bin/awk '{print $1}')
test "$staged_archive_sha" = "$archive_sha256" \
    || fail 'private release archive copy does not match'
request_file="$bridge/request"
status_file="$bridge/status"
pid_file="$bridge/pid"
command_file="$bridge/notarize.command"
automation_file="$bridge/dispatch.scpt"
/usr/bin/printf '%s\0%s\0%s\0%s\0%s\0%s\0' \
    "$staged_archive" "$archive_sha256" "$version" "$revision" \
    "$output_root" "$timeout_seconds" > "$request_file"
/bin/chmod 600 "$request_file"
printf '#!/bin/bash\nexec /bin/bash %q --worker %q\n' \
    "$script_path" "$bridge" > "$command_file"
/bin/chmod 700 "$command_file"
cat > "$automation_file" <<'APPLESCRIPT'
on run arguments
    set commandPath to item 1 of arguments
    tell application "Terminal"
        do script "/bin/bash " & quoted form of commandPath
    end tell
end run
APPLESCRIPT
/bin/chmod 600 "$automation_file"

dispatcher=/usr/bin/osascript
test_mode=${BARRIER_NOTARY_TEST_MODE:-0}
case "$test_mode" in
    0) ;;
    1)
        test -n "${BARRIER_NOTARY_DISPATCH_BIN:-}" \
            || fail 'test dispatcher is unavailable' 2
        dispatcher=$BARRIER_NOTARY_DISPATCH_BIN
        test -x "$dispatcher" || fail 'test dispatcher is not executable' 2
        ;;
    *) fail 'invalid notarization test mode' 2 ;;
esac

start_seconds=$SECONDS
dispatcher_pid=
terminate_worker() {
    if test -f "$pid_file"; then
        worker_pid=$(/bin/cat "$pid_file")
        case "$worker_pid" in
            ''|*[!0-9]*) ;;
            *) /bin/kill -TERM "$worker_pid" >/dev/null 2>&1 || true ;;
        esac
    fi
}
timeout_release() {
    timeout_stage=$1
    if test -n "$dispatcher_pid"; then
        /bin/kill -TERM "$dispatcher_pid" >/dev/null 2>&1 || true
        wait "$dispatcher_pid" >/dev/null 2>&1 || true
    fi
    terminate_worker
    fail "$timeout_stage timed out; bridge retained at $bridge" 124
}

"$dispatcher" "$automation_file" "$command_file" >/dev/null &
dispatcher_pid=$!
while /bin/kill -0 "$dispatcher_pid" >/dev/null 2>&1; do
    elapsed=$((SECONDS - start_seconds))
    if test "$elapsed" -ge "$timeout_seconds"; then
        timeout_release 'Terminal automation dispatch'
    fi
    /bin/sleep 1
done
set +e
wait "$dispatcher_pid"
dispatcher_status=$?
set -e
dispatcher_pid=
test "$dispatcher_status" -eq 0 \
    || fail "Terminal automation dispatch failed; bridge retained at $bridge"

while test ! -f "$status_file"; do
    elapsed=$((SECONDS - start_seconds))
    if test "$elapsed" -ge "$timeout_seconds"; then
        timeout_release 'notarization worker'
    fi
    /bin/sleep 1
done

worker_status=$(/bin/cat "$status_file")
case "$worker_status" in
    RUNNING)
        while test "$worker_status" = RUNNING; do
            elapsed=$((SECONDS - start_seconds))
            if test "$elapsed" -ge "$timeout_seconds"; then
                timeout_release 'notarization worker'
            fi
            /bin/sleep 1
            worker_status=$(/bin/cat "$status_file")
        done
        ;;
esac

case "$worker_status" in
    OK:[0-9a-f][0-9a-f]*)
        dmg_sha=${worker_status#OK:}
        [[ "$dmg_sha" =~ ^[0-9a-f]{64}$ ]] \
            || fail "notarization worker returned invalid status; bridge retained at $bridge"
        dmg="$output_root/Barrier-$version-release-arm64.dmg"
        checksum="$output_root/Barrier-$version-release-arm64.dmg.sha256"
        test -f "$dmg" && test -f "$checksum" \
            || fail "notarization output is incomplete; bridge retained at $bridge"
        actual_dmg_sha=$(/usr/bin/shasum -a 256 "$dmg" \
            | /usr/bin/awk '{print $1}')
        test "$actual_dmg_sha" = "$dmg_sha" \
            || fail "notarization output SHA-256 does not match; bridge retained at $bridge"
        /usr/bin/codesign --verify "$dmg" >/dev/null 2>&1
        /usr/bin/xcrun stapler validate "$dmg" >/dev/null 2>&1
        /usr/bin/hdiutil verify "$dmg" >/dev/null 2>&1
        /usr/sbin/spctl --assess --type open \
            --context context:primary-signature "$dmg" >/dev/null 2>&1
        /bin/rm -rf "$bridge"
        /usr/bin/printf 'NOTARIZED_DMG_SHA256=%s\n' "$dmg_sha"
        /usr/bin/printf 'NOTARIZED_OUTPUT_ROOT=%s\n' "$output_root"
        ;;
    FAILED:*)
        fail "notarization worker ${worker_status}; bridge retained at $bridge"
        ;;
    *)
        fail "notarization worker returned invalid status; bridge retained at $bridge"
        ;;
esac
