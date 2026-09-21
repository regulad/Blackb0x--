#!/bin/sh
# Build the xyz.regulad.blackb0x .deb with Theos's dm.pl.
#
# Usage: package/build.sh <staging-dir> <output.deb> [version] [packages-file]
#
# [packages-file] overrides package/packages.txt; only useful for testing.
#
# Environment:
#   THEOS                Theos checkout (default $HOME/theos).
#   BLACKB0X_DEBCACHE_DIR    where the local-only .deb files live
#                        (default debcache).
#   LOCAL_ONLY_LIST      overrides package/local_only_debs.txt.
#
# <staging-dir> is a complete, ready-to-package tree: DEBIAN/ plus the payload
# laid out at its final on-device paths. BakeRamdisk.cpp assembles that tree at
# bake time (the payload is not static -- postinstall.sh is templated with the
# resolved package list, and the bundled local-repo .debs depend on what that
# bake actually resolved), then calls this.
#
# This used to run inside ghcr.io/regulad/dotfiles:latest under podman. The
# container existed to supply a Linux box with Theos on it; Theos is macOS-
# native and dm.pl is plain Perl, so on this project's only supported host
# there is nothing left for a container to provide. See the ownership note
# above the dm.pl call for why running it natively is also strictly MORE
# correct than running it as container root.
#
# Why Theos and not plain dpkg-deb: dm.pl builds a correct .deb without root
# and without fakeroot, and it is the tool this ecosystem actually uses. That
# matters here because nothing else in the bake needs root any more (the one
# thing that did, the Linux HFS+ loop mount, is gone) and we do not want to
# reintroduce a reason to need it.
#
# Notably we use NONE of Theos's compilation half. This package is pure data --
# apt sources, gpg keys, a LaunchDaemon plist, shell scripts, and the bundled
# .debs. Nothing is compiled, so Theos's SDKs never come into it. If this
# package ever grows a real binary, that is a genuine problem to solve, not a
# detail -- no modern SDK can target armv7.
set -eu

THEOS_DIR="${THEOS:-$HOME/theos}"

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <staging-dir> <output.deb> [version] [packages-file]" >&2
    exit 2
fi

STAGING=$(cd "$1" && pwd)
OUTPUT=$2
VERSION=${3:-}
PACKAGES_FILE=${4:-}
# Empty means "use package/packages.txt" (set below).

if [ ! -x "$THEOS_DIR/bin/dm.pl" ]; then
    echo "$0: no dm.pl at $THEOS_DIR/bin/dm.pl" >&2
    echo "    Install Theos (https://theos.dev) or point \$THEOS at an existing checkout." >&2
    exit 1
fi

if ! command -v dpkg-scanpackages >/dev/null 2>&1; then
    echo "$0: dpkg-scanpackages not found -- install dpkg (brew install dpkg)" >&2
    exit 1
fi

if [ ! -f "$STAGING/DEBIAN/control" ]; then
    echo "$0: $STAGING has no DEBIAN/control -- not a staging tree" >&2
    exit 1
fi

# Not `sed -i`: this used to run under GNU sed inside the container, and BSD
# sed (the one on the host now) spells the in-place flag differently. A temp
# file is the spelling both agree on.
if [ -n "$VERSION" ]; then
    sed "s/__BLACKB0X_VERSION__/$VERSION/" "$STAGING/DEBIAN/control" > "$STAGING/DEBIAN/control.tmp"
    mv "$STAGING/DEBIAN/control.tmp" "$STAGING/DEBIAN/control"
fi
if grep -q '__BLACKB0X_VERSION__' "$STAGING/DEBIAN/control"; then
    echo "$0: DEBIAN/control still has an unsubstituted __BLACKB0X_VERSION__" >&2
    exit 1
fi

# The bundled file-backed apt repository at /usr/share/blackb0x/local-debs.
#
# Contents are exactly package/local_only_debs.txt -- every .deb that can only
# ever come from a local repo, because no live repo carries a usable stanza for
# it.
#
# THE SYSTEM PARTITION IS ITS PERMANENT HOME, not a staging area it gets moved
# out of. Two reasons, and the second is the one that made it the right call
# rather than merely the convenient one.
#
# It has to be off /var: the restore ramdisk that installs this package cannot
# create a regular file on the data partition at all -- open(O_CREAT) returns
# EPERM there even as root while mkdir() on the same volume succeeds, which is
# iOS content protection with no keybag loaded. 130 merge entries failed on
# hardware and every one of them was bound for /var.
#
# But it also BELONGS here. A read-only repository of .deb bytes that ships
# with the package and is never written to is exactly what /usr/share is for,
# and everything bound for /var has to be copied by the first-boot daemon and
# then deleted from the stage -- work this repo would be paying for no reason.
# So it is not staged and migrated like the rest; it simply lives here, and
# sources.list.d/local.list points straight at it.
#
# The Packages index is generated below by the real dpkg-scanpackages -- apt
# needs a real index, not just loose .deb bytes, to resolve these by name. The
# index is unsigned, which is why postinstall.sh installs with
# --allow-unauthenticated.
: "${LOCAL_ONLY_LIST:=$(dirname "$0")/local_only_debs.txt}"
: "${BLACKB0X_DEBCACHE_DIR:=$(dirname "$0")/../debcache}"
LOCAL_DEBS_DEST="$STAGING/usr/share/blackb0x/local-debs"

