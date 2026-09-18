#!/usr/bin/env python3
"""
build_deb_cache.py — resolve package/packages.txt's flat package-name
list against this project's real apt sources (Blackb0x/Misc/apt/*.list),
fetch the full dependency closure, and grow Blackb0x/Debs/ with whatever's
new.

*** LINUX-ONLY MAINTENANCE TOOL. THE BAKE NEVER RUNS THIS. ***

It needs podman, and podman does not exist on macOS — which is this
project's only supported platform now. bakeRamdisk()'s
computeGlobalDebcacheOnce() calls
scripts/build_deb_cache_experimental_no_container.py instead,
unconditionally; nothing in the build or the bake invokes this file at all.

It is kept, rather than deleted with the rest of the podman machinery,
because it is the only thing that can correctly GROW Blackb0x/Debs/: that
needs a real apt-get dependency solve with real version constraints and
GPG-verified fetches from live repos, and the no-container stand-in
explicitly cannot do any of that (see its own docstring's "Known gaps").
It is also the reference this project's debcache contract is defined
against, cited by name throughout BakeRamdisk.cpp and Blackb0x/Misc/README.md.

So: run this by hand, on a Linux box with podman, when and only when
Blackb0x/Debs/ needs new packages in it. Then commit the resulting .deb
bytes, which is what every macOS bake actually consumes.

Deliberately does NOT reimplement dependency resolution, repo-index parsing,
or GPG/hash verification — those are exactly what apt itself already does
correctly, and hand-rolling a second implementation of any of them is
precisely the kind of wheel-reinvention this project's own conventions
warn against (see AGENTS.md: "prefer adapting an existing, battle-tested
library"). There's no actively-maintained *pure*-Python apt implementation
that does real dependency resolution + fetch + verification well enough to
trust here (python-apt/apt_pkg is the real thing, but it's a C-extension
binding to libapt-pkg, not pure Python, and it's finicky to sandbox against
a foreign architecture on a non-Debian host) — so this shells out to the
real `apt-get`/`apt-cache` binaries instead, inside a throwaway podman
container (matching entrypoint/'s and fetch_firmware_component.py's own
established "heavy tooling runs in a container, nothing touches the host"
convention), pointed at a fully sandboxed apt root (`-o Dir=...`) that never
touches the container's own system state either.

How trust is decided per repo, matching Blackb0x/Misc/README.md's own
documented findings from testing each of these directly — and one real
surprise hit while building this script, documented here rather than
silently worked around:
  - A repo with a paired Blackb0x/Misc/apt/<name>.gpg.key (awkwardtv, saurik,
    regulad) gets that key imported and its Release/Release.gpg verified
    directly with plain `gpgv` — the whole run aborts if that fails.
  - This does NOT go through apt's own built-in Release verification,
    even though that was the original design: apt's internal gpgv call
    passes `--weak-digest sha1` (rejecting SHA-1 as an *insecure* digest,
    not "tolerate weak" — a very common gpg option-naming trap), which
    correctly rejects awkwardtv's and saurik's Release.gpg signatures —
    both made with 2008-era DSA-1024 keys, both using SHA-1. These
    signatures are NOT forged or tampered — confirmed by running plain
    `gpgv`/`gpg --verify` (no weak-digest flag) against the exact same
    keyring and getting a real "Good signature" both times — apt just
    refuses on algorithm-age policy grounds no `apt.conf` knob overrides
    (`Acquire::AllowWeakRepositories` does NOT affect this; tested).
    ios.regulad.xyz's own key is modern (RSA-4096) and verifies fine
    through apt directly — it's specifically the two legacy DSA keys apt
    itself won't accept. So: verify with plain gpgv ourselves (still real
    GPG verification, still aborts the run on failure), then mark every
    source `[trusted=yes]` for apt's own fetch step, since apt's internal
    check is what's unusable here, not the signatures.
  - A repo with no paired key (xbmc.list — confirmed to have no Release/
    Release.gpg/InRelease at all, a bare unsigned flat repo) is marked
    `[trusted=yes]` for the same reason it always was: there's nothing to
    verify at all, keyed or not.

Never deletes or overwrites anything already in Blackb0x/Debs/ — an
existing file at a given filename is left completely untouched, even if
this run would have fetched different bytes for that exact name (matters
for the already-documented pinned-filename mismatches — see Misc/README.md's
"Deb filenames are pinned to exact old revisions" section). Only adds files
that don't already exist.

Package names in packages.txt that don't resolve to a real package in any
configured repo are FATAL for the whole run, with a deliberate exception
(KNOWN_EXPECTED_UNRESOLVABLE below): com.ih8sn0w-squiffy-winocm.p0sixspwn
has a real Packages stanza but genuinely doesn't satisfy its own Depends:
firmware constraint against this sandbox's synthetic firmware version —
expected, not a bug (see INNER_SCRIPT's own comment). net.tihmstar.etasonuntether
is a second exception for the same reason — it is applied statically by
BakeRamdisk.cpp's stageEtasonatv() rather than by apt. Anything else failing to resolve is exactly
the kind of silent breakage this used to just warn-and-skip past — real
packages this project actually needs
(see the "essential" incident: it turned out to be genuinely gone from
every configured repo's live Packages index, and nothing noticed for a
while because the warning was easy to miss in a long build log) — so it
raises SystemExit immediately instead.

Package names that will never resolve through live apt AT ALL — dead
upstream, not merely firmware-gated — but that this project already has a
real, previously-recovered .deb for go in package/local_only_debs.txt
instead of packages.txt's normal resolution flow entirely: see that file's
own comment for the full rationale and how it differs from the p0sixspwn
case above. Their filenames still go straight into picklist.txt (so
they're cached in Blackb0x/Debs/ and staged into the real on-device apt
cache like anything else), and this script ALSO builds a real local `deb
file://` repository for them — a genuine `dpkg-scanpackages`-generated
Packages index, not just loose .deb bytes with no index apt could ever
resolve by name — because a bake-time cache alone doesn't help the actual
DEVICE: postinstall.sh's own runtime `apt-get install` has to be able to
find "essential" by name too, the exact same way it finds every other
package, and there's no live repo anywhere that still carries it. See
Blackb0x/Misc/apt/local.list and BakeRamdisk.cpp's staging of
<output-dir>/local-repo/ for the on-device half of this.

Writes <output-dir>/picklist.txt: the sorted list of every .deb filename
this run's dependency resolution actually needs — this is what
BakeRamdisk.cpp's stageDebcache() copies from Blackb0x/Debs/ directly into
the real device's own apt cache (/var/cache/apt/archives/), not the whole
(append-only, never-pruned) Debs/ directory. <output-dir> is caller-owned,
not this script's concern to create or clean up.

Writes <output-dir>/resolved_packages.txt: the subset of
package/packages.txt's own names that genuinely resolved through real
apt (as opposed to picklist.txt's full transitive closure of .deb
filenames) — this is what stageDebcache() bakes into postinstall.sh's own
install array, so postinstall.sh never has to guess which of
packages.txt's entries are real, installable package names versus the
handful of known leaked-in non-names (bigboss/modmyifone/saurik/zodttd —
see Blackb0x/Misc/README.md) that would abort `apt-get install` outright if
ever passed to it directly. Deliberately excludes local_only_debs.txt's
entries — apt could never resolve those by name either, on-device any more
than here.

Writes <output-dir>/local-repo/: a real, `dpkg-scanpackages`-generated
Packages index plus copies of local_only_debs.txt's actual .deb files —
BakeRamdisk.cpp stages this verbatim at
/var/.blackb0x/local-debs/ on-device, matching Blackb0x/Misc/apt/
local.list's `deb [trusted=yes] file:///var/.blackb0x/local-debs ./`
source entry, so the real device's own apt-get can resolve and install
these by name (e.g. "essential") exactly like anything else, with zero
network involved.

This script has no notion of "which of these resolved packages get
preinstalled at bake time vs. left for postinstall.sh's own apt-get" —
that whole decision (the prebake blacklist, the dependency-graph closure
over it, the actual `dpkg --unpack`/status-flip/consistency-audit work)
lives entirely in BakeRamdisk.cpp's stagePreinstalledPackages(), which
reads the real .deb files this script fetched directly. This script's job
ends at "here's what's needed and here's where it's fetched" — it doesn't
need to know what happens to any of it afterward.

Writes <output-dir>/apt-lists/: a verbatim copy of this run's own sandboxed
`apt-get update` cache (/var/lib/apt/lists/ from inside the container) —
staged onto the device at the same real path so apt already knows what
every configured repo currently offers without needing network at install
time, even for packages whose .deb bytes weren't small enough to also ship
locally (see BakeRamdisk.cpp's kNeverStageDebs — Kodi's ~40MB .deb alone
would blow this old ramdisk's 70MB ceiling). Fetched with
`Acquire::By-Hash=no` specifically so the on-disk file layout stays the
plain, flat naming convention this project's own vendored apt7 0.7.25.3
(confirmed by extracting Blackb0x/Debs/apt7_*.deb directly and reading its
apt-get binary's own strings) is old enough to predate by-hash support for.

The local Packages index (local-repo/, above) is back for a
narrow reason after once being removed entirely: that removal was correct
for every package that resolves through a real, live repo (apt already
knows about and can find those through its normal configuration, no local
index needed) — it just didn't anticipate a package resolving through NO
live repo at all while still genuinely being needed. local_only_debs.txt
is exactly that narrow case, not a reversion of the original decision.

Usage:
    scripts/build_deb_cache.py --output-dir DIR --firmware-version VERSION [--keep-sandbox]

Requires: podman on PATH (or /usr/bin/podman directly — see PODMAN below).
Nothing else; the container provides apt/dpkg/gnupg itself.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MISC_DIR = REPO_ROOT / "Blackb0x" / "Misc"
APT_DIR = MISC_DIR / "apt"
DEBS_DIR = REPO_ROOT / "Blackb0x" / "Debs"
PACKAGE_DIR = REPO_ROOT / "package"
# packages.txt and local_only_debs.txt describe what the
# xyz.regulad.blackb0x package installs, so they live with the package
# rather than in Blackb0x/Misc (which is bake-time host assets).
PACKAGES_LIST = PACKAGE_DIR / "packages.txt"
LOCAL_ONLY_LIST = PACKAGE_DIR / "local_only_debs.txt"

# See this script's own module docstring and local_only_debs.txt's comment
# for why p0sixspwn specifically is a tolerated, expected resolution
# failure rather than the fatal one everything else now is — a real
# firmware-version-gated persistence payload (6.1.4) that BakeRamdisk.cpp's
# stageP0sixspwn() extracts straight out of its real .deb in
# Blackb0x/Debs/ instead of going through apt resolution.
#
# net.tihmstar.etasonuntether is here for the same firmware-gated reason:
# Depends: firmware (= 8.4.1) cannot satisfy this sandbox's synthetic firmware
# pin below. It briefly moved to local_only_debs.txt instead (resolving through
# a local file:// repo rather than repo.tihmstar.net, whose live
# Packages/Release have been seen out of sync — see
# Blackb0x/Misc/apt/net.tihmstar.list.disabled), but it is applied STATICALLY
# by BakeRamdisk.cpp's stageEtasonatv(), with this project's own untether.bin
# overriding the one its .deb ships. Shipping it through a local repo as well
# meant two install paths for one package, so it is back to being a tolerated
# resolution failure here, exactly like p0sixspwn above it.
KNOWN_EXPECTED_UNRESOLVABLE = {
    "com.ih8sn0w-squiffy-winocm.p0sixspwn",
    "net.tihmstar.etasonuntether",
}

# The one and only architecture every repo this project uses actually
# ships — not a "foreign" arch needing multiarch juggling, just the
# sandbox's native one.
ARCH = "iphoneos-arm"

# /usr/bin/podman explicitly, not whatever "podman" resolves to on PATH —
# this dev machine has a broken linuxbrew-shadowed podman earlier on PATH
# than the real system one (see entrypoint/README.md's own note on this,
# and scripts/fetch_firmware_component.py's identical constant).
PODMAN = "/usr/bin/podman" if os.path.exists("/usr/bin/podman") else "podman"

DEB_LINE_RE = re.compile(r"^deb\s+(?:\[[^\]]*\]\s+)?(\S+)\s+(\S+)(?:\s+(.*))?$")

INNER_SCRIPT = r"""#!/bin/sh
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq gnupg ca-certificates curl dpkg-dev >/dev/null

