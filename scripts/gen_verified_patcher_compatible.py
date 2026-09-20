#!/usr/bin/env python3
"""
gen_verified_patcher_compatible.py — regenerate misc/verified_patcher_compatible.txt,
the record of which (device, buildID) tuples this project's two GPL patch tools can
ACTUALLY patch, established by running them rather than by reasoning about them.

Why this list has to exist at all. `src/Cli.cpp` hard-pins one build
(`kJailbreakTargetBuild`), and the obvious question — "can we move that to something
newer?" — has three independent answers stacked on top of each other, and only the
last one is about the patchers:

  1. Is the IPSW fetchable the way IPSWDownloader.cpp fetches it (HTTP Range into a
     remote zip)? That is misc/download_blacklist.txt's question, answered by
     scripts/gen_download_blacklist.py. A blacklisted tuple is dead before anything
     here runs, so this script honours that file.

  2. Do we have the IMG3 decryption keys? Patching a component means decrypting it
     first, so a build with no keys/<device>/<device>_<buildID>.keys file is
     untargetable no matter how capable the patchers are. That is a *key
     availability* fact, not a patcher fact, and this file records it as such
     (`no-keys`) so nobody misreads it as a patcher defect.

  3. Given the bytes and the keys, do iBoot32Patcher (iBSS + iBEC) and CBPatcher
     (kernelcache) find their instruction patterns in THIS build? That is the only
     question this script actually measures, and the only way to measure it is to
     run them: both tools are signature/pattern matchers against a specific
     bootloader or kernel, and whether a given pattern survives into a given build
     is not predictable from the version number.

How it measures. It shells out to the project's own standalone bake tools, which
already do the full decrypt -> patch -> re-encrypt round trip for one tuple and fail
loudly:

    build/bake-iboot  --device <model> --build <buildID> --out <dir> --force
    build/bake-kernel --device <model> --build <buildID> --out <dir> --force

Those are the same code paths (PatcherPatch.cpp) that bake-firmware's bootchain half
runs, so a pass here is a real pass, not a simulation. Output goes to a throwaway
directory per tuple — this script never writes into dist/, so it cannot poison a real
bake. Both tools reuse the shared IPSW cache under ipswDataRoot()
($XDG_DATA_HOME/blackb0x, else ~/.local/share/blackb0x) and pull individual
components over HTTP Range, so a tuple costs tens of megabytes rather than a ~700MB
IPSW. The two tools for one tuple run SEQUENTIALLY because they share that tuple's
cache directory; different tuples run concurrently.

What is deliberately NOT claimed. A tuple that passes here has been patched
successfully on the host. It has NOT been booted on hardware. "Both patchers report
success" is necessary for a target and nowhere near sufficient — see docs/HISTORY.md
on the 10B329a kernelcache, which patches and re-encrypts perfectly and still does
not boot. Every row carries an explicit evidence column for exactly this reason: a
row is only ever `verified:<date>` if the patchers really ran, and anything else says
so in the file rather than being quietly presented as a result.

Two columns for the kernel, and why. The kernelcache must be decrypted before
CBPatcher sees it, and the vendored xpwn's IMG3 reader has a defect that makes most
builds fail at that step -- so with the tree as committed, the `kernel` column
mostly measures xpwn rather than CBPatcher. `readImg3Element()` reads only
`dataSize` bytes of the DATA element while `setKeyImg3()` goes on to AES-CBC
decrypt the full 16-aligned body; the bytes in between are zeros instead of real
ciphertext, so the final cipher block decrypts to garbage, the LZSS stream stops a
few dozen bytes short of its declared length, and `createAbstractFileFromComp()`
rejects the kernelcache. It is the exact mirror of the encrypt-side 16-align fix
already landed in regulad/xpwn@legacy. Run this script with --xpwn-read-fix against
a bake-kernel built from an xpwn carrying the read-side fix and it fills the
`kernel-readfix` column instead, which is what CBPatcher can really do. Both
columns are kept because both facts matter: what the tree does today, and what the
patcher is capable of.

Usage:
  scripts/gen_verified_patcher_compatible.py                  # test every keyed tuple
  scripts/gen_verified_patcher_compatible.py --device AppleTV3,2   # repeatable
  scripts/gen_verified_patcher_compatible.py --build 10B329a       # repeatable
  scripts/gen_verified_patcher_compatible.py --jobs 4              # default 4
  scripts/gen_verified_patcher_compatible.py --dry-run             # list work, run nothing
  scripts/gen_verified_patcher_compatible.py --output -            # stdout
  scripts/gen_verified_patcher_compatible.py --keep-untested       # merge, don't retest
  scripts/gen_verified_patcher_compatible.py --xpwn-read-fix       # fill kernel-readfix
"""

