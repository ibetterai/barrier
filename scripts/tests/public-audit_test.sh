#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
audit_script=${1:-$test_dir/../public-audit.sh}
test_root=$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/barrier-public-audit-test.XXXXXX")
fake_bin="$test_root/fake-bin"
fake_gitleaks_call_log="$test_root/fake-gitleaks-calls.log"
fake_secret_needle=
vendor_root=ext/openssl/windows
iconv_bin=$(command -v iconv) || {
  echo "public-audit test requires iconv" >&2
  exit 1
}

cleanup() {
  /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM
trap 'printf "public-audit test failed at line %s\n" "$LINENO" >&2' ERR

run_audit() {
  (
    cd "$test_root/repo"
    FAKE_GITLEAKS_CALL_LOG="$fake_gitleaks_call_log" \
      FAKE_GITLEAKS_NEEDLE="$fake_secret_needle" \
      PATH="$fake_bin:/usr/bin:/bin" "$audit_script"
  )
}

expect_missing_scanner_rejection() {
  local log_path="$test_root/missing-scanner.log"

  if (
    cd "$test_root/repo"
    PATH=/usr/bin:/bin "$audit_script"
  ) >"$log_path" 2>&1; then
    echo "public audit accepted an unavailable secret scanner" >&2
    exit 1
  fi
  grep -F "required secret scanner is unavailable" "$log_path" >/dev/null
}

expect_rejection() {
  local fixture_name=$1
  local fixture_value=$2
  local log_path="$test_root/$fixture_name.log"

  printf '%s\n' "$fixture_value" > "$test_root/repo/$fixture_name.txt"
  git -C "$test_root/repo" add "$fixture_name.txt"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted protected metadata" >&2
    exit 1
  fi
  grep -F "possible local path or private network address found" "$log_path" >/dev/null
  if grep -F "$fixture_value" "$log_path" >/dev/null; then
    echo "public audit exposed the protected value" >&2
    exit 1
  fi
  git -C "$test_root/repo" rm --cached -q "$fixture_name.txt"
  /bin/rm -f "$test_root/repo/$fixture_name.txt"
}

expect_binary_rejection() {
  local fixture_value=$1
  local log_path="$test_root/binary.log"

  printf '\0%s\0' "$fixture_value" > "$test_root/repo/unexpected.bin"
  git -C "$test_root/repo" add unexpected.bin
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted protected binary metadata" >&2
    exit 1
  fi
  grep -F "found in tracked files" "$log_path" >/dev/null
  if grep -F "$fixture_value" "$log_path" >/dev/null; then
    echo "public audit exposed the protected binary value" >&2
    exit 1
  fi
  git -C "$test_root/repo" rm --cached -q unexpected.bin
  /bin/rm -f "$test_root/repo/unexpected.bin"
}

expect_encoded_binary_rejection() {
  local fixture_name=$1
  local encoding=$2
  local fixture_value=$3
  local fixture_path="$test_root/repo/unexpected-$fixture_name.bin"
  local log_path="$test_root/$fixture_name.log"

  printf '\177' > "$fixture_path"
  if ! printf '%s' "$fixture_value" \
      | "$iconv_bin" -f UTF-8 -t "$encoding" >> "$fixture_path"; then
    echo "unable to encode public-audit fixture" >&2
    exit 1
  fi
  git -C "$test_root/repo" add "unexpected-$fixture_name.bin"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted encoded protected metadata" >&2
    exit 1
  fi
  grep -F "found in tracked files" "$log_path" >/dev/null
  if grep -F "$fixture_value" "$log_path" >/dev/null; then
    echo "public audit exposed an encoded protected value" >&2
    exit 1
  fi
  git -C "$test_root/repo" rm --cached -q \
    "unexpected-$fixture_name.bin"
  /bin/rm -f "$fixture_path"
}

expect_missing_tracked_file_rejection() {
  local log_path="$test_root/missing-tracked-file.log"

  printf '\0public fixture\0' > "$test_root/repo/missing.bin"
  git -C "$test_root/repo" add missing.bin
  /bin/rm -f "$test_root/repo/missing.bin"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a missing tracked file" >&2
    exit 1
  fi
  grep -F "unable to inspect tracked file: missing.bin" "$log_path" >/dev/null
  git -C "$test_root/repo" rm --cached -q missing.bin
}

assert_single_metadata_path() {
  local fixture_value=$1
  local log_path=$2
  local tracked_path=$3
  local path_count

  grep -F "found in tracked files" "$log_path" >/dev/null
  path_count=$(grep -Fxc "$tracked_path" "$log_path" || true)
  if [ "$path_count" -ne 1 ]; then
    echo "public audit did not report one tracked path" >&2
    exit 1
  fi
  if grep -F "$fixture_value" "$log_path" >/dev/null; then
    echo "public audit exposed indexed or worktree metadata" >&2
    exit 1
  fi
}

restore_readme_fixture() {
  git -C "$test_root/repo" restore --source=HEAD \
    --staged --worktree -- README.md
}

make_fake_credential() {
  printf '%s%s%s%s' 'g' 'hp_' \
    'AAAAAAAAAAAAAAAAAA' 'BBBBBBBBBBBBBBBBBB'
}

reset_fake_gitleaks_calls() {
  : > "$fake_gitleaks_call_log"
}

assert_fake_gitleaks_surfaces() {
  local history_count
  local snapshot_count

  history_count=$(grep -Fxc detect "$fake_gitleaks_call_log" || true)
  snapshot_count=$(grep -Fxc dir "$fake_gitleaks_call_log" || true)
  if [ "$history_count" -ne 1 ] || [ "$snapshot_count" -ne 2 ]; then
    echo "public audit did not scan history, index, and worktree secrets" >&2
    exit 1
  fi
}

assert_generic_secret_rejection() {
  local fixture_value=$1
  local log_path=$2
  local category_count
  local line_count

  category_count=$(grep -Fxc \
    "public-audit: secret scan failed or found possible secrets" \
    "$log_path" || true)
  line_count=$(/usr/bin/wc -l < "$log_path" | /usr/bin/tr -d ' ')
  if [ "$category_count" -ne 1 ] || [ "$line_count" -ne 1 ]; then
    echo "public audit returned an ambiguous secret-scan failure" >&2
    exit 1
  fi
  if grep -F "$fixture_value" "$log_path" >/dev/null \
      || grep -F "$fixture_value" "$fake_gitleaks_call_log" >/dev/null; then
    echo "public audit exposed a secret-scan fixture value" >&2
    exit 1
  fi
  assert_fake_gitleaks_surfaces
}

expect_index_only_secret_rejection() {
  local fixture_value
  local log_path="$test_root/index-only-secret.log"

  fixture_value=$(make_fake_credential)
  printf '%s\n' "$fixture_value" > "$test_root/repo/README.md"
  git -C "$test_root/repo" add README.md
  git -C "$test_root/repo" restore --source=HEAD --worktree -- README.md
  fake_secret_needle=$fixture_value
  reset_fake_gitleaks_calls
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted an indexed credential" >&2
    exit 1
  fi
  assert_generic_secret_rejection "$fixture_value" "$log_path"
  fake_secret_needle=
  restore_readme_fixture
}

expect_worktree_only_secret_rejection() {
  local fixture_value
  local log_path="$test_root/worktree-only-secret.log"

  fixture_value=$(make_fake_credential)
  printf '%s\n' "$fixture_value" > "$test_root/repo/README.md"
  fake_secret_needle=$fixture_value
  reset_fake_gitleaks_calls
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a worktree credential" >&2
    exit 1
  fi
  assert_generic_secret_rejection "$fixture_value" "$log_path"
  fake_secret_needle=
  restore_readme_fixture
}

expect_index_only_symlink_secret_rejection() {
  local fixture_value
  local link_name=index-only-secret-link
  local link_path="$test_root/repo/$link_name"
  local log_path="$test_root/index-only-symlink-secret.log"

  fixture_value=$(make_fake_credential)
  /bin/ln -s "$fixture_value" "$link_path"
  git -C "$test_root/repo" add -- "$link_name"
  assert_tracked_symlink "$link_name"
  /bin/rm -f "$link_path"
  /bin/ln -s safe-relative-target "$link_path"
  fake_secret_needle=$fixture_value
  reset_fake_gitleaks_calls
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted an indexed symlink credential" >&2
    exit 1
  fi
  assert_generic_secret_rejection "$fixture_value" "$log_path"
  fake_secret_needle=
  git -C "$test_root/repo" rm --cached -q -f -- "$link_name"
  /bin/rm -f "$link_path"
}

expect_worktree_only_symlink_secret_rejection() {
  local fixture_value
  local link_name=worktree-only-secret-link
  local link_path="$test_root/repo/$link_name"
  local log_path="$test_root/worktree-only-symlink-secret.log"

  fixture_value=$(make_fake_credential)
  /bin/ln -s safe-relative-target "$link_path"
  git -C "$test_root/repo" add -- "$link_name"
  assert_tracked_symlink "$link_name"
  /bin/rm -f "$link_path"
  /bin/ln -s "$fixture_value" "$link_path"
  fake_secret_needle=$fixture_value
  reset_fake_gitleaks_calls
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a worktree symlink credential" >&2
    exit 1
  fi
  assert_generic_secret_rejection "$fixture_value" "$log_path"
  fake_secret_needle=
  git -C "$test_root/repo" rm --cached -q -f -- "$link_name"
  /bin/rm -f "$link_path"
}

expect_untracked_secret_ignored() {
  local fixture_value
  local fixture_path="$test_root/repo/untracked-secret.txt"
  local log_path="$test_root/untracked-secret.log"

  fixture_value=$(make_fake_credential)
  printf '%s\n' "$fixture_value" > "$fixture_path"
  fake_secret_needle=$fixture_value
  reset_fake_gitleaks_calls
  if ! run_audit >"$log_path" 2>&1; then
    echo "public audit scanned an unrelated untracked credential" >&2
    exit 1
  fi
  if grep -F "$fixture_value" "$log_path" >/dev/null; then
    echo "public audit exposed an untracked secret fixture" >&2
    exit 1
  fi
  assert_fake_gitleaks_surfaces
  fake_secret_needle=
  /bin/rm -f "$fixture_path"
}

expect_staged_protected_worktree_clean_rejection() {
  local fixture_value=$1
  local log_path="$test_root/staged-protected.log"

  printf '%s\n' "$fixture_value" > "$test_root/repo/README.md"
  git -C "$test_root/repo" add README.md
  git -C "$test_root/repo" restore --source=HEAD --worktree -- README.md
  if run_audit >"$log_path" 2>&1; then
    echo "public audit ignored protected indexed content" >&2
    exit 1
  fi
  assert_single_metadata_path "$fixture_value" "$log_path" README.md
  restore_readme_fixture
}

expect_staged_clean_worktree_protected_rejection() {
  local fixture_value=$1
  local log_path="$test_root/worktree-protected.log"

  printf '%s\n' "$fixture_value" > "$test_root/repo/README.md"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit ignored protected worktree content" >&2
    exit 1
  fi
  assert_single_metadata_path "$fixture_value" "$log_path" README.md
  restore_readme_fixture
}

expect_duplicate_source_deduplication() {
  local fixture_value=$1
  local log_path="$test_root/duplicate-source.log"

  printf '%s\n' "$fixture_value" > "$test_root/repo/README.md"
  git -C "$test_root/repo" add README.md
  if run_audit >"$log_path" 2>&1; then
    echo "public audit ignored duplicate protected content" >&2
    exit 1
  fi
  assert_single_metadata_path "$fixture_value" "$log_path" README.md
  restore_readme_fixture
}

assert_generic_pathname_rejection() {
  local log_path=$1
  local line_count

  grep -Fx \
    "public-audit: protected or unsafe tracked pathname found" \
    "$log_path" >/dev/null
  line_count=$(/usr/bin/wc -l < "$log_path" | /usr/bin/tr -d ' ')
  if [ "$line_count" -ne 1 ]; then
    echo "public audit exposed a protected or unsafe pathname" >&2
    exit 1
  fi
}

expect_staged_pathname_rejection() {
  local fixture_value=$1
  local tracked_path="staged-$fixture_value.txt"
  local log_path="$test_root/staged-pathname.log"

  printf 'public fixture\n' > "$test_root/repo/$tracked_path"
  git -C "$test_root/repo" add -- "$tracked_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a protected staged pathname" >&2
    exit 1
  fi
  assert_generic_pathname_rejection "$log_path"
  git -C "$test_root/repo" rm --cached -q -- "$tracked_path"
  /bin/rm -f "$test_root/repo/$tracked_path"
}

expect_index_only_pathname_rejection() {
  local fixture_value=$1
  local tracked_path="indexed-$fixture_value.txt"
  local log_path="$test_root/indexed-pathname.log"

  printf 'public fixture\n' > "$test_root/repo/$tracked_path"
  git -C "$test_root/repo" add -- "$tracked_path"
  /bin/rm -f "$test_root/repo/$tracked_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a protected indexed pathname" >&2
    exit 1
  fi
  assert_generic_pathname_rejection "$log_path"
  git -C "$test_root/repo" rm --cached -q -- "$tracked_path"
}

expect_committed_worktree_pathname_rejection() {
  local fixture_value=$1
  local tracked_path="fixture${fixture_value}/committed.txt"
  local log_path="$test_root/committed-pathname.log"

  /bin/mkdir -p "$test_root/repo/$(/usr/bin/dirname "$tracked_path")"
  printf 'public fixture\n' > "$test_root/repo/$tracked_path"
  git -C "$test_root/repo" add -- "$tracked_path"
  git -C "$test_root/repo" -c core.hooksPath=/dev/null \
    commit -qm "add pathname fixture"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a protected committed pathname" >&2
    exit 1
  fi
  assert_generic_pathname_rejection "$log_path"
  git -C "$test_root/repo" rm -q -- "$tracked_path"
  git -C "$test_root/repo" -c core.hooksPath=/dev/null \
    commit -qm "remove pathname fixture"
}

expect_symlink_pathname_rejection() {
  local fixture_value=$1
  local tracked_path="symlink-$fixture_value"
  local log_path="$test_root/symlink-pathname.log"

  /bin/ln -s safe-relative-target "$test_root/repo/$tracked_path"
  git -C "$test_root/repo" add -- "$tracked_path"
  assert_tracked_symlink "$tracked_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a protected symlink pathname" >&2
    exit 1
  fi
  assert_generic_pathname_rejection "$log_path"
  git -C "$test_root/repo" rm --cached -q -f -- "$tracked_path"
  /bin/rm -f "$test_root/repo/$tracked_path"
}

expect_unsafe_pathname_log_safety() {
  local fixture_value=$1
  local forged_marker='forged-public-audit-line'
  local tracked_path
  local log_path="$test_root/unsafe-pathname.log"

  tracked_path=$(printf 'logs/unsafe\n::error::%s.log' "$forged_marker")
  /bin/mkdir -p "$test_root/repo/logs"
  printf '%s\n' "$fixture_value" > "$test_root/repo/$tracked_path"
  git -C "$test_root/repo" add -- "$tracked_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted an unsafe tracked pathname" >&2
    exit 1
  fi
  assert_generic_pathname_rejection "$log_path"
  if /usr/bin/grep -F "$forged_marker" "$log_path" >/dev/null \
      || /usr/bin/grep -F "$fixture_value" "$log_path" >/dev/null; then
    echo "public audit exposed an unsafe pathname or protected value" >&2
    exit 1
  fi
  git -C "$test_root/repo" rm --cached -q -- "$tracked_path"
  /bin/rm -f "$test_root/repo/$tracked_path"
}

assert_tracked_symlink() {
  local link_name=$1
  local tracked_mode

  tracked_mode=$(git -C "$test_root/repo" ls-files -s -- "$link_name" \
    | /usr/bin/awk '{print $1}')
  if [ "$tracked_mode" != "120000" ]; then
    echo "public audit test did not create a tracked symlink" >&2
    exit 1
  fi
}

expect_unsafe_index_symlink_target_rejection() {
  local link_name=unsafe-index-link-target
  local link_path="$test_root/repo/$link_name"
  local forged_marker=forged-index-link-line
  local unsafe_target
  local log_path="$test_root/unsafe-index-link-target.log"

  unsafe_target=$(printf 'safe\n::error::%s' "$forged_marker")
  /bin/ln -s "$unsafe_target" "$link_path"
  git -C "$test_root/repo" add -- "$link_name"
  assert_tracked_symlink "$link_name"
  /bin/rm -f "$link_path"
  /bin/ln -s safe-relative-target "$link_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted an unsafe indexed link target" >&2
    exit 1
  fi
  assert_single_metadata_path "$forged_marker" "$log_path" "$link_name"
  git -C "$test_root/repo" rm --cached -q -f -- "$link_name"
  /bin/rm -f "$link_path"
}

expect_unsafe_worktree_symlink_target_rejection() {
  local link_name=unsafe-worktree-link-target
  local link_path="$test_root/repo/$link_name"
  local forged_marker=forged-worktree-link-line
  local unsafe_target
  local log_path="$test_root/unsafe-worktree-link-target.log"

  unsafe_target=$(printf 'safe\n::error::%s' "$forged_marker")
  /bin/ln -s safe-relative-target "$link_path"
  git -C "$test_root/repo" add -- "$link_name"
  assert_tracked_symlink "$link_name"
  /bin/rm -f "$link_path"
  /bin/ln -s "$unsafe_target" "$link_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted an unsafe worktree link target" >&2
    exit 1
  fi
  assert_single_metadata_path "$forged_marker" "$log_path" "$link_name"
  git -C "$test_root/repo" rm --cached -q -f -- "$link_name"
  /bin/rm -f "$link_path"
}

assert_single_inspection_failure() {
  local log_path=$1
  local tracked_path=$2
  local failure_count

  failure_count=$(grep -Fxc \
    "public-audit: unable to inspect tracked file: $tracked_path" \
    "$log_path" || true)
  if [ "$failure_count" -ne 1 ]; then
    echo "public audit did not report one tracked-link failure" >&2
    exit 1
  fi
}

expect_protected_absolute_symlink_rejection() {
  local fixture_value=$1
  local link_name=protected-absolute-link
  local link_path="$test_root/repo/$link_name"
  local log_path="$test_root/protected-absolute-link.log"

  /bin/ln -s "$fixture_value" "$link_path"
  git -C "$test_root/repo" add "$link_name"
  assert_tracked_symlink "$link_name"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a protected absolute link target" >&2
    exit 1
  fi
  assert_single_metadata_path "$fixture_value" "$log_path" "$link_name"
  git -C "$test_root/repo" rm --cached -q -f "$link_name"
  /bin/rm -f "$link_path"
}

expect_worktree_protected_symlink_rejection() {
  local fixture_value=$1
  local link_name=worktree-protected-link
  local link_path="$test_root/repo/$link_name"
  local log_path="$test_root/worktree-protected-link.log"

  /bin/ln -s safe-relative-target "$link_path"
  git -C "$test_root/repo" add "$link_name"
  assert_tracked_symlink "$link_name"
  /bin/rm -f "$link_path"
  /bin/ln -s "$fixture_value" "$link_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit ignored a protected worktree link target" >&2
    exit 1
  fi
  assert_single_metadata_path "$fixture_value" "$log_path" "$link_name"
  git -C "$test_root/repo" rm --cached -q -f "$link_name"
  /bin/rm -f "$link_path"
}

expect_missing_worktree_symlink_rejection() {
  local link_name=missing-worktree-link
  local link_path="$test_root/repo/$link_name"
  local log_path="$test_root/missing-worktree-link.log"

  /bin/ln -s safe-relative-target "$link_path"
  git -C "$test_root/repo" add "$link_name"
  assert_tracked_symlink "$link_name"
  /bin/rm -f "$link_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a missing worktree link" >&2
    exit 1
  fi
  assert_single_inspection_failure "$log_path" "$link_name"
  git -C "$test_root/repo" rm --cached -q -f "$link_name"
}

expect_replaced_worktree_symlink_rejection() {
  local link_name=replaced-worktree-link
  local link_path="$test_root/repo/$link_name"
  local log_path="$test_root/replaced-worktree-link.log"

  /bin/ln -s safe-relative-target "$link_path"
  git -C "$test_root/repo" add "$link_name"
  assert_tracked_symlink "$link_name"
  /bin/rm -f "$link_path"
  printf 'public fixture\n' > "$link_path"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a replaced worktree link" >&2
    exit 1
  fi
  assert_single_inspection_failure "$log_path" "$link_name"
  git -C "$test_root/repo" rm --cached -q -f "$link_name"
  /bin/rm -f "$link_path"
}

expect_safe_relative_symlink_acceptance() {
  local fixture_value=$1
  local link_name=safe-relative-link
  local target_name=untracked-relative-target.txt
  local log_path="$test_root/safe-relative-link.log"

  printf '%s\n' "$fixture_value" > "$test_root/repo/$target_name"
  /bin/ln -s "$target_name" "$test_root/repo/$link_name"
  git -C "$test_root/repo" add "$link_name"
  assert_tracked_symlink "$link_name"
  if ! run_audit >"$log_path" 2>&1; then
    echo "public audit followed a safe relative link target" >&2
    exit 1
  fi
  if grep -F "$fixture_value" "$log_path" >/dev/null; then
    echo "public audit exposed a relative-link target value" >&2
    exit 1
  fi
  git -C "$test_root/repo" rm --cached -q "$link_name"
  /bin/rm -f "$test_root/repo/$link_name"
  /bin/rm -f "$test_root/repo/$target_name"
}

expect_protected_escaping_symlink_rejection() {
  local fixture_value=$1
  local link_name=protected-escaping-link
  local link_target="../../missing-$fixture_value/target"
  local log_path="$test_root/protected-escaping-link.log"

  /bin/ln -s "$link_target" "$test_root/repo/$link_name"
  git -C "$test_root/repo" add "$link_name"
  assert_tracked_symlink "$link_name"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a protected escaping link target" >&2
    exit 1
  fi
  grep -F "found in tracked files" "$log_path" >/dev/null
  grep -Fx "$link_name" "$log_path" >/dev/null
  if grep -F "$link_target" "$log_path" >/dev/null \
      || grep -F "$fixture_value" "$log_path" >/dev/null \
      || grep -F "unable to inspect tracked file" "$log_path" >/dev/null; then
    echo "public audit exposed or followed an escaping link target" >&2
    exit 1
  fi
  git -C "$test_root/repo" rm --cached -q "$link_name"
  /bin/rm -f "$test_root/repo/$link_name"
}

expect_gitlink_acceptance() {
  local gitlink_path="$test_root/repo/vendor-link"
  local log_path="$test_root/gitlink.log"
  local tracked_mode

  /bin/mkdir -p "$gitlink_path"
  git -C "$gitlink_path" init -q
  git -C "$gitlink_path" config user.name "Audit Test"
  git -C "$gitlink_path" config user.email "audit@example.invalid"
  printf 'public fixture\n' > "$gitlink_path/README.md"
  git -C "$gitlink_path" add README.md
  git -C "$gitlink_path" -c core.hooksPath=/dev/null commit -qm fixture
  git -C "$test_root/repo" add vendor-link 2>/dev/null
  tracked_mode=$(git -C "$test_root/repo" ls-files -s vendor-link \
    | /usr/bin/awk '{print $1}')
  if [ "$tracked_mode" != "160000" ]; then
    echo "public audit test did not create a tracked gitlink" >&2
    exit 1
  fi
  if ! run_audit >"$log_path" 2>&1; then
    echo "public audit rejected a tracked gitlink" >&2
    exit 1
  fi
  git -C "$test_root/repo" rm --cached -q -f vendor-link
  /bin/mv "$gitlink_path" "$test_root/accepted-gitlink-fixture"
}

restore_vendor_fixture() {
  git -C "$test_root/repo" restore --source=HEAD --staged --worktree -- \
    "$vendor_root"
}

expect_vendor_mutation_rejection() {
  local log_path="$test_root/vendor-mutation.log"

  # vendor_file is expanded by the inner shell.
  # shellcheck disable=SC2016
  /usr/bin/find "$test_root/repo/$vendor_root" -type f \
    -exec /bin/sh -c \
      'for vendor_file do printf "public fixture\n" > "$vendor_file"; done' \
      sh {} +
  git -C "$test_root/repo" add "$vendor_root"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a mutated inherited binary baseline" >&2
    exit 1
  fi
  grep -F "inherited binary baseline has unreviewed changes" \
    "$log_path" >/dev/null
  restore_vendor_fixture
}

expect_vendor_deletion_rejection() {
  local log_path="$test_root/vendor-deletion.log"

  git -C "$test_root/repo" rm -rq "$vendor_root"
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a deleted inherited binary baseline" >&2
    exit 1
  fi
  grep -F "inherited binary baseline has unreviewed changes" \
    "$log_path" >/dev/null
  restore_vendor_fixture
}

assert_clean_vendor_fixture() {
  if [ -n "$(git -C "$test_root/repo" status --porcelain=v1)" ]; then
    echo "public audit vendor fixture is not clean" >&2
    exit 1
  fi
}

assert_generic_vendor_failure() {
  local category_count
  local expected_category=$1
  local log_path=$2

  grep -Fx "public-audit: $expected_category" "$log_path" >/dev/null
  category_count=$(grep -Ec \
    '^public-audit: (inherited binary baseline has unreviewed changes|inherited binary baseline does not match the reviewed tree|unable to verify inherited binary baseline)$' \
    "$log_path" || true)
  if [ "$category_count" -ne 1 ]; then
    echo "public audit returned an ambiguous vendor failure category" >&2
    exit 1
  fi
  if grep -F "$test_root" "$log_path" >/dev/null \
      || grep -F "$reviewed_vendor_commit" "$log_path" >/dev/null \
      || grep -F "$reviewed_vendor_tree" "$log_path" >/dev/null; then
    echo "public audit exposed protected vendor fixture metadata" >&2
    exit 1
  fi
}

restore_committed_vendor_fixture() {
  git -C "$test_root/repo" restore --source="$reviewed_vendor_commit" \
    --staged --worktree -- "$vendor_root"
  git -C "$test_root/repo" -c core.hooksPath=/dev/null \
    commit -qm "restore reviewed vendor fixture"
  assert_clean_vendor_fixture
}

expect_committed_vendor_mismatch_rejection() {
  local log_path="$test_root/committed-vendor-mismatch.log"

  printf 'committed vendor mismatch fixture\n' \
    > "$test_root/repo/$vendor_root/committed-mismatch.fixture"
  git -C "$test_root/repo" add "$vendor_root"
  git -C "$test_root/repo" -c core.hooksPath=/dev/null \
    commit -qm "change reviewed vendor fixture"
  assert_clean_vendor_fixture
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a committed vendor-tree mismatch" >&2
    exit 1
  fi
  assert_generic_vendor_failure \
    "inherited binary baseline does not match the reviewed tree" \
    "$log_path"
  restore_committed_vendor_fixture
}

expect_committed_missing_vendor_rejection() {
  local log_path="$test_root/committed-missing-vendor.log"

  git -C "$test_root/repo" rm -rq "$vendor_root"
  git -C "$test_root/repo" -c core.hooksPath=/dev/null \
    commit -qm "remove reviewed vendor fixture"
  assert_clean_vendor_fixture
  if run_audit >"$log_path" 2>&1; then
    echo "public audit accepted a committed missing vendor tree" >&2
    exit 1
  fi
  assert_generic_vendor_failure \
    "unable to verify inherited binary baseline" \
    "$log_path"
  restore_committed_vendor_fixture
}

/bin/mkdir -p "$test_root/repo" "$fake_bin"
# The single-quoted lines are the literal body of the generated fake scanner.
# shellcheck disable=SC2016
printf '%s\n' \
  '#!/bin/sh' \
  'set -eu' \
  'command_name=${1:-}' \
  'if [ -n "${FAKE_GITLEAKS_CALL_LOG:-}" ]; then' \
  '  printf "%s\n" "$command_name" >> "$FAKE_GITLEAKS_CALL_LOG"' \
  'fi' \
  'case "$command_name" in' \
  '  detect)' \
  '    exit 0' \
  '    ;;' \
  '  dir)' \
  '    scan_root=${2:-}' \
  '    test -d "$scan_root" || exit 2' \
  '    test -n "${FAKE_GITLEAKS_NEEDLE:-}" || exit 0' \
  '    if /usr/bin/grep -R -F -l -- "$FAKE_GITLEAKS_NEEDLE" "$scan_root" >/dev/null 2>&1; then' \
  '      printf "fake finding: %s\n" "$FAKE_GITLEAKS_NEEDLE"' \
  '      printf "fake finding: %s\n" "$FAKE_GITLEAKS_NEEDLE" >&2' \
  '      exit 99' \
  '    fi' \
  '    exit 0' \
  '    ;;' \
  'esac' \
  'exit 2' > "$fake_bin/gitleaks"
/bin/chmod +x "$fake_bin/gitleaks"
git -C "$test_root/repo" init -q
git -C "$test_root/repo" config user.name "Audit Test"
git -C "$test_root/repo" config user.email "audit@example.invalid"
git -C "$repo_root" archive --format=tar HEAD -- "$vendor_root" \
  | /usr/bin/tar -xf - -C "$test_root/repo"
printf 'public fixture\n' > "$test_root/repo/README.md"
git -C "$test_root/repo" add README.md "$vendor_root"
git -C "$test_root/repo" -c core.hooksPath=/dev/null commit -qm baseline
reviewed_vendor_commit=$(git -C "$test_root/repo" rev-parse HEAD)
reviewed_vendor_tree=$(git -C "$test_root/repo" rev-parse "HEAD:$vendor_root")
expect_missing_scanner_rejection
run_audit >"$test_root/clean.log" 2>&1
expect_index_only_secret_rejection
expect_worktree_only_secret_rejection
expect_index_only_symlink_secret_rejection
expect_worktree_only_symlink_secret_rejection
expect_untracked_secret_ignored

macos_home="/""Users""/example/private-build"
linux_home="/""home""/example/private-build"
windows_users=Users
windows_home="C:\\${windows_users}\\example\\private-build"
private_ten="10.""20.30.40"
private_link_local="169.""254.20.30"
private_172_low="172.""16.20.30"
private_172_high="172.""31.20.30"
private_address="192.""168.20.30"
expect_staged_protected_worktree_clean_rejection "$macos_home"
expect_staged_clean_worktree_protected_rejection "$private_address"
expect_duplicate_source_deduplication "$linux_home"
expect_staged_pathname_rejection "$private_address"
expect_index_only_pathname_rejection "$private_ten"
expect_committed_worktree_pathname_rejection "$linux_home"
expect_symlink_pathname_rejection "$private_172_low"
expect_unsafe_pathname_log_safety "$macos_home"
expect_rejection local-home "$macos_home"
expect_rejection linux-home "$linux_home"
expect_rejection windows-home "$windows_home"
expect_rejection private-ten "$private_ten"
expect_rejection private-link-local "$private_link_local"
expect_rejection private-172-low "$private_172_low"
expect_rejection private-172-high "$private_172_high"
expect_rejection private-address "$private_address"
expect_binary_rejection "$macos_home"
expect_encoded_binary_rejection utf16le UTF-16LE "$macos_home"
expect_encoded_binary_rejection utf16be UTF-16BE "$private_address"
expect_encoded_binary_rejection utf32le UTF-32LE "$macos_home"
expect_encoded_binary_rejection utf32be UTF-32BE "$private_address"
expect_protected_absolute_symlink_rejection "$macos_home"
expect_worktree_protected_symlink_rejection "$private_address"
expect_unsafe_index_symlink_target_rejection
expect_unsafe_worktree_symlink_target_rejection
expect_missing_worktree_symlink_rejection
expect_replaced_worktree_symlink_rejection
expect_safe_relative_symlink_acceptance "$macos_home"
expect_protected_escaping_symlink_rejection "$private_address"
expect_missing_tracked_file_rejection
expect_gitlink_acceptance
expect_vendor_mutation_rejection
expect_vendor_deletion_rejection
expect_committed_vendor_mismatch_rejection
expect_committed_missing_vendor_rejection

printf 'public-audit tests passed\n'
