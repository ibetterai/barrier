#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
helper="$repo_root/scripts/verify-release-tag.py"
test_root=$(/usr/bin/mktemp -d \
  "${TMPDIR:-/tmp}/barrier-release-tag-test.XXXXXX")

cleanup() {
    /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

export GIT_AUTHOR_NAME='Barrier Release Fixture'
export GIT_AUTHOR_EMAIL='release-fixture@example.invalid'
export GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME"
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL=/dev/null

case_number=0

fail() {
  echo "release tag verifier test failed: $1" >&2
  exit 1
}

expect_failure() {
  local label=$1
  local category=$2
  shift 2
  case_number=$((case_number + 1))
  local stdout_path="$test_root/failure-$case_number.stdout"
  local stderr_path="$test_root/failure-$case_number.stderr"

  if "$@" >"$stdout_path" 2>"$stderr_path"; then
    fail "$label unexpectedly succeeded"
  fi
  if test -s "$stdout_path"; then
    fail "$label emitted stdout"
  fi
  if ! /usr/bin/grep -Fqx "verify-release-tag: $category" "$stderr_path"; then
    fail "$label returned the wrong safe failure category"
  fi
}

expect_success() {
  local label=$1
  local output_path=$2
  shift 2
  local stderr_path="$output_path.stderr"

  if ! "$@" >"$output_path" 2>"$stderr_path"; then
    /usr/bin/grep -E '^verify-release-tag: [a-z0-9-]+$' \
      "$stderr_path" >&2 || true
    fail "$label failed"
  fi
  if test -s "$stderr_path"; then
    fail "$label emitted stderr"
  fi
  if /usr/bin/grep -Ev \
      '^[A-Z][A-Z0-9_]*=[A-Za-z0-9._:/+@-]+$' "$output_path" >/dev/null; then
    fail "$label emitted an unsafe output record"
  fi
}

output_value() {
  local key=$1
  local output_path=$2
  /usr/bin/awk -F= -v key="$key" '$1 == key { print $2 }' "$output_path"
}

init_work_repo() {
  local path=$1
  git init -q -b main "$path"
}

init_bare_repo() {
  local path=$1
  git init -q --bare "$path"
  git --git-dir="$path" symbolic-ref HEAD refs/heads/main
}

write_version() {
  local repo=$1
  local major=$2
  local minor=$3
  local patch=$4
  /bin/mkdir -p "$repo/cmake"
  {
    printf 'cmake_minimum_required (VERSION 3.4)\n\n'
    printf 'set (BARRIER_VERSION_MAJOR %s)\n' "$major"
    printf 'set (BARRIER_VERSION_MINOR %s)\n' "$minor"
    printf 'set (BARRIER_VERSION_PATCH %s)\n' "$patch"
    printf 'set (BARRIER_VERSION_STAGE "release")\n'
  } >"$repo/cmake/Version.cmake"
}

public_submodule_url='https://github.''com/example/barrier-release-fixture.git'
public_nested_url='https://github.''com/example/barrier-nested-fixture.git'
unsafe_submodule_url='git''@github.com:example/barrier-release-fixture.git'

# Build a release history with an annotated source tag followed by automation.
release_remote="$test_root/release-remote.git"
release_work="$test_root/release-work"
init_bare_repo "$release_remote"
init_work_repo "$release_work"
write_version "$release_work" 1 2 3
printf 'release source\n' >"$release_work/README.md"
git -C "$release_work" add cmake/Version.cmake README.md
git -C "$release_work" commit -q -m 'release source fixture'
release_source_sha=$(git -C "$release_work" rev-parse HEAD)
release_version_sha=$(/usr/bin/shasum -a 256 \
  "$release_work/cmake/Version.cmake" | /usr/bin/awk '{print $1}')
git -C "$release_work" tag -a v1.2.3 -m 'release 1.2.3' "$release_source_sha"
printf 'automation only\n' >>"$release_work/README.md"
git -C "$release_work" add README.md
git -C "$release_work" commit -q -m 'automation fixture'
automation_sha=$(git -C "$release_work" rev-parse HEAD)
automation_tag=v1.2.3-automation.1
git -C "$release_work" tag -a "$automation_tag" \
  -m 'release automation 1.2.3' "$automation_sha"
git -C "$release_work" remote add origin "$release_remote"
git -C "$release_work" push -q origin main --tags
automation_tag_object_sha=$(git -C "$release_work" rev-parse "$automation_tag")
# actions/checkout replaces the local annotated tag ref with the peeled commit
# while leaving the protected remote annotated tag intact.
git -C "$release_work" update-ref \
  "refs/tags/$automation_tag" "$automation_sha"

resolve_output="$test_root/resolve.out"
expect_success resolve "$resolve_output" /usr/bin/python3 "$helper" resolve \
  --repo "$release_work" \
  --remote origin \
  --release-tag v1.2.3 \
  --automation-tag "$automation_tag" \
  --automation-sha "$automation_sha" \
  --version-file-sha256 "$release_version_sha"

test "$(output_value RELEASE_TAG "$resolve_output")" = v1.2.3 \
  || fail 'resolve returned the wrong tag'
test "$(output_value RELEASE_VERSION "$resolve_output")" = 1.2.3 \
  || fail 'resolve returned the wrong version'
test "$(output_value SOURCE_SHA "$resolve_output")" = "$release_source_sha" \
  || fail 'resolve returned the wrong source SHA'
test "$(output_value AUTOMATION_SHA "$resolve_output")" = "$automation_sha" \
  || fail 'resolve returned the wrong automation SHA'
test "$(output_value AUTOMATION_TAG_OBJECT_SHA "$resolve_output")" = \
  "$automation_tag_object_sha" \
  || fail 'resolve returned the wrong remote automation tag object'
tag_object_sha=$(output_value TAG_OBJECT_SHA "$resolve_output")

recheck_output="$test_root/recheck.out"
expect_success recheck "$recheck_output" /usr/bin/python3 "$helper" recheck \
  --repo "$release_work" \
  --remote origin \
  --release-tag v1.2.3 \
  --tag-object-sha "$tag_object_sha" \
  --source-sha "$release_source_sha" \
  --automation-tag "$automation_tag" \
  --automation-tag-object-sha "$automation_tag_object_sha" \
  --automation-sha "$automation_sha"
test "$(output_value REMOTE_TAG_STABLE "$recheck_output")" = 1 \
  || fail 'recheck did not report a stable tag'

# Reject a shell-shaped tag without reflecting or executing it.
injection_marker="$test_root/injection-marker"
injection_tag='v1.2.3;to''uch '"$injection_marker"
expect_failure injection invalid-release-tag /usr/bin/python3 "$helper" resolve \
  --repo "$release_work" \
  --remote origin \
  --release-tag "$injection_tag" \
  --automation-tag "$automation_tag" \
  --automation-sha "$automation_sha" \
  --version-file-sha256 "$release_version_sha"
test ! -e "$injection_marker" || fail 'malformed tag executed as shell input'

# Reject a lightweight tag and a tag whose semantic version differs from source.
git -C "$release_work" tag v1.2.4 "$release_source_sha"
git -C "$release_work" push -q origin refs/tags/v1.2.4
expect_failure lightweight local-annotated-tag /usr/bin/python3 "$helper" resolve \
  --repo "$release_work" --remote origin --release-tag v1.2.4 \
  --automation-tag "$automation_tag" \
  --automation-sha "$automation_sha" \
  --version-file-sha256 "$release_version_sha"

git -C "$release_work" tag -a v9.9.9 -m 'wrong version' "$release_source_sha"
git -C "$release_work" push -q origin refs/tags/v9.9.9
expect_failure wrong-version source-version-mismatch \
  /usr/bin/python3 "$helper" resolve \
  --repo "$release_work" --remote origin --release-tag v9.9.9 \
  --automation-tag "$automation_tag" \
  --automation-sha "$automation_sha" \
  --version-file-sha256 "$release_version_sha"

# The workflow pins the reviewed version file, so textual duplicate,
# conditional, and bracket-comment decoys cannot alter what is built.
expect_version_drift_failure() {
  local label=$1
  local patch=$2
  local mutation=$3
  local work="$test_root/version-drift-$label"
  local tag="v1.2.$patch"
  local version_file="$work/cmake/Version.cmake"
  local original="$work/cmake/Version.original.cmake"

  git clone -q "$release_remote" "$work"
  git -C "$work" checkout -q --detach "$release_source_sha"
  write_version "$work" 1 2 "$patch"
  local reviewed_sha
  reviewed_sha=$(/usr/bin/shasum -a 256 "$version_file" \
    | /usr/bin/awk '{print $1}')

  case "$mutation" in
    duplicate)
      printf 'set (BARRIER_VERSION_MAJOR 9)\n' >>"$version_file"
      ;;
    conditional)
      /bin/mv "$version_file" "$original"
      {
        printf 'if (FALSE)\n'
        printf '  set (BARRIER_VERSION_MAJOR 1)\n'
        printf 'endif()\n'
        /usr/bin/sed \
          's/^set (BARRIER_VERSION_MAJOR 1)$/set (BARRIER_VERSION_MAJOR 9)/' \
          "$original"
      } >"$version_file"
      ;;
    comment)
      /bin/mv "$version_file" "$original"
      {
        printf '#[[\n'
        printf 'set (BARRIER_VERSION_MAJOR 1)\n'
        printf ']]\n'
        /usr/bin/sed \
          's/^set (BARRIER_VERSION_MAJOR 1)$/set (BARRIER_VERSION_MAJOR 9)/' \
          "$original"
      } >"$version_file"
      ;;
    *)
      fail 'unknown version drift fixture'
      ;;
  esac

  git -C "$work" add cmake/Version.cmake
  git -C "$work" commit -q -m "version drift $label fixture"
  git -C "$work" tag -a "$tag" -m "version drift $label"
  git -C "$work" push -q origin "refs/tags/$tag"
  git -C "$release_work" fetch -q origin "refs/tags/$tag:refs/tags/$tag"
  expect_failure "version-$label" source-version-digest \
    /usr/bin/python3 "$helper" resolve \
    --repo "$release_work" --remote origin --release-tag "$tag" \
    --automation-tag "$automation_tag" \
    --automation-sha "$automation_sha" \
    --version-file-sha256 "$reviewed_sha"
}

