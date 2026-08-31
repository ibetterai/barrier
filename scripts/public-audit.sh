#!/usr/bin/env bash
set -euo pipefail

status=0
tracked_file_list=
safe_tracked_file_list=
worktree_link_file=
worktree_secret_file_list=
worktree_secret_symlink_list=
indexed_secret_snapshot=
worktree_secret_snapshot=
secret_snapshot_path=
secret_scanner_available=0
secret_scan_failed=0
worktree_snapshot_failed=0

if ! script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd); then
  echo "public-audit: unable to locate audit helpers" >&2
  exit 1
fi
metadata_scanner="$script_dir/scan-protected-metadata.pl"
if [ ! -x /usr/bin/perl ] || [ ! -f "$metadata_scanner" ]; then
  echo "public-audit: protected-metadata scanner is unavailable" >&2
  exit 1
fi

# Invoked by the EXIT trap below.
# shellcheck disable=SC2329
cleanup() {
  if [ -n "$tracked_file_list" ]; then
    /bin/rm -f "$tracked_file_list"
  fi
  if [ -n "$safe_tracked_file_list" ]; then
    /bin/rm -f "$safe_tracked_file_list"
  fi
  if [ -n "$worktree_link_file" ]; then
    /bin/rm -f "$worktree_link_file"
  fi
  if [ -n "$worktree_secret_file_list" ]; then
    /bin/rm -f "$worktree_secret_file_list"
  fi
  if [ -n "$worktree_secret_symlink_list" ]; then
    /bin/rm -f "$worktree_secret_symlink_list"
  fi
  if [ -n "$indexed_secret_snapshot" ]; then
    /bin/rm -rf "$indexed_secret_snapshot" >/dev/null 2>&1
  fi
  if [ -n "$worktree_secret_snapshot" ]; then
    /bin/rm -rf "$worktree_secret_snapshot" >/dev/null 2>&1
  fi
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

if command -v gitleaks >/dev/null 2>&1; then
  secret_scanner_available=1
  if ! gitleaks detect --source . --redact --no-banner --exit-code 99 \
      >/dev/null 2>&1; then
    secret_scan_failed=1
  fi
else
  echo "public-audit: required secret scanner is unavailable" >&2
  status=1
fi

if ! tracked_file_list=$(/usr/bin/mktemp \
    "${TMPDIR:-/tmp}/barrier-public-audit-files.XXXXXX"); then
  echo "public-audit: unable to prepare tracked-file scan" >&2
  exit 1
fi
if ! git ls-files -s -z > "$tracked_file_list"; then
  echo "public-audit: unable to enumerate tracked files" >&2
  exit 1
fi
if ! safe_tracked_file_list=$(/usr/bin/mktemp \
    "${TMPDIR:-/tmp}/barrier-public-audit-safe-files.XXXXXX"); then
  echo "public-audit: unable to prepare tracked-path scan" >&2
  exit 1
fi
if ! worktree_link_file=$(/usr/bin/mktemp \
    "${TMPDIR:-/tmp}/barrier-public-audit-link.XXXXXX"); then
  echo "public-audit: unable to prepare tracked-file scan" >&2
  exit 1
fi

tracked_artifacts=
protected_or_unsafe_pathname=0
pathname_scan_failed=0
while IFS= read -r -d '' tracked_entry; do
  case "$tracked_entry" in
    *$'\t'*) tracked_file=${tracked_entry#*$'\t'} ;;
    *) echo "public-audit: unable to parse tracked files" >&2; exit 1 ;;
  esac

  pathname_status=0
  /usr/bin/perl "$metadata_scanner" --source-path "$tracked_file" \
    >/dev/null 2>&1 || pathname_status=$?
  case "$pathname_status" in
    0)
      if ! printf '%s\0' "$tracked_entry" >> "$safe_tracked_file_list"; then
        echo "public-audit: unable to prepare tracked-path scan" >&2
        exit 1
      fi
      ;;
    1)
      protected_or_unsafe_pathname=1
      continue
      ;;
    *)
      pathname_scan_failed=1
      continue
      ;;
  esac

  case "$tracked_file" in
    tmp/*|*/tmp/*|log/*|*/log/*|logs/*|*/logs/*|*.dmg|*.pkg|*.log)
      tracked_artifacts="${tracked_artifacts}${tracked_file}"$'\n'
      ;;
  esac
done < "$tracked_file_list"

if [ "$protected_or_unsafe_pathname" -eq 1 ]; then
  echo "public-audit: protected or unsafe tracked pathname found" >&2
  status=1
fi
if [ "$pathname_scan_failed" -eq 1 ]; then
  echo "public-audit: unable to inspect tracked pathnames" >&2
  status=1
fi

if [ -n "$tracked_artifacts" ]; then
  echo "public-audit: generated artifacts/logs are tracked:" >&2
  printf '%s' "$tracked_artifacts" >&2
  status=1
fi

# Materialize only scanner-approved tracked bytes. Symlink targets are copied
# as regular inputs so the secret scanner neither follows links nor reaches
# unrelated untracked files. The staged and worktree surfaces remain separate.
if ! indexed_secret_snapshot=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-public-audit-index-secrets.XXXXXX" \
    2>/dev/null); then
  echo "public-audit: secret scan failed or found possible secrets" >&2
  exit 1
fi
if ! worktree_secret_snapshot=$(/usr/bin/mktemp -d \
    "${TMPDIR:-/tmp}/barrier-public-audit-worktree-secrets.XXXXXX" \
    2>/dev/null); then
  echo "public-audit: secret scan failed or found possible secrets" >&2
  exit 1
fi
if ! worktree_secret_file_list=$(/usr/bin/mktemp \
    "${TMPDIR:-/tmp}/barrier-public-audit-worktree-files.XXXXXX" \
    2>/dev/null); then
  echo "public-audit: secret scan failed or found possible secrets" >&2
  exit 1
fi
if ! worktree_secret_symlink_list=$(/usr/bin/mktemp \
    "${TMPDIR:-/tmp}/barrier-public-audit-worktree-links.XXXXXX" \
    2>/dev/null); then
  echo "public-audit: secret scan failed or found possible secrets" >&2
  exit 1
fi

prepare_secret_snapshot_path() {
  local snapshot_root=$1
  local tracked_path=$2
  local snapshot_parent

  secret_snapshot_path="$snapshot_root/$tracked_path"
  snapshot_parent=${secret_snapshot_path%/*}
  [ -d "$snapshot_parent" ] \
    || /bin/mkdir -p "$snapshot_parent" >/dev/null 2>&1
}

# Freeze the exact inherited Windows OpenSSL vendor tree. Its prebuilt binaries
# predate this release audit; any change to that tree requires a dedicated
# Windows dependency rebuild and review rather than a broad binary exclusion.
legacy_vendor_root=ext/openssl/windows
legacy_vendor_tree=42bc69cca6dea60817f6cf9dc3225d1db9264e1e
protected_metadata_files=

while IFS= read -r -d '' tracked_entry; do
  case "$tracked_entry" in
    *$'\t'*)
      tracked_prefix=${tracked_entry%%$'\t'*}
      tracked_file=${tracked_entry#*$'\t'}
      ;;
    *)
      echo "public-audit: unable to parse tracked files" >&2
      exit 1
      ;;
  esac
  tracked_mode=${tracked_prefix%% *}
  tracked_remainder=${tracked_prefix#* }
  tracked_object=${tracked_remainder%% *}
  tracked_stage=${tracked_prefix##* }
  scan_worktree=0
  scan_worktree_link=0
  scan_mode=--source
  if [ "$tracked_stage" != 0 ]; then
    echo "public-audit: unable to inspect tracked file: $tracked_file" >&2
    status=1
    continue
  fi
  case "$tracked_mode" in
    160000)
      continue
      ;;
    120000)
      scan_worktree_link=1
      scan_mode=--source-path-file
      ;;
    100644|100755)
      scan_worktree=1
      ;;
    *)
      echo "public-audit: unable to inspect tracked file: $tracked_file" >&2
      status=1
      continue
      ;;
  esac

  # Materialize and scan the exact indexed bytes. Writing cat-file directly to
  # the safe snapshot avoids checkout filters and a second per-file copy.
  if ! prepare_secret_snapshot_path "$indexed_secret_snapshot" \
      "$tracked_file" \
      || ! git cat-file blob "$tracked_object" \
        > "$secret_snapshot_path" 2>/dev/null; then
    echo "public-audit: unable to inspect tracked file: $tracked_file" >&2
    secret_scan_failed=1
    status=1
    continue
  fi

  scan_files=("$secret_snapshot_path")
  worktree_invalid=0
  if [ "$scan_worktree_link" -eq 1 ]; then
    if [ ! -L "$tracked_file" ] \
        || ! /usr/bin/readlink -n "./$tracked_file" \
          > "$worktree_link_file" 2>/dev/null; then
      worktree_invalid=1
    else
      scan_files+=("$worktree_link_file")
      if ! printf '%s\0' "$tracked_file" \
          >> "$worktree_secret_file_list" \
          || ! printf '%s\0' "$tracked_file" \
            >> "$worktree_secret_symlink_list"; then
        worktree_snapshot_failed=1
      fi
    fi
  fi
  if [ "$scan_worktree" -eq 1 ]; then
    if [ -L "$tracked_file" ] || [ ! -f "$tracked_file" ]; then
      worktree_invalid=1
    else
      scan_files+=("$tracked_file")
      if ! printf '%s\0' "$tracked_file" \
          >> "$worktree_secret_file_list"; then
        worktree_snapshot_failed=1
      fi
    fi
  fi

  file_has_protected_metadata=0
  file_scan_failed=0
  for scan_file in "${scan_files[@]}"; do
    scan_status=0
    /usr/bin/perl "$metadata_scanner" "$scan_mode" "$scan_file" \
      >/dev/null 2>&1 || scan_status=$?
    case "$scan_status" in
      0)
        ;;
      1)
        file_has_protected_metadata=1
        ;;
      *)
        file_scan_failed=1
        break
        ;;
    esac
  done

  if [ "$worktree_invalid" -eq 1 ] || [ "$file_scan_failed" -eq 1 ]; then
      echo "public-audit: unable to inspect tracked file: $tracked_file" >&2
      status=1
      continue
  fi
  if [ "$file_has_protected_metadata" -eq 1 ]; then
    case "$tracked_file" in
      "$legacy_vendor_root"/*)
        ;;
      *)
        protected_metadata_files="${protected_metadata_files}${tracked_file}"$'\n'
        ;;
    esac
  fi
done < "$safe_tracked_file_list"

# Copy the prevalidated tracked worktree in one archive stream. Links remain
# links during transport, then their exact target bytes replace them as regular
# scanner inputs. No untracked path is present in the NUL-delimited file list.
if [ "$worktree_snapshot_failed" -eq 0 ] \
    && [ -s "$worktree_secret_file_list" ]; then
  if ! COPYFILE_DISABLE=1 /usr/bin/tar --no-xattrs --null \
      -T "$worktree_secret_file_list" -cf - 2>/dev/null \
      | COPYFILE_DISABLE=1 /usr/bin/tar --no-xattrs -xf - \
        -C "$worktree_secret_snapshot" 2>/dev/null; then
    worktree_snapshot_failed=1
  fi
fi
if [ "$worktree_snapshot_failed" -eq 0 ]; then
  while IFS= read -r -d '' tracked_file; do
    snapshot_link="$worktree_secret_snapshot/$tracked_file"
    if [ ! -L "$snapshot_link" ] \
        || ! /usr/bin/readlink -n "$snapshot_link" \
          > "$worktree_link_file" 2>/dev/null \
        || ! /bin/rm -f "$snapshot_link" >/dev/null 2>&1 \
        || ! /bin/cp "$worktree_link_file" "$snapshot_link" \
          >/dev/null 2>&1; then
      worktree_snapshot_failed=1
      break
    fi
  done < "$worktree_secret_symlink_list"
fi
if [ "$worktree_snapshot_failed" -eq 1 ]; then
  secret_scan_failed=1
fi

if [ "$secret_scanner_available" -eq 1 ]; then
  for secret_snapshot in \
      "$indexed_secret_snapshot" "$worktree_secret_snapshot"; do
    if ! gitleaks dir "$secret_snapshot" --redact --no-banner --exit-code 99 \
        >/dev/null 2>&1; then
      secret_scan_failed=1
    fi
  done
  if [ "$secret_scan_failed" -eq 1 ]; then
    echo "public-audit: secret scan failed or found possible secrets" >&2
    status=1
  fi
fi

if [ -n "$protected_metadata_files" ]; then
  echo "public-audit: possible local path or private network address found in tracked files" >&2
  printf '%s' "$protected_metadata_files" >&2
  status=1
fi

if ! git diff --quiet -- "$legacy_vendor_root" \
    || ! git diff --cached --quiet -- "$legacy_vendor_root"; then
  echo "public-audit: inherited binary baseline has unreviewed changes" >&2
  status=1
elif ! current_legacy_tree=$(git rev-parse "HEAD:$legacy_vendor_root" 2>/dev/null); then
  echo "public-audit: unable to verify inherited binary baseline" >&2
  status=1
elif [ "$current_legacy_tree" != "$legacy_vendor_tree" ]; then
  echo "public-audit: inherited binary baseline does not match the reviewed tree" >&2
  status=1
fi

exit "$status"
