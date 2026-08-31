#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
verifier="$repo_root/scripts/verify-release-checks.sh"
test_root=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-release-check-test.XXXXXX")

cleanup() {
    /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

fake_gh="$test_root/gh"
# The following single-quoted lines are intentionally literal script source.
# shellcheck disable=SC2016
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'set -euo pipefail' \
    'sha=' \
    'saw_branch=0' \
    'saw_event=0' \
    'saw_status=0' \
    'for argument in "$@"; do' \
    '  case "$argument" in' \
    '    head_sha=*) sha=${argument#head_sha=} ;;' \
    '    branch=main) saw_branch=1 ;;' \
    '    event=push) saw_event=1 ;;' \
    '    status=completed) saw_status=1 ;;' \
    '  esac' \
    'done' \
    'test "$saw_branch:$saw_event:$saw_status" = 1:1:1' \
    'case "${FAKE_SCENARIO:-success}" in' \
    '  success) printf "%s\tmain\tpush\tcompleted\tsuccess\n" "$sha" ;;' \
    '  pr) printf "%s\tmain\tpull_request\tcompleted\tsuccess\n" "$sha" ;;' \
    '  pending) printf "%s\tmain\tpush\tin_progress\t\n" "$sha" ;;' \
    '  failure) printf "%s\tmain\tpush\tcompleted\tfailure\n" "$sha" ;;' \
    '  branch) printf "%s\tfeature\tpush\tcompleted\tsuccess\n" "$sha" ;;' \
    '  wrong-sha) printf "%040d\tmain\tpush\tcompleted\tsuccess\n" 0 ;;' \
    '  empty) exit 0 ;;' \
    '  api-failure) exit 1 ;;' \
    '  *) exit 2 ;;' \
    'esac' \
    > "$fake_gh"
/bin/chmod +x "$fake_gh"

first_sha=1111111111111111111111111111111111111111
second_sha=2222222222222222222222222222222222222222
BARRIER_GH_BIN="$fake_gh" "$verifier" \
    public-owner/public-repo "$first_sha" "$second_sha" >/dev/null

for scenario in pr pending failure branch wrong-sha empty api-failure; do
    if FAKE_SCENARIO="$scenario" BARRIER_GH_BIN="$fake_gh" \
        "$verifier" public-owner/public-repo "$first_sha" \
        > "$test_root/$scenario.stdout" 2> "$test_root/$scenario.stderr"; then
        echo 'release-check verifier accepted a non-current check' >&2
        exit 1
    fi
done

for invalid_sha in bad 1234 ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ; do
    if BARRIER_GH_BIN="$fake_gh" \
        "$verifier" public-owner/public-repo "$invalid_sha" >/dev/null 2>&1; then
        echo 'release-check verifier accepted an invalid commit' >&2
        exit 1
    fi
done

for invalid_repo in /absolute owner/repo/extra owner@host/repo ''; do
    if BARRIER_GH_BIN="$fake_gh" \
        "$verifier" "$invalid_repo" "$first_sha" >/dev/null 2>&1; then
        echo 'release-check verifier accepted an invalid repository' >&2
        exit 1
    fi
done

printf 'release workflow-run verifier tests passed\n'