expect_version_drift_failure duplicate 5 duplicate
expect_version_drift_failure conditional 6 conditional
expect_version_drift_failure comment 7 comment

# A correctly versioned side-history tag still cannot be released from main.
side_work="$test_root/side-work"
git clone -q --no-checkout "$release_remote" "$side_work"
git -C "$side_work" checkout -q --orphan side-release
git -C "$side_work" rm -q -rf .
write_version "$side_work" 2 0 0
printf 'side history\n' >"$side_work/README.md"
git -C "$side_work" add cmake/Version.cmake README.md
git -C "$side_work" commit -q -m 'side release fixture'
side_version_sha=$(/usr/bin/shasum -a 256 \
  "$side_work/cmake/Version.cmake" | /usr/bin/awk '{print $1}')
git -C "$side_work" tag -a v2.0.0 -m 'side release'
git -C "$side_work" push -q origin refs/tags/v2.0.0
git -C "$release_work" fetch -q origin \
  refs/tags/v2.0.0:refs/tags/v2.0.0
expect_failure non-main source-ancestry /usr/bin/python3 "$helper" resolve \
  --repo "$release_work" --remote origin --release-tag v2.0.0 \
  --automation-tag "$automation_tag" \
  --automation-sha "$automation_sha" \
  --version-file-sha256 "$side_version_sha"