# local_only_debs.txt's entries (see the module docstring), if any — a
# real Packages index for the exact loose .deb files the host side already
# copied into /work/local-debs/, generated with the real dpkg-dev tool
# rather than hand-rolling the format. dpkg-scanpackages writes Filename:
# relative to its own cwd, which is exactly the layout a flat `deb
# file:///.../local-debs ./` on-device source needs — the Packages file
# and the .debs it describes end up sitting right next to each other, both
# relative to the same repo root. Plain, uncompressed Packages only —
# apt has always fallen back to this when no compressed variant exists,
# and there's no bandwidth reason to gzip a local file:// source anyway.
if [ -d /work/local-debs ] && [ -n "$(ls -A /work/local-debs 2>/dev/null)" ]; then
    ( cd /work/local-debs && dpkg-scanpackages . /dev/null 2>/dev/null > Packages )
fi

mkdir -p /sandbox/etc/apt/trusted.gpg.d /sandbox/etc/apt/preferences.d
mkdir -p /sandbox/var/lib/dpkg /sandbox/var/lib/apt/lists/partial
mkdir -p /sandbox/var/cache/apt/archives/partial
cp /work/sources.list /sandbox/etc/apt/sources.list

# Real devices' own dpkg status always declares a synthetic "firmware"
# package at their actual running OS version — a chunk of this ecosystem's
# packages gate their Depends: on a firmware version range instead of a
# real package (e.g. "firmware (>= 6.0)"), and on a real device that's
# satisfied by Cydia's own injected entry for whatever iOS the device
# happens to run. Our sandbox has no real device, so nothing satisfies
# those Depends: at all unless we declare the same thing ourselves.
# Was previously hardcoded to a single literal pin, 8.4.2 (AppleTV3,2 —
# this project's newest/most-capable supported target at the time, and the
# version-era saurik.list's own "ios/8.0" dist choice already targets).
# That was wrong for every OTHER (device, buildID) tuple this project
# bakes: a package correctly gated to (say) `firmware (>= 7.0)` would
# falsely resolve/install even while baking a 6.1.x ramdisk, since the
# sandbox always claimed to be running 8.4.2 regardless of which real
# firmware was actually being baked. Now filled in from --firmware-version
# below, the real, per-tuple ProductVersion from that exact build's own
# BuildManifest.plist (threaded down from BakeFirmware.cpp's main loop
# through bakeRamdisk() -> stageBlackb0xTree() -> stageDebcache() ->
# computeGlobalDebcacheOnce()), so this sandbox's synthetic firmware
# declaration always matches the real device this run is actually baking
# for. Some packages are genuinely gated to a DIFFERENT range only
# (com.ih8sn0w-squiffy-winocm.p0sixspwn needs firmware < 7.0, since it's a
# 6.1.4-only untether payload) — with the real per-tuple version now in
# play, whether that correctly resolves or correctly fails depends on
# which firmware is actually being baked, which is the accurate behavior;
# it no longer unconditionally fails the way it did under the old fixed
# 8.4.2 pin. This project's own persistence-establishing packages for
# OTHER firmware branches are already installed as direct loose-file
# copies, not through apt (see .claude/NEO_FLOW.md) — packages.txt driving
# apt resolution is for the firmware-independent base Cydia system, and
# per-branch persistence payloads dropping out of it here (when they do)
# is exactly the signal that they don't belong being resolved this way.
cat > /sandbox/var/lib/dpkg/status <<'EOF'
Package: firmware
Status: install ok installed
Priority: required
Section: base
Installed-Size: 0
Maintainer: Blackb0x build_deb_cache.py <noreply@regulad.xyz>
Architecture: iphoneos-arm
Version: __FIRMWARE_VERSION__
Description: Synthetic package declared by build_deb_cache.py to satisfy
 firmware-version-gated Depends: lines the same way a real device's own
 dpkg status would (see that script's own comment for why).
EOF

for key in /work/keys/*.gpg.key; do
    [ -e "$key" ] || continue
    name=$(basename "$key" .gpg.key)
    gpg --dearmor -o "/sandbox/etc/apt/trusted.gpg.d/$name.gpg" "$key"
done

# Real GPG verification, done ourselves with plain gpgv rather than
# delegating to apt's own Release check — see the module docstring for
# why: apt's internal gpgv call rejects SHA-1 (common on these old
# 2008-era keys) on algorithm-age policy grounds, not because the
# signatures are bad. This is not a weaker check than apt's own — it's
# the same gpgv binary, same keyring, just without that extra policy
# flag — and it still aborts the whole run on a real failure.
if [ -s /work/verify.txt ]; then
    while IFS='|' read -r name release_url gpg_url; do
        [ -z "$name" ] && continue
        echo "Verifying $name ($release_url)..."
        curl -sfL -o "/tmp/$name.Release" "$release_url"
        curl -sfL -o "/tmp/$name.Release.gpg" "$gpg_url"
        gpgv --keyring "/sandbox/etc/apt/trusted.gpg.d/$name.gpg" "/tmp/$name.Release.gpg" "/tmp/$name.Release"
    done < /work/verify.txt
fi

APT_OPTS="-o Dir=/sandbox -o APT::Architecture=iphoneos-arm -o APT::Architectures::=iphoneos-arm -o Acquire::Languages=none"

# Acquire::By-Hash=no: keep the lists cache in the plain, flat naming
# convention this old on-device apt7 0.7.25.3 predates by-hash support for
# — see the module docstring's apt-lists/ paragraph. This cache gets
# shipped to the real device verbatim (BakeRamdisk.cpp's
# stageAptListsCache()), not just used for this script's own resolution.
apt-get $APT_OPTS -o Acquire::By-Hash=no update
cp -a /sandbox/var/lib/apt/lists /work/out/apt-lists
# apt-get itself drops privilege to a low-priv user (_apt) for parts of the
# real fetch, so some of what just got copied (lists/partial/ in
# particular) can come out owned by a uid that only maps to something
# host-side root can reach — confirmed directly: without this, the
# (non-root) Python side's shutil.copytree() failed with a plain
# PermissionError trying to read it back. Nothing under here needs to
# preserve its original owner — this cache gets read-only Packages/Release
# data, not anything permission-sensitive — so just make the whole tree
# universally readable/traversable before this container exits, while it
# still has the real root privilege (inside its own namespace) to do so.
chmod -R a+rX /work/out/apt-lists

: > /work/out/skipped.txt
: > /work/out/skip_reasons.txt
: > /work/out/uris.txt
: > /work/out/resolved_packages.txt
while IFS= read -r pkg; do
    [ -z "$pkg" ] && continue
    # --print-uris, not `apt-cache show`/`policy` or a real simulated
    # install (-s) — show/policy only prove a Packages stanza exists, not
    # that it's actually installable (bigboss/modmyifone/zodttd/saurik all
    # have stanzas but were replaced/obsoleted with no real candidate —
    # see docs/HISTORY.md); `-s` goes further and actually catches those
    # plus unmet firmware-version-gated Depends: (see the firmware note
    # above) — but `-s` ALSO simulates dpkg's real *configure* ordering,
    # which trips a genuine dependency cycle in this old bootstrap-era
    # package set (grep/coreutils/etc. depend on each other) when tested
    # one package at a time in isolation, even though installing them
    # together resolves fine (real dpkg/debootstrap always unpacks
    # everything before configuring anything, precisely to break cycles
    # like this — simulating one package alone can't do that).
    # --print-uris resolves the same real dependency closure without ever
    # touching configure-ordering at all, so it's immune to that false
    # positive while still catching every other real failure class.
    if out=$(apt-get $APT_OPTS install --download-only --no-install-recommends -y --print-uris "$pkg" 2>&1); then
        echo "$out" | grep "^'" >> /work/out/uris.txt
        echo "$pkg" >> /work/out/resolved_packages.txt
    else
        echo "$pkg" >> /work/out/skipped.txt
        echo "-- $pkg --" >> /work/out/skip_reasons.txt
        echo "$out" | grep -E '^E:' >> /work/out/skip_reasons.txt
    fi
done < /work/packages.txt

mkdir -p /work/out/debs
: > /work/out/resolved.txt

if [ -s /work/out/uris.txt ]; then
    # Dedupe by filename (field 2) — the same file is named again for
    # every package that (transitively) depends on it.
    awk '!seen[$2]++' /work/out/uris.txt > /work/out/uris_unique.txt

    # Fetch and verify ourselves via plain curl + shasum/md5sum, rather
    # than apt's own downloader — see the module docstring: modern apt
    # hard-refuses (no config override exists) to download any file whose
    # Packages entry only offers an MD5Sum, which is simply how these old
    # (mid-2010s) repos were built and will never change. The URIs and
    # expected hashes still come from real apt's own dependency
    # resolution above, not reimplemented — we just do the fetch +
    # hash-check step apt would otherwise refuse to do, checking against
    # the exact same hash apt itself resolved.
    while read -r line; do
        uri=$(echo "$line" | sed -E "s/^'([^']*)'.*/\1/")
        filename=$(echo "$line" | awk '{print $2}')
        hashfield=$(echo "$line" | awk '{print $4}')
        hashtype=${hashfield%%:*}
        hashvalue=${hashfield#*:}
        case "$hashtype" in
            SHA512) cmd=sha512sum ;;
            SHA256) cmd=sha256sum ;;
            SHA1) cmd=sha1sum ;;
            MD5Sum) cmd=md5sum ;;
            *) echo "unknown hash type '$hashtype' for $filename" >&2; exit 1 ;;
        esac
        dest="/sandbox/var/cache/apt/archives/$filename"
        curl -sfL --retry 20 --retry-delay 2 -C - -o "$dest" "$uri"
        actual=$($cmd "$dest" | awk '{print $1}')
        if [ "$actual" != "$hashvalue" ]; then
            echo "HASH MISMATCH for $filename: expected $hashtype:$hashvalue, got $actual" >&2
            exit 1
        fi
        cp "$dest" "/work/out/debs/$filename"
        echo "$filename" >> /work/out/resolved.txt
    done < /work/out/uris_unique.txt
