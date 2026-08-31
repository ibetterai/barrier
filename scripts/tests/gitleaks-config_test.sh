#!/usr/bin/env bash
set -euo pipefail

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$test_dir/../.." && pwd)
test_root=$(/usr/bin/mktemp -d \
  "${TMPDIR:-/tmp}/barrier-gitleaks-config-test.XXXXXX")

cleanup() {
  /bin/rm -rf "$test_root"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

if ! command -v gitleaks >/dev/null 2>&1; then
  echo "gitleaks config test requires gitleaks" >&2
  exit 1
fi

make_fake_credential() {
  printf '%s%s%s' 'AK' 'IA' 'ABCDEFGHIJKLMNOP'
}

expect_detection() {
  local relative_path=$1
  local fixture_path="$test_root/$relative_path"
  local scan_status=0

  /bin/mkdir -p "${fixture_path%/*}"
  make_fake_credential > "$fixture_path"

  (
    cd "$test_root"
    gitleaks dir . --config "$repo_root/.gitleaks.toml" \
      --redact --no-banner --exit-code 99 >/dev/null 2>&1
  ) || scan_status=$?

  if [ "$scan_status" -ne 99 ]; then
    echo "gitleaks config accepted a finding in an inherited source path" >&2
    exit 1
  fi

  /bin/rm -f "$fixture_path"
}

expect_detection tools/cryptopp561/TestVectors/audit-fixture.txt
expect_detection src/lib/platform/COSXKeyState.cpp

printf 'gitleaks config tests passed\n'
