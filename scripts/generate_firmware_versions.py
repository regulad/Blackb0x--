#!/usr/bin/env python3
"""
generate_firmware_versions.py — regenerate Blackb0x/Misc/firmware_versions.txt,
the (device, buildID) -> real ProductVersion binding.

That file used to be a hand-collected snapshot: an agent ran the ipsw.me
queries, range-fetched the BuildManifest.plists, and cross-checked AppleDB by
hand, then wrote the answers down. The answers were right, but nothing could
reproduce them -- adding a tuple to Blackb0x/ImageKeys/ meant redoing that
research by hand or leaving the file stale. This script is that research,
checked in and rerunnable (.claude/TODO.md item 8).

Why the binding exists at all: bake-time dependency resolution has to evaluate
packages whose Depends: is gated on a firmware version range (`Depends:
firmware (>= 6.0)`), and that needs Apple's real ProductVersion ("6.1.3"), not
the opaque buildID ("10B329a").

Sourcing, in order, matching what the hand-collected file documented:

  1. ipsw.me, one GET per DEVICE (not per tuple):
       https://api.ipsw.me/v4/device/<model>?type=ipsw
     and read the "buildid"/"version" pairs out of the "firmwares" array.
     This is the same endpoint IPSW.cpp's signedBuildsForDevice() already
     hits. It covered 86 of 95 tuples when the file was first built.

  2. AppleDB for the rest:
       https://api.appledb.dev/ios/Apple%20TV%20Software;<buildID>.json
     The remaining buildIDs are internal/beta builds that never got a public
     release, so ipsw.me 404s them both per-device and globally.

     CRITICAL, and the single easiest thing to get wrong here: AppleDB's
     "version" field is an internal/marketing number that is NOT the
     ProductVersion. Its "iosVersion" field is the one that lines up. For
     build 11B553 AppleDB reports version "6.0.2" and iosVersion "7.0.4";
     the real ProductVersion is 7.0.4. Always read iosVersion.

  3. --verify (optional, slow, network-heavy): prove step 2 rather than
     trusting it. Resolve the build's real IPSW URL from AppleDB's own
     "sources", then range-fetch JUST that IPSW's BuildManifest.plist (HTTP
     Range requests against the remote zip's central directory -- no
     full-IPSW download) and read its real ProductVersion key. The original
     hand pass did this for 7 of the 9 AppleDB-sourced tuples and found
     iosVersion correct 7 times out of 7.

     The HTTP-range machinery is imported from fetch_firmware_component.py
     rather than reimplemented -- it already does exactly this.

Known un-verifiable: AppleTV3,2 12B401 has no IPSW or OTA source archived in
AppleDB at all, so there is nothing to range-fetch a real manifest from. Its
version comes from iosVersion alone and is marked as such in the output.

Usage:
  scripts/generate_firmware_versions.py                 # regenerate to stdout-ish (writes the file)
  scripts/generate_firmware_versions.py --check         # verify the checked-in file is current; exit 1 on drift
  scripts/generate_firmware_versions.py --verify        # also range-fetch real manifests for AppleDB entries
  scripts/generate_firmware_versions.py --output -      # write to stdout instead of the file
"""

import argparse
import datetime
import json
import os
import plistlib
import sys
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_firmware_component import fetch_zip_member  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE_KEYS = os.path.join(REPO_ROOT, "Blackb0x", "ImageKeys")
DEFAULT_OUTPUT = os.path.join(REPO_ROOT, "Blackb0x", "Misc", "firmware_versions.txt")

IPSW_ME_DEVICE = "https://api.ipsw.me/v4/device/{device}?type=ipsw"
# AppleDB keys Apple TV entries under the "Apple TV Software" OS string, even
# for builds Apple itself later called tvOS.
APPLEDB_BUILD = "https://api.appledb.dev/ios/{os_str};{build}.json"
APPLEDB_OS_STR = "Apple TV Software"


def known_tuples():
    """Every (device, buildID) with a .keys file, read from the directory
    structure rather than any separate list -- same derivation
    bake-all-ramdisks and bake-all-bootloaders use, so all three always
    agree on what "every known firmware" means."""
    found = []
    for device in sorted(os.listdir(IMAGE_KEYS)):
        device_dir = os.path.join(IMAGE_KEYS, device)
        if not os.path.isdir(device_dir):
            continue
        prefix = device + "_"
        for name in sorted(os.listdir(device_dir)):
            if not name.endswith(".keys") or not name.startswith(prefix):
                continue
            found.append((device, name[len(prefix):-len(".keys")]))
    return sorted(found)


