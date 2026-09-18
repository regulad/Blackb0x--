#!/bin/sh
# Build the xyz.regulad.blackb0x .deb with Theos's dm.pl, inside
# ghcr.io/regulad/dotfiles:latest.
#
# Usage: package/build.sh <staging-dir> <output.deb> [version] [packages-file]
#
# [packages-file] overrides package/packages.txt; only useful for testing.
#
# <staging-dir> is a complete, ready-to-package tree: DEBIAN/ plus the payload
# laid out at its final on-device paths. BakeRamdisk.cpp assembles that tree at
# bake time (the payload is not static -- postinstall.sh is templated with the
# resolved package list, and the bundled local-repo .debs depend on what that
# bake actually resolved), then calls this.
#
# Why Theos and not plain dpkg-deb: dm.pl builds a correct .deb without root
# and without fakeroot, and it is the tool this ecosystem actually uses. That
# matters here because bake-all-ramdisks already needs root for its loop mount
# and we do not want to add a second, different reason to need it.
#
# Notably we use NONE of Theos's compilation half. This package is pure data --
# apt sources, gpg keys, a LaunchDaemon plist, shell scripts, and the bundled
# .debs. Nothing is compiled, so the image's SDKs (AppleTVOS12.4, iPhoneOS16.5,
# both arm64-era and both useless for this project's armv7 target) never come
# into it. If this package ever grows a real binary, that is a genuine problem
# to solve, not a detail -- neither SDK can target armv7.
set -eu

IMAGE="${BLACKB0X_THEOS_IMAGE:-ghcr.io/regulad/dotfiles:latest}"
# Set explicitly rather than relying on the image's login profile to export it.
# The profile does set THEOS, but it also triggers a Homebrew API fetch, which
# makes every bake slow, network-dependent and non-deterministic. A non-login
# shell with THEOS passed in avoids all of that.
THEOS_DIR="${BLACKB0X_THEOS_DIR:-/home/regulad.linux/theos}"
THEOS_UID="${BLACKB0X_THEOS_UID:-}"

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <staging-dir> <output.deb> [version] [packages-file]" >&2
    exit 2
fi

STAGING=$(cd "$1" && pwd)
OUTPUT=$2
VERSION=${3:-}
PACKAGES_FILE=${4:-}
# Empty means "use package/packages.txt" (set below).

if [ ! -f "$STAGING/DEBIAN/control" ]; then
    echo "$0: $STAGING has no DEBIAN/control -- not a staging tree" >&2
    exit 1
fi

if [ -n "$VERSION" ]; then
    sed -i "s/__BLACKB0X_VERSION__/$VERSION/" "$STAGING/DEBIAN/control"
fi
if grep -q '__BLACKB0X_VERSION__' "$STAGING/DEBIAN/control"; then
    echo "$0: DEBIAN/control still has an unsubstituted __BLACKB0X_VERSION__" >&2
    exit 1
fi

# postinstall.sh's install list is substituted HERE, at package-build time,
# from package/packages.txt.
#
# Deliberately packages.txt and NOT the ramdisk baker's resolved closure. The
# package needs no state beyond its own tree and the bundled local repo, so it
# can be built without a debcache run having happened at all -- which is what
# keeps this a plain package build rather than something entangled with the
# bake.
#
# It is also more correct on-device. Bake-time resolution runs sandboxed
# against a synthetic "firmware" package, and a few entries legitimately fail
# there (build_deb_cache.py's KNOWN_EXPECTED_UNRESOLVABLE) while resolving
# fine against the real repos on a real device. Templating the resolved subset
# silently dropped those; packages.txt asks for what was actually asked for.
#
# cydia is filtered out: postinstall.sh installs it first, on its own, because
# its postinst does its own thing and nothing else may assume Cydia is
# configured yet. Leaving it in the array would install it twice.
POSTINSTALL="$STAGING/var/.blackb0x/postinstall.sh"
PLACEHOLDER=__BLACKB0X_PACKAGES__
: "${PACKAGES_FILE:=$(dirname "$0")/packages.txt}"

if [ -f "$POSTINSTALL" ] && grep -q "$PLACEHOLDER" "$POSTINSTALL"; then
    if [ ! -f "$PACKAGES_FILE" ]; then
        echo "$0: package list $PACKAGES_FILE does not exist" >&2
        exit 1
    fi
    PACKAGES=$(sed -e 's/#.*//' "$PACKAGES_FILE" | tr "\n" " " | xargs -n1 echo 2>/dev/null \
                 | grep -v '^cydia$' | tr "\n" " " | xargs echo)
    if [ -z "$PACKAGES" ]; then
        echo "$0: package list $PACKAGES_FILE yielded no package names" >&2
        exit 1
    fi
    # awk, not sed: package names are arbitrary text and sed would treat any
    # & or / among them as replacement syntax.
    awk -v repl="$PACKAGES" -v ph="$PLACEHOLDER" \
        '{ i = index($0, ph); if (i) { $0 = substr($0, 1, i - 1) repl substr($0, i + length(ph)) } print }' \
        "$POSTINSTALL" > "$POSTINSTALL.tmp"
    mv "$POSTINSTALL.tmp" "$POSTINSTALL"
    chmod 755 "$POSTINSTALL"
fi

OUT_DIR=$(cd "$(dirname "$OUTPUT")" && pwd)
OUT_NAME=$(basename "$OUTPUT")

# Deliberately NOT passing -u. Under rootless podman the CONTAINER's root maps
# to the invoking host user, so running as container root is what makes the
# output .deb come back owned by the caller. Passing -u 1000 instead maps to a
# subuid that owns nothing on the host, and dm.pl fails with a bare
# "Permission denied" writing its output -- which looks like a dm.pl bug rather
# than a uid-mapping one. BLACKB0X_THEOS_UID is kept as an escape hatch for a
# rootful podman/docker setup, where the mapping is the other way around.
UID_ARGS=""
if [ -n "$THEOS_UID" ]; then
    UID_ARGS="-u $THEOS_UID"
fi

# shellcheck disable=SC2086  # UID_ARGS is deliberately word-split
exec podman run --rm \
    $UID_ARGS \
    -e "THEOS=$THEOS_DIR" \
    -e "HOME=/tmp" \
    -v "$STAGING:/stage:Z" \
    -v "$OUT_DIR:/out:Z" \
    "$IMAGE" \
    bash -c "set -eu; \"\$THEOS/bin/dm.pl\" -b -Zgzip /stage \"/out/$OUT_NAME\""
