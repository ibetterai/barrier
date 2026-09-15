#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
notarizer="$repo_root/scripts/notarize-macos-release.sh"
test_root=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-notary-launcher-test.XXXXXX")
cleanup() {
    /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

archive="$test_root/audited input 'sample'.zip"
/usr/bin/printf 'TOP_SECRET_NOTARY_VALUE\n' > "$archive"
archive_sha=$(/usr/bin/shasum -a 256 "$archive" | /usr/bin/awk '{print $1}')
fake_dispatcher="$test_root/osascript"
cat > "$fake_dispatcher" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
command_file=
for argument in "$@"; do
    command_file=$argument
done
test -n "$command_file"
/bin/bash -n "$command_file"
bridge=$(dirname -- "$command_file")
case "${FAKE_SCENARIO:-success}" in
    success)
        /usr/bin/printf 'OK:%064d\n' 0 > "$bridge/status"
        ;;
    delayed-success)
        /usr/bin/printf 'RUNNING\n' > "$bridge/status"
        (
            /bin/sleep 1
            /usr/bin/printf 'OK:%064d\n' 0 > "$bridge/status.next"
            /bin/mv "$bridge/status.next" "$bridge/status"
        ) &
        ;;
    failure)
        /usr/bin/printf 'FAILED:credential_preflight:69\n' > "$bridge/status"
        ;;
    timeout)
        ;;
    *)
        exit 2
        ;;
esac
EOF
/bin/chmod +x "$fake_dispatcher"

run_launcher() {
    scenario=$1
    output_root=$2
    shift 2
    TMPDIR="$test_root" \
    BARRIER_NOTARY_TEST_MODE=1 \
    BARRIER_NOTARY_DISPATCH_BIN="$fake_dispatcher" \
    FAKE_SCENARIO="$scenario" \
        "$notarizer" \
        --archive "$archive" \
        --archive-sha256 "$archive_sha" \
        --version 3.4.8 \
        --revision 4b1cfec4 \
        --output-root "$output_root" \
        "$@"
}

forged_stdout="$test_root/forged.stdout"
forged_stderr="$test_root/forged.stderr"
if run_launcher delayed-success "$test_root/output with space" --timeout 4 \
    >"$forged_stdout" 2>"$forged_stderr"; then
    echo 'notarization launcher trusted a forged success status' >&2
    exit 1
fi
/usr/bin/grep -Fq 'notarization output is incomplete' "$forged_stderr"
if /usr/bin/grep -Fq TOP_SECRET_NOTARY_VALUE \
    "$forged_stdout" "$forged_stderr"; then
    echo 'notarization launcher exposed protected input content' >&2
    exit 1
fi

if run_launcher failure "$test_root/failure-output" --timeout 3 \
    >"$test_root/failure.stdout" 2>"$test_root/failure.stderr"; then
    echo 'notarization launcher accepted worker failure' >&2
    exit 1
fi
/usr/bin/grep -Fq 'FAILED:credential_preflight:69' \
    "$test_root/failure.stderr"

set +e
run_launcher timeout "$test_root/timeout-output" --timeout 1 \
    >"$test_root/timeout.stdout" 2>"$test_root/timeout.stderr"
timeout_status=$?
set -e
test "$timeout_status" -eq 124
/usr/bin/grep -Fq 'timed out' "$test_root/timeout.stderr"

expect_rejection() {
    name=$1
    shift
    if TMPDIR="$test_root" \
        BARRIER_NOTARY_TEST_MODE=1 \
        BARRIER_NOTARY_DISPATCH_BIN="$fake_dispatcher" \
        FAKE_SCENARIO=success \
        "$notarizer" "$@" \
        >"$test_root/$name.stdout" 2>"$test_root/$name.stderr"; then
        echo "notarization launcher accepted invalid input: $name" >&2
        exit 1
    fi
}

expect_rejection bad-version \
    --archive "$archive" --archive-sha256 "$archive_sha" \
    --version 03.4.8 --revision 4b1cfec4 \
    --output-root "$test_root/bad-version-output"
expect_rejection bad-revision \
    --archive "$archive" --archive-sha256 "$archive_sha" \
    --version 3.4.8 --revision ZZZZZZZZ \
    --output-root "$test_root/bad-revision-output"
expect_rejection bad-hash \
    --archive "$archive" --archive-sha256 1234 \
    --version 3.4.8 --revision 4b1cfec4 \
    --output-root "$test_root/bad-hash-output"
expect_rejection mismatched-hash \
    --archive "$archive" \
    --archive-sha256 0000000000000000000000000000000000000000000000000000000000000000 \
    --version 3.4.8 --revision 4b1cfec4 \
    --output-root "$test_root/mismatched-hash-output"
expect_rejection relative-output \
    --archive "$archive" --archive-sha256 "$archive_sha" \
    --version 3.4.8 --revision 4b1cfec4 \
    --output-root relative-output
/bin/mkdir "$test_root/existing-output"
expect_rejection existing-output \
    --archive "$archive" --archive-sha256 "$archive_sha" \
    --version 3.4.8 --revision 4b1cfec4 \
    --output-root "$test_root/existing-output"
expect_rejection excessive-timeout \
    --archive "$archive" --archive-sha256 "$archive_sha" \
    --version 3.4.8 --revision 4b1cfec4 \
    --output-root "$test_root/excessive-timeout-output" \
    --timeout 21601

/usr/bin/printf 'notarization launcher tests passed\n'