# Create public-safe nested and top-level submodule repositories.
nested_remote="$test_root/nested-remote.git"
nested_work="$test_root/nested-work"
init_bare_repo "$nested_remote"
init_work_repo "$nested_work"
printf 'nested source\n' >"$nested_work/nested.txt"
git -C "$nested_work" add nested.txt
git -C "$nested_work" commit -q -m 'nested fixture'
git -C "$nested_work" remote add origin "$nested_remote"
git -C "$nested_work" push -q origin main

nested_contact_marker="$test_root/nested-contacted"
marker_transport="$test_root/marker-transport"
{
  printf '#!/usr/bin/env bash\n'
  printf '/usr/bin/touch %q\n' "$nested_contact_marker"
  printf 'exec git-upload-pack "$%s"\n' 1
} >"$marker_transport"
/bin/chmod 700 "$marker_transport"
unsafe_nested_url="ext::$marker_transport $nested_remote"
GIT_ALLOW_PROTOCOL=ext git ls-remote "$unsafe_nested_url" >/dev/null
test -e "$nested_contact_marker" \
  || fail 'nested marker transport fixture did not execute'
/bin/rm -f "$nested_contact_marker"

submodule_remote="$test_root/submodule-remote.git"
submodule_work="$test_root/submodule-work"
init_bare_repo "$submodule_remote"
init_work_repo "$submodule_work"
printf 'submodule source\n' >"$submodule_work/module.txt"
git -C "$submodule_work" add module.txt
git -C "$submodule_work" commit -q -m 'submodule fixture'
submodule_source_sha=$(git -C "$submodule_work" rev-parse HEAD)
git -C "$submodule_work" remote add origin "$submodule_remote"
git -C "$submodule_work" push -q origin main

# Preserve a branch whose nested URL is unsafe for the recursive negative path.
git -C "$submodule_work" switch -q -c unsafe-nested
git -C "$submodule_work" \
  -c protocol.file.allow=always \
  -c "url.file://${nested_remote}.insteadOf=${public_nested_url}" \
  submodule add -q "$public_nested_url" nested/item
