#!/bin/bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage:
  verify-macos-deployment-target.sh --app APP [--target VERSION] [--arch ARCH] [--manifest FILE]
  verify-macos-deployment-target.sh --root DIR --target VERSION [--arch ARCH] [--include-static-archives] [--manifest FILE]
EOF
    exit 2
}

root=
plist=
target=
arch=arm64
app_mode=0
include_static_archives=0
manifest=
bundle_main_executable=
bundle_main_relative=

while test "$#" -gt 0; do
    case "$1" in
        --app)
            test "$#" -ge 2 || usage
            test -z "$root" || usage
            root="$2/Contents"
            plist="$2/Contents/Info.plist"
            app_mode=1
            shift 2
            ;;
        --root)
            test "$#" -ge 2 || usage
            test -z "$root" || usage
            root="$2"
            shift 2
            ;;
        --target)
            test "$#" -ge 2 || usage
            target="$2"
            shift 2
            ;;
        --arch)
            test "$#" -ge 2 || usage
            arch="$2"
            shift 2
            ;;
        --manifest)
            test "$#" -ge 2 || usage
            manifest="$2"
            shift 2
            ;;
        --include-static-archives)
            include_static_archives=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            usage
            ;;
    esac
done

test -n "$root" || usage
test -d "$root" || { echo "verification root does not exist" >&2; exit 1; }

if test -n "$target"; then
    case "$target" in
        *[!0-9.]*|'') echo "invalid macOS target" >&2; exit 1 ;;
    esac
fi
case "$arch" in
    *[!A-Za-z0-9_]*|'') echo "invalid architecture" >&2; exit 1 ;;
esac

if ! script_dir=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd); then
    echo "unable to locate verification helpers" >&2
    exit 1
fi
metadata_scanner="$script_dir/scan-protected-metadata.pl"
closure_helper="$script_dir/macos-macho-closure.py"
if test ! -x /usr/bin/perl || test ! -f "$metadata_scanner"; then
    echo "protected-metadata scanner is unavailable" >&2
    exit 1
fi

