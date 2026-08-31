#!/bin/bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage: build-macos-release-prefix.sh --prefix DIR --work-dir DIR [--jobs N]

Builds the checksum-pinned macOS arm64 release dependency prefix. The prefix
must be absent, empty, or a complete cache produced by this exact recipe.
EOF
    exit 2
}

script_dir=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(unset CDPATH; cd -- "$script_dir/.." && pwd)
lock_file="$repo_root/dist/macos/release-dependencies.lock"
verify_script="$script_dir/verify-macos-deployment-target.sh"
closure_helper="$script_dir/macos-macho-closure.py"
metadata_scanner="$script_dir/scan-protected-metadata.pl"

prefix=
work_dir=
jobs=3

while test "$#" -gt 0; do
    case "$1" in
        --prefix)
            test "$#" -ge 2 || usage
            prefix=$2
            shift 2
            ;;
        --work-dir)
            test "$#" -ge 2 || usage
            work_dir=$2
            shift 2
            ;;
        --jobs)
            test "$#" -ge 2 || usage
            jobs=$2
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            usage
            ;;
    esac
done

test -n "$prefix" || usage
test -n "$work_dir" || usage
case "$prefix" in /*) ;; *) echo "prefix must be an absolute path" >&2; exit 1 ;; esac
case "$work_dir" in /*) ;; *) echo "work directory must be an absolute path" >&2; exit 1 ;; esac
case "$jobs" in *[!0-9]*|'') echo "jobs must be a positive integer" >&2; exit 1 ;; esac
test "$jobs" -gt 0 || { echo "jobs must be a positive integer" >&2; exit 1; }
test "$prefix" != / || { echo "refusing to use / as the prefix" >&2; exit 1; }
test "$work_dir" != / || { echo "refusing to use / as the work directory" >&2; exit 1; }
test "$prefix" != "$work_dir" || { echo "prefix and work directory must differ" >&2; exit 1; }

test -f "$lock_file" || {
    echo "required source component is missing: dist/macos/release-dependencies.lock" >&2
    exit 1
}
test -f "$verify_script" || {
    echo "required source component is missing: scripts/verify-macos-deployment-target.sh" >&2
    exit 1
}
test -f "$closure_helper" || {
    echo "required source component is missing: scripts/macos-macho-closure.py" >&2
    exit 1
}
test -f "$metadata_scanner" || {
    echo "protected-metadata scanner is missing" >&2
    exit 1
}

invalid_lock_lines=$(/usr/bin/grep -Ev '^(#.*|[[:space:]]*|[A-Z0-9_]+=[A-Za-z0-9._:/-]+)$' "$lock_file" || true)
test -z "$invalid_lock_lines" || {
    echo "dependency lock contains unsupported syntax" >&2
    exit 1
}

# shellcheck disable=SC1090
source "$lock_file"

: "${BARRIER_RELEASE_LOCK_VERSION:?}"
: "${BARRIER_RELEASE_PREFIX_SCHEMA:?}"
: "${MACOS_RELEASE_ARCH:?}"
: "${MACOSX_DEPLOYMENT_TARGET:?}"
: "${QT_VERSION:?}"
: "${QTBASE_ARCHIVE:?}"
: "${QTBASE_URL:?}"
: "${QTBASE_SHA256:?}"
: "${QTSVG_ARCHIVE:?}"
: "${QTSVG_URL:?}"
: "${QTSVG_SHA256:?}"
: "${QTTOOLS_ARCHIVE:?}"
: "${QTTOOLS_URL:?}"
: "${QTTOOLS_SHA256:?}"
: "${QTBASE_PATCH:?}"
: "${QTBASE_PATCH_SHA256:?}"
: "${OPENSSL_VERSION:?}"
: "${OPENSSL_ARCHIVE:?}"
: "${OPENSSL_URL:?}"
: "${OPENSSL_SHA256:?}"
: "${OPENSSL_CONFIG_TARGET:?}"
: "${OPENSSL_RUNTIME_PREFIX:?}"

test "$BARRIER_RELEASE_LOCK_VERSION" = 1 || {
    echo "unsupported dependency lock version: $BARRIER_RELEASE_LOCK_VERSION" >&2
    exit 1
}
test "$MACOS_RELEASE_ARCH" = arm64 || {
    echo "unsupported release architecture: $MACOS_RELEASE_ARCH" >&2
    exit 1
}
case "$OPENSSL_RUNTIME_PREFIX" in
    /*) ;;
    *) echo "OpenSSL runtime prefix must be an absolute path" >&2; exit 1 ;;
esac
test "$OPENSSL_RUNTIME_PREFIX" != / || {
    echo "refusing to use / as the OpenSSL runtime prefix" >&2
    exit 1
}

# Host build tools come from macOS/Xcode. Runtime dependencies must come only
# from the locked sources below, never from an ambient Homebrew/MacPorts PATH.
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C

patch_file="$repo_root/$QTBASE_PATCH"
test -f "$patch_file" || {
    echo "required source component is missing: $QTBASE_PATCH" >&2
    exit 1
}

for tool in /usr/bin/curl /usr/bin/file /usr/bin/make /usr/bin/patch \
    /usr/bin/shasum /usr/bin/tar /usr/bin/xcrun; do
    test -x "$tool" || { echo "required build tool is unavailable: $tool" >&2; exit 1; }
done
command -v perl >/dev/null || { echo "Perl is required to build OpenSSL" >&2; exit 1; }

test "$(/usr/bin/uname -s)" = Darwin || { echo "this recipe requires macOS" >&2; exit 1; }
test "$(/usr/bin/uname -m)" = "$MACOS_RELEASE_ARCH" || {
    echo "this recipe requires a native $MACOS_RELEASE_ARCH host" >&2
    exit 1
}

actual_patch_sha=$(/usr/bin/shasum -a 256 "$patch_file" | /usr/bin/awk '{print $1}')
test "$actual_patch_sha" = "$QTBASE_PATCH_SHA256" || {
    echo "Qt patch checksum mismatch" >&2
    exit 1
}

lock_sha=$(/usr/bin/shasum -a 256 "$lock_file" | /usr/bin/awk '{print $1}')
builder_sha=$(/usr/bin/shasum -a 256 "$0" | /usr/bin/awk '{print $1}')
verifier_sha=$(/usr/bin/shasum -a 256 "$verify_script" | /usr/bin/awk '{print $1}')
closure_sha=$(/usr/bin/shasum -a 256 "$closure_helper" | /usr/bin/awk '{print $1}')
scanner_sha=$(/usr/bin/shasum -a 256 "$metadata_scanner" | /usr/bin/awk '{print $1}')
recipe_sha=$(printf '%s\n%s\n%s\n%s\n%s\n' \
    "$builder_sha" "$verifier_sha" "$closure_sha" "$scanner_sha" \
    "$actual_patch_sha" \
    | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}')
cache_id="macos-${MACOS_RELEASE_ARCH}-prefix-v${BARRIER_RELEASE_PREFIX_SCHEMA}-${lock_sha:0:16}-${recipe_sha:0:16}"
manifest="$prefix/.barrier-release-prefix-manifest"

manifest_matches() {
    test -f "$manifest" \
        && /usr/bin/grep -Fqx "LOCK_SHA256=$lock_sha" "$manifest" \
        && /usr/bin/grep -Fqx "RECIPE_SHA256=$recipe_sha" "$manifest" \
        && /usr/bin/grep -Fqx "CACHE_ID=$cache_id" "$manifest" \
        && /usr/bin/grep -Fqx "MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET" "$manifest" \
        && /usr/bin/grep -Fqx "MACOS_RELEASE_ARCH=$MACOS_RELEASE_ARCH" "$manifest"
}

verify_prefix() {
    test -x "$prefix/bin/qmake"
    test -x "$prefix/bin/macdeployqt"
    test -f "$prefix/lib/libssl.a"
    test -f "$prefix/lib/libcrypto.a"
    test -f "$prefix/lib/QtSvg.framework/QtSvg"
    test -f "$prefix/plugins/iconengines/libqsvgicon.dylib"
    /bin/bash "$verify_script" \
        --root "$prefix" \
        --target "$MACOSX_DEPLOYMENT_TARGET" \
        --arch "$MACOS_RELEASE_ARCH" \
        --include-static-archives
    test "$("$prefix/bin/qmake" -query QT_VERSION)" = "$QT_VERSION"
}

if manifest_matches; then
    verify_prefix
    printf 'PREFIX_CACHE_STATUS=hit\n'
    printf 'PREFIX_CACHE_ID=%s\n' "$cache_id"
    printf 'PREFIX=%s\n' "$prefix"
    exit 0
fi

if test -e "$prefix" && test -n "$(/bin/ls -A "$prefix" 2>/dev/null)"; then
    echo "release prefix cache does not match the locked recipe" >&2
    exit 1
fi
if test -e "$work_dir" && test -n "$(/bin/ls -A "$work_dir" 2>/dev/null)"; then
    echo "release prefix work directory must be absent or empty" >&2
    exit 1
fi

/bin/mkdir -p "$prefix" "$work_dir/downloads" "$work_dir/sources" "$work_dir/build"

download_locked() {
    archive_name=$1
    archive_url=$2
    archive_sha=$3
    archive_path="$work_dir/downloads/$archive_name"
    partial_path="$archive_path.partial"

    if test -f "$archive_path"; then
        actual_sha=$(/usr/bin/shasum -a 256 "$archive_path" | /usr/bin/awk '{print $1}')
        test "$actual_sha" = "$archive_sha" || {
            echo "cached source checksum mismatch: $archive_name" >&2
            return 1
        }
        return 0
    fi

    /usr/bin/curl \
        --fail \
        --location \
        --proto '=https' \
        --retry 3 \
        --show-error \
        --silent \
        --tlsv1.2 \
        --output "$partial_path" \
        "$archive_url"
    actual_sha=$(/usr/bin/shasum -a 256 "$partial_path" | /usr/bin/awk '{print $1}')
    test "$actual_sha" = "$archive_sha" || {
        echo "downloaded source checksum mismatch: $archive_name" >&2
        return 1
    }
    /bin/mv "$partial_path" "$archive_path"
}

download_locked "$OPENSSL_ARCHIVE" "$OPENSSL_URL" "$OPENSSL_SHA256"
download_locked "$QTBASE_ARCHIVE" "$QTBASE_URL" "$QTBASE_SHA256"
download_locked "$QTSVG_ARCHIVE" "$QTSVG_URL" "$QTSVG_SHA256"
download_locked "$QTTOOLS_ARCHIVE" "$QTTOOLS_URL" "$QTTOOLS_SHA256"

/usr/bin/tar -xf "$work_dir/downloads/$OPENSSL_ARCHIVE" -C "$work_dir/sources"
/usr/bin/tar -xf "$work_dir/downloads/$QTBASE_ARCHIVE" -C "$work_dir/sources"
/usr/bin/tar -xf "$work_dir/downloads/$QTSVG_ARCHIVE" -C "$work_dir/sources"
/usr/bin/tar -xf "$work_dir/downloads/$QTTOOLS_ARCHIVE" -C "$work_dir/sources"

openssl_source="$work_dir/sources/openssl-$OPENSSL_VERSION"
qtbase_source=$(printf '%s\n' "$work_dir"/sources/qtbase-everywhere-* | /usr/bin/head -1)
qtsvg_source=$(printf '%s\n' "$work_dir"/sources/qtsvg-everywhere-* | /usr/bin/head -1)
qttools_source=$(printf '%s\n' "$work_dir"/sources/qttools-everywhere-* | /usr/bin/head -1)
test -d "$openssl_source" || { echo "OpenSSL archive layout changed" >&2; exit 1; }
test -d "$qtbase_source" || { echo "qtbase archive layout changed" >&2; exit 1; }
test -d "$qtsvg_source" || { echo "qtsvg archive layout changed" >&2; exit 1; }
test -d "$qttools_source" || { echo "qttools archive layout changed" >&2; exit 1; }

(
    cd "$qtbase_source"
    /usr/bin/patch --batch --forward -p1 < "$patch_file"
)

sdk_path=$(/usr/bin/xcrun --sdk macosx --show-sdk-path)
clang_path=$(/usr/bin/xcrun --sdk macosx --find clang)
probe_source="$work_dir/build/deployment-target-probe.c"
probe_binary="$work_dir/build/deployment-target-probe"
printf 'int main(void) { return 0; }\n' > "$probe_source"
"$clang_path" \
    -arch "$MACOS_RELEASE_ARCH" \
    -isysroot "$sdk_path" \
    -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" \
    "$probe_source" \
    -o "$probe_binary"
/bin/bash "$verify_script" \
    --root "$work_dir/build" \
    --target "$MACOSX_DEPLOYMENT_TARGET" \
    --arch "$MACOS_RELEASE_ARCH"

export MACOSX_DEPLOYMENT_TARGET
export SDKROOT="$sdk_path"

(
    cd "$openssl_source"
    ./Configure \
        "$OPENSSL_CONFIG_TARGET" \
        --prefix="$OPENSSL_RUNTIME_PREFIX" \
        --openssldir=/etc/ssl \
        no-docs \
        no-engine \
        no-module \
        no-shared \
        no-tests
    /usr/bin/make -j "$jobs"
    /usr/bin/make DESTDIR="$work_dir/install/openssl" install_sw
)

openssl_staged_prefix="$work_dir/install/openssl$OPENSSL_RUNTIME_PREFIX"
test -f "$openssl_staged_prefix/lib/libssl.a" || {
    echo "staged OpenSSL installation is incomplete" >&2
    exit 1
}
/usr/bin/ditto "$openssl_staged_prefix" "$prefix"

# Do not use grep -q in these pipelines. With pipefail, an early match can
# SIGPIPE strings and invert a true match into a false pipeline status.
for physical_build_path in "$prefix" "$work_dir"; do
    if /usr/bin/strings "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a" \
        | /usr/bin/grep -F "$physical_build_path" >/dev/null; then
        echo "OpenSSL archives contain a physical build path" >&2
        exit 1
    fi
done
macos_users_root="/""Users"
unix_home_root="/""home"
local_home_pattern="(${macos_users_root}/[^/[:space:]]+|${unix_home_root}/[^/[:space:]]+)"
if /usr/bin/strings "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a" \
    | /usr/bin/grep -E "$local_home_pattern" >/dev/null; then
    echo "OpenSSL archives contain a local user path" >&2
    exit 1
fi

qtbase_build="$work_dir/build/qtbase"
/bin/mkdir -p "$qtbase_build"
(
    cd "$qtbase_build"
    "$qtbase_source/configure" \
        -prefix "$prefix" \
        -opensource \
        -confirm-license \
        -release \
        -shared \
        -framework \
        -platform macx-clang \
        -sdk macosx \
        -nomake examples \
        -nomake tests \
        -no-dbus \
        -no-glib \
        -no-icu \
        -no-openssl \
        -securetransport \
        -no-pkg-config \
        -qt-doubleconversion \
        -qt-freetype \
        -qt-harfbuzz \
        -qt-libjpeg \
        -qt-libpng \
        -qt-pcre \
        -qt-zlib \
        QMAKE_APPLE_DEVICE_ARCHS="$MACOS_RELEASE_ARCH" \
        QMAKE_MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
    /usr/bin/make -j "$jobs"
    /usr/bin/make install
)

qtsvg_build="$work_dir/build/qtsvg"
/bin/mkdir -p "$qtsvg_build"
(
    cd "$qtsvg_build"
    "$prefix/bin/qmake" \
        "$qtsvg_source/qtsvg.pro" \
        CONFIG+=release \
        QMAKE_APPLE_DEVICE_ARCHS="$MACOS_RELEASE_ARCH" \
        QMAKE_MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
    /usr/bin/make -j "$jobs"
    /usr/bin/make install
)

qttools_build="$work_dir/build/qttools"
/bin/mkdir -p "$qttools_build"
(
    cd "$qttools_build"
    "$prefix/bin/qmake" \
        "$qttools_source/qttools.pro" \
        CONFIG+=release \
        QMAKE_APPLE_DEVICE_ARCHS="$MACOS_RELEASE_ARCH" \
        QMAKE_MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
    /usr/bin/make -s qmake_all
    /usr/bin/make -C src/macdeployqt -j "$jobs" sub-macdeployqt
    /usr/bin/make -C src/macdeployqt sub-macdeployqt-install_subtargets
)

test -x "$prefix/bin/macdeployqt" || {
    echo "pinned macdeployqt was not produced: bin/macdeployqt" >&2
    exit 1
}

cat > "$manifest" <<EOF
LOCK_VERSION=$BARRIER_RELEASE_LOCK_VERSION
PREFIX_SCHEMA=$BARRIER_RELEASE_PREFIX_SCHEMA
LOCK_SHA256=$lock_sha
RECIPE_SHA256=$recipe_sha
CACHE_ID=$cache_id
MACOS_RELEASE_ARCH=$MACOS_RELEASE_ARCH
MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET
QT_VERSION=$QT_VERSION
OPENSSL_VERSION=$OPENSSL_VERSION
OPENSSL_RUNTIME_PREFIX=$OPENSSL_RUNTIME_PREFIX
EOF

verify_prefix

printf 'PREFIX_CACHE_STATUS=built\n'
printf 'PREFIX_CACHE_ID=%s\n' "$cache_id"
printf 'PREFIX=%s\n' "$prefix"
