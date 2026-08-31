#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
scanner=${1:-$test_dir/../scan-protected-metadata.pl}
test_root=$(/usr/bin/mktemp -d \
  "${TMPDIR:-/tmp}/barrier-protected-metadata-test.XXXXXX")
iconv_bin=$(command -v iconv) || {
  echo "protected-metadata test requires iconv" >&2
  exit 1
}

cleanup() {
  /bin/chmod -R u+rwx "$test_root" 2>/dev/null || true
  /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

run_scanner() {
  local policy=$1
  local file_path=$2
  local log_path=$3
  local scan_status=0

  /usr/bin/perl "$scanner" "$policy" "$file_path" >"$log_path" 2>&1 \
    || scan_status=$?
  printf '%s\n' "$scan_status"
}

expect_status() {
  local expected_status=$1
  local fixture_name=$2
  local file_path=$3
  local policy=${4:---artifact}
  local log_path="$test_root/$fixture_name.log"
  local actual_status

  actual_status=$(run_scanner "$policy" "$file_path" "$log_path")
  if [ "$actual_status" -ne "$expected_status" ]; then
    echo "unexpected protected-metadata scanner status: $fixture_name" >&2
    exit 1
  fi
  if [ -s "$log_path" ]; then
    echo "protected-metadata scanner produced output: $fixture_name" >&2
    exit 1
  fi
}

expect_invocation_status() {
  local expected_status=$1
  local fixture_name=$2
  local log_path="$test_root/$fixture_name.log"
  local actual_status=0
  shift 2

  /usr/bin/perl "$scanner" "$@" >"$log_path" 2>&1 \
    || actual_status=$?
  if [ "$actual_status" -ne "$expected_status" ]; then
    echo "unexpected protected-metadata invocation status: $fixture_name" >&2
    exit 1
  fi
  if [ -s "$log_path" ]; then
    echo "protected-metadata invocation produced output: $fixture_name" >&2
    exit 1
  fi
}

write_encoded_fixture() {
  local encoding=$1
  local fixture_value=$2
  local output_path=$3
  local with_bom=${4:-0}

  printf '\177' > "$output_path"
  if [ "$with_bom" -eq 1 ]; then
    case "$encoding" in
      UTF-16LE) printf '\377\376' >> "$output_path" ;;
      UTF-16BE) printf '\376\377' >> "$output_path" ;;
      UTF-32LE) printf '\377\376\000\000' >> "$output_path" ;;
      UTF-32BE) printf '\000\000\376\377' >> "$output_path" ;;
      *) echo "unsupported fixture encoding" >&2; exit 1 ;;
    esac
  fi
  if ! printf '%s' "$fixture_value" \
      | "$iconv_bin" -f UTF-8 -t "$encoding" >> "$output_path"; then
    echo "unable to encode protected-metadata fixture" >&2
    exit 1
  fi
}

macos_root="/""Users""/"
protected_path="${macos_root}example/private-build"
linux_superuser_home="/""root""/example/private-build"
linux_superuser_sibling="/""rooted""/example/public-build"
protected_address=$(printf '%s.%s.%s.%s' 192 168 20 30)
public_address=$(printf '%s.%s.%s.%s' 203 0 113 42)

printf '\000%s\000' "$protected_path" > "$test_root/ascii.bin"
expect_status 1 ascii "$test_root/ascii.bin"

printf '%s\303\251' "$protected_path" > "$test_root/utf8.bin"
expect_status 1 utf8 "$test_root/utf8.bin"

printf '%s\n' "$linux_superuser_home" > "$test_root/linux-superuser-home.bin"
expect_status 1 linux-superuser-home-source \
  "$test_root/linux-superuser-home.bin" --source
expect_status 1 linux-superuser-home-artifact \
  "$test_root/linux-superuser-home.bin" --artifact
printf '%s\n' "$linux_superuser_sibling" \
  > "$test_root/linux-superuser-sibling.bin"
expect_status 0 linux-superuser-sibling-source \
  "$test_root/linux-superuser-sibling.bin" --source
expect_status 0 linux-superuser-sibling-artifact \
  "$test_root/linux-superuser-sibling.bin" --artifact

