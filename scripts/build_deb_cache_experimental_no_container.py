#!/usr/bin/env python3
"""
build_deb_cache_experimental_no_container.py — EXPERIMENTAL, portable
(no podman, no container, no network) stand-in for build_deb_cache.py's
own picklist.txt/resolved_packages.txt generation, for platforms where
containerization is genuinely unavailable (this exists specifically for
macOS — see .claude/TODO.md item 4a and docs/HISTORY.md's own entry on
why: podman/all container runtimes were confirmed unavailable there, and
neither MacPorts nor Fink provide a usable native real-apt-get on Apple
Silicon).

*** NOT a long-term replacement for build_deb_cache.py. *** That script
shells out to real apt-get's own dependency solver — full version-
constraint handling, real Provides:/Conflicts:/Breaks: semantics, GPG-
verified fetches of genuinely new packages from live repos — deliberately
NOT reimplemented here (see that script's own docstring for why
reimplementing apt is something this project's conventions warn against).
This script does a much smaller, much more naive job: a plain transitive
closure walk by bare package NAME (no version-constraint comparison at
all — every Depends:/Pre-Depends:/Provides: field gets stripped straight
to package names, same simplification BakeRamdisk.cpp's own
parseDependencyGroups() already makes for a different purpose) over
*only* the .deb files already vendored in debcache/ — it cannot
fetch anything new, and will fail loudly (not silently) if packages.txt's
closure needs something not already present. Keep using
build_deb_cache.py on a real Linux+podman machine whenever debcache/
needs to grow; this script only reconstructs picklist.txt/
resolved_packages.txt from what's already there, for machines where that
script can't run at all.

Known gaps versus build_deb_cache.py, on top of the above:
  - No version-constraint checking (a Depends: foo (>= 2.0) is satisfied
    by ANY locally-vendored "foo", regardless of its actual Version:).
  - First-match-wins for both `|` alternatives and virtual-package
    Provides: — real apt's own priority/heuristic ordering isn't
    reproduced.
  - No apt-lists/ or local-repo/ generation (GPG verification, live repo
    fetching, dpkg-scanpackages) — writes an empty apt-lists/ directory
    so stageDebcache()'s own path assumptions still hold, and skips
    local-repo/ entirely (already-optional downstream — see
    computeGlobalDebcacheOnce()). Concretely: a macOS-baked ramdisk's
    on-device apt won't have pre-cached package lists, and
    local_only_debs.txt's own entries (e.g. "essential") won't be
    resolvable by name via the on-device apt-get the way a Linux-baked
    one's would be, even though their .deb bytes are still correctly
    included in picklist.txt.

Usage matches build_deb_cache.py's own CLI exactly, so BakeRamdisk.cpp's
computeGlobalDebcacheOnce() can select between the two scripts by name
alone, no other call-site changes:
    scripts/build_deb_cache_experimental_no_container.py --output-dir DIR
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MISC_DIR = REPO_ROOT / "Blackb0x" / "Misc"
DEBS_DIR = REPO_ROOT / "debcache"
PACKAGE_DIR = REPO_ROOT / "package"
# packages.txt and local_only_debs.txt describe what the
# xyz.regulad.blackb0x package installs, so they live with the package
# rather than in Blackb0x/Misc (which is bake-time host assets).
PACKAGES_LIST = PACKAGE_DIR / "packages.txt"
LOCAL_ONLY_LIST = PACKAGE_DIR / "local_only_debs.txt"

# Reused directly from build_deb_cache.py rather than duplicated, so the
# two scripts can't silently drift apart on which names are expected to
# fail real resolution.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_deb_cache import KNOWN_EXPECTED_UNRESOLVABLE  # noqa: E402


class PackageInfo:
    __slots__ = ("name", "filename", "depends", "pre_depends", "provides")

    def __init__(self, name, filename, depends, pre_depends, provides):
        self.name = name
        self.filename = filename
        self.depends = depends
        self.pre_depends = pre_depends
        self.provides = provides


def read_lines(path: Path):
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        out.append(line)
    return out


def parse_dependency_groups(field: str):
    """Mirrors BakeRamdisk.cpp's own parseDependencyGroups(): comma-
    separated groups, each a list of `|`-alternatives, version
    constraints and whitespace stripped down to bare package names."""
    groups = []
    for group in field.split(","):
        group = group.strip()
        if not group:
            continue
        alts = []
        for alt in group.split("|"):
            name = re.sub(r"\(.*?\)", "", alt).strip()
            if name:
                alts.append(name)
        if alts:
            groups.append(alts)
    return groups


def read_deb_control_info(deb_path: Path) -> PackageInfo:
    """Real `ar`/`tar`, same tools (and same one-field-at-a-time RFC822
    continuation-line folding) BakeRamdisk.cpp's own readDebControlInfo()
    uses on the C++ side — not a second, competing archive/format parser,
    just the same technique in Python since this script (matching
    build_deb_cache.py's own house style) has no C++ build step of its
    own to call into."""
    with tempfile.TemporaryDirectory(prefix="blackb0x-resolver-") as tmp:
        subprocess.run(["ar", "x", str(deb_path.resolve())], cwd=tmp, check=True, capture_output=True)
        control_tar = next(Path(tmp).glob("control.tar.*"), None)
        if control_tar is None:
            raise SystemExit(f"{deb_path}: no control.tar.* member found")
        # The control member's own path inside control.tar.* isn't always
        # "./control" -- confirmed directly against this project's real,
        # already-vendored debcache/: 14 org.tihmstar.* .debs store it
        # as plain "control" (no "./" prefix) instead. Listing the archive
        # first and matching either spelling is more robust than assuming
        # one fixed path (BakeRamdisk.cpp's own readDebControlInfo() makes
        # that same fixed-path assumption on the C++ side -- this is a
        # latent, pre-existing gap there too, not something new to this
        # script; out of scope to fix here, but worth knowing about).
        listing = subprocess.run(
            ["tar", "--auto-compress", "-tf", str(control_tar)], cwd=tmp, check=True, capture_output=True, text=True
        ).stdout.splitlines()
        control_member = next((m for m in listing if m in ("./control", "control")), None)
        if control_member is None:
            raise SystemExit(f"{deb_path}: no control member found in {control_tar.name} (listing: {listing})")
        control_text = subprocess.run(
            ["tar", "--auto-compress", "-xOf", str(control_tar), control_member],
            cwd=tmp, check=True, capture_output=True, text=True
        ).stdout

    fields = {}
    field, value = None, ""

    def flush():
        if field is not None:
            fields[field] = value

    for line in control_text.splitlines():
        if line[:1] in (" ", "\t"):
            if field is not None:
                value += " " + line[1:]
            continue
        flush()
        if ":" not in line:
            field = None
            continue
        field, _, value = line.partition(":")
        value = value.strip()
    flush()

    name = fields.get("Package", "").strip()
    if not name:
        raise SystemExit(f"{deb_path}: control file has no Package: field")
    return PackageInfo(
        name=name,
        filename=deb_path.name,
        depends=parse_dependency_groups(fields.get("Depends", "")),
        pre_depends=parse_dependency_groups(fields.get("Pre-Depends", "")),
        provides=[p.strip() for p in fields.get("Provides", "").split(",") if p.strip()],
    )


def build_local_index(debs_dir: Path):
    """Scans every .deb already vendored in debs_dir, returns
    (by_name, by_provides): by_name maps a real Package: name to its
    PackageInfo (last one wins if duplicated — this project's own
    debcache/ has never carried more than one version of the same
    package name at once in practice); by_provides maps a virtual
    package name to the list of real package names that provide it.

    Also declares a synthetic "firmware" package with no deps of its
    own, matching the identical synthetic declaration both
    build_deb_cache.py's own INNER_SCRIPT and BakeRamdisk.cpp's
    kPreinstallInnerScript already make (see either's own comment) — a
    real, confirmed-necessary fix, not a hack: rtadvd's own real Depends:
    is a bare, non-alternative "firmware" with no fallback, and it's
    already vendored in debcache/, so without this every one of its
    own dependents (network-cmds, several others) would falsely fail to
    resolve even though nothing is actually missing."""
    by_name = {"firmware": PackageInfo(name="firmware", filename=None, depends=[], pre_depends=[], provides=[])}
    by_provides = {}
    for deb_path in sorted(debs_dir.glob("*.deb")):
        info = read_deb_control_info(deb_path)
        by_name[info.name] = info
        for virtual in info.provides:
            by_provides.setdefault(virtual, []).append(info.name)
    return by_name, by_provides


def resolve(top_level_names, by_name, by_provides, known_unresolvable, local_only_filenames, local_only_package_names):
    """Transitive closure walk. Returns (picklist_filenames,
    resolved_top_level_names, unresolved_top_level_names).

    local_only_filenames vs. local_only_package_names: local_only_debs.txt
    itself lists .deb *filenames*, but packages.txt lists real package
    *names* — the same distinction build_deb_cache.py's own main() makes
    (local_only_filenames/local_only_package_names there) — so both are
    needed here: names to know which packages.txt entries to skip
    ordinary resolution for, filenames to actually include their bytes.
    """
    picklist = set()
    resolved_top_level = set()
    unresolved_top_level = set()

    def resolve_name(name):
        """Returns the real package name satisfying `name` (itself, or a
        Provides: candidate), or None."""
        if name in by_name:
            return name
        candidates = by_provides.get(name)
        return candidates[0] if candidates else None

    visited = set()

    def visit(name) -> bool:
        if name in visited:
            return name in by_name or name in by_provides
        visited.add(name)
        real_name = resolve_name(name)
        if real_name is None:
            return False
        info = by_name[real_name]
        if info.filename:
            picklist.add(info.filename)
        ok = True
        for group in (info.pre_depends, info.depends):
            for alts in group:
                if not any(visit(alt) for alt in alts):
                    ok = False
        return ok

    for name in top_level_names:
        if name in known_unresolvable or name in local_only_package_names:
            continue
        if visit(name):
            resolved_top_level.add(name)
        else:
            unresolved_top_level.add(name)

    # local_only_debs.txt entries: included directly by filename (their
    # bytes are already vendored, matching build_deb_cache.py's own
    # treatment), not through the ordinary by-name resolution walk above
    # — see that file's own comment for why apt would never find them by
    # name either.
    for filename in local_only_filenames:
        deb_path = DEBS_DIR / filename
        if not deb_path.exists():
            raise SystemExit(f"local_only_debs.txt entry {filename} not found in {DEBS_DIR}")
        picklist.add(filename)

    return picklist, resolved_top_level, unresolved_top_level


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output-dir", required=True, type=Path)
    args = ap.parse_args()

    output_dir = args.output_dir.resolve()
    if not output_dir.is_dir():
        raise SystemExit(f"--output-dir {output_dir} does not exist or is not a directory")

    print(
        "build_deb_cache_experimental_no_container.py: EXPERIMENTAL, no-container picklist generation -- "
        "see this script's own module docstring for its real, documented gaps versus build_deb_cache.py.",
        file=sys.stderr,
    )

    top_level_names = read_lines(PACKAGES_LIST)
    local_only_filenames = set(read_lines(LOCAL_ONLY_LIST))
    local_only_package_names = set()
    for filename in local_only_filenames:
        deb_path = DEBS_DIR / filename
        if not deb_path.exists():
            raise SystemExit(f"{LOCAL_ONLY_LIST} lists {filename}, but {deb_path} does not exist")
        local_only_package_names.add(read_deb_control_info(deb_path).name)

    by_name, by_provides = build_local_index(DEBS_DIR)
    picklist, resolved_top_level, unresolved_top_level = resolve(
        top_level_names, by_name, by_provides, KNOWN_EXPECTED_UNRESOLVABLE,
        local_only_filenames, local_only_package_names,
    )

    if unresolved_top_level:
        for name in sorted(unresolved_top_level):
            print(f"build_deb_cache_experimental_no_container.py: FATAL: {name} did not resolve against "
                  f"the locally-vendored debcache/ set", file=sys.stderr)
        raise SystemExit(
            "one or more packages.txt entries did not resolve -- grow debcache/ on a real Linux+podman "
            "machine via build_deb_cache.py first, then retry here"
        )

    (output_dir / "picklist.txt").write_text("\n".join(sorted(picklist)) + "\n")
    (output_dir / "resolved_packages.txt").write_text("\n".join(sorted(resolved_top_level)) + "\n")
    (output_dir / "apt-lists").mkdir(exist_ok=True)

    print(
        f"build_deb_cache_experimental_no_container.py: resolved {len(resolved_top_level)} top-level packages "
        f"to {len(picklist)} .deb files",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