if test -n "$plist"; then
    test -f "$plist" || { echo "app Info.plist does not exist" >&2; exit 1; }
    plist_target=$(/usr/bin/plutil -extract LSMinimumSystemVersion raw \
        "$plist" 2>/dev/null) || {
        echo "unable to read LSMinimumSystemVersion" >&2
        exit 1
    }
    bundle_executable_name=$(/usr/bin/plutil -extract CFBundleExecutable raw \
        "$plist" 2>/dev/null) || {
        echo "unable to read CFBundleExecutable" >&2
        exit 1
    }
    case "$bundle_executable_name" in
        ''|.|..|*/*|*\\*)
            echo "invalid CFBundleExecutable" >&2
            exit 1
            ;;
    esac
    bundle_main_relative="MacOS/$bundle_executable_name"
    bundle_main_executable="$root/$bundle_main_relative"
    if test -n "$target" && test "$target" != "$plist_target"; then
        echo "requested target differs from LSMinimumSystemVersion" >&2
        exit 1
    fi
    target="$plist_target"
fi

test -n "$target" || usage
case "$target" in
    *[!0-9.]*|'') echo "invalid macOS target" >&2; exit 1 ;;
esac

manifest_body=$(/usr/bin/mktemp "${TMPDIR:-/tmp}/barrier-macho-manifest.XXXXXX")
archive_list=$(/usr/bin/mktemp "${TMPDIR:-/tmp}/barrier-archive-list.XXXXXX")
file_list=$(/usr/bin/mktemp "${TMPDIR:-/tmp}/barrier-file-list.XXXXXX")
manifest_tmp=
pathname_list=$(/usr/bin/mktemp "${TMPDIR:-/tmp}/barrier-pathname-list.XXXXXX")
symlink_list=$(/usr/bin/mktemp "${TMPDIR:-/tmp}/barrier-symlink-list.XXXXXX")
symlink_target_file=$(/usr/bin/mktemp \
    "${TMPDIR:-/tmp}/barrier-symlink-target.XXXXXX")
closure_manifest=$(/usr/bin/mktemp "${TMPDIR:-/tmp}/barrier-closure-manifest.XXXXXX")

cleanup() {
    /bin/rm -f "$archive_list" "$file_list" "$manifest_body" \
        "$pathname_list" "$symlink_list" "$symlink_target_file" \
        "$closure_manifest"
    test -z "$manifest_tmp" || /bin/rm -f "$manifest_tmp"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

if test -n "$manifest"; then
    /bin/mkdir -p "$(/usr/bin/dirname "$manifest")"
    /bin/rm -f "$manifest"
    manifest_tmp="$manifest.tmp.$$"
fi

version_gt() {
    /usr/bin/awk -v left="$1" -v right="$2" 'BEGIN {
        left_count = split(left, left_parts, ".")
        right_count = split(right, right_parts, ".")
        count = left_count > right_count ? left_count : right_count
        for (part_index = 1; part_index <= count; ++part_index) {
            left_value = part_index in left_parts ? left_parts[part_index] + 0 : 0
            right_value = part_index in right_parts ? right_parts[part_index] + 0 : 0
            if (left_value > right_value) exit 0
            if (left_value < right_value) exit 1
        }
        exit 1
    }'
}

check_architecture() {
    local file_path=$1
    local display_path=$2
    file_archs=$(/usr/bin/lipo -archs "$file_path" 2>/dev/null) || {
        echo "unable to read architectures: $display_path" >&2
        return 1
    }
    test "$file_archs" = "$arch" || {
        echo "unexpected architectures ($file_archs): $display_path" >&2
        return 1
    }
}

check_versions() {
    local file_path=$1
    local display_path=$2
    local version_output=$3
    local file_target
    test -n "$version_output" || {
        echo "missing macOS deployment load command: $display_path" >&2
        return 1
    }
    for file_target in $version_output; do
        case "$file_target" in
            *[!0-9.]*|'')
                echo "invalid macOS deployment version ($file_target): $display_path" >&2
                return 1
                ;;
        esac
        if version_gt "$file_target" "$target"; then
            echo "deployment target $file_target exceeds $target: $display_path" >&2
            return 1
        fi
        if version_gt "$file_target" "$max_target"; then
            max_target=$file_target
        fi
    done
}

check_privacy_strings() {
    local file_path=$1
    local display_path=$2
    local scan_status=0

    /usr/bin/perl "$metadata_scanner" --artifact "$file_path" >/dev/null 2>&1 \
        || scan_status=$?
    case "$scan_status" in
        0)
            return 0
            ;;
        1)
            echo "local-environment or private-network string found: $display_path" >&2
            return 1
            ;;
    esac

    echo "unable to inspect file privacy: $display_path" >&2
    return 1
}

check_privacy_path() {
    local display_path=$1
    local scan_status=0

    /usr/bin/perl "$metadata_scanner" --artifact-path "$display_path" \
        >/dev/null 2>&1 || scan_status=$?
    case "$scan_status" in
        0)
            return 0
            ;;
        1)
            echo "protected or unsafe app pathname found" >&2
            return 1
            ;;
    esac

    echo "unable to inspect app pathname" >&2
    return 1
}

check_privacy_path_file() {
    local path_file=$1
    local scan_status=0

    /usr/bin/perl "$metadata_scanner" --artifact-path-file "$path_file" \
        >/dev/null 2>&1 || scan_status=$?
    case "$scan_status" in
        0)
            return 0
            ;;
        1)
            echo "protected or unsafe symlink target found" >&2
            return 1
            ;;
    esac

    echo "unable to inspect symlink target" >&2
    return 1
}

symlink_target_is_lexically_contained() {
    local link_relative=$1
    local target=$2
    local parent=
    local remaining
    local component
    local has_more
    local depth=0

    case "$link_relative" in
        */*)
            parent=${link_relative%/*}
            while :; do
                depth=$((depth + 1))
                case "$parent" in
                    */*) parent=${parent#*/} ;;
                    *) break ;;
                esac
            done
            ;;
    esac

    remaining=$target
    while :; do
        case "$remaining" in
            */*)
                component=${remaining%%/*}
                remaining=${remaining#*/}
                has_more=1
                ;;
            *)
                component=$remaining
                remaining=
                has_more=0
                ;;
        esac
        case "$component" in
            ''|.) ;;
            ..)
                test "$depth" -gt 0 || return 1
                depth=$((depth - 1))
                ;;
            *) depth=$((depth + 1)) ;;
        esac
        test "$has_more" -eq 1 || break
    done
    return 0
}

