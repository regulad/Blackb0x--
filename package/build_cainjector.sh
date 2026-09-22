#!/bin/sh
# Build the xyz.regulad.blackb0x.cainjector .deb with Theos's dm.pl.
#
# Usage: package/build_cainjector.sh <output.deb> [version]
#
# Environment:
#   THEOS               Theos checkout (default $HOME/theos).
#   CAINJECTOR_BINARY   prebuilt cainjector binary (default: build it here).
#   XCODE_TOOLCHAIN     passed through to cainjector/Makefile (see that file).
#   XCODE_SEARCH_DIR    likewise.
#
# This mirrors package/build.sh -- same dm.pl invocation, same ownership
# reasoning, same refusal to silently do the wrong thing -- but it is a
# SEPARATE script rather than a flag on that one, because the two packages
# have genuinely different inputs: that one is pure data templated from a
# resolved package list, this one has a real compiled armv7 binary.
#
# THAT DIFFERENCE IS THE INTERESTING PART. package/build.sh's own header says
# "we use NONE of Theos's compilation half... If this package ever grows a
# real binary, that is a genuine problem to solve, not a detail -- no modern
# SDK can target armv7." This is that case, and the answer is the one
# entrypoint/ already worked out: Theos supplies the PACKAGING half (dm.pl,
# plain Perl, no SDK involved), and a pinned era-matched Xcode supplies the
# COMPILATION half. Theos's own SDKs are useless for it -- the vendored ones
# are arm64-only, since Apple dropped 32-bit iOS SDKs after ~2018.
set -eu

THEOS_DIR="${THEOS:-$HOME/theos}"
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

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

if ! command -v openssl >/dev/null 2>&1; then
    echo "$0: openssl not found -- needed to split the CA bundle into DER" >&2
    exit 1
fi

LAYOUT="$REPO/package/cainjector-layout"
PEM="$REPO/cainjector/ca-certificates.pem"

[ -f "$LAYOUT/DEBIAN/control" ] || { echo "$0: missing $LAYOUT/DEBIAN/control" >&2; exit 1; }
[ -f "$PEM" ] || { echo "$0: missing $PEM (see cainjector/README.md)" >&2; exit 1; }

STAGING=$(mktemp -d "${TMPDIR:-/tmp}/blackb0x-cainjector-stage-XXXXXX")
trap 'rm -rf "$STAGING"' EXIT INT TERM

cp -R "$LAYOUT/." "$STAGING/"

# ---- the binary ----------------------------------------------------------
#
# Built by cainjector/Makefile against a pinned, era-matched Xcode, or taken
# prebuilt from $CAINJECTOR_BINARY (which is what CI hands over, so a runner
# does not need to resolve an Xcode at package time).
if [ -n "${CAINJECTOR_BINARY:-}" ]; then
    [ -f "$CAINJECTOR_BINARY" ] || { echo "$0: CAINJECTOR_BINARY=$CAINJECTOR_BINARY does not exist" >&2; exit 1; }
    BIN="$CAINJECTOR_BINARY"
else
    make -C "$REPO/cainjector" >&2
    BIN="$REPO/cainjector/cainjector"
fi

# Refuse to package something that is not what it claims to be. A build that
# silently produced an x86_64 host binary, or an unsigned one, would install
# fine and then simply never run on the device -- with no console to say why.
case "$(file -b "$BIN")" in
    *arm_v7*) : ;;
    *) echo "$0: $BIN is not an armv7 Mach-O ($(file -b "$BIN"))" >&2; exit 1 ;;
esac
if ! codesign -dv "$BIN" >/dev/null 2>&1; then
    echo "$0: $BIN carries no code signature -- cainjector/Makefile signs with ldid; is ldid installed?" >&2
    exit 1
fi

mkdir -p "$STAGING/usr/libexec/blackb0x"
cp "$BIN" "$STAGING/usr/libexec/blackb0x/cainjector"

# ---- the certificates ----------------------------------------------------
#
# ONE DER FILE PER CERTIFICATE, split and converted HERE rather than on the
# device. cainjector deliberately does not parse PEM: doing the base64 decode
# on the build host, where there is a real openssl, keeps a base64 decoder and
# a PEM state machine out of a program that runs as root on a device with no
# console. It also means the on-device work is a plain read() + CFDataCreate.
CERT_DEST="$STAGING/usr/share/blackb0x/cainjector/certs"
mkdir -p "$CERT_DEST"

# Split the concatenated bundle on END CERTIFICATE boundaries, then convert
# each one. awk rather than csplit: csplit's flags differ between BSD and GNU
# and this has to run on macOS.
SPLIT_DIR="$STAGING/.pem-split"
mkdir -p "$SPLIT_DIR"
awk -v dir="$SPLIT_DIR" '
    /BEGIN CERTIFICATE/ { n++; f = sprintf("%s/%04d.pem", dir, n) }
    f { print > f }
    /END CERTIFICATE/ { close(f); f = "" }
