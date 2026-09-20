#!/usr/bin/env python3
"""
gen_download_blacklist.py — regenerate misc/download_blacklist.txt, the list of
(device, buildID) tuples whose IPSW can no longer actually be pulled from
Apple's CDN.

Why a *download* blacklist is a different question from "does this build
exist". ipsw.me happily enumerates every firmware Apple ever shipped for a
device, including ones whose `url` now 403s, 404s, or points at a host that
quietly dropped byte-range support. A build being listed proves nothing about
whether Blackb0x can consume it.

And "can Blackb0x consume it" is a stricter question than "does the URL work",
because Blackb0x never downloads a whole IPSW. src/IPSWDownloader.cpp drives
libfragmentzip, which pulls individual zip members out of the *remote* archive
with HTTP Range requests. So the download path depends on three separate
things holding, and a plain `HEAD -> 200` only proves the first:

  1. the URL resolves (after redirects) to a 200 with a plausible
     Content-Length — libfragmentzip's fragmentzip_open() does exactly this
     first, with CURLOPT_NOBODY + CURLOPT_FOLLOWLOCATION, and reads the
     length out of CURLINFO_CONTENT_LENGTH_DOWNLOAD;

  2. the server honours `Range:` — it must answer 206 Partial Content, not
     200-with-the-whole-file. A CDN that ignores Range turns every member
     fetch into a full multi-gigabyte download, which for our purposes is a
     dead URL;

  3. the tail of the archive really is a zip End-Of-Central-Directory record.
     libfragmentzip fetches exactly the last sizeof(fragmentzip_end_of_cd)
     == 22 bytes and asserts they begin with "PK\\x05\\x06"
     (third_party/libfragmentzip/libfragmentzip/libfragmentzip.c, in
     fragmentzip_open_extended). It does NOT scan backwards for the record,
     so an archive with a nonempty zip comment fails to open even though
     every standard zip reader would handle it fine.

This script probes all three, per tuple, concurrently, and writes the failures
out. It is deliberately biased against blacklisting: a false positive here
permanently hides a perfectly usable build from the tool, which is a much worse
outcome than leaving a dead build in. So only *deterministic* failures — a 4xx,
a structurally wrong archive tail, a server that answers a range request with
200 — are ever written to the file. Timeouts, connection resets, 5xx and
anything else that could plausibly be the network having a bad afternoon are
retried, and if they still fail they are reported as "undetermined" on stderr
and left OUT of the blacklist.

Usage:
  scripts/gen_download_blacklist.py                   # probe everything, write the file
  scripts/gen_download_blacklist.py --output -        # write to stdout instead
  scripts/gen_download_blacklist.py --jobs 4          # fewer parallel probes
  scripts/gen_download_blacklist.py --device AppleTV3,2   # repeatable; default is all three
"""

import argparse
import concurrent.futures
import datetime
import http.client
import json
import os
import re
import socket
import sys
import time
import urllib.error
import urllib.request

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUTPUT = os.path.join(REPO_ROOT, "misc", "download_blacklist.txt")

# The three models Blackb0x supports, matching keys/<device>/ and
# misc/firmware_versions.txt.
DEVICES = ["AppleTV2,1", "AppleTV3,1", "AppleTV3,2"]

IPSW_ME_DEVICE = "https://api.ipsw.me/v4/device/{device}?type=ipsw"

# Same endpoint generate_firmware_versions.py and src/IPSW.cpp's
# signedBuildsForDevice() use; same reason for a real User-Agent (some of
# these CDNs 403 "Python-urllib/3.x" outright, which would look exactly like
# a dead URL and poison the whole blacklist).
USER_AGENT = "blackb0x-gen-download-blacklist/1.0 (+https://github.com/regulad/Blackb0x--)"

# How much of the archive tail to range-fetch. 22 bytes is all libfragmentzip
# itself reads, but fetching 65535 (the max zip comment length) + 1024 + the
# 22-byte record lets us tell "no EOCD anywhere near the end" (a truncated or
# non-zip body — genuinely broken) apart from "EOCD present but behind a
# comment" (a real zip that libfragmentzip specifically cannot open). Those
# are different failures and a future reader deserves to know which one bit.
TAIL_BYTES = 66560
EOCD_SIG = b"PK\x05\x06"
EOCD_SIZE = 22