realpath_bin=$(command -v realpath) || {
    echo "realpath is required for app dependency verification" >&2
    exit 1
}
canonical_root=$("$realpath_bin" "$root" 2>/dev/null) || {
    echo "unable to resolve verification root" >&2
    exit 1
}
root=$canonical_root
if test "$app_mode" -eq 1; then
    bundle_main_executable="$root/$bundle_main_relative"
    test -f "$bundle_main_executable" || {
        echo "bundle main executable is missing" >&2
        exit 1
    }
    main_file_description=$(/usr/bin/file -b "$bundle_main_executable" 2>/dev/null) || {
        echo "unable to classify bundle main executable" >&2
        exit 1
    }
    case "$main_file_description" in
        *Mach-O*executable*) ;;
        *)
            echo "bundle main executable is not Mach-O" >&2
            exit 1
            ;;
    esac
    test -f "$closure_helper" || {
        echo "Mach-O dependency closure helper is missing" >&2
        exit 1
    }
    python3_bin=$(/usr/bin/xcrun --find python3 2>/dev/null) || {
        echo "Xcode Python is required for app dependency verification" >&2
        exit 1
    }
fi

vtool=$(/usr/bin/xcrun --find vtool)
macho_count=0
archive_count=0
max_target=0

if ! /usr/bin/find "$root" -mindepth 1 -print0 2>/dev/null \
    | /usr/bin/sort -z > "$pathname_list" 2>/dev/null; then
    echo "unable to enumerate verification pathnames" >&2
    exit 1
