#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
helper="$repo_root/scripts/parse-release-provenance.py"
test_root=$(/usr/bin/mktemp -d \
  "${TMPDIR:-/tmp}/barrier-release-provenance-test.XXXXXX")

cleanup() {
  /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

fail() {
  echo "release provenance parser test failed: $1" >&2
  exit 1
}

assert_absent() {
  local path=$1
  if test -e "$path" || test -L "$path"; then
    fail 'a rejected case created an output path'
  fi
}

assert_no_staging_files() {
  local directory=$1
  if /usr/bin/find "$directory" -name '.release-provenance.*' \
      -print -quit | /usr/bin/grep . >/dev/null; then
    fail 'a rejected case retained a staging file'
  fi
}

write_valid_input() {
  local path=$1
  {
    # Deliberately shuffled: successful output must use canonical key order.
    printf 'AUTOMATION_TAG_OBJECT_SHA=4444444444444444444444444444444444444444\n'
    printf 'SOURCE_SHA=2222222222222222222222222222222222222222\n'
    printf 'RELEASE_VERSION=3.4.0\n'
    printf 'AUTOMATION_SHA=3333333333333333333333333333333333333333\n'
    printf 'RELEASE_TAG=v3.4.0\n'
    printf 'TAG_OBJECT_SHA=1111111111111111111111111111111111111111\n'
    printf 'AUTOMATION_TAG=v3.4.0-automation.1\n'
  } >"$path"
}

expect_rejection() {
  local label=$1
  local category=$2
  local input_path=$3
  local case_dir="$test_root/rejected-$label"
  local environment_path="$case_dir/github.env"
  local output_path="$case_dir/github.output"
  local stdout_path="$case_dir/stdout"
  local stderr_path="$case_dir/stderr"
  /bin/mkdir -p "$case_dir"

  if /usr/bin/python3 "$helper" \
      --input "$input_path" \
      --github-env "$environment_path" \
      --github-output "$output_path" \
      >"$stdout_path" 2>"$stderr_path"; then
    fail "$label unexpectedly succeeded"
  fi
  test ! -s "$stdout_path" || fail "$label emitted stdout"
  /usr/bin/grep -Fqx "parse-release-provenance: $category" \
    "$stderr_path" || fail "$label returned the wrong safe category"
  assert_absent "$environment_path"
  assert_absent "$output_path"
  assert_no_staging_files "$case_dir"
}

valid_input="$test_root/valid.input"
write_valid_input "$valid_input"
success_dir="$test_root/success"
/bin/mkdir -p "$success_dir"
success_env="$success_dir/github.env"
success_output="$success_dir/github.output"
success_stdout="$success_dir/stdout"
success_stderr="$success_dir/stderr"
/usr/bin/python3 "$helper" \
  --input "$valid_input" \
  --github-env "$success_env" \
  --github-output "$success_output" \
  >"$success_stdout" 2>"$success_stderr"
test ! -s "$success_stdout" || fail 'success emitted stdout'
test ! -s "$success_stderr" || fail 'success emitted stderr'

expected_env="$success_dir/expected.env"
expected_output="$success_dir/expected.output"
{
  printf 'RELEASE_TAG=v3.4.0\n'
  printf 'RELEASE_VERSION=3.4.0\n'
  printf 'TAG_OBJECT_SHA=1111111111111111111111111111111111111111\n'
  printf 'SOURCE_SHA=2222222222222222222222222222222222222222\n'
  printf 'AUTOMATION_SHA=3333333333333333333333333333333333333333\n'
  printf 'AUTOMATION_TAG=v3.4.0-automation.1\n'
  printf 'AUTOMATION_TAG_OBJECT_SHA=4444444444444444444444444444444444444444\n'
} >"$expected_env"
{
  printf 'release_tag=v3.4.0\n'
  printf 'release_version=3.4.0\n'
  printf 'tag_object_sha=1111111111111111111111111111111111111111\n'
  printf 'source_sha=2222222222222222222222222222222222222222\n'
  printf 'automation_sha=3333333333333333333333333333333333333333\n'
  printf 'automation_tag=v3.4.0-automation.1\n'
  printf 'automation_tag_object_sha=4444444444444444444444444444444444444444\n'
} >"$expected_output"
/usr/bin/cmp -s "$expected_env" "$success_env" \
  || fail 'environment output is not canonical'
/usr/bin/cmp -s "$expected_output" "$success_output" \
  || fail 'step output is not canonical'
assert_no_staging_files "$success_dir"

duplicate_input="$test_root/duplicate.input"
/bin/cp "$valid_input" "$duplicate_input"
printf 'SOURCE_SHA=5555555555555555555555555555555555555555\n' \
  >>"$duplicate_input"
expect_rejection duplicate duplicate-key "$duplicate_input"

missing_input="$test_root/missing.input"
/usr/bin/sed '/^AUTOMATION_TAG_OBJECT_SHA=/d' "$valid_input" >"$missing_input"
expect_rejection missing missing-key "$missing_input"

extra_input="$test_root/extra.input"
/bin/cp "$valid_input" "$extra_input"
printf 'EXTRA=value\n' >>"$extra_input"
expect_rejection extra unexpected-key "$extra_input"

multiline_input="$test_root/multiline.input"
/usr/bin/sed '/^AUTOMATION_TAG=/d' "$valid_input" >"$multiline_input"
{
  printf 'AUTOMATION_TAG=v3.4.0-automation.1\n'
  printf 'INJECTED=unsafe\n'
} >>"$multiline_input"
expect_rejection multiline unexpected-key "$multiline_input"

unsafe_character_input="$test_root/unsafe-character.input"
/usr/bin/sed 's/^RELEASE_TAG=.*/RELEASE_TAG=v3.4.0\r/' \
  "$valid_input" >"$unsafe_character_input"
expect_rejection unsafe-character invalid-input-character \
  "$unsafe_character_input"

unterminated_input="$test_root/unterminated.input"
/usr/bin/sed '$d' "$valid_input" >"$unterminated_input"
printf 'AUTOMATION_TAG_OBJECT_SHA=4444444444444444444444444444444444444444' \
  >>"$unterminated_input"
expect_rejection unterminated invalid-input-record "$unterminated_input"

mismatch_input="$test_root/mismatch.input"
/usr/bin/sed 's/^RELEASE_VERSION=.*/RELEASE_VERSION=3.4.1/' \
  "$valid_input" >"$mismatch_input"
expect_rejection mismatch version-mismatch "$mismatch_input"

automation_mismatch_input="$test_root/automation-mismatch.input"
/usr/bin/sed \
  's/^AUTOMATION_TAG=.*/AUTOMATION_TAG=v3.5.0-automation.1/' \
  "$valid_input" >"$automation_mismatch_input"
expect_rejection automation-mismatch automation-version-mismatch \
  "$automation_mismatch_input"

invalid_semver_input="$test_root/invalid-semver.input"
/usr/bin/sed 's/^RELEASE_TAG=.*/RELEASE_TAG=v03.4.0/' \
  "$valid_input" >"$invalid_semver_input"
expect_rejection invalid-semver invalid-value "$invalid_semver_input"

invalid_automation_tag_input="$test_root/invalid-automation-tag.input"
/usr/bin/sed \
  's/^AUTOMATION_TAG=.*/AUTOMATION_TAG=v3.4.0-automation.01/' \
  "$valid_input" >"$invalid_automation_tag_input"
expect_rejection invalid-automation-tag invalid-value \
  "$invalid_automation_tag_input"

zero_automation_tag_input="$test_root/zero-automation-tag.input"
/usr/bin/sed \
  's/^AUTOMATION_TAG=.*/AUTOMATION_TAG=v3.4.0-automation.0/' \
  "$valid_input" >"$zero_automation_tag_input"
expect_rejection zero-automation-tag invalid-value \
  "$zero_automation_tag_input"

invalid_sha_input="$test_root/invalid-sha.input"
/usr/bin/sed 's/^SOURCE_SHA=.*/SOURCE_SHA=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/' \
  "$valid_input" >"$invalid_sha_input"
expect_rejection invalid-sha invalid-value "$invalid_sha_input"

oversized_input="$test_root/oversized.input"
/usr/bin/perl -e 'print "A" x 8193' >"$oversized_input"
expect_rejection oversized invalid-input-size "$oversized_input"

existing_dir="$test_root/existing-output"
/bin/mkdir -p "$existing_dir"
existing_env="$existing_dir/github.env"
existing_step="$existing_dir/github.output"
printf 'sentinel-env\n' >"$existing_env"
if /usr/bin/python3 "$helper" \
    --input "$valid_input" \
    --github-env "$existing_env" \
    --github-output "$existing_step" \
    >"$existing_dir/stdout" 2>"$existing_dir/stderr"; then
  fail 'existing environment output unexpectedly succeeded'
fi
/usr/bin/grep -Fqx 'sentinel-env' "$existing_env" \
  || fail 'existing environment output was modified'
assert_absent "$existing_step"
test ! -s "$existing_dir/stdout" || fail 'existing output failure emitted stdout'
/usr/bin/grep -Fqx 'parse-release-provenance: output-exists' \
  "$existing_dir/stderr" || fail 'existing output returned the wrong category'
assert_no_staging_files "$existing_dir"

existing_second_dir="$test_root/existing-second-output"
/bin/mkdir -p "$existing_second_dir"
new_env="$existing_second_dir/github.env"
existing_step="$existing_second_dir/github.output"
printf 'sentinel-step\n' >"$existing_step"
if /usr/bin/python3 "$helper" \
    --input "$valid_input" \
    --github-env "$new_env" \
    --github-output "$existing_step" \
    >"$existing_second_dir/stdout" 2>"$existing_second_dir/stderr"; then
  fail 'existing step output unexpectedly succeeded'
fi
assert_absent "$new_env"
/usr/bin/grep -Fqx 'sentinel-step' "$existing_step" \
  || fail 'existing step output was modified'
assert_no_staging_files "$existing_second_dir"

symlink_dir="$test_root/symlink-output"
/bin/mkdir -p "$symlink_dir"
symlink_target="$symlink_dir/target"
symlink_env="$symlink_dir/github.env"
symlink_step="$symlink_dir/github.output"
printf 'sentinel-target\n' >"$symlink_target"
/bin/ln -s "$symlink_target" "$symlink_step"
if /usr/bin/python3 "$helper" \
    --input "$valid_input" \
    --github-env "$symlink_env" \
    --github-output "$symlink_step" \
    >"$symlink_dir/stdout" 2>"$symlink_dir/stderr"; then
  fail 'symlink output unexpectedly succeeded'
fi
assert_absent "$symlink_env"
test -L "$symlink_step" || fail 'symlink output was replaced'
/usr/bin/grep -Fqx 'sentinel-target' "$symlink_target" \
  || fail 'symlink target was modified'
assert_no_staging_files "$symlink_dir"

same_path_dir="$test_root/same-output-path"
/bin/mkdir -p "$same_path_dir"
same_path="$same_path_dir/github.commands"
if /usr/bin/python3 "$helper" \
    --input "$valid_input" \
    --github-env "$same_path" \
    --github-output "$same_path" \
    >"$same_path_dir/stdout" 2>"$same_path_dir/stderr"; then
  fail 'identical output paths unexpectedly succeeded'
fi
assert_absent "$same_path"
/usr/bin/grep -Fqx 'parse-release-provenance: unsafe-output-path' \
  "$same_path_dir/stderr" || fail 'identical paths returned the wrong category'
assert_no_staging_files "$same_path_dir"

printf 'release provenance parser tests passed\n'
