#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo 'release-check verification requires a repository and commit' >&2
    exit 2
fi

repository=$1
shift
case "$repository" in
    *[!A-Za-z0-9_./-]*|*/*/*|/*|*/|'')
        echo 'release-check verification received an invalid repository' >&2
        exit 2
        ;;
esac

gh_bin=${BARRIER_GH_BIN:-gh}
if ! command -v "$gh_bin" >/dev/null 2>&1; then
    echo 'release-check verification is unavailable' >&2
    exit 2
fi

for release_sha in "$@"; do
    if [[ ! "$release_sha" =~ ^[0-9a-f]{40}$ ]]; then
        echo 'release-check verification received an invalid commit' >&2
        exit 2
    fi
    for workflow in ci.yml public-audit.yml; do
        run_record=
        if ! run_record=$("$gh_bin" api \
            "repos/$repository/actions/workflows/$workflow/runs" \
            --method GET \
            -f head_sha="$release_sha" \
            -f branch=main \
            -f event=push \
            -f status=completed \
            -f per_page=20 \
            --jq '.workflow_runs | sort_by(.created_at) | reverse | .[0] | select(. != null) | [.head_sha, .head_branch, .event, .status, .conclusion] | @tsv' \
            2>/dev/null); then
            echo 'release-check query failed' >&2
            exit 1
        fi
        IFS=$'\t' read -r head_sha head_branch event status conclusion \
            <<< "$run_record"
        if [ "$head_sha" != "$release_sha" ] \
            || [ "$head_branch" != main ] \
            || [ "$event" != push ] \
            || [ "$status" != completed ] \
            || [ "$conclusion" != success ]; then
            echo 'required release check is not a current successful main push' >&2
            exit 1
        fi
    done
done

printf 'release checks verified\n'
