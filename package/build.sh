#!/bin/sh
# Build the xyz.regulad.blackb0x .deb with Theos's dm.pl, inside
# ghcr.io/regulad/dotfiles:latest.
#
# Usage: package/build.sh <staging-dir> <output.deb> [version]
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
    echo "usage: $0 <staging-dir> <output.deb> [version]" >&2
    exit 2
fi

STAGING=$(cd "$1" && pwd)
OUTPUT=$2
VERSION=${3:-}

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