fi
while IFS= read -r -d '' pathname; do
    display_path=${pathname#"$root"/}
    check_privacy_path "$display_path"
done < "$pathname_list"

if ! /usr/bin/find "$root" -type l -print0 2>/dev/null \
    | /usr/bin/sort -z > "$symlink_list" 2>/dev/null; then
    echo "unable to enumerate verification symlinks" >&2
    exit 1
fi
while IFS= read -r -d '' symlink_path; do
    display_path=${symlink_path#"$root"/}
    if test "$app_mode" -eq 0; then
        case "$display_path" in
            *.a)
                echo "symlinked static archive is not allowed: $display_path" >&2
                exit 1
                ;;
        esac
    fi
    if ! /usr/bin/readlink -n "$symlink_path" \
        > "$symlink_target_file" 2>/dev/null; then
        echo "unable to read symlink target: $display_path" >&2
        exit 1
    fi
    symlink_target=$(/usr/bin/readlink "$symlink_path" 2>/dev/null) || {
        echo "unable to read symlink target: $display_path" >&2
        exit 1
    }
    case "$symlink_target" in
        /*)
            echo "absolute symlink target found: $display_path" >&2
            exit 1
            ;;
    esac
    check_privacy_path_file "$symlink_target_file"
    if ! symlink_target_is_lexically_contained \
        "$display_path" "$symlink_target"; then
        echo "symlink target escapes verification root: $display_path" >&2
        exit 1
    fi
    resolved_symlink=$("$realpath_bin" "$symlink_path" 2>/dev/null) || {
        echo "unresolved symlink target: $display_path" >&2
        exit 1
    }
    case "$resolved_symlink" in
        "$canonical_root"|"$canonical_root"/*) ;;
        *)
            echo "symlink target escapes verification root: $display_path" >&2
            exit 1
            ;;
    esac
done < "$symlink_list"

if ! /usr/bin/find "$root" -type f -print0 2>/dev/null \
    | /usr/bin/sort -z > "$file_list" 2>/dev/null; then
    echo "unable to enumerate verification files" >&2
    exit 1
fi
while IFS= read -r -d '' file_path; do
    display_path=${file_path#"$root"/}
    file_description=$(/usr/bin/file -b "$file_path" 2>/dev/null) || {
        echo "unable to classify file: $display_path" >&2
        exit 1
    }

    case "$file_description" in
        *Mach-O*)
            check_architecture "$file_path" "$display_path"
            versions=$("$vtool" -show-build "$file_path" 2>/dev/null \
                | /usr/bin/awk '$1 == "minos" {print $2}')
            check_versions "$file_path" "$display_path" "$versions"
            versions_csv=${versions//$'\n'/,}
            printf 'file\t%s\tarch=%s\tminos=%s\n' \
                "$display_path" "$file_archs" "$versions_csv" >> "$manifest_body"

            if test "$app_mode" -eq 1; then
                check_privacy_strings "$file_path" "$display_path"
            fi

            macho_count=$((macho_count + 1))
            ;;
        *)
            if test "$app_mode" -eq 1; then
                check_privacy_strings "$file_path" "$display_path"
            fi
            ;;
    esac
done < "$file_list"

test "$macho_count" -gt 0 || {
    echo "no Mach-O files found under verification root" >&2
    exit 1
}

if test "$app_mode" -eq 1; then
    "$python3_bin" "$closure_helper" \
        --root "$root" \
        --main "$bundle_main_relative" \
        --arch "$arch" \
        --target "$target" \
        --output "$closure_manifest"
    /bin/cat "$closure_manifest" >> "$manifest_body"
fi

if test "$include_static_archives" -eq 1; then
    if ! /usr/bin/find "$root" -type f -name '*.a' -print0 2>/dev/null \
        | /usr/bin/sort -z > "$archive_list" 2>/dev/null; then
        echo "unable to enumerate static archives" >&2
        exit 1
    fi
    while IFS= read -r -d '' archive_path; do
        display_path=${archive_path#"$root"/}
        check_architecture "$archive_path" "$display_path"
        versions=$(/usr/bin/otool -l "$archive_path" 2>/dev/null | /usr/bin/awk '$1 == "minos" {print $2}')
        check_versions "$archive_path" "$display_path" "$versions"
        versions_csv=${versions//$'\n'/,}
        printf 'archive\t%s\tarch=%s\tminos=%s\n' \
            "$display_path" "$file_archs" "$versions_csv" >> "$manifest_body"
        archive_count=$((archive_count + 1))
    done < "$archive_list"
fi

if test -n "$manifest"; then
    {
        printf 'format\tbarrier-macho-manifest-v2\n'
        printf 'declared_minimum_macos\t%s\n' "$target"
        printf 'required_architecture\t%s\n' "$arch"
        printf 'maximum_macos_deployment_target\t%s\n' "$max_target"
        printf 'mach_o_count\t%s\n' "$macho_count"
        printf 'static_archive_count\t%s\n' "$archive_count"
        /bin/cat "$manifest_body"
    } > "$manifest_tmp"
    /bin/mv "$manifest_tmp" "$manifest"
    manifest_tmp=
    printf 'MANIFEST_WRITTEN=1\n'
fi

printf 'VERIFIED_SCOPE=%s\n' "$([ "$app_mode" -eq 1 ] && printf app || printf root)"
printf 'VERIFIED_ARCH=%s\n' "$arch"
printf 'MAX_MACOS_DEPLOYMENT_TARGET=%s\n' "$max_target"
printf 'MACHO_FILES_VERIFIED=%s\n' "$macho_count"
printf 'STATIC_ARCHIVES_VERIFIED=%s\n' "$archive_count"