# An IPSW for this hardware is ~500MB-1.5GB. Anything under this is a stub,
# an error page that got served with a 200, or a redirect landing page.
MIN_PLAUSIBLE_LENGTH = 16 * 1024 * 1024

# Verdicts. Only BLACKLIST ever reaches the file.
OK = "ok"
BLACKLIST = "blacklist"
UNDETERMINED = "undetermined"


def version_key(version):
    """Integer-tuple compare, same as newest_firmware_versions.py: "8.4.6"
    must sort above "8.4.10", and "10.x" above "9.x"."""
    return tuple(int(part) for part in re.findall(r"\d+", version or ""))


def get_json(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def firmwares_for_device(device, timeout=30):
    """Every firmware ipsw.me knows about for one model, newest first.

    ipsw.me already returns them newest-first, but that is not contractual, so
    re-sort rather than trusting the order. Version first, releasedate only as
    a tiebreaker: ipsw.me omits releasedate entirely for the pre-7.x builds
    (20 of AppleTV2,1's 29, for instance), so sorting on it primarily would
    bury every one of them under an empty string.
    """
    data = get_json(IPSW_ME_DEVICE.format(device=device), timeout=timeout)
    firmwares = data.get("firmwares", [])
    firmwares.sort(
        key=lambda fw: (version_key(fw.get("version")), fw.get("releasedate") or "", fw.get("buildid") or ""),
        reverse=True,
    )
    return firmwares


class Probe:
    """One tuple's verdict, plus enough detail to explain it in the file."""

    def __init__(self, device, build, version, url):
        self.device = device
        self.build = build
        self.version = version
        self.url = url
        self.verdict = UNDETERMINED
        self.reason = "not probed"
        self.length = None

    def key(self):
        return (self.device, self.build)


class Transient(Exception):
    """Something that might just be the network. Retry; never blacklist on it."""


def _request(url, headers, method, timeout):
    req = urllib.request.Request(url, headers=dict(headers, **{"User-Agent": USER_AGENT}), method=method)
    # urllib follows 301/302/303/307 for GET and HEAD by itself, which is what
    # libfragmentzip's CURLOPT_FOLLOWLOCATION does.
    try:
        return urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as exc:
        # A 4xx is a real answer about this URL; a 5xx is the server having a
        # moment and must not cost a build its place in the tool.
        if 500 <= exc.code < 600:
            raise Transient(f"HTTP {exc.code}") from exc
        return exc
    except (urllib.error.URLError, socket.timeout, http.client.HTTPException, ConnectionError, OSError) as exc:
        raise Transient(str(exc)) from exc


def probe_once(probe, timeout):
    """One full attempt at the three checks. Returns (verdict, reason)."""
    # (1) HEAD, redirects followed, 200 with a plausible Content-Length.
    with _request(probe.url, {}, "HEAD", timeout) as resp:
        status = resp.status
        length_header = resp.headers.get("Content-Length")
    if status != 200:
        return BLACKLIST, f"head-http-{status}"
    try:
        length = int(length_header)
    except (TypeError, ValueError):
        return BLACKLIST, "head-no-content-length"
    if length < MIN_PLAUSIBLE_LENGTH:
        return BLACKLIST, f"head-implausible-length-{length}"
    probe.length = length

    # (2) Range GET of the archive tail must be answered 206, not 200.
    start = max(0, length - TAIL_BYTES)
    with _request(probe.url, {"Range": f"bytes={start}-{length - 1}"}, "GET", timeout) as resp:
        status = resp.status
        if status != 206:
            resp.read(1)  # don't pull a gigabyte just to close it
            if status == 200:
                return BLACKLIST, "range-ignored-http-200"
            return BLACKLIST, f"range-http-{status}"
        tail = resp.read()

    # (3) The tail has to actually be a zip EOCD, in the exact place
    #     libfragmentzip looks for it.
    if len(tail) < EOCD_SIZE:
        raise Transient(f"short range body ({len(tail)} bytes)")
    if tail[-EOCD_SIZE:][:4] == EOCD_SIG:
        return OK, "ok"
    if EOCD_SIG in tail:
        # A valid zip, but libfragmentzip reads exactly the last 22 bytes and
        # asserts the signature there — it never scans back past a comment.
        return BLACKLIST, "eocd-behind-zip-comment"
    return BLACKLIST, "no-eocd-signature"


def probe(probe_obj, timeout, retries):
    """probe_once with retries, collapsing repeated transients to UNDETERMINED.

    A deterministic verdict is also re-confirmed once: "only blacklist on a
    reproducible failure" is the whole point, and a single 403 from one CDN
    edge is not reproducible until it has been reproduced.
    """
    if not probe_obj.url:
        probe_obj.verdict, probe_obj.reason = BLACKLIST, "no-url-in-ipsw.me"
        return probe_obj

    last_transient = "unknown"
    first_failure = None
    for attempt in range(retries + 1):
        try:
            verdict, reason = probe_once(probe_obj, timeout)
        except Transient as exc:
            last_transient = str(exc)
            time.sleep(min(2 ** attempt, 8))
            continue
        if verdict == OK:
            probe_obj.verdict, probe_obj.reason = OK, reason
            return probe_obj
        if first_failure is None:
            first_failure = reason
            time.sleep(1)  # re-probe before condemning it
            continue
        probe_obj.verdict, probe_obj.reason = BLACKLIST, reason
        return probe_obj

    if first_failure is not None:
        # It failed deterministically at least once but we never got a clean
        # second opinion. Not reproducible -> not blacklisted, just reported.
        probe_obj.verdict = UNDETERMINED
        probe_obj.reason = f"unconfirmed-{first_failure} (then: {last_transient})"
    else:
        probe_obj.verdict, probe_obj.reason = UNDETERMINED, f"transient: {last_transient}"
    return probe_obj


def render(entries, devices, counts, newest, generated_on):
    """The file itself. Comment style follows misc/'s other list files: a
    header block explaining what it is and how to regenerate, then plain
    tab-separated data lines."""
    lines = [
        "# download_blacklist.txt -- (device, buildID) tuples whose IPSW can no",
        "# longer actually be fetched from Apple's CDN *the way Blackb0x fetches it*.",
        "#",
        "# Blackb0x never downloads a whole IPSW: src/IPSWDownloader.cpp drives",
        "# libfragmentzip, which pulls individual zip members out of the remote",
        "# archive with HTTP Range requests. So a build is usable only if all three",
        "# of these hold, and a plain HEAD->200 only proves the first:",
        "#   1. HEAD (redirects followed) returns 200 with a plausible Content-Length",
        "#   2. a Range: GET of the archive tail returns 206 Partial Content",
        "#      (a server that ignores Range and returns 200 is useless to us)",
        "#   3. the last 22 bytes are a zip End-Of-Central-Directory record --",
        "#      libfragmentzip reads exactly those bytes and asserts 'PK\\x05\\x06'",
        "#      there; it never scans backwards past a zip comment.",
        "#",
        "# Everything listed here failed at least one of those, reproducibly. Builds",
        "# that only failed transiently (timeout, 5xx, connection reset) are NOT",
        "# listed: a false positive here permanently hides a usable build, which is",
        "# worse than leaving a dead one in.",
        "#",
        "# Scope: the universe probed is whatever ipsw.me's v4 device endpoint",
        "# enumerates for each model. A handful of keys/<device>/*.keys tuples are",
        "# internal/beta builds ipsw.me does not list at all (see",
        "# scripts/generate_firmware_versions.py's AppleDB fallback) -- those have no",
        "# IPSW URL to probe in the first place and are a separate problem, not",
        "# blacklist entries.",
        "#",
        "# GENERATED FILE -- do not hand-edit. Regenerate with:",
        "#   scripts/gen_download_blacklist.py",
        "#",
        "# Format: one line per tuple, TAB-separated:",
        "#   <device>\t<buildID>\t<reason>",
        "# Sorted by device then buildID (plain `LC_ALL=C sort`, matching this",
        "# directory's other list files). Blank lines and #-comments are skippable.",
        "#",
        "# Reason codes:",
        "#   head-http-<code>            HEAD did not return 200 (403 = pulled, 404 = gone)",
        "#   head-no-content-length      200 but no usable Content-Length to range against",
        "#   head-implausible-length-<n> 200 but far too small to be a real IPSW",
        "#   range-http-<code>           the Range: GET itself failed",
        "#   range-ignored-http-200      server ignored Range: -- no partial fetch possible",
        "#   eocd-behind-zip-comment     real zip, but libfragmentzip cannot open it",
        "#   no-eocd-signature           archive tail is not a zip at all",
        "#   no-url-in-ipsw.me           ipsw.me lists the build with no download URL",
        "#",
        f"# Generated {generated_on}. Probed {counts['total']} tuples across "
        f"{len(devices)} device{'' if len(devices) == 1 else 's'}: {counts['ok']} ok, "
        f"{counts['blacklist']} blacklisted, {counts['undetermined']} undetermined (left out).",
        "#",
        "# Newest build per device at generation time, and whether it passed:",
    ]
    for device in devices:
        entry = newest.get(device)
        if entry is None:
            lines.append(f"#   {device}: no firmwares listed by ipsw.me")
        else:
            lines.append(
                f"#   {device}: {entry.build} ({entry.version}) -- {entry.verdict} [{entry.reason}]"
            )
    lines.append("#")

    if not entries:
        lines.append("# No entries: every probed tuple downloaded cleanly.")
    for entry in entries:
        lines.append(f"{entry.device}\t{entry.build}\t{entry.reason}")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--output", default=DEFAULT_OUTPUT, help='output file, or "-" for stdout')
    ap.add_argument("--device", action="append", dest="devices", metavar="MODEL",
                    help="probe only this model (repeatable); default is all three")
    ap.add_argument("--jobs", type=int, default=8, help="parallel probes (default 8)")
    ap.add_argument("--retries", type=int, default=3, help="retries per tuple on transient errors (default 3)")
    ap.add_argument("--timeout", type=int, default=60, help="per-request timeout in seconds (default 60)")
    args = ap.parse_args()

    devices = args.devices or DEVICES

    probes = []
    newest = {}
    for device in devices:
        firmwares = firmwares_for_device(device, timeout=args.timeout)
        print(f"{device}: {len(firmwares)} firmwares listed by ipsw.me", file=sys.stderr)
        for index, fw in enumerate(firmwares):
            entry = Probe(device, fw.get("buildid"), fw.get("version"), fw.get("url"))
            probes.append(entry)
            if index == 0:
                newest[device] = entry

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(probe, entry, args.timeout, args.retries): entry for entry in probes}
        done = 0
        for future in concurrent.futures.as_completed(futures):
            entry = future.result()
            done += 1
            if entry.verdict != OK:
                print(f"  [{done}/{len(probes)}] {entry.device} {entry.build}: "
                      f"{entry.verdict} ({entry.reason})", file=sys.stderr)

    counts = {
        "total": len(probes),
        "ok": sum(1 for p in probes if p.verdict == OK),
        "blacklist": sum(1 for p in probes if p.verdict == BLACKLIST),
        "undetermined": sum(1 for p in probes if p.verdict == UNDETERMINED),
    }
    entries = sorted((p for p in probes if p.verdict == BLACKLIST), key=lambda p: p.key())
    generated_on = datetime.date.today().isoformat()
    text = render(entries, devices, counts, newest, generated_on)

    if args.output == "-":
        sys.stdout.write(text)
    else:
        os.makedirs(os.path.dirname(args.output), exist_ok=True)
        with open(args.output, "w") as f:
            f.write(text)
        print(f"Wrote {args.output} ({counts['blacklist']} blacklisted of {counts['total']})", file=sys.stderr)

    for entry in probes:
        if entry.verdict == UNDETERMINED:
            print(f"UNDETERMINED (not blacklisted): {entry.device} {entry.build}: {entry.reason}",
                  file=sys.stderr)
    for device in devices:
        entry = newest.get(device)
        if entry is not None:
            print(f"newest {device}: {entry.build} ({entry.version}) -> {entry.verdict} [{entry.reason}]",
                  file=sys.stderr)


if __name__ == "__main__":
    main()
