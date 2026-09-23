#!/bin/sh
# Build the xyz.regulad.blackb0x.icanhasrez .deb with Theos's dm.pl.
#
# Usage: tweaks/icanhasrez/build.sh <output.deb> [version]
#
# Environment:
#   THEOS               Theos checkout (default $HOME/theos).
#   TWEAK_DYLIB         prebuilt icanhasrez.dylib (default: build it).
#   XCODE_TOOLCHAIN     passed through to this tweak's Makefile.
#   XCODE_SEARCH_DIR    likewise.
#
# Mirrors tweaks/appliancetvtweak/build.sh exactly -- same dm.pl invocation,
# same mode assertion, same ownership reasoning -- because it has the same shape:
# a real compiled armv7 artifact plus a fixed layout, with no templating and
# no dependency on what a bake resolved.
#
# The split that applies to both: Theos supplies the PACKAGING half (dm.pl,
# plain Perl, no SDK), and a pinned era-matched Xcode supplies the COMPILATION
# half. Theos's own SDKs are arm64-only and cannot build either artifact. See
# this tweak's Makefile header for why Logos is not used either.
set -eu

THEOS_DIR="${THEOS:-$HOME/theos}"
# Everything this script needs is beside it -- the Makefile, the dylib it
# builds and layout/ -- so there is no repo-root variable at all any more.
# That is the point of the tweak owning its own build: moving the directory
# moves the whole thing, and nothing outside it has a path to fix.
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <output.deb> [version]" >&2
    exit 2
fi

OUTPUT=$1
VERSION=${2:-}

if [ ! -x "$THEOS_DIR/bin/dm.pl" ]; then
    echo "$0: no dm.pl at $THEOS_DIR/bin/dm.pl" >&2
    echo "    Install Theos (https://theos.dev) or point \$THEOS at an existing checkout." >&2
    exit 1
fi

LAYOUT="$HERE/layout"
[ -f "$LAYOUT/DEBIAN/control" ] || { echo "$0: missing $LAYOUT/DEBIAN/control" >&2; exit 1; }

STAGING=$(mktemp -d "${TMPDIR:-/tmp}/blackb0x-icanhasrez-stage-XXXXXX")
trap 'rm -rf "$STAGING"' EXIT INT TERM

cp -R "$LAYOUT/." "$STAGING/"

# ---- the dylib -----------------------------------------------------------
if [ -n "${TWEAK_DYLIB:-}" ]; then
    [ -f "$TWEAK_DYLIB" ] || { echo "$0: TWEAK_DYLIB=$TWEAK_DYLIB does not exist" >&2; exit 1; }
    DYLIB="$TWEAK_DYLIB"
else
    make -C "$HERE" >&2
    DYLIB="$HERE/icanhasrez.dylib"
fi

# Refuse to package something that is not what it claims to be. A tweak built
# for the wrong architecture, or built as an executable instead of a dylib,
# would install perfectly and then simply never load -- and the only symptom
# would be a resolution menu that looks exactly like it does today, which is
# indistinguishable from this tweak loading and finding nothing to add.
case "$(file -b "$DYLIB")" in
    *"dynamically linked shared library arm_v7"*) : ;;
    *) echo "$0: $DYLIB is not an armv7 Mach-O dylib ($(file -b "$DYLIB"))" >&2; exit 1 ;;
esac
if ! codesign -dv "$DYLIB" >/dev/null 2>&1; then
    echo "$0: $DYLIB carries no code signature -- this tweak's Makefile signs with ldid; is ldid installed?" >&2
    exit 1
fi

# The dylib and its filter plist must sit side by side under the same name --
# that pairing is the whole of MobileSubstrate's loading convention, and a
# mismatch means the dylib is simply never considered.
mkdir -p "$STAGING/Library/MobileSubstrate/DynamicLibraries"
cp "$DYLIB" "$STAGING/Library/MobileSubstrate/DynamicLibraries/icanhasrez.dylib"

if [ ! -f "$STAGING/Library/MobileSubstrate/DynamicLibraries/icanhasrez.plist" ]; then
    echo "$0: layout is missing the companion filter plist -- the dylib would never load" >&2
    exit 1
fi

# ---- version -------------------------------------------------------------
if [ -n "$VERSION" ]; then
    sed "s/__BLACKB0X_VERSION__/$VERSION/" "$STAGING/DEBIAN/control" > "$STAGING/DEBIAN/control.tmp"
    mv "$STAGING/DEBIAN/control.tmp" "$STAGING/DEBIAN/control"
fi
if grep -q '__BLACKB0X_VERSION__' "$STAGING/DEBIAN/control"; then
    echo "$0: DEBIAN/control still has an unsubstituted __BLACKB0X_VERSION__ -- pass a version" >&2
    exit 1
fi

# ---- modes and ownership -------------------------------------------------
#
# Same reasoning as package/build_cainjector.sh and package/build.sh: modes
# asserted here rather than inherited from the checkout's umask, ownership
# left to dm.pl's non-root branch (which stamps root:wheel by construction) or
# asserted explicitly when running as root so its preserve branch records the
# same thing.
find "$STAGING" -type d -exec chmod 755 {} +
find "$STAGING" -type f -exec chmod 644 {} +
# This layout ships no /etc/rc.d entry, unlike appliancetvtweak's: there is
# nothing for this package to do at every boot. The dylib is injected by
# MobileSubstrate into backboardd and that is the whole mechanism.
for f in DEBIAN/postinst DEBIAN/preinst DEBIAN/prerm DEBIAN/postrm \
         Library/MobileSubstrate/DynamicLibraries/icanhasrez.dylib; do
    [ -f "$STAGING/$f" ] && chmod 755 "$STAGING/$f"
done

if command -v plutil >/dev/null 2>&1; then
    plutil -lint "$STAGING/Library/MobileSubstrate/DynamicLibraries/icanhasrez.plist" >/dev/null \
        || { echo "$0: the Substrate filter plist is malformed" >&2; exit 1; }
fi

if [ "$(id -u)" = "0" ]; then
    chown -R 0:0 "$STAGING"
fi

exec "$THEOS_DIR/bin/dm.pl" -b -Zgzip "$STAGING" "$OUTPUT"
