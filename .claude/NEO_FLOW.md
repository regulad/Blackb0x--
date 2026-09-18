# NEO_FLOW.md — the current install-time design

Supersedes everything described in `.claude/LEGACY_FLOW.md`, which is kept
purely as historical record of the pre-rewrite Objective-C tool's actual
behavior — nothing in it describes what runs today. This file used to be a
design doc for work not yet implemented; that work is done, has been
validated end-to-end against real hardware-adjacent artifacts (real bakes,
real `fsck.hfsplus`, a real downloaded Cydia package), and has moved past
its own original plan in several places. This is the as-built design, not
a proposal — where the two disagree, the code is right and this file was
wrong until now.

Only one of the six original source tarballs `Blackb0x/ramdisk/` was ever
extracted from is still kept packed, directly under `Blackb0x/Misc/`:
`tihmstar-untether.tar` (no independently re-downloadable copy exists
anywhere). The other five were dropped outright, and `RamdiskBins.tar`'s
own extracted contents have themselves mostly been deleted since (real
installed packages or Apple's own base OS cover what they provided) — see
`Blackb0x/Misc/README.md` for the full per-file provenance, including a
few genuine dead ends investigated at length (`Misc/dirhelper`'s origin
could not be traced past "someone compiled this, adapting a well-known
technique, and never published it").

## Why (unchanged from the original plan)

Two structural problems with the legacy flow (see `LEGACY_FLOW.md`'s
closing section for the full reasoning):

1. `postinstall.sh` hit the live network on every single real-OS boot,
   forever (`apt-get update`/`upgrade`/`install` several times over), with
   no offline fallback.
2. The base Cydia filesystem was a flat, pre-extracted overlay with no
   corresponding `dpkg` database entries — `dpkg` never knew those files
   existed even though they were present on disk.

The fix that shipped: a real, `dpkg`/`apt`-driven package install,
resolved once at bake time and run once per device at first real boot —
but **not** a fully network-disabled install. That specific piece of the
original plan turned out to be impossible (see below) and was replaced
with an opportunistic-network design instead.

## What ships in `/blackb0x` (the ramdisk overlay), and how it gets there

Nothing gets copied into `/blackb0x` by hand at runtime anymore — the
entire tree is assembled once, at bake time, by
`BakeRamdisk.cpp`'s `stageBlackb0xTree()`, then blindly replicated onto
the real device by `entrypoint.c`'s `merge_tree()` at boot. The pipeline,
in the order it actually runs:

1. **Resolve.** `scripts/build_deb_cache.py` reads `Blackb0x/Misc/packages.txt`
   (a flat list of real package *names*, not `.deb` filenames) and resolves
   it against this project's five real configured repos
   (`Blackb0x/Misc/apt/{regulad,saurik,bigboss,awkwardtv,xbmc}.list` — each
   GPG-verified directly with `gpgv` where a key exists, since modern
   apt's own weak-digest hardening rejects several of these repos' real,
   legitimate, merely-old 2008-era DSA-1024/SHA-1 signatures) inside a
   throwaway `debian:bookworm-slim` container. A resolution failure is now
   **fatal** for the whole run, with exactly one documented exception:
   `com.ih8sn0w-squiffy-winocm.p0sixspwn`, whose own `Depends: firmware`
   range genuinely doesn't match this sandbox's synthetic firmware
   version (a real, expected mismatch, not a dead package name — see
   `KNOWN_EXPECTED_UNRESOLVABLE` in the script). Everything else that
   fails to resolve used to just print a warning and get silently
   skipped, which is exactly how a real gap (`com.ericasadun.utilities`
   providing `plutil` on BigBoss; the dead-upstream `essential` package)
   went unnoticed for a while.
2. **Local-only fallback.** A small number of packages will never resolve
   through *any* live repo, but this project already has a real,
   previously-recovered `.deb` for them (`essential`, currently the only
   entry). `Blackb0x/Misc/local_only_debs.txt` names these by filename;
   `build_deb_cache.py` skips the normal resolution attempt for them
   entirely, generates a real `dpkg-scanpackages` index for them
   (`local-repo/`), and both (a) folds their filename straight into
   `picklist.txt` so their bytes get cached like anything else, and (b)
   adds their package name to `resolved_packages.txt` so `postinstall.sh`
   installs them by name too — through `Blackb0x/Misc/apt/local.list`
   (`deb file:///var/.blackb0x/local-debs ./`, staged on-device by
   `BakeRamdisk.cpp` at the exact same path `local-repo/` lands at), never
   through any of this project's own bake-time force-install machinery.
3. **Decide what gets force-installed at bake time vs. left for real apt.**
   `BakeRamdisk.cpp`'s `computePreinstallEligibleFilenames()` walks
   `picklist.txt`'s full resolved closure against
   `Blackb0x/Misc/prebake_package_blacklist.txt` — packages with a real,
   stateful, uninspectable, or device-only postinst/preinst (`cydia`,
   `firmware-sbin`, `rtadvd`, `pam`, `pam-modules`, `essential` for an
   unmet-Depends reason of its own, the exploit-specific packages) are
   excluded, and so is anything that transitively depends on an excluded
   package — propagated to a fixpoint, not just one level. Anything
   eligible that still happens to have a postinst gets it stripped
   between unpack and configure (safe: postinst only ever runs at
   `--configure` time); anything eligible with a preinst gets its `.deb`
   rebuilt with the preinst member removed before ever being unpacked
   (not safe to strip after the fact, since preinst runs *during*
   `--unpack`) — both decided dynamically per real `.deb` content, never
   hardcoded, confirmed case-by-case by real disassembly where a compiled
   binary preinst was involved (`ncurses`'s turned out to be a trivial,
   platform-independent symlink migration).
4. **Actually install the eligible set, for real.** Inside a
   `debian:stretch` container (shipping an era-appropriate `dpkg`
   1.18.26), every eligible `.deb` gets force-unpacked
   (`--force-architecture --force-depends --unpack`, ignoring dependency
   ordering — this ecosystem has a genuine, unbreakable circular
   Pre-Depends chain, `dpkg → tar → gzip/lzma → sed → dpkg`), then a
   single `dpkg --force-depends --configure -a` pass lets dpkg's own
   internal solver break the cycle — the real debootstrap two-phase
   pattern. A synthetic `Package: firmware` stanza (matching a real
   device's own Cydia-injected entry) is declared in this sandbox's own
   `/var/lib/dpkg/status` first, satisfying any package's
   `Depends: firmware (>= X)` the same way a real device would — this
   needed its own `.list`/`.md5sums` stub files too, since `dpkg --audit`
   expects those for anything claiming to be installed. `dpkg --audit`
   and `apt-get check` both have to come back clean or the whole bake
   fails. The resulting `dpkg` status + `info/` become this run's real
   on-device dpkg database for the eligible set; everything else's `.deb`
   bytes land in the real on-device apt cache
   (`private/var/cache/apt/archives/`) untouched, for `postinstall.sh`'s
   own `apt-get install` to pick up later.
5. **Cache all of this once per process, not once per firmware.**
   `computeGlobalDebcacheOnce()` runs steps 1–4 exactly once per
   `bake-firmware` invocation (a static-local cache) — this whole
   pipeline is firmware-independent, and re-running it per target used to
   mean repeated identical network fetches and container work for no
   reason.
6. **Assemble `/blackb0x` for this specific firmware.** `stageBlackb0xTree()`
   stages, per firmware target: the apt sources.list.d/trusted.gpg.d
   entries for all five real repos plus `local.list`; the real, bake-time
   apt-get-update lists cache (`stageAptListsCache()`, so on-device apt
   already knows what every repo offered even with zero network at
   install time); the non-preinstalled `.deb` bytes and preinstalled
   dpkg state from the cached debcache result; `postinstall.sh` with its
   one placeholder (`__BLACKB0X_PACKAGES__`) templated in from the real,
   apt-resolved package name set (minus `cydia`, which installs
   separately first); and exactly one of three per-firmware persistence
   payloads, picked by this firmware's own `ProductVersion` (see below).
7. **Build the entrypoint binary once.** `BakeFirmware.cpp`'s `main()`
   calls `buildEntrypointBinary()` once, before its per-firmware loop
   (same pattern as the debcache cache), and passes the same built path
   into every `bakeRamdisk()` call.

## Persistence payloads — unchanged in kind, one location moved

Picked by `stageVersionBranch()` off this firmware's real `ProductVersion`,
exactly as before:

- **8.4.x** → `stageEtasonatv()`: `daemonload`/`orphan_commander`/
  `untether.bin`/`expl.js` from `tihmstar-untether.tar`, plus a symlink of
  Apple's own signed `jsc` over `/usr/libexec/rtbuddyd` and
  `/untether/expl.js` over `/--early-boot` — the only branch with a real
  untether.
- **7.x / 8.x (non-8.4)** → `stageIos7Tether()`: swaps a custom binary
  into `/usr/libexec/dirhelper`, genuinely tethered, no untether exists
  for this range at all. This file moved from `Misc/bin/dirhelper` to
  `Misc/dirhelper` directly this pass — it was never a utility, just
  packaged alongside some by an old tarball. See `Misc/README.md`'s own
  dedicated section for the full (dead-end) provenance investigation:
  the technique traces cleanly to evasi0n6's own real, published
  `dirhelper`-hijack mechanism (an exact matching `remount()` call,
  confirmed directly against evasi0n6's real archived source), but the
  compiled binary itself was never open-sourced by anyone, including by
  evasi0n6's own team.
- **6.1.4** → `stageP0sixspwn()`: p0sixspwn's own untether payload,
  extracted directly from the real vendored `.deb`.
- Anything else logs a warning and stages the common content only — no
  hard failure, since a firmware this project doesn't have a persistence
  answer for yet still gets everything else correctly.

## `entrypoint.c` — replaces `/sbin/launchd` outright, not `/etc/rc.boot`

This is the one point where this project's own mid-rewrite exploration
went somewhere and came back. Real disassembly of an AppleTV2,1 10B809
`RestoreRamdisk` showed `/etc/rc.boot` is itself `LC_MAIN`-entered
directly by the kernel on that firmware, so the splice target was moved
there from `/sbin/launchd` for a while, on the theory that injecting at
the true first entry point is strictly better. That didn't generalize: a
real bake against AppleTV3,1/AppleTV3,2 12H606 failed because that
firmware's ramdisk has no `/etc/rc.boot` at all (confirmed by mounting it
directly — `/etc/` is nearly empty there). Reverted to always targeting
`/sbin/launchd`, the one thing guaranteed to exist and be real PID-1
across every known firmware generation — see `docs/HISTORY.md`'s
"Entrypoint injection point" entry for the full account. The ad-hoc
signing identity changed to match: `com.apple.launchd`.

`do_install()` (`entrypoint/entrypoint.c`) is now fully unconditional
apart from one safety guard:

```c
if (sys_access("/mnt1/Applications/AppleTV.app/AppleTV", F_OK) != 0) {
    console_print("Not an AppleTV...\n");
    return 0;
}
if (sys_access("/mnt1/var/.blackb0x/install-done", F_OK) == 0) {
    panic("/var/.blackb0x/install-done already exists — refusing to re-run (would clobber live dpkg state)\n");
}
log_to_file("Merging blackb0x payload\n");
merge_tree("/blackb0x", "/mnt1");
log_to_file("Finished install\n");
```

No version branching happens on-device at all anymore — that decision was
already made at bake time (step 6 above), so `entrypoint.c` doesn't need
to know what firmware it's running on. `merge_tree()` is a generic
recursive directory merge (real `stat()`-read owner/mode, symlinks
recreated verbatim, existing destination directories left untouched and
only recursed into) — this is what replaced the old, removed
`create_cydia_directories()`/`MYSTERY_DIR_MODE`/`clone_directory()`
machinery entirely; every directory `/blackb0x` needs already exists as a
plain entry in it, correctly staged, with nothing extra to special-case.

`panic()` deliberately never returns and never reboots — an automatic
reboot on a genuine failure would just re-run the same ramdisk into the
same panic every cycle, with nothing to show a human debugging over
console/serial that anything is wrong.

The mount target is `/mnt1`, not `/mnt` — confirmed directly against a
real pristine ramdisk (`/mnt1`/`/mnt2` both exist as real, pre-existing
empty mountpoints; `/mnt` never did).

`set_auto_boot()` runs the real, pristine ramdisk's own `/usr/sbin/nvram`
(`auto-boot=1`) right before every `reboot(2)` call — SecureROM/iBoot
apparently clears this NVRAM variable once a real DFU payload has
executed, which the original ssh-rd-derived `rc.boot` this project once
looked at also worked around; without it, a plain `reboot(2)` risks
leaving the device sitting at the iBoot/DFU prompt instead of continuing
into the real, already-installed OS.

## `postinstall.sh` — one-shot, network-opportunistic (not network-hard-disabled)

Runs once the device boots into its real OS
(`xyz.regulad.blackb0x.postinstall.plist`, `RunAtLoad`, root — renamed
from `com.blackb0x.postinstall` so the file's own name matches its
`Label`, and `StandardOutPath`/`StandardErrorPath` point at two separate
log files, `postinstall.out.log`/`postinstall.err.log`, since pointing
both at the same path is a real, long-documented launchd bug).

**The original plan called for hard-disabling the network for the whole
install window. That turned out to be impossible in practice** — Kodi and
a few other real packages are simply too big to fit in this old A4-era
restore ramdisk's ~70MB budget (a rule of thumb, not a hard protocol
limit), so they're deliberately never staged locally at all
(`kNeverStageDebs`). The design that actually shipped uses the network
opportunistically instead of disabling it:

- The real `regulad`/`saurik`/`bigboss`/`awkwardtv`/`xbmc` sources.list.d
  entries are always present. `apt-get update` is free to actually reach
  them over the network if it can — this is the **one and only** command
  in the whole script allowed to fail (`|| true`); everything else runs
  under a bare `set -ex`, so a real failure anywhere else stops the
  script immediately, `install-done` never gets written, and the next
  boot (this LaunchDaemon is `RunAtLoad`) tries the whole install again
  from scratch.
- If the network genuinely isn't reachable, apt still has something to
  work with: the real bake-time `apt-get update` cache is already staged
  at `/private/var/lib/apt/lists/`, so apt already knows what every
  configured repo offered as of bake time.
- Whatever `.deb` bytes did fit locally sit directly in apt's own real
  cache directory (`/private/var/cache/apt/archives/`) — apt finds them
  itself via its normal cache-before-download check, no local `file://`
  source or synthetic index needed for the *main* debcache (only
  `local_only_debs.txt`'s handful of dead-upstream packages need that,
  via `local.list`, described above).

Real sequence:

1. Exit immediately if `/var/.blackb0x/install-done` already
   exists (real signal, a UTC timestamp — not just an empty touch, and
   not the pre-rewrite tool's old `/var/mobile/Media/.blackb0x` marker).
2. Wait at least 60 real seconds since boot before touching `apt-get` —
   this `RunAtLoad` job fires before networking has necessarily
   associated, and there's no portable one-time-delay knob on this old
   launchd. `sleep` itself isn't guaranteed present on a stock retail OS
   (real evidence: `dpkg`'s own control file lists `bash` as a
   `Depends:`, meaning this ecosystem treats even a shell as something it
   has to supply) — if missing, `coreutils-bin`'s cached `.deb` gets
   `dpkg -i`'d directly with no network involved. In practice this branch
   is usually moot: `coreutils-bin` has no postinst, so it's normally
   already bake-time-preinstalled by the mechanism above.
3. `apt-get update || true`.
4. `apt-get install -y --allow-unauthenticated cydia` — on its own,
   first, since its real postinst does its own first-run `/var/stash`
   relocation and nothing else should assume that environment exists yet.
5. `apt-get install -y --allow-unauthenticated "${PACKAGES[@]}"` — the
   exact, real, apt-resolved package name set `stagePostinstallScript()`
   baked in (minus `cydia`), not hand-copied from `packages.txt` (which
   still carries a few permanently-dead sources.list.d-basename leaks —
   `bigboss`/`modmyifone`/`saurik`/`zodttd` — that would abort this line
   outright if ever passed to apt directly).
6. `apt-get install -f -y --allow-unauthenticated`, then plain
   `apt-get upgrade -y --allow-unauthenticated` and
   `apt-get autoremove -y --allow-unauthenticated` — plain `upgrade`, not
   `dist-upgrade`/`full-upgrade`: this on-device apt (`apt7 0.7.25.3`) has
   no unified `apt` command and no `full-upgrade` at all (both later
   additions), and while `dist-upgrade` genuinely does exist in it, plain
   `upgrade` only touches packages already installed, never demanding
   more from the network than what's already staged.
7. Write `/var/.blackb0x/install-done` as the last thing the
   script does.

## Ramdisk sizing — not addressed in the original plan at all

The original plan never mentioned how the destination volume gets sized.
The real problem turned out to be non-trivial: `/blackb0x` doesn't fit in
the pristine ramdisk's own free space, and growing an existing HFS+
volume in place is fundamentally broken on Linux (every tool tried —
xpwn's own `grow_hfs()`, `libhfsp`, the Linux kernel driver's own resize
path — has a real, confirmed-by-testing bug in exactly this operation).
The shipped design instead assembles the *real* final content (original
ramdisk + spliced `launchd` + `/blackb0x`) onto a generously oversized,
throwaway scratch HFS+ volume, measures that mounted volume's own real
disk usage (`du` against a live HFS+ mount reports real block-rounded
usage, unlike estimating from a plain host directory, which undercounted
by several MB in practice), then creates the actual shipped volume sized
from that real number plus a margin, and does one `cp -a` from scratch to
final. Both volumes are created case-sensitive (`mkfs.hfsplus -s`) —
matching the real iOS/tvOS root filesystem (HFSX) — after a real bake
failure showed ncurses' own terminfo tree needs genuinely distinct
case-varying sibling directories (`e`/`E`, `a`/`A`) that a case-insensitive
volume can't hold. The 70MB final-size check is a warning now, not a hard
failure — a rule of thumb worth a loud flag, not a real protocol ceiling.

## Confirmed (not just theorized) against the real on-device apt

- **A `.deb` sitting in `var/cache/apt/archives/` with no matching
  `Packages` index entry is invisible to apt's dependency resolver** —
  verified with a real minimal repro. This is exactly why
  `local_only_debs.txt`'s dead-upstream packages need a real
  `dpkg-scanpackages` index (`local.list`) even though the *main*
  debcache doesn't (it relies on the real repos' own staged lists cache
  instead).
- **`file://` sources, `--allow-unauthenticated`, and the complete
  absence of modern apt's weak-hash/weak-digest hardening** — confirmed
  directly from `apt7`/`apt7-lib`'s real shipped binaries.
- **The real `cydia` package (1.1.30, downloaded and inspected directly
  from `apt.saurik.com`) ships its own `/usr/libexec/cydia/startup` +
  `firmware.sh` + a real `com.saurik.Cydia.Startup.plist` LaunchDaemon
  (`RunAtLoad`)** — the synthetic-`firmware`-package declaration, GSC
  capability-bit stanzas, `Media/Cydia/AutoInstall` handling, and
  `uicache` refresh all happen automatically once `apt-get install cydia`
  actually runs through real `dpkg`, with zero need to replicate any of
  it in `postinstall.sh`. The old, pre-rewrite tool had to reimplement
  this manually because its own install method (a flat file copy) never
  ran real `dpkg`/postinst in the first place, so Cydia's own
  LaunchDaemon never got properly registered.