# AppleDB's CDN 403s urllib's default "Python-urllib/3.x" User-Agent outright
# (the same URL fetched with curl works fine), so every request here carries a
# real one. Not cosmetic -- without it the AppleDB fallback fails for every
# build, which looks exactly like "that build does not exist".
USER_AGENT = "blackb0x-generate-firmware-versions/1.0 (+https://github.com/regulad/Blackb0x--)"


def get_json(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.load(r)
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            return None
        raise


def ipsw_me_versions(device):
    """buildID -> version for one device model, in one request."""
    data = get_json(IPSW_ME_DEVICE.format(device=urllib.parse.quote(device)))
    if not data:
        return {}
    out = {}
    for fw in data.get("firmwares", []):
        build, version = fw.get("buildid"), fw.get("version")
        if build and version:
            out[build] = version
    return out


def appledb_entry(build):
    url = APPLEDB_BUILD.format(os_str=urllib.parse.quote(APPLEDB_OS_STR), build=urllib.parse.quote(build))
    return get_json(url)


def appledb_ipsw_url(entry):
    """First IPSW download link in an AppleDB entry's own "sources", or None.

    AppleDB models each source as {type, links: [{url, ...}], ...}; only
    "ipsw" sources point at something with a BuildManifest.plist in it.
    """
    for source in entry.get("sources", []) or []:
        if source.get("type") != "ipsw":
            continue
        for link in source.get("links", []) or []:
            url = link.get("url")
            if url and url.lower().endswith(".ipsw"):
                return url
    return None


def product_version_from_ipsw(url):
    """Real ProductVersion, read out of the remote IPSW's own
    BuildManifest.plist via HTTP Range requests. No full download."""
    name, data = fetch_zip_member(url, lambda n: n.rsplit("/", 1)[-1] == "BuildManifest.plist")
    if not data:
        return None
    return plistlib.loads(data).get("ProductVersion")


def resolve(device, build, ipsw_me_cache, verify):
    """-> (version, source_label, note) with source_label one of
    'ipsw.me' / 'appledb' / 'appledb+manifest' / None."""
    if device not in ipsw_me_cache:
        ipsw_me_cache[device] = ipsw_me_versions(device)
    version = ipsw_me_cache[device].get(build)
    if version:
        return version, "ipsw.me", ""

    entry = appledb_entry(build)
    if not entry:
        return None, None, "not in ipsw.me or AppleDB"

    # iosVersion, NOT version -- see this file's module docstring.
    version = entry.get("iosVersion")
    if not version:
        return None, None, "AppleDB entry has no iosVersion"
    version = version.split(" ")[0]  # "8.1 beta" -> "8.1"

    if not verify:
        return version, "appledb", ""

    url = appledb_ipsw_url(entry)
    if not url:
        return version, "appledb", "no IPSW source archived; iosVersion unverified"
    try:
        real = product_version_from_ipsw(url)
    except Exception as exc:  # noqa: BLE001 - any network/zip failure is just "unverified"
        return version, "appledb", "manifest fetch failed (%s); iosVersion unverified" % type(exc).__name__
    if not real:
        return version, "appledb", "no ProductVersion in manifest; iosVersion unverified"
    if real != version:
        # Worth shouting about: it would mean the iosVersion rule this file
        # depends on has broken down for this build.
        return real, "appledb+manifest", "iosVersion %s DISAGREED with manifest %s; using manifest" % (version, real)
    return real, "appledb+manifest", ""


def render(rows, verify, notes):
    today = datetime.date.today().isoformat()
    by_source = {}
    for _, _, _, source, _ in rows:
        by_source[source] = by_source.get(source, 0) + 1

    lines = []
    add = lines.append
    add("# Real, human-readable firmware version strings (Apple's own ProductVersion")
    add('# field, e.g. "7.2.2") for every (device, buildID) tuple currently present')
    add("# under Blackb0x/ImageKeys/<device>/<device>_<buildID>.keys — NOT the")
    add('# opaque buildID itself (e.g. "12H606"). The package-dependency resolver')
    add("# used when baking ramdisks needs this to evaluate packages whose Depends:")
    add("# is gated on a firmware version range (e.g. `Depends: firmware (>= 6.0)`),")
    add("# since resolving those correctly requires the real version number, not")
    add("# just the buildID.")
    add("#")
    add("# GENERATED FILE — do not hand-edit. Regenerate with:")
    add("#   scripts/generate_firmware_versions.py [--verify]")
    add("# (.claude/TODO.md item 8: this used to be a hand-collected snapshot.)")
    add("#")
    add("# Format: one line per tuple, space-separated:")
    add("#   <device> <buildID> <version>")
    add("# Sorted by device then buildID (plain `LC_ALL=C sort`, matching this")
    add("# directory's other list files).")
    add("#")
    add("# Generated %s. %d tuples: %s." % (
        today, len(rows),
        ", ".join("%d via %s" % (n, s) for s, n in sorted(by_source.items()))))
    add("#")
    add("# Sourcing:")
    add("#   ipsw.me         — https://api.ipsw.me/v4/device/<model>?type=ipsw,")
    add("#                     one GET per device model, reading each entry's")
    add('#                     "buildid"/"version" pair from the "firmwares" array.')
    add("#   appledb         — https://api.appledb.dev/ios/Apple%20TV%20Software;<buildID>.json,")
    add("#                     for internal/beta builds ipsw.me does not carry at")
    add('#                     all. Reads "iosVersion", NOT "version": AppleDB\'s')
    add('#                     "version" is an internal/marketing number that is not')
    add("#                     the ProductVersion (build 11B553 reports version")
    add('#                     "6.0.2" against a real ProductVersion of "7.0.4").')
    add("#   appledb+manifest — the same, then independently verified by resolving")
    add('#                     the build\'s real IPSW URL from AppleDB\'s own "sources"')
    add("#                     and range-fetching just that IPSW's BuildManifest.plist")
    add("#                     (HTTP Range against the remote zip's central directory,")
    add("#                     no full download) to read its real ProductVersion.")
    if not verify:
        add("#")
        add("# NOTE: run without --verify, so no AppleDB-sourced entry was checked")
        add("# against a real BuildManifest.plist on this run.")
    if notes:
        add("#")
        add("# Per-tuple notes from this run:")
        for device, build, note in notes:
            add("#   %s %s: %s" % (device, build, note))
    add("#")

    for device, build, version, _, _ in rows:
        lines.append("%s %s %s" % (device, build, version))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output", default=DEFAULT_OUTPUT,
                    help='where to write ("-" for stdout; default: %(default)s)')
    ap.add_argument("--verify", action="store_true",
                    help="range-fetch real BuildManifest.plists to verify AppleDB-sourced versions")
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if the checked-in file differs from what would be generated")
    args = ap.parse_args()

    tuples = known_tuples()
    if not tuples:
        raise SystemExit("no .keys files found under %s" % IMAGE_KEYS)
    print("Resolving %d (device, buildID) tuples..." % len(tuples), file=sys.stderr)

    ipsw_me_cache = {}
    rows, notes, unresolved = [], [], []
    for device, build in tuples:
        version, source, note = resolve(device, build, ipsw_me_cache, args.verify)
        if version is None:
            unresolved.append((device, build, note))
            print("  %-14s %-10s UNRESOLVED (%s)" % (device, build, note), file=sys.stderr)
            continue
        rows.append((device, build, version, source, note))
        if note:
            notes.append((device, build, note))
        print("  %-14s %-10s %-8s %s%s" % (device, build, version, source,
                                            (" -- " + note) if note else ""), file=sys.stderr)

    if unresolved:
        # Deliberately fatal: a silently-missing tuple would surface much
        # later as a dependency-resolution failure with no obvious cause.
        print("\n%d tuple(s) could not be resolved from any source:" % len(unresolved), file=sys.stderr)
        for device, build, note in unresolved:
            print("  %s %s: %s" % (device, build, note), file=sys.stderr)
        return 1

    rendered = render(rows, args.verify, notes)

    if args.check:
        try:
            with open(DEFAULT_OUTPUT) as fh:
                current = fh.read()
        except OSError as exc:
            print("--check: cannot read %s: %s" % (DEFAULT_OUTPUT, exc), file=sys.stderr)
            return 1
        # Compare the data lines only. The header carries a generation date
        # and per-run notes, which would otherwise make every check fail.
        def data(text):
            return [l for l in text.splitlines() if l and not l.startswith("#")]
        if data(current) == data(rendered):
            print("--check: up to date (%d tuples)." % len(rows), file=sys.stderr)
            return 0
        print("--check: DRIFT between %s and freshly-resolved data." % DEFAULT_OUTPUT, file=sys.stderr)
        cur, new = set(data(current)), set(data(rendered))
        for line in sorted(cur - new):
            print("  -%s" % line, file=sys.stderr)
        for line in sorted(new - cur):
            print("  +%s" % line, file=sys.stderr)
        return 1

    if args.output == "-":
        sys.stdout.write(rendered)
    else:
        with open(args.output, "w") as fh:
            fh.write(rendered)
        print("\nWrote %d tuples to %s" % (len(rows), args.output), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