git -C "$submodule_work" config -f .gitmodules \
  submodule.nested/item.url "$unsafe_nested_url"
git -C "$submodule_work" add .gitmodules nested/item
git -C "$submodule_work" commit -q -m 'unsafe nested URL fixture'
unsafe_nested_submodule_sha=$(git -C "$submodule_work" rev-parse HEAD)
git -C "$submodule_work" push -q origin unsafe-nested
git -C "$submodule_work" switch -q main
printf 'second submodule commit\n' >>"$submodule_work/module.txt"
git -C "$submodule_work" add module.txt
git -C "$submodule_work" commit -q -m 'mismatch fixture'
submodule_mismatch_sha=$(git -C "$submodule_work" rev-parse HEAD)
git -C "$submodule_work" push -q origin main

source_remote="$test_root/source-remote.git"
source_work="$test_root/source-work"
init_bare_repo "$source_remote"
init_work_repo "$source_work"
write_version "$source_work" 3 4 0
printf 'source checkout fixture\n' >"$source_work/README.md"
git -C "$source_work" add cmake/Version.cmake README.md
git -C "$source_work" \
  -c protocol.file.allow=always \
  -c "url.file://${submodule_remote}.insteadOf=${public_submodule_url}" \
  submodule add -q "$public_submodule_url" deps/example
git -C "$source_work/deps/example" checkout -q "$submodule_source_sha"
git -C "$source_work" add .gitmodules deps/example
git -C "$source_work" commit -q -m 'source with public submodule fixture'
verified_source_sha=$(git -C "$source_work" rev-parse HEAD)
printf 'later source commit\n' >>"$source_work/README.md"
git -C "$source_work" add README.md
git -C "$source_work" commit -q -m 'wrong HEAD fixture'
wrong_head_sha=$(git -C "$source_work" rev-parse HEAD)
git -C "$source_work" remote add origin "$source_remote"
git -C "$source_work" push -q origin main

source_checkout="$test_root/source-checkout"
git clone -q --no-recurse-submodules "$source_remote" "$source_checkout"
git -C "$source_checkout" checkout -q --detach "$verified_source_sha"

pre_output="$test_root/pre-init.out"
expect_success pre-init "$pre_output" /usr/bin/python3 "$helper" verify-source \
  --source-dir "$source_checkout" \
  --source-sha "$verified_source_sha" \
  --phase pre-init
test "$(output_value SUBMODULE_COUNT "$pre_output")" = 1 \
  || fail 'pre-init returned the wrong submodule count'
test "$(output_value SUBMODULE_000_SHA "$pre_output")" = \
  "$submodule_source_sha" || fail 'pre-init returned the wrong gitlink'

expect_failure missing-submodule submodule-state \
  /usr/bin/python3 "$helper" verify-source \
  --source-dir "$source_checkout" --source-sha "$verified_source_sha" \
  --phase post-init

test_git_config="$test_root/gitconfig"
git config --file "$test_git_config" protocol.file.allow always
git config --file "$test_git_config" \
  "url.file://${submodule_remote}.insteadOf" "$public_submodule_url"
export GIT_CONFIG_GLOBAL="$test_git_config"
init_output="$test_root/initialize.out"
expect_success initialize "$init_output" /usr/bin/python3 "$helper" \
  initialize-submodules \
  --source-dir "$source_checkout" \
  --source-sha "$verified_source_sha"
test "$(output_value SOURCE_PHASE "$init_output")" = initialized \
  || fail 'initialize returned the wrong phase'
test "$(output_value SUBMODULE_COUNT "$init_output")" = 1 \
  || fail 'initialize returned the wrong recursive submodule count'
post_output="$test_root/post-init.out"
expect_success post-init "$post_output" /usr/bin/python3 "$helper" verify-source \
  --source-dir "$source_checkout" \
  --source-sha "$verified_source_sha" \
  --phase post-init
test "$(output_value SUBMODULE_COUNT "$post_output")" = 1 \
  || fail 'post-init returned the wrong recursive submodule count'

git -C "$source_checkout" checkout -q --detach "$wrong_head_sha"
expect_failure wrong-head source-head /usr/bin/python3 "$helper" verify-source \
  --source-dir "$source_checkout" --source-sha "$verified_source_sha" \
  --phase pre-init
git -C "$source_checkout" checkout -q --detach "$verified_source_sha"

printf 'dirty source\n' >"$source_checkout/untracked-fixture.txt"
expect_failure dirty-source source-dirty /usr/bin/python3 "$helper" verify-source \
  --source-dir "$source_checkout" --source-sha "$verified_source_sha" \
  --phase pre-init
/bin/rm -f "$source_checkout/untracked-fixture.txt"