for encoding in UTF-16LE UTF-16BE UTF-32LE UTF-32BE; do
  fixture_name=$(printf '%s' "$encoding" | /usr/bin/tr '[:upper:]' '[:lower:]')
  write_encoded_fixture "$encoding" "$protected_path" \
    "$test_root/$fixture_name.bin"
  expect_status 1 "$fixture_name" "$test_root/$fixture_name.bin"

  write_encoded_fixture "$encoding" "$protected_address" \
    "$test_root/$fixture_name-bom.bin" 1
  expect_status 1 "$fixture_name-bom" "$test_root/$fixture_name-bom.bin"

  write_encoded_fixture "$encoding" "$public_address" \
    "$test_root/$fixture_name-public.bin"
  expect_status 0 "$fixture_name-public" \
    "$test_root/$fixture_name-public.bin"

  write_encoded_fixture "$encoding" "$linux_superuser_home" \
    "$test_root/$fixture_name-linux-superuser-home.bin"
  expect_status 1 "$fixture_name-linux-superuser-home-source" \
    "$test_root/$fixture_name-linux-superuser-home.bin" --source
  expect_status 1 "$fixture_name-linux-superuser-home-artifact" \
    "$test_root/$fixture_name-linux-superuser-home.bin" --artifact
done

/usr/bin/perl -e '
  use strict;
  use warnings;
  binmode STDOUT;
  my $value = shift;
  print join(chr(0) x 5, split(//, $value));
' "$protected_path" > "$test_root/malformed-nul.bin"
expect_status 0 malformed-nul "$test_root/malformed-nul.bin"

artifact_only_root="/""usr""/local/opt/"
printf '%sfixture\n' "$artifact_only_root" > "$test_root/policy-delta.txt"
expect_status 0 source-policy "$test_root/policy-delta.txt" --source
expect_status 1 artifact-policy "$test_root/policy-delta.txt" --artifact

expect_status 0 scanner-self "$scanner" --artifact
expect_status 2 missing-input "$test_root/missing.bin" --artifact

protected_pathname="fixtures/$protected_address/fixture.txt"
control_pathname=$(printf 'fixtures/safe\nforged-line.txt')
format_pathname=$(printf 'fixtures/format-\342\200\256-name.txt')
surrogate_pathname=$(printf 'fixtures/surrogate-\355\240\200-name.txt')
safe_unicode_pathname=$(printf 'fixtures/caf\303\251.txt')
expect_invocation_status 0 safe-pathname \
  --source-path fixtures/public.txt
expect_invocation_status 0 safe-unicode-pathname \
  --source-path "$safe_unicode_pathname"
expect_invocation_status 1 protected-pathname \
  --source-path "$protected_pathname"
expect_invocation_status 1 linux-superuser-home-pathname \
  --source-path "$linux_superuser_home"
expect_invocation_status 1 control-pathname \
  --source-path "$control_pathname"
expect_invocation_status 1 format-pathname \
  --source-path "$format_pathname"
expect_invocation_status 1 surrogate-pathname \
  --source-path "$surrogate_pathname"
expect_invocation_status 1 workflow-command-pathname \
  --source-path 'fixtures/::error::forged-line.txt'
expect_invocation_status 1 empty-pathname --source-path ''

printf 'fixtures/public-target.txt' > "$test_root/safe-pathname.txt"
printf '%s' "$protected_pathname" > "$test_root/protected-pathname.txt"
printf 'safe\n::error::forged-line' > "$test_root/unsafe-pathname.txt"
expect_status 0 safe-pathname-file "$test_root/safe-pathname.txt" \
  --source-path-file
expect_status 1 protected-pathname-file "$test_root/protected-pathname.txt" \
  --source-path-file
expect_status 1 unsafe-pathname-file "$test_root/unsafe-pathname.txt" \
  --source-path-file
expect_status 2 missing-pathname-file "$test_root/missing-pathname.txt" \
  --source-path-file
expect_invocation_status 2 missing-mode "$test_root/ascii.bin"
expect_invocation_status 2 wrong-mode --unknown "$test_root/ascii.bin"
expect_invocation_status 2 missing-arguments

printf 'protected-metadata scanner tests passed\n'
