#!/usr/bin/env python3
"""build_deb_cache_apt.py -- resolve package/packages.txt with REAL apt.

Drop-in replacement for build_deb_cache_experimental_no_container.py, which
is a naive transitive walk by bare package name: no version-constraint
comparison, no real Provides:/Conflicts:/Breaks: semantics, and no notion of
firmware-version-gated Depends: at all. This runs Debian's own solver over
the same locally-vendored debcache/ and produces the same output files, so
BakeRamdisk.cpp's contract is unchanged:

    <output-dir>/picklist.txt           .deb filenames, one per line
    <output-dir>/resolved_packages.txt  top-level package names
    <output-dir>/apt-lists/             (empty, as before -- see below)

The apt it uses is this project's own vendored build, third_party/apt built
into build/apt-tools/ (see CMakeLists.txt's apt_ext block and .claude/TODO.md
item 17). It is NOT the host's apt, and there is no host apt on macOS to fall
back to -- Homebrew's formula cannot build on Darwin at all.

WHY THIS CAN DO SOMETHING THE OLD ONE COULD NOT: --firmware-version. The
debcache contains packages gated on `Depends: firmware (>= X)`, and whether
they resolve depends on which real firmware is being declared. The old
resolver stripped version constraints outright and so had to ignore the
question; this declares a synthetic `firmware` package at the real, per-tuple
ProductVersion and lets apt decide. That is the same synthetic package
build_deb_cache.py and kPreinstallInnerScript already rely on.

apt-lists/ is still written empty, deliberately. The bake stages it so
on-device apt knows what the real repos offered, and apt's lists cache from
THIS run would only describe the synthetic file:// repo built out of
debcache/ -- which is not that, and would be actively misleading on-device.
Growing debcache/ from live repos remains build_deb_cache.py's job.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def die(msg):
    raise SystemExit(f"build_deb_cache_apt.py: {msg}")


def read_package_list(path):
    names = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.append(line)
    return names


def parse_packages_index(text):
    """Map package name -> Filename: from an apt-ftparchive index."""
    mapping = {}
    name = filename = None
    for line in text.splitlines():
        if line.startswith("Package: "):
            name = line[9:].strip()
        elif line.startswith("Filename: "):
            filename = line[10:].strip()
        elif not line.strip():
            if name and filename:
                mapping[name] = filename
            name = filename = None
    if name and filename:
        mapping[name] = filename
    return mapping


def parse_new_packages(stdout):
    """Pull the package set out of apt's 'following NEW packages' block.

    Parsed rather than taken from `Inst ` lines on purpose: this ecosystem has
    a genuine circular Pre-Depends chain (dpkg -> tar -> gzip/lzma -> sed ->
    dpkg), so apt reports "Couldn't configure <pkg>, probably a dependency
    cycle" and never reaches the Inst listing. That is an ORDERING failure,
    not a resolution failure -- the set is fully computed and printed first,
    and ordering is irrelevant here because nothing is being installed. See
    AGENTS.md's install-time design notes on the same cycle.

    THE SET IS COMPLETE DESPITE THE FAILURE, verified rather than assumed: the
    block's entry count matches apt's own "N newly installed" summary line
    (67 and 67 on this project's real package list), and an independent walk of
    every resolved package's Depends:/Pre-Depends: against the resulting
    picklist finds zero unsatisfied dependency groups. apt finishes resolving
    and prints the answer, then fails trying to put it in install order.

    THE CYCLE IS NOT AVOIDABLE FROM APT'S SIDE, and it is not worth retrying.
    apt separates resolution (pkgDepCache/pkgProblemResolver, which succeeds)
    from ordering (pkgPackageManager/pkgOrderList, which is what fails), and
    every install-shaped command runs both even under `-s`. Measured, all
    failing identically on this package set:

        APT::Immediate-Configure=false        APT::Immediate-Configure-All=false
        APT::Force-LoopBreak=true             Dpkg::TriggersPending=true
        apt-get --print-uris                  apt-get satisfy
        apt-get --solver apt  (EDSP external-solver protocol)

    There is also no "debootstrap mode" to ask for: debootstrap never asks apt
    to order anything. It force-unpacks with dpkg and then runs a single
    `dpkg --configure -a`, letting dpkg's own solver break the cycle -- the
    same two-phase pattern kPreinstallInnerScript used and
    computePreinstalledPackages() now implements portably.
    """
    out = []
    collecting = False
    for line in stdout.splitlines():
        if re.match(r"^The following NEW packages will be installed:", line):
            collecting = True
            continue
        if collecting:
            if line.startswith((" ", "\t")):
                out.extend(line.split())
            else:
                break
    return out


UNMET_RE = re.compile(r"^\s+(\S+)\s*:\s*(Conflicts|Depends|Breaks|Pre-Depends):\s*(\S+)")

# The one apt failure this script tolerates -- see parse_new_packages().
ORDERING_CYCLE_RE = re.compile(r"Couldn't configure .*probably a dependency cycle", re.I)


def solve_with_drops(apt_get, env, top_level, allow_drops):
    """Solve, dropping top-level packages apt reports as unsatisfiable.

    packages.txt is one flat, firmware-independent list, but a few of its
    entries are neither: `apt7-lib` and `apt7-ssl` genuinely Conflict with each
    other, and the two persistence payloads
    (net.tihmstar.etasonuntether, com.ih8sn0w-squiffy-winocm.p0sixspwn) are
    pinned to `firmware (= 8.4.1)` and to the 6.1.x era respectively, so at most
    one can ever be installable on a given target. The previous resolver could
    not see any of that -- it stripped version constraints and had no
    Conflicts: handling -- so it silently put all of them in the picklist.
    Real apt refuses the whole solve instead, which is correct and also
    useless, so the offending TOP-LEVEL entries are dropped one at a time and
    the solve retried.

    Nothing is dropped quietly: every removal is reported by the caller. And
    only top-level entries are ever dropped -- a dependency apt pulled in
    itself is never second-guessed.

    Which side of a complaint to drop is decided by what apt actually said:
    `X : Conflicts: Y` means Y is the newcomer to remove (X is typically the
    one with `Priority: required`), while an unsatisfiable
    `X : Depends: ...` means X itself cannot be had.
    """
    remaining = list(top_level)
    dropped = []
    for _ in range(len(top_level) + 1):
        solve = subprocess.run(
            [str(apt_get), "-s", "-o", "APT::Immediate-Configure=false", "install", *remaining],
            capture_output=True, text=True, env=env)
        resolved = parse_new_packages(solve.stdout)
        if resolved:
            # apt exits non-zero here every time. Exactly one failure is
            # acceptable -- the ordering cycle documented in
            # parse_new_packages(). Anything else means the run failed for a
            # reason not accounted for, and taking the package list anyway
            # would mean trusting output from a command that did not do what
            # was asked.
            if solve.returncode != 0 and not ORDERING_CYCLE_RE.search(solve.stdout + solve.stderr):
                die("apt produced a package list but failed for a reason other than the known\n"
                    "  ordering cycle -- refusing to trust it:\n" + solve.stdout + solve.stderr)
            # Cross-check the block against apt's own count, so a future change
            # to apt's output format shows up as a loud mismatch rather than a
            # silently short picklist.
            m = re.search(r"(\d+) newly installed", solve.stdout)
            if m and int(m.group(1)) != len(resolved):
                die(f"parsed {len(resolved)} packages from apt's NEW block but apt reports "
                    f"{m.group(1)} newly installed -- output format changed?")
            return resolved, dropped
        if not allow_drops:
            die("apt could not satisfy package/packages.txt and --allow-drops was not given:\n"
                + solve.stdout + solve.stderr)
        victim = why = None
        for line in (solve.stdout + solve.stderr).splitlines():
            m = UNMET_RE.match(line)
            if not m:
                continue
            left, kind, right = m.group(1), m.group(2), m.group(3)
            cand = right if kind == "Conflicts" else left
            if cand in remaining:
                victim, why = cand, line.strip()
                break
        if victim is None:
            die("apt could not satisfy package/packages.txt, and none of the packages it "
                "complained about is a top-level entry this script may drop:\n"
                + solve.stdout + solve.stderr)
        remaining.remove(victim)
        dropped.append((victim, why))
    die("gave up dropping top-level packages without reaching a solution")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output-dir", required=True, type=Path)
    ap.add_argument("--apt-tools", required=True, type=Path,
                    help="directory holding apt-get/apt-ftparchive/methods (build/apt-tools)")
    ap.add_argument("--firmware-version", required=True,
                    help="real ProductVersion to declare the synthetic `firmware` package at")
    ap.add_argument("--debcache", type=Path, default=REPO_ROOT / "debcache")
    ap.add_argument("--packages-file", type=Path, default=REPO_ROOT / "package" / "packages.txt")
    ap.add_argument("--dpkg", default=shutil.which("dpkg") or "/usr/bin/dpkg")
    ap.add_argument("--allow-drops", action="store_true",
                    help="drop top-level packages real apt cannot satisfy for this firmware, "
                         "reporting each (see solve_with_drops)")
    args = ap.parse_args()

    out = args.output_dir.resolve()
    tools = args.apt_tools.resolve()
    debcache = args.debcache.resolve()

    apt_get = tools / "apt-get"
    ftparchive = tools / "apt-ftparchive"
    methods = tools / "methods"
    for p in (apt_get, ftparchive, methods):
        if not p.exists():
            die(f"{p} is missing -- build it first: cmake --build build --target apt")
    if not debcache.is_dir():
        die(f"{debcache} is not a directory")

    root = out / "aptroot"
    for sub in ("etc/apt/apt.conf.d", "etc/apt/preferences.d", "etc/apt/sources.list.d",
                "var/lib/apt/lists/partial", "var/lib/dpkg",
                "var/cache/apt/archives/partial", "var/log/apt", "flat"):
        (root / sub).mkdir(parents=True, exist_ok=True)

    index = subprocess.run([str(ftparchive), "packages", debcache.name],
                           cwd=debcache.parent, capture_output=True, text=True)
    if index.returncode != 0:
        die(f"apt-ftparchive failed:\n{index.stderr}")
    (root / "flat" / "Packages").write_text(index.stdout)
    name_to_filename = parse_packages_index(index.stdout)
    if not name_to_filename:
        die("apt-ftparchive produced no usable Packages index")

    (root / "etc/apt/sources.list").write_text(
        f"deb [trusted=yes] file://{root / 'flat'} ./\n")

    # The synthetic `firmware` package, declared installed at the real target
    # version. Without it, anything with `Depends: firmware (>= X)` (e.g.
    # com.saurik.patcyh) is correctly unsatisfiable and the whole solve fails.
    (root / "var/lib/dpkg/status").write_text(
        "Package: firmware\n"
        "Status: install ok installed\n"
        "Priority: required\n"
        "Section: System\n"
        "Installed-Size: 0\n"
        "Architecture: iphoneos-arm\n"
        f"Version: {args.firmware_version}\n"
        "Description: Apple TV firmware (synthetic, declared for dependency resolution)\n\n")

    conf = root / "apt.conf"
    conf.write_text(
        f'Dir::State "{root / "var/lib/apt"}";\n'
        f'Dir::State::status "{root / "var/lib/dpkg/status"}";\n'
        f'Dir::Cache "{root / "var/cache/apt"}";\n'
        f'Dir::Etc "{root / "etc/apt"}";\n'
        f'Dir::Log "{root / "var/log/apt"}";\n'
        f'Dir::Bin::dpkg "{args.dpkg}";\n'
        # Without this apt cannot run methods/file and every fetch fails with
        # "The method driver .../file could not be found".
        f'Dir::Bin::methods "{methods}";\n'
        # Without this apt exits "Unable to determine a suitable packaging
        # system type" before doing anything at all.
        'APT::System "Debian dpkg interface";\n'
        'APT::Architecture "iphoneos-arm";\n'
        'APT::Architectures { "iphoneos-arm"; };\n'
        'APT::Get::AllowUnauthenticated "true";\n'
        'Acquire::AllowInsecureRepositories "true";\n')

    env = dict(os.environ, APT_CONFIG=str(conf))
    upd = subprocess.run([str(apt_get), "update"], capture_output=True, text=True, env=env)
    if upd.returncode != 0:
        die(f"apt-get update failed:\n{upd.stdout}\n{upd.stderr}")

    top_level = read_package_list(args.packages_file)
    if not top_level:
        die(f"{args.packages_file} lists no packages")

    resolved, dropped = solve_with_drops(apt_get, env, top_level, args.allow_drops)
    if not resolved:
        die("apt resolved no packages")
    if dropped:
        sys.stderr.write(
            "build_deb_cache_apt.py: WARNING: dropped %d top-level package(s) real apt could not\n"
            "  satisfy against firmware %s. The old resolver installed these silently because it\n"
            "  compared no versions and honoured no Conflicts:. See .claude/TODO.md item 18.\n"
            % (len(dropped), args.firmware_version))
        for name, why in dropped:
            sys.stderr.write(f"    {name}: {why}\n")

    missing = [n for n in resolved if n not in name_to_filename]
    if missing:
        die("apt resolved packages with no .deb in the local debcache: "
            + ", ".join(sorted(missing))
            + "\n  Grow debcache/ on a real Linux+podman host with build_deb_cache.py.")

    picklist = sorted({os.path.basename(name_to_filename[n]) for n in resolved})
    out.mkdir(parents=True, exist_ok=True)
    (out / "picklist.txt").write_text("\n".join(picklist) + "\n")
    (out / "resolved_packages.txt").write_text("\n".join(sorted(resolved)) + "\n")
    (out / "apt-lists").mkdir(exist_ok=True)

    print(f"build_deb_cache_apt.py: real apt resolved {len(top_level)} top-level packages "
          f"to {len(picklist)} .deb files (firmware {args.firmware_version})")


if __name__ == "__main__":
    main()