' "$PEM"

COUNT=0
for p in "$SPLIT_DIR"/*.pem; do
    [ -f "$p" ] || continue
    base=$(basename "$p" .pem)
    if openssl x509 -inform PEM -outform DER -in "$p" -out "$CERT_DEST/$base.der" 2>/dev/null; then
        COUNT=$((COUNT + 1))
    else
        echo "$0: $p is not a parseable certificate" >&2
        exit 1
    fi
done
rm -rf "$SPLIT_DIR"

# ---- the same store, again, for OpenSSL ----------------------------------
#
# The DER directory above is for SecTrustStore, which is Apple's stack and the
# only one apt's CFNetwork-based transport consults. The vendored
# openssl 0.9.8zg is a SECOND, completely independent trust store that knows
# nothing about it, so it gets its own copy in the form it actually looks for.
#
# /usr/lib/ssl/cert.pem, specifically, and all three parts of that were
# measured rather than assumed:
#   * OPENSSLDIR is "/usr/lib/ssl" (a literal in the real
#     libcrypto.0.9.8.dylib, alongside "/usr/lib/ssl/cert.pem" and
#     "/usr/lib/ssl/certs"), so this is where SSL_CTX_set_default_verify_paths
#     looks with no environment set.
#   * Nothing owns that path. The openssl package ships /etc/ssl/openssl.cnf,
#     an EMPTY /etc/ssl/certs, and a /usr/lib/ssl/certs symlink to it -- but no
#     cert.pem at all, and it has no postinst, so that directory ships empty
#     and stays empty forever. Stock 12H1006 has no /etc/ssl, /usr/lib/ssl,
#     /usr/ssl or /System/Library/OpenSSL either.
#   * The single-file form is REQUIRED, not merely simpler. OpenSSL's hashed
#     directory form needs <subject_hash>.0 names, and 0.9.8 hashes subjects
#     with MD5 while 1.0.0+ uses SHA-1 -- ISRG_Root_X1 is 6187b673 on the
#     device and 4042bcee from any modern openssl. A directory rehashed on
#     this build host would be silently invisible to the device.
#
# CURL IS NOT FIXED BY THIS, and no file placement can fix it: its libcurl
# imports _SSL_CTX_load_verify_locations and NOT
# _SSL_CTX_set_default_verify_paths (checked with nm -u), and
# `curl-config --ca` is empty, so it was built with no compiled-in bundle and
# never falls back to OpenSSL's defaults. curl needs CURL_CA_BUNDLE, --cacert,
# or CURLOPT_CAINFO pointed at this same file. See cainjector/README.md.
mkdir -p "$STAGING/usr/lib/ssl"
cp "$PEM" "$STAGING/usr/lib/ssl/cert.pem"

if [ "$COUNT" -eq 0 ]; then
    echo "$0: $PEM yielded no certificates" >&2
    exit 1
fi
echo "$0: staged $COUNT DER certificates" >&2

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
# Same reasoning as package/build.sh: modes asserted here rather than
# inherited from the checkout's umask, and ownership left to dm.pl's non-root
# branch (which stamps root:wheel by construction) or asserted explicitly when
# running as root so its preserve branch records the same thing. launchd
# REFUSES to load a plist that is not root-owned or is group/world-writable,
# and fails as "the daemon simply never ran" with nothing pointing at
# permissions.
find "$STAGING" -type d -exec chmod 755 {} +
find "$STAGING" -type f -exec chmod 644 {} +
# The executables, re-marked after the blanket 644 above. DEBIAN/postinst is
# on this list because dm.pl refuses outright otherwise ("maintainer script
# 'postinst' has bad permissions 644 (must be >=0555 and <=0775)") -- which is
# a good failure, but only a failure at build time if it is remembered here.
#
# The profile drop-in is 755 to match the ecosystem: profile.d's own loader
# only tests `[ -r "$i" ]` before sourcing it, so the executable bit is not
# strictly required -- but coreutils.sh and less.sh, the two drop-ins already
# installed on these devices, are both 755, and matching them keeps `ls
# /etc/profile.d` from having one odd entry out.
for f in DEBIAN/postinst DEBIAN/preinst DEBIAN/prerm DEBIAN/postrm \
         usr/share/blackb0x/cainjector/run.sh usr/libexec/blackb0x/cainjector \
         etc/profile.d/blackb0x-cainjector.sh; do
    [ -f "$STAGING/$f" ] && chmod 755 "$STAGING/$f"
done

if ! bash -n "$STAGING/usr/share/blackb0x/cainjector/run.sh"; then
    echo "$0: run.sh is not valid bash" >&2
    exit 1
fi

if [ "$(id -u)" = "0" ]; then
    chown -R 0:0 "$STAGING"
fi

exec "$THEOS_DIR/bin/dm.pl" -b -Zgzip "$STAGING" "$OUTPUT"
