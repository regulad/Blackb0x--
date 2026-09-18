#!/bin/sh
# Build the xyz.regulad.blackb0x .deb with Theos's dm.pl, inside
# ghcr.io/regulad/dotfiles:latest.
#
# Usage: package/build.sh <staging-dir> <output.deb> [version] [packages-file]
#
# [packages-file] overrides package/packages.txt; only useful for testing.
#
# Environment:
#   BLACKB0X_DEBS_DIR    where the local-only .deb files live
#                        (default Blackb0x/Debs).
#   LOCAL_ONLY_LIST      overrides package/local_only_debs.txt.
#
# <staging-dir> is a complete, ready-to-package tree: DEBIAN/ plus the payload
# laid out at its final on-device paths. BakeRamdisk.cpp assembles that tree at
# bake time (the payload is not static -- postinstall.sh is templated with the
# resolved package list, and the bundled local-repo .debs depend on what that
# bake actually resolved), then calls this.
#
# Why Theos and not plain dpkg-deb: dm.pl builds a correct .deb without root
# and without fakeroot, and it is the tool this ecosystem actually uses. That
# matters here because nothing else in the bake needs root any more (the one
# thing that did, the Linux HFS+ loop mount, is gone) and we do not want to
# reintroduce a reason to need it.
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

# The bundled file-backed apt repository at /var/.blackb0x/local-debs.
#
# Contents are exactly package/local_only_debs.txt -- every .deb that can only
# ever come from a local repo, because no live repo carries a usable stanza for
# it. No intersection with the bake's picklist: build_deb_cache.py adds every
# local-only filename to that picklist unconditionally (it only checks the file
# exists, fatally), so picklist n local_only_debs.txt is always just
# local_only_debs.txt. Filtering against it would be a guaranteed no-op that
# made this script need bake state it does not otherwise want.
#
# That is what keeps the package buildable on its own: everything it needs is
# checked in -- this list, packages.txt, layout/, and Blackb0x/Debs.
#
# The Packages index is generated in the container by the real
# dpkg-scanpackages (see the container command at the bottom) -- apt needs a
# real index, not just loose .deb bytes, to resolve these by name. The index
# is unsigned, which is why postinstall.sh installs with
# --allow-unauthenticated.
: "${LOCAL_ONLY_LIST:=$(dirname "$0")/local_only_debs.txt}"
: "${BLACKB0X_DEBS_DIR:=$(dirname "$0")/../Blackb0x/Debs}"
LOCAL_DEBS_DEST="$STAGING/var/.blackb0x/local-debs"

if [ -f "$LOCAL_ONLY_LIST" ]; then
    mkdir -p "$LOCAL_DEBS_DEST"
    WANTED=$(sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$LOCAL_ONLY_LIST" | grep -v '^$' || true)
    if [ -z "$WANTED" ]; then
        echo "$0: note: $LOCAL_ONLY_LIST lists nothing; local repo will be empty" >&2
    fi
    for f in $WANTED; do
        if [ ! -f "$BLACKB0X_DEBS_DIR/$f" ]; then
            echo "$0: $LOCAL_ONLY_LIST lists $f but $BLACKB0X_DEBS_DIR/$f is missing" >&2
            exit 1
        fi
        cp "$BLACKB0X_DEBS_DIR/$f" "$LOCAL_DEBS_DEST/$f"
        echo "$0: local repo <- $f" >&2
    done
else
    echo "$0: $LOCAL_ONLY_LIST does not exist" >&2
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
# Ownership and modes are ASSERTED here, immediately before dm.pl tars the
# tree -- not inherited from whatever the staging tree happened to carry.
#
# A .deb can express arbitrary uid/gid/mode per entry (data.tar records them),
# but a staging tree on a non-root host cannot: an ordinary user cannot chown
# a file to root, and modes come from whatever the checkout/umask produced. So
# the assertion has to happen at the point where we are root, which is inside
# the container.
#
# Without this it only LOOKED correct: under rootless podman the container's
# root maps to the invoking host user, so a host-owned bind mount appears
# root-owned inside and tar recorded 0/root by accident. Rootful podman, a set
# BLACKB0X_THEOS_UID, or a checkout with odd modes would all have changed the
# answer silently.
#
# Everything is root:wheel (0:0). That is correct for all of it: /etc/apt
# sources and keyrings, the LaunchDaemon plist, root's own .profile, and
# /var/.blackb0x. launchd in particular REFUSES to load a plist that is not
# root-owned or is group/world-writable, which fails as "the daemon simply
# never ran" with nothing pointing at permissions.
#
# Directories 0755, files 0644, then the two things that must execute.
PERMS_SCRIPT='
set -eu
# Real dpkg-scanpackages, not a hand-rolled index: apt is strict about the
# fields and checksums it expects here.
if [ -d /stage/var/.blackb0x/local-debs ] && ls /stage/var/.blackb0x/local-debs/*.deb >/dev/null 2>&1; then
    ( cd /stage/var/.blackb0x/local-debs && dpkg-scanpackages . /dev/null 2>/dev/null > Packages )
fi
chown -R 0:0 /stage
find /stage -type d -exec chmod 755 {} +
find /stage -type f -exec chmod 644 {} +
[ -f /stage/DEBIAN/postinst ] && chmod 755 /stage/DEBIAN/postinst
[ -f /stage/DEBIAN/preinst ] && chmod 755 /stage/DEBIAN/preinst
[ -f /stage/DEBIAN/prerm ] && chmod 755 /stage/DEBIAN/prerm
[ -f /stage/DEBIAN/postrm ] && chmod 755 /stage/DEBIAN/postrm
[ -f /stage/var/.blackb0x/postinstall.sh ] && chmod 755 /stage/var/.blackb0x/postinstall.sh
true
'

exec podman run --rm \
    $UID_ARGS \
    -e "THEOS=$THEOS_DIR" \
    -e "HOME=/tmp" \
    -v "$STAGING:/stage:Z" \
    -v "$OUT_DIR:/out:Z" \
    "$IMAGE" \
    bash -c "$PERMS_SCRIPT
set -eu
\"\$THEOS/bin/dm.pl\" -b -Zgzip /stage \"/out/$OUT_NAME\""