fi
"""


def parse_list_file(path: Path):
    """Returns (url, dist, components) or None if the file has no `deb`
    line (shouldn't happen for anything under Misc/, but skip rather than
    crash on a malformed/commented-out entry)."""
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = DEB_LINE_RE.match(line)
        if m:
            return m.group(1), m.group(2), m.group(3) or ""
    return None


def read_deb_package_name(deb_path: Path) -> str:
    """Reads the real `Package:` field straight out of a .deb's own control
    archive — plain `ar`/`tar`, no root or container needed, matching how
    BakeRamdisk.cpp's readDebControlInfo() does the same thing on the C++
    side. Used only for local_only_debs.txt's handful of entries, so apt
    resolution can skip attempting (and warning about) a name we already
    know it can never find."""
    with tempfile.TemporaryDirectory(prefix="blackb0x-debname-") as tmp:
        subprocess.run(["ar", "x", str(deb_path.resolve())], cwd=tmp, check=True, capture_output=True)
        control_tar = next(Path(tmp).glob("control.tar.*"), None)
        if control_tar is None:
            raise SystemExit(f"{deb_path}: no control.tar.* member found")
        control_text = subprocess.run(
            ["tar", "-xOf", str(control_tar), "./control"], cwd=tmp, check=True, capture_output=True, text=True
        ).stdout
    for line in control_text.splitlines():
        if line.startswith("Package:"):
            return line.split(":", 1)[1].strip()
    raise SystemExit(f"{deb_path}: control file has no Package: field")


def is_flat_repo(dist: str) -> bool:
    # `deb URI ./` (or any directory-style second field) vs a real
    # `deb URI <distribution> <component...>` — flat repos have no
    # components at all, just a trailing-slash-style path as `dist`.
    return dist.endswith("/")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output-dir", required=True, type=Path, help="Where to write picklist.txt/resolved_packages.txt/apt-lists/ — caller-owned, not created or cleaned up here")
    ap.add_argument("--firmware-version", required=True, help="Real, per-tuple ProductVersion (e.g. \"6.1.3\") to declare as the sandbox's synthetic `firmware` package — see INNER_SCRIPT's own comment for why this must be the real version for whichever (device, buildID) is actually being baked, not a fixed pin")
    ap.add_argument("--keep-sandbox", action="store_true", help="Don't delete the temp working directory (for debugging)")
    args = ap.parse_args()

    output_dir = args.output_dir.resolve()
    if not output_dir.is_dir():
        raise SystemExit(f"--output-dir {output_dir} does not exist or is not a directory")
    picklist_path = output_dir / "picklist.txt"
    resolved_packages_path = output_dir / "resolved_packages.txt"
    apt_lists_path = output_dir / "apt-lists"

    # local.list is deliberately excluded here: it points at
    # /var/.blackb0x/local-debs, a path that only exists once
    # BakeRamdisk.cpp stages local-repo/ onto a real device — nothing on
    # this host/container can ever resolve it, and it isn't meant to be
    # part of this sandbox's own apt resolution anyway (see the module
    # docstring's local_only_debs.txt paragraph). This script builds that
    # repo's actual content directly, it doesn't need to source FROM it.
    list_files = sorted(p for p in APT_DIR.glob("*.list") if p.name != "local.list")
    if not list_files:
        raise SystemExit(f"No .list files found under {APT_DIR}")

    sources_lines = []
    keyed = []
    verify_lines = []
    for lf in list_files:
        parsed = parse_list_file(lf)
        if not parsed:
            print(f"warning: {lf.name} has no `deb` line, skipping", file=sys.stderr)
            continue
        url, dist, components = parsed
        key_path = APT_DIR / f"{lf.stem}.gpg.key"
        has_key = key_path.exists()

        # Always [trusted=yes]: for keyed repos this is real GPG
        # verification done ourselves via plain gpgv below (see inner
        # script / module docstring for why apt's own check can't be used
        # here); for unkeyed ones there's nothing to verify at all either
        # way.
        base = url.rstrip("/")
        if has_key:
            keyed.append(key_path)
            release_url = f"{base}/Release" if is_flat_repo(dist) else f"{base}/dists/{dist}/Release"
            verify_lines.append(f"{lf.stem}|{release_url}|{release_url}.gpg")

        line = f"deb [trusted=yes] {url} {dist}" + (f" {components}" if components else "")
        sources_lines.append(line.strip())
        print(f"{lf.name}: {'GPG-verified (gpgv, ourselves)' if has_key else 'unsigned, no paired key'} — {line.strip()}")

    if not PACKAGES_LIST.exists():
        raise SystemExit(f"{PACKAGES_LIST} not found")
    wanted_packages = [p.strip() for p in PACKAGES_LIST.read_text().splitlines() if p.strip()]

    local_only_filenames = []
    local_only_package_names = set()
    if LOCAL_ONLY_LIST.exists():
        for line in LOCAL_ONLY_LIST.read_text().splitlines():
            filename = line.strip()
            if not filename or filename.startswith("#"):
                continue
            deb_path = DEBS_DIR / filename
            if not deb_path.exists():
                raise SystemExit(f"{LOCAL_ONLY_LIST} lists {filename}, but {deb_path} does not exist")
            local_only_filenames.append(filename)
            local_only_package_names.add(read_deb_package_name(deb_path))

    # Never even attempt real apt resolution for these — we already know
    # from local_only_debs.txt's own contents that it can't find them, and
    # attempting anyway would just produce the exact silent-warning noise
    # this script no longer tolerates for anything else.
    apt_attempted_packages = [p for p in wanted_packages if p not in local_only_package_names]

    work = Path(tempfile.mkdtemp(prefix="blackb0x-debcache-"))
    try:
        (work / "keys").mkdir()
        (work / "out").mkdir()
        (work / "sources.list").write_text("\n".join(sources_lines) + "\n")
        (work / "packages.txt").write_text("\n".join(apt_attempted_packages) + "\n")
        (work / "verify.txt").write_text("\n".join(verify_lines) + "\n")
        (work / "inner.sh").write_text(INNER_SCRIPT.replace("__FIRMWARE_VERSION__", args.firmware_version))
        for k in keyed:
            shutil.copy(k, work / "keys" / k.name)

        if local_only_filenames:
            local_debs_dir = work / "local-debs"
            local_debs_dir.mkdir()
            for filename in local_only_filenames:
                shutil.copy(DEBS_DIR / filename, local_debs_dir / filename)

        print(f"\nRunning apt-get inside a throwaway debian:bookworm-slim container ({ARCH})...")
        result = subprocess.run(
            [
                PODMAN, "run", "--rm", "--security-opt", "label=disable",
                "-v", f"{work}:/work",
                "debian:bookworm-slim", "sh", "/work/inner.sh",
            ],
        )
        if result.returncode != 0:
            raise SystemExit(f"apt-get container run failed (exit {result.returncode}) — see output above")

        skipped_path = work / "out" / "skipped.txt"
        reasons_path = work / "out" / "skip_reasons.txt"
        reasons_text = reasons_path.read_text() if reasons_path.exists() else ""
        if skipped_path.exists():
            skipped = [p for p in skipped_path.read_text().splitlines() if p]
            for pkg in skipped:
                level = "warning" if pkg in KNOWN_EXPECTED_UNRESOLVABLE else "FATAL"
                print(f"{level}: package '{pkg}' is not installable:", file=sys.stderr)
                block = reasons_text.split(f"-- {pkg} --\n", 1)
                if len(block) == 2:
                    for line in block[1].split("-- ", 1)[0].strip().splitlines():
                        print(f"    {line}", file=sys.stderr)
            unexpected = [p for p in skipped if p not in KNOWN_EXPECTED_UNRESOLVABLE]
            if unexpected:
                raise SystemExit(
                    f"{len(unexpected)} package(s) failed apt resolution unexpectedly (see FATAL lines above) — "
                    "if this is a real, permanently dead package (not just temporarily offline), either remove it "
                    "from packages.txt or, if a real vendored .deb already exists for it, move it to "
                    f"{LOCAL_ONLY_LIST.relative_to(REPO_ROOT)} instead"
                )

        resolved_path = work / "out" / "resolved.txt"
        resolved = sorted(p for p in resolved_path.read_text().splitlines() if p) if resolved_path.exists() else []
        if not resolved and not local_only_filenames:
            raise SystemExit("apt resolved zero packages — nothing to do (check the warnings above)")

        debs_out = work / "out" / "debs"
        added = []
        for filename in resolved:
            dest = DEBS_DIR / filename
            if dest.exists():
                continue
            shutil.copy(debs_out / filename, dest)
            added.append(filename)

        # local_only_debs.txt's entries were never sent to apt at all (see
        # apt_attempted_packages above) — they're already real files in
        # Blackb0x/Debs/, just merged into the same picklist.txt apt's own
        # resolved set feeds, so stageDebcache() stages them into the
        # on-device apt cache exactly like anything else.
        resolved = sorted(resolved + local_only_filenames)

        picklist_path.write_text(
            "# Generated by scripts/build_deb_cache.py — do not edit by hand.\n"
            "# Every .deb filename the ramdisk builder needs to pull from Blackb0x/Debs/\n"
            "# for the package set currently in package/packages.txt.\n"
            + "\n".join(resolved) + "\n"
        )

        resolved_packages_txt = work / "out" / "resolved_packages.txt"
        resolved_packages = (
            sorted(p for p in resolved_packages_txt.read_text().splitlines() if p)
            if resolved_packages_txt.exists()
            else []
        )
        # local_only_debs.txt's own packages ARE genuinely by-name
        # installable now — through the local-repo/ index below, not any
        # of the normal live repos — so postinstall.sh's install array
        # should include them same as anything else apt actually resolved.
        resolved_packages = sorted(set(resolved_packages) | local_only_package_names)
        resolved_packages_path.write_text(
            "# Generated by scripts/build_deb_cache.py — do not edit by hand.\n"
            "# The subset of package/packages.txt's own names that genuinely\n"
            "# resolved through real apt — see this script's own module docstring.\n"
            + "\n".join(resolved_packages) + "\n"
        )

        apt_lists_src = work / "out" / "apt-lists"
        if apt_lists_path.exists():
            shutil.rmtree(apt_lists_path)
        shutil.copytree(apt_lists_src, apt_lists_path)

        local_repo_path = output_dir / "local-repo"
        if local_repo_path.exists():
            shutil.rmtree(local_repo_path)
        if local_only_filenames:
            local_debs_dir = work / "local-debs"
            packages_index = local_debs_dir / "Packages"
            if not packages_index.exists():
                raise SystemExit(f"dpkg-scanpackages did not produce {packages_index} — see container output above")
            shutil.copytree(local_debs_dir, local_repo_path)

        print(f"\n{len(wanted_packages)} packages requested, {len(resolved_packages)} resolved as real "
              f"packages, {len(resolved)} resolved (full dependency closure).")
        if skipped_path.exists() and skipped_path.read_text().strip():
            print(f"{len(skipped_path.read_text().splitlines())} package name(s) not found — see warnings above.")
        print(f"{len(added)} new .deb(s) added to {DEBS_DIR.relative_to(REPO_ROOT)}:")
        for f in added:
            print(f"  + {f}")
        print(f"{len(resolved) - len(added)} already present, left untouched.")
        print(f"Wrote {picklist_path} ({len(resolved)} entries).")
        print(f"Wrote {resolved_packages_path} ({len(resolved_packages)} entries).")
        print(f"Wrote {apt_lists_path}.")
        if local_only_filenames:
            print(f"Wrote {local_repo_path} ({len(local_only_filenames)} local-only package(s): "
                  f"{', '.join(sorted(local_only_package_names))}).")
    finally:
        if args.keep_sandbox:
            print(f"\n--keep-sandbox: working directory left at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
