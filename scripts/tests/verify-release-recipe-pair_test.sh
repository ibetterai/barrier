#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
verifier="$repo_root/scripts/verify-release-recipe-pair.py"
automation_workflow="$repo_root/.github/workflows/release-macos-arm64.yml"
test_root=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-recipe-pair-test.XXXXXX")

cleanup() {
    /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

source_workflow="$test_root/source.yml"
if ! git -C "$repo_root" show \
    v3.4.0:.github/workflows/release-macos-arm64.yml \
    > "$source_workflow"; then
    echo 'unable to prepare tagged release workflow fixture' >&2
    exit 1
fi

# The v3.4.0 automation side is the reviewed post-#52 workflow, pinned from
# the v3.4.1 tree (byte-identical to the audited file, unlike the working
# tree once a newer driver lands).
automation_workflow_340="$test_root/automation-340.yml"
if ! git -C "$repo_root" show \
    v3.4.1:.github/workflows/release-macos-arm64.yml \
    > "$automation_workflow_340"; then
    echo 'unable to prepare v3.4.0 automation workflow fixture' >&2
    exit 1
fi

/usr/bin/python3 "$verifier" \
    --release-tag v3.4.0 \
    --source-workflow "$source_workflow" \
    --automation-workflow "$automation_workflow_340" >/dev/null

mutated_source="$test_root/mutated-source.yml"
/bin/cp "$source_workflow" "$mutated_source"
printf '\n# unknown source field\n' >> "$mutated_source"
if /usr/bin/python3 "$verifier" \
    --release-tag v3.4.0 \
    --source-workflow "$mutated_source" \
    --automation-workflow "$automation_workflow_340" >/dev/null 2>&1; then
    echo 'release-recipe verifier accepted source workflow drift' >&2
    exit 1
fi

mutated_automation="$test_root/mutated-automation.yml"
/bin/cp "$automation_workflow_340" "$mutated_automation"
printf '\n# unknown automation field\n' >> "$mutated_automation"
if /usr/bin/python3 "$verifier" \
    --release-tag v3.4.0 \
    --source-workflow "$source_workflow" \
    --automation-workflow "$mutated_automation" >/dev/null 2>&1; then
    echo 'release-recipe verifier accepted automation workflow drift' >&2
    exit 1
fi

if /usr/bin/python3 "$verifier" \
    --release-tag v3.4.1 \
    --source-workflow "$source_workflow" \
    --automation-workflow "$automation_workflow_340" >/dev/null 2>&1; then
    echo 'release-recipe verifier accepted an unaudited tag' >&2
    exit 1
fi

source_workflow_346="$test_root/source-346.yml"
if ! git -C "$repo_root" show \
    v3.4.6:.github/workflows/release-macos-arm64.yml \
    > "$source_workflow_346"; then
    echo 'unable to prepare v3.4.6 tagged release workflow fixture' >&2
    exit 1
fi

# The v3.4.6 automation side is the file from its automation tag.  The
# working tree file has moved on (it now carries the next release pins),
# so it must not be used as this row's fixture.
automation_workflow_346="$test_root/automation-346.yml"
if ! git -C "$repo_root" show \
    v3.4.6-automation.1:.github/workflows/release-macos-arm64.yml \
    > "$automation_workflow_346"; then
    echo 'unable to prepare v3.4.6 automation workflow fixture' >&2
    exit 1
fi

/usr/bin/python3 "$verifier" \
    --release-tag v3.4.6 \
    --source-workflow "$source_workflow_346" \
    --automation-workflow "$automation_workflow_346" >/dev/null

mutated_automation_346="$test_root/mutated-automation-346.yml"
/bin/cp "$automation_workflow_346" "$mutated_automation_346"
printf '\n# unknown automation field\n' >> "$mutated_automation_346"
if /usr/bin/python3 "$verifier" \
    --release-tag v3.4.6 \
    --source-workflow "$source_workflow_346" \
    --automation-workflow "$mutated_automation_346" >/dev/null 2>&1; then
    echo 'release-recipe verifier accepted automation workflow drift' >&2
    exit 1
fi

# v3.4.7 has no tag yet (it lands after this change merges), so both
# fixtures are the working tree file: the tag tree will carry byte-identical
# content by construction, and resolve() enforces it at release time.
/usr/bin/python3 "$verifier" \
    --release-tag v3.4.7 \
    --source-workflow "$automation_workflow" \
    --automation-workflow "$automation_workflow" >/dev/null

mutated_automation_347="$test_root/mutated-automation-347.yml"
/bin/cp "$automation_workflow" "$mutated_automation_347"
printf '\n# unknown automation field\n' >> "$mutated_automation_347"
if /usr/bin/python3 "$verifier" \
    --release-tag v3.4.7 \
    --source-workflow "$automation_workflow" \
    --automation-workflow "$mutated_automation_347" >/dev/null 2>&1; then
    echo 'release-recipe verifier accepted automation workflow drift' >&2
    exit 1
fi

if /usr/bin/python3 "$verifier" \
    --release-tag v3.4.0 \
    --source-workflow "$test_root/missing.yml" \
    --automation-workflow "$automation_workflow_340" >/dev/null 2>&1; then
    echo 'release-recipe verifier accepted a missing workflow' >&2
    exit 1
fi

printf 'release workflow-pair fingerprint tests passed\n'