import argparse
import concurrent.futures
import datetime
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.error
import urllib.parse
import urllib.request

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUTPUT = os.path.join(REPO_ROOT, "misc", "verified_patcher_compatible.txt")
KEYS_DIR = os.path.join(REPO_ROOT, "keys")
BLACKLIST = os.path.join(REPO_ROOT, "misc", "download_blacklist.txt")
FIRMWARE_VERSIONS = os.path.join(REPO_ROOT, "misc", "firmware_versions.txt")
BAKE_IBOOT = os.path.join(REPO_ROOT, "build", "bake-iboot")
BAKE_KERNEL = os.path.join(REPO_ROOT, "build", "bake-kernel")

DEVICES = ["AppleTV2,1", "AppleTV3,1", "AppleTV3,2"]

IPSW_ME_DEVICE = "https://api.ipsw.me/v4/device/{device}?type=ipsw"
USER_AGENT = "blackb0x-gen-verified-patcher-compatible/1.0 (+https://github.com/regulad/Blackb0x--)"

# A single tuple is two subprocesses, each of which downloads a few components.
# Generous, because a cold cache on a slow CDN morning is not a failure.
TIMEOUT_SECONDS = 900

PRINT_LOCK = threading.Lock()


def log(msg):
    with PRINT_LOCK:
        print(msg, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------


def keyed_tuples():
    """Every (device, buildID) with a keys/<device>/<device>_<buildID>.keys file."""
    out = set()
    for device in DEVICES:
        d = os.path.join(KEYS_DIR, device)
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            if not name.endswith(".keys"):
                continue
            stem = name[: -len(".keys")]
            prefix = device + "_"
            if not stem.startswith(prefix):
                continue
            out.add((device, stem[len(prefix) :]))
    return out


def read_blacklist(path=BLACKLIST):
    """(device, buildID) pairs whose IPSW cannot be range-fetched. Honoured, not assumed."""
    out = set()
    if not os.path.exists(path):
        return out
    with open(path) as fh:
        for line in fh:
            line = line.split("#")[0].strip()
            if not line:
                continue
            parts = line.split("\t") if "\t" in line else line.split()
            if len(parts) >= 2:
                out.add((parts[0], parts[1]))
    return out


def read_versions(path=FIRMWARE_VERSIONS):
    out = {}
    if not os.path.exists(path):
        return out
    with open(path) as fh:
        for line in fh:
            parts = line.split("#")[0].split()
            if len(parts) == 3:
                out[(parts[0], parts[1])] = parts[2]
    return out


def ipsw_me_tuples(device):
    """Every build ipsw.me enumerates for a device, as {buildID: version}.

    Used only to find builds that EXIST but have no keys — the ones that have to be
    reported as key-blocked rather than silently omitted. A network failure here is
    not fatal; the script just cannot report that class for that device.
    """
    req = urllib.request.Request(
        IPSW_ME_DEVICE.format(device=urllib.parse.quote(device)),
        headers={"User-Agent": USER_AGENT, "Accept": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = json.load(resp)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        log(f"warning: could not enumerate ipsw.me for {device}: {exc}")
        return {}
    out = {}
    for fw in data.get("firmwares", []):
        build = fw.get("buildid")
        if build:
            out[build] = fw.get("version", "")
    return out


# ---------------------------------------------------------------------------
# Running the patchers
# ---------------------------------------------------------------------------

# Result codes, most specific first. `ok` means the tool exited 0 and published its
# output. Everything else distinguishes "the patcher could not patch this build"
# (the finding this file exists for) from the several unrelated ways a run can die.
#
#   fail:patch       the patch tool itself returned nonzero -- it could not find one
#                    of its target instruction patterns in this build. THE finding.
#   fail:decrypt     xpwn's decrypt() produced nothing usable (the IMG3/lzss path),
#                    so the patcher was never reached. Not a patcher verdict.
#   fail:keys        the .keys file exists but has no entry for this component.
#   fail:download    the component could not be fetched.
#   fail:no-url      ipsw.me lists no download URL (internal/beta builds).
#   fail:crash       the tool died on a signal.
#   fail:other       nonzero exit this script could not classify -- read the log.
def classify(returncode, output):
    if returncode == 0:
        return "ok"
    if "no firmware URL from ipsw.me" in output or "could not open remote IPSW" in output:
        return "fail:no-url"
    if re.search(r"no (iBSS|iBEC|Kernelcache) keys loaded", output):
        return "fail:keys"
    if "produced no usable output" in output:
        return "fail:decrypt"
    if re.search(r"(iBoot32Patcher|CBPatcher) failed", output):
        return "fail:patch"
    if "failed to download" in output or "failed to parse BuildManifest" in output:
        return "fail:download"
    if returncode < 0 or returncode >= 128:
        return "fail:crash"
    return "fail:other"


def iboot_detail(code, output):
    """iBSS and iBEC are patched by the same tool in the same run; say which one broke."""
    if code == "ok":
        return "ok"
    # bake-iboot patches iBSS first and stops on failure, so whichever component
    # name appears in a diagnostic line is the one that broke.
    if "patchiBEC" in output:
        which = "ibec"
    elif "patchiBSS" in output:
        which = "ibss"
    else:
        which = "iboot"
    return code.replace("fail:", f"fail:{which}-")


# A build whose boot chain Apple shipped UNENCRYPTED needs no keys at all, and the
# right .keys file for it is one with empty strings throughout -- exactly what
# keys/<device>/<device>_12H914.keys already looks like. So "keys/ has no file for
# this tuple" is NOT the same as "this tuple is key-blocked": it may simply be a
# missing (empty) file. This template lets the script tell those two apart by
# running the real pipeline against synthetic empty keys. If the component carries
# no KBAG, xpwn never decrypts and the patch succeeds; if it IS encrypted, decrypt
# produces garbage and the run fails, which is the honest `no-keys` answer.
EMPTY_KEY_COMPONENTS = [
    "AppleLogo",
    "DeviceTree",
    "Kernelcache",
    "LLB",
    "RecoveryMode",
    "RestoreRamdisk",
    "RootFileSystem",
    "iBEC",
    "iBSS",
    "iBoot",
]


def build_probe_keys_dir(keyless):
    """A BLACKB0X_IMAGEKEYS_DIR that mirrors keys/ plus an empty file per keyless tuple.

    Symlinks, not copies, so every keyed tuple resolves to the real committed file and
    this can never shadow or corrupt keys/. Returns the directory, or None.
    """
    if not keyless:
        return None
    root = tempfile.mkdtemp(prefix="patchcompat-keys-")
    for device in DEVICES:
        dst = os.path.join(root, device)
        os.makedirs(dst, exist_ok=True)
        src = os.path.join(KEYS_DIR, device)
        if os.path.isdir(src):
            for name in os.listdir(src):
                if name.endswith(".keys"):
                    os.symlink(os.path.join(src, name), os.path.join(dst, name))
    for device, build in keyless:
        path = os.path.join(root, device, f"{device}_{build}.keys")
        with open(path, "wb") as fh:
            plistlib.dump({c: ["", ""] for c in EMPTY_KEY_COMPONENTS}, fh)
    return root


def run_one(device, build, log_dir, keys_dir=None, bake_iboot=BAKE_IBOOT, bake_kernel=BAKE_KERNEL):
    """Run both patchers for one tuple. Returns (iboot_result, kernel_result)."""
    results = {}
    outdir = tempfile.mkdtemp(prefix=f"patchcompat-{device.replace(',', '_')}-{build}-")
    env = dict(os.environ)
    if keys_dir:
        env["BLACKB0X_IMAGEKEYS_DIR"] = keys_dir
    try:
        for name, binary in (("iboot", bake_iboot), ("kernel", bake_kernel)):
            cmd = [binary, "--device", device, "--build", build, "--out", outdir, "--force"]
            try:
                proc = subprocess.run(
                    cmd,
                    cwd=REPO_ROOT,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=TIMEOUT_SECONDS,
                )
                output = proc.stdout.decode("utf-8", "replace")
                code = classify(proc.returncode, output)
            except subprocess.TimeoutExpired as exc:
                output = (exc.output or b"").decode("utf-8", "replace")
                code = "fail:timeout"
            if log_dir:
                os.makedirs(log_dir, exist_ok=True)
                fn = f"{device.replace(',', '_')}_{build}.{name}.log"
                with open(os.path.join(log_dir, fn), "w") as fh:
                    fh.write(output)
            results[name] = iboot_detail(code, output) if name == "iboot" else code
    finally:
        shutil.rmtree(outdir, ignore_errors=True)
    return results["iboot"], results["kernel"]


# ---------------------------------------------------------------------------
# The file
# ---------------------------------------------------------------------------


def parse_existing(path):
    """Existing rows, so a partial re-run merges instead of discarding evidence."""
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path) as fh:
        for line in fh:
            line = line.split("#")[0].rstrip("\n")
            if not line.strip():
                continue
            parts = line.split("\t")
            if len(parts) >= 7:
                rows[(parts[0], parts[1])] = parts[2:7]
    return rows


HEADER = """\
# verified_patcher_compatible.txt -- for each (device, buildID) tuple, whether this
# project's two GPL patch tools can actually patch that firmware. Established by
# RUNNING them, not by reasoning about version numbers.
#
# The two tools, and what the verdict columns mean:
#
#   iBoot32Patcher  third_party/iBoot32Patcher (regulad/iBoot32Patcher@blackb0x).
#                   Patches the BOOTLOADER -- iBSS with -r, iBEC with -r -k -t
#                   (signature check, KASLR, ticket check). The `iboot` column is
#                   one verdict for both components; a failure says which one
#                   (ibss- or ibec-) broke. Exercised via build/bake-iboot.
#
#   CBPatcher       third_party/CBPatcher (zzanehip/CBPatcher, upstream). Patches
#                   the KERNELCACHE (tfp0, AMFI/memcmp, sandbox policy). Two
#                   columns, `kernel` and `kernel-readfix` -- see below.
#                   Exercised via build/bake-kernel.
#
# Both tools are pattern matchers against a specific bootloader or kernel build.
# Whether a given signature survives into a given build is not predictable from the
# version string, which is why this is measured rather than inferred.
#
# WHY THE KERNEL HAS TWO COLUMNS. The kernelcache has to be decrypted before
# CBPatcher ever sees it, and the vendored xpwn's IMG3 reader has a defect that
# makes most builds fail at that step -- so with the tree as committed, the `kernel`
# column mostly measures xpwn, not CBPatcher. readImg3Element() reads only
# `dataSize` bytes of the DATA element while setKeyImg3() goes on to AES-CBC
# decrypt the full 16-aligned body, so the bytes between the two are zeros instead
# of real ciphertext and the whole final cipher block decrypts to garbage. The LZSS
# stream then stops a few dozen bytes short of its declared length and
# createAbstractFileFromComp() rejects the kernelcache ("error: cannot open
# infile"). It is the exact mirror of the encrypt-side 16-align fix already landed
# in regulad/xpwn@legacy (see docs/HISTORY.md) -- the read path was never brought
# into line with it. `kernel-readfix` is the same measurement with that one-line
# read-side fix applied, and it is what CBPatcher can really do.
#
# THIS FILE DOES NOT SAY A BUILD IS JAILBREAKABLE. It says the host-side
# decrypt -> patch -> re-encrypt round trip completed. Nothing here has been booted
# on hardware, and "both patchers report success" is necessary but nowhere near
# sufficient -- see docs/HISTORY.md on 10B329a, whose kernelcache patches and
# re-encrypts perfectly and still did not boot until the img3 alignment fix.
#
# Three independent gates stack before a tuple can even be attempted, and only the
# third is about the patchers:
#   1. the IPSW must be range-fetchable      -> misc/download_blacklist.txt
#   2. the IMG3 keys must exist              -> keys/<device>/<device>_<build>.keys
#   3. the patchers must find their patterns -> this file
# A tuple failing gate 2 is recorded as `no-keys`, NOT as a patcher failure -- that
# is "nobody published the keys", a completely different fact from "the patcher
# cannot handle it". But note gate 2 is weaker than it looks: Apple shipped the late
# Apple TV 3 builds with an UNENCRYPTED boot chain (no KBAG element at all), so a
# missing keys/ file there means "no keys are needed", not "no keys exist". This
# script tells the two apart by running the pipeline against synthetic empty keys,
# and marks the unencrypted ones `verified-unencrypted:<date>`.
#
# GENERATED FILE -- do not hand-edit. Regenerate with:
#   scripts/gen_verified_patcher_compatible.py
# and, for the `kernel-readfix` column, the same command with --xpwn-read-fix
# against a build/bake-kernel built from an xpwn carrying the read-side fix.
#
# Format: one line per tuple, TAB-separated:
#   <device>\t<buildID>\t<version>\t<iboot>\t<kernel>\t<kernel-readfix>\t<evidence>
# Sorted by device then buildID (plain `LC_ALL=C sort`, matching this directory's
# other list files). Blank lines and #-comments are skippable.
#
# Verdict codes (<iboot>, <kernel>, <kernel-readfix>):
#   ok                  the tool ran and published a patched, re-encrypted component
#   fail:patch          THE finding -- the patcher could not find a target pattern
#                       (spelled fail:ibss-patch / fail:ibec-patch in the iboot
#                       column, to say which component it choked on)
#   fail:decrypt        xpwn's decrypt() produced nothing usable; the patcher was
#                       never reached, so this is NOT a patcher verdict
#   fail:keys           .keys file exists but has no entry for that component
#   fail:download       the component could not be fetched -- on the earliest
#                       AppleTV2,1 builds this is our own BuildManifest identity
#                       selection picking a *dev* iBSS that the IPSW does not carry,
#                       also not a patcher verdict
#   fail:no-url         ipsw.me lists no downloadable IPSW (internal/beta builds)
#   fail:crash          the tool died on a signal
#   fail:timeout        the tool did not finish
#   fail:other          nonzero exit this script could not classify
#   no-keys             encrypted, and no keys/<device>/<device>_<build>.keys exists
#   blacklisted         misc/download_blacklist.txt says the IPSW is unfetchable
#   untested            not attempted in this run and no prior result to carry over
#
# Evidence codes:
#   verified:<date>              the patchers really ran, on that date. The only
#                                code that means a verdict was measured.
#   verified-unencrypted:<date>  same, and the tuple needed NO keys at all -- Apple
#                                shipped its boot chain with no KBAG. keys/ has no
#                                file for it and does not need one.
#   no-keys                      gate 2 genuinely blocks it; nothing was run
#   blacklisted                  gate 1 blocks it; nothing was run
#   untested                     no measurement exists. NEVER read an untested row
#                                as a pass or a failure -- it is an absence of
#                                evidence.
#
"""


def summarize(rows, versions):
    """The per-device headline: newest build each patcher is known to handle."""
    lines = []

    def vkey(v):
        return tuple(int(p) for p in re.findall(r"\d+", v or "0"))

    for device in DEVICES:
        mine = [(b, r) for (d, b), r in rows.items() if d == device]
        # Sort on the row's OWN version column, not the versions map: a narrowed
        # re-run does not re-fetch versions for the devices it skipped, and falling
        # back to 0 there would silently sort those rows to the front and hand back
        # the wrong "newest".
        mine.sort(key=lambda kv: vkey(kv[1][0] if kv[1][0] != "-" else versions.get((device, kv[0]), "")))
        newest = {"iboot": None, "kernel": None, "readfix": None, "both": None}
        for build, r in mine:
            if not r[4].startswith("verified"):
                continue
            if r[1] == "ok":
                newest["iboot"] = build
            if r[2] == "ok":
                newest["kernel"] = build
            if r[3] == "ok":
                newest["readfix"] = build
            if r[1] == "ok" and r[3] == "ok":
                newest["both"] = build
        blocked = sorted(b for b, r in mine if r[1] == "no-keys")
        # The only rows that are a real verdict ON A PATCHER: it ran, against real
        # decrypted bytes, and could not find a pattern. Everything else that fails
        # is the fetch, the keys, or xpwn.
        genuine = sorted(
            b
            for b, r in mine
            if "-patch" in r[1] or r[1] == "fail:patch" or r[3] == "fail:patch"
        )
        unenc = sorted(b for b, r in mine if r[4].startswith("verified-unencrypted"))
        lines.append(
            f"#   {device}: newest verified iBoot32Patcher-OK = {newest['iboot'] or 'none'}; "
            f"newest CBPatcher-OK as committed = {newest['kernel'] or 'none'}; "
            f"with the xpwn read fix = {newest['readfix'] or 'none'}; "
            f"newest with BOTH (read fix) = {newest['both'] or 'none'}"
        )
        if genuine:
            lines.append(f"#     real patcher failures (pattern not found): {' '.join(genuine)}")
        if unenc:
            lines.append(
                f"#     unencrypted boot chain, no keys needed or published: {' '.join(unenc)}"
            )
        if blocked:
            lines.append(f"#     genuinely key-blocked (encrypted, no published keys): {' '.join(blocked)}")
    return lines


def write_output(path, rows, versions, stats, quiet=False):
    today = datetime.date.today().isoformat()
    body = []
    for (device, build) in sorted(rows, key=lambda t: (t[0], t[1])):
        version, iboot, kernel, readfix, evidence = rows[(device, build)]
        body.append("\t".join([device, build, version or "-", iboot, kernel, readfix, evidence]))

    out = [HEADER]
    out.append(f"# Generated {today}. {stats}\n#\n")
    out.append("# Headline, derived from the rows below:\n")
    out.extend(line + "\n" for line in summarize(rows, versions))
    out.append("#\n")
    out.append("\n".join(body) + "\n")
    text = "".join(out)

    if path == "-":
        sys.stdout.write(text)
    else:
        with open(path, "w") as fh:
            fh.write(text)
        if not quiet:
            log(f"wrote {path} ({len(body)} tuples)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", action="append", choices=DEVICES, help="repeatable; default all three")
    ap.add_argument("--build", action="append", help="repeatable; default every keyed build")
    ap.add_argument("--jobs", type=int, default=4, help="tuples in flight (default 4)")
    ap.add_argument("--output", default=DEFAULT_OUTPUT, help="'-' for stdout")
    ap.add_argument("--log-dir", default=os.path.join(REPO_ROOT, "build", "patcher-compat-logs"))
    ap.add_argument("--dry-run", action="store_true", help="list the work, run nothing")
    ap.add_argument("--keep-untested", action="store_true", help="carry prior rows over; test nothing new")
    ap.add_argument("--no-network", action="store_true", help="skip the ipsw.me no-keys enumeration")
    ap.add_argument(
        "--xpwn-read-fix",
        action="store_true",
        help="the build/bake-kernel about to run carries the xpwn IMG3 DATA read-side "
        "16-align fix; write kernel verdicts into the kernel-readfix column",
    )
    ap.add_argument(
        "--no-keyless-probe",
        action="store_true",
        help="do not try keyless tuples against synthetic empty keys (that probe is what "
        "distinguishes 'Apple shipped it unencrypted' from 'the keys were never published')",
    )
    args = ap.parse_args()

    devices = args.device or DEVICES
    # Which of the two kernel columns this run fills in. Index into the row:
    # [version, iboot, kernel, kernel-readfix, evidence].
    kernel_col = 3 if args.xpwn_read_fix else 2

    for binary in (BAKE_IBOOT, BAKE_KERNEL):
        if not os.path.exists(binary) and not args.dry_run:
            raise SystemExit(
                f"{os.path.basename(binary)} not built: {binary}\n"
                "  cmake --build build --target authoring -j$(sysctl -n hw.ncpu)"
            )

    versions = read_versions()
    blacklist = read_blacklist()
    keyed = {t for t in keyed_tuples() if t[0] in devices}
    prior = parse_existing(args.output) if args.output != "-" else {}

    def blank(version, code="untested"):
        return [version, code, code, code, code]

    # Seed with EVERY prior row before any filtering. --device/--build narrow what
    # gets RE-TESTED; they must never silently drop rows for the tuples they exclude
    # (each one cost a real download and two patch runs to measure).
    rows = {k: list(v) for k, v in prior.items()}

    # Gate 2, the negative side: builds that exist on ipsw.me with no keys/ file.
    # Probed rather than assumed -- see EMPTY_KEY_COMPONENTS. Only if the probe
    # fails is the tuple really key-blocked.
    keyless = []
    if not args.no_network:
        for device in devices:
            for build, version in ipsw_me_tuples(device).items():
                if (device, build) in keyed:
                    continue
                versions.setdefault((device, build), version)
                # prior first: a keyless tuple can already carry a measured verdict
                # in the other kernel column from an earlier run, and re-probing it
                # here must not throw that away.
                rows[(device, build)] = prior.get((device, build), blank(version, "no-keys"))
                if args.no_keyless_probe or args.keep_untested or (device, build) in blacklist:
                    continue
                if not args.build or build in args.build:
                    keyless.append((device, build))

    todo = list(keyless)
    for (device, build) in sorted(keyed):
        version = versions.get((device, build), "")
        if (device, build) in blacklist:
            rows[(device, build)] = blank(version, "blacklisted")
            continue
        if args.build and build not in args.build:
            rows[(device, build)] = prior.get((device, build), blank(version))
            continue
        if args.keep_untested and (device, build) in prior:
            rows[(device, build)] = prior[(device, build)]
            continue
        todo.append((device, build))
        rows[(device, build)] = prior.get((device, build), blank(version))

    log(
        f"{len(todo)} tuples to test ({len(keyless)} of them keyless probes), "
        f"{len(rows) - len(todo)} carried/known, jobs={args.jobs}, "
        f"column={'kernel-readfix' if args.xpwn_read_fix else 'kernel'}"
    )
    if args.dry_run:
        for device, build in todo:
            print(f"{device}\t{build}")
        return

    keyless_set = set(keyless)
    probe_keys = build_probe_keys_dir(keyless)
    today = datetime.date.today().isoformat()
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(run_one, d, b, args.log_dir, probe_keys if (d, b) in keyless_set else None): (d, b)
            for d, b in todo
        }
        for fut in concurrent.futures.as_completed(futures):
            device, build = futures[fut]
            try:
                iboot, kernel = fut.result()
            except Exception as exc:  # noqa: BLE001 - one bad tuple must not lose the run
                log(f"{device} {build}: harness error: {exc}")
                iboot = kernel = "fail:other"
            row = list(rows.get((device, build)) or blank(versions.get((device, build), "")))
            if (device, build) in keyless_set and iboot != "ok":
                # The empty-key probe did not patch it, so it really is encrypted
                # with keys nobody has published. Leave the honest verdict.
                row = blank(versions.get((device, build), ""), "no-keys")
            else:
                row[0] = versions.get((device, build), row[0])
                row[1] = iboot
                row[kernel_col] = kernel
                row[4] = (
                    f"verified-unencrypted:{today}" if (device, build) in keyless_set else f"verified:{today}"
                )
            rows[(device, build)] = row
            done += 1
            log(f"[{done}/{len(todo)}] {device} {build}: iboot={iboot} kernel={kernel}")
            # Checkpoint after every tuple. A sweep is long and every row costs a
            # real download plus two patch runs; losing the lot to a Ctrl-C or a
            # dead network at tuple 90 would be a genuinely expensive mistake.
            if args.output != "-":
                write_output(
                    args.output, rows, versions, "IN PROGRESS -- partial sweep, not final.", quiet=True
                )

    verified = sum(1 for r in rows.values() if r[4].startswith("verified"))
    unenc = sum(1 for r in rows.values() if r[4].startswith("verified-unencrypted"))
    both_committed = sum(1 for r in rows.values() if r[1] == "ok" and r[2] == "ok")
    both_readfix = sum(1 for r in rows.values() if r[1] == "ok" and r[3] == "ok")
    nokeys = sum(1 for r in rows.values() if r[1] == "no-keys")
    stats = (
        f"{len(rows)} tuples: {verified} verified by running the patchers "
        f"({unenc} of them needing no keys at all), {nokeys} genuinely key-blocked, "
        f"{len(rows) - verified - nokeys} neither. Both patchers OK: "
        f"{both_committed} with the tree as committed, {both_readfix} with the xpwn read fix."
    )
    write_output(args.output, rows, versions, stats)


if __name__ == "__main__":
    main()