if [ -f "$LOCAL_ONLY_LIST" ]; then
    mkdir -p "$LOCAL_DEBS_DEST"
    WANTED=$(sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$LOCAL_ONLY_LIST" | grep -v '^$' || true)
    if [ -z "$WANTED" ]; then
        echo "$0: note: $LOCAL_ONLY_LIST lists nothing; local repo will be empty" >&2
    fi
    for f in $WANTED; do
        if [ ! -f "$BLACKB0X_DEBCACHE_DIR/$f" ]; then
            echo "$0: $LOCAL_ONLY_LIST lists $f but $BLACKB0X_DEBCACHE_DIR/$f is missing" >&2
            exit 1
        fi
        cp "$BLACKB0X_DEBCACHE_DIR/$f" "$LOCAL_DEBS_DEST/$f"
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
# there while resolving fine against the real repos on a real device.
# Templating the resolved subset silently dropped those; packages.txt asks for
# what was actually asked for.
#
# cydia is filtered out: postinstall.sh installs it first, on its own, because
# its postinst does its own thing and nothing else may assume Cydia is
# configured yet. Leaving it in the array would install it twice.
POSTINSTALL="$STAGING/usr/share/blackb0x/postinstall.sh"
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

# Real dpkg-scanpackages, not a hand-rolled index: apt is strict about the
# fields and checksums it expects here.
if [ -d "$LOCAL_DEBS_DEST" ] && ls "$LOCAL_DEBS_DEST"/*.deb >/dev/null 2>&1; then
    ( cd "$LOCAL_DEBS_DEST" && dpkg-scanpackages . /dev/null 2>/dev/null > Packages )
fi

# Modes are ASSERTED here, immediately before dm.pl tars the tree -- not
# inherited from whatever the checkout/umask produced. Directories 0755,
# files 0644, then the handful of things that must execute.
find "$STAGING" -type d -exec chmod 755 {} +
find "$STAGING" -type f -exec chmod 644 {} +
for f in DEBIAN/postinst DEBIAN/preinst DEBIAN/prerm DEBIAN/postrm usr/share/blackb0x/postinstall.sh; do
    [ -f "$STAGING/$f" ] && chmod 755 "$STAGING/$f"
done

# OWNERSHIP: deliberately NOT chown'd, and this script deliberately refuses to
# run as root.
#
# dm.pl decides entry ownership by looking at its own euid:
#     if ($< == 0) { $tf->chown($stat[4], $stat[5]); }   # preserve on-disk
#     else         { $tf->chown("root", "wheel"); }      # force 0:0
# So a NON-root dm.pl stamps every entry root:wheel by construction, which is
# exactly what this package wants for all of it: /etc/apt sources and
# keyrings, the LaunchDaemon plist, root's own .profile, and
# /usr/share/blackb0x.
# launchd in particular REFUSES to load a plist that is not root-owned or is
# group/world-writable, and fails as "the daemon simply never ran" with
# nothing pointing at permissions.
#
# As root, dm.pl takes the other branch and records whatever the staging tree
# actually carries -- which is the invoking user, not root:wheel, unless we say
# otherwise. So under root we assert the ownership explicitly and let dm.pl
# preserve it. Both branches then produce byte-identical root:wheel entries;
# they just get there from opposite directions.
#
# This used to be a hard refusal ("run as an ordinary user"), which deadlocked
# the one caller that matters: bakeRamdisk() REQUIRES root (it chown()s staged
# content to root:wheel and writes into root-owned files on the mounted
# ramdisk), and it invokes this script through stageBlackb0xPackage(). Root
# required on one side, root refused on the other, so the ramdisk bake could
# never complete. The refusal was never wrong about dm.pl's behaviour, only
# about there being nothing to correct -- the old containerized build corrected
# it with exactly this `chown -R 0:0`, and that remains the right answer
# whenever euid is 0.
if [ "$(id -u)" = "0" ]; then
    echo "$0: running as root -- asserting root:wheel on the staging tree so dm.pl's" >&2
    echo "    preserve-on-disk branch records the same ownership its non-root branch forces." >&2
    chown -R 0:0 "$STAGING"
fi

exec "$THEOS_DIR/bin/dm.pl" -b -Zgzip "$STAGING" "$OUTPUT"