# A different checked-out commit than the parent gitlink is rejected.
git -C "$source_checkout/deps/example" fetch -q "$submodule_remote" main
git -C "$source_checkout/deps/example" checkout -q "$submodule_mismatch_sha"
expect_failure mismatched-submodule submodule-state \
  /usr/bin/python3 "$helper" verify-source \
  --source-dir "$source_checkout" --source-sha "$verified_source_sha" \
  --phase post-init
git -C "$source_checkout" submodule update -q --force

# Top-level URL validation runs before submodule initialization.
git -C "$source_checkout" switch -q -c unsafe-top "$verified_source_sha"
git -C "$source_checkout" config -f .gitmodules \
  submodule.deps/example.url "$unsafe_submodule_url"
git -C "$source_checkout" add .gitmodules
git -C "$source_checkout" commit -q -m 'unsafe top-level URL fixture'
unsafe_top_sha=$(git -C "$source_checkout" rev-parse HEAD)
expect_failure unsafe-top-url submodule-url \
  /usr/bin/python3 "$helper" verify-source \
  --source-dir "$source_checkout" --source-sha "$unsafe_top_sha" \
  --phase pre-init

# A safe top-level entry cannot hide an unsafe nested URL. The one-depth
# initializer must reject it before Git can invoke the marker transport.
git -C "$source_checkout" switch -q -C unsafe-nested-source "$verified_source_sha"
git -C "$source_checkout/deps/example" fetch -q \
  "$submodule_remote" unsafe-nested
git -C "$source_checkout/deps/example" checkout -q \
  "$unsafe_nested_submodule_sha"
git -C "$source_checkout" add deps/example
git -C "$source_checkout" commit -q -m 'unsafe nested source fixture'
unsafe_nested_source_sha=$(git -C "$source_checkout" rev-parse HEAD)
expect_failure unsafe-nested-url submodule-url \
  /usr/bin/env GIT_ALLOW_PROTOCOL=file:https:ext \
  /usr/bin/python3 "$helper" initialize-submodules \
  --source-dir "$source_checkout" --source-sha "$unsafe_nested_source_sha"
test ! -e "$nested_contact_marker" \
  || fail 'unsafe nested transport was contacted before validation'

# Moving either protected remote tag after resolution invalidates the release.
automation_mover="$test_root/automation-mover"
git clone -q "$release_remote" "$automation_mover"
git -C "$automation_mover" tag -f -a "$automation_tag" \
  -m 'moved automation tag' "$release_source_sha"
git -C "$automation_mover" push -q --force origin \
  "refs/tags/$automation_tag"
expect_failure moved-automation-during-resolve remote-automation-tag-mismatch \
  /usr/bin/python3 "$helper" resolve \
  --repo "$release_work" --remote origin --release-tag v1.2.3 \
  --automation-tag "$automation_tag" \
  --automation-sha "$automation_sha" \
  --version-file-sha256 "$release_version_sha"
expect_failure moved-automation-before-upload remote-automation-tag-changed \
  /usr/bin/python3 "$helper" recheck \
  --repo "$release_work" --remote origin --release-tag v1.2.3 \
  --tag-object-sha "$tag_object_sha" --source-sha "$release_source_sha" \
  --automation-tag "$automation_tag" \
  --automation-tag-object-sha "$automation_tag_object_sha" \
  --automation-sha "$automation_sha"
git -C "$release_work" update-ref \
  "refs/tags/$automation_tag" "$automation_tag_object_sha"
git -C "$release_work" push -q --force origin \
  "refs/tags/$automation_tag"

tag_mover="$test_root/tag-mover"
git clone -q "$release_remote" "$tag_mover"
git -C "$tag_mover" tag -f -a v1.2.3 -m 'moved annotation' \
  "$release_source_sha"
git -C "$tag_mover" push -q --force origin refs/tags/v1.2.3
expect_failure moved-during-resolve remote-tag-mismatch \
  /usr/bin/python3 "$helper" resolve \
  --repo "$release_work" --remote origin --release-tag v1.2.3 \
  --automation-tag "$automation_tag" \
  --automation-sha "$automation_sha" \
  --version-file-sha256 "$release_version_sha"
expect_failure moved-before-upload remote-tag-changed \
  /usr/bin/python3 "$helper" recheck \
  --repo "$release_work" --remote origin --release-tag v1.2.3 \
  --tag-object-sha "$tag_object_sha" --source-sha "$release_source_sha" \
  --automation-tag "$automation_tag" \
  --automation-tag-object-sha "$automation_tag_object_sha" \
  --automation-sha "$automation_sha"

printf 'release tag verifier tests passed\n'
