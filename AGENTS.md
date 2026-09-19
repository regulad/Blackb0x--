# AGENTS.md — Blackb0x portable port

Short and operational: what an agent needs to move around this repo and build it
correctly, right now. For the *why* — every bug hunt, dead end, and decision's
evidence — see **[`docs/HISTORY.md`](docs/HISTORY.md)**. If something here seems to
need more justification than it gives, it's almost certainly explained there.

## What Blackb0x is

A jailbreak tool for 2nd/3rd-gen Apple TV (A1378/A1427/A1469) via the checkm8/SHAtter
DFU-mode boot exploit, which then side-loads Cydia + Kodi. A portable CLI port of an
original macOS Cocoa/Objective-C app (fully ported and deleted — see
`docs/HISTORY.md` or git history if you need to see what it looked like).

## Conventions (don't re-litigate without asking)

- **CLI-only, macOS-only.** No GUI — dropped, not dual-maintained. Linux support was
  removed outright (checkm8 never worked there; see "Current status" below and
  `docs/HISTORY.md`), and `CMakeLists.txt` hard-fails off Apple. Apple Silicon is the
  tested host.
- **Every third-party dependency is a git submodule under `third_party/`, built from
  source, statically linked.** Not FetchContent, not system packages, no exceptions —
  see the "vendored dependencies" table below and `CMakeLists.txt`'s
  `ExternalProject_Add` blocks. `zlib` used to be the one system-provided exception;
  it no longer is.
- **The only dynamic dependencies in the final binary are libc/libstdc++/libm/
  libgcc_s.** Verify with `ldd build/blackb0x` after any dependency change — it should
  never grow beyond those five lines.
- **If a vendored library needs a code change, fork it to its own branch — never leave
  an uncommitted local patch sitting in a submodule's working tree.** Push the fork,
  point `.gitmodules`'s `url`/`branch` at it, keep the diff against the pinned base
  small and clean (`git reset --soft` + re-commit + `git push --force` on that branch
  if it needs cleaning up, not a growing pile of fixup commits). See "Vendored
  dependencies" below for which submodules are already forks and why.
- **Prefer adapting an existing, battle-tested library over hand-rolling the
  equivalent.** This has been the deciding factor twice already: a from-scratch
  lockdownd/AFC client was fully verified working, then deleted in favor of forking
  real `libimobiledevice` once it became clear that was strictly less code to
  maintain. Default to this; don't reach for a rewrite first. (The counter-example
  is worth knowing: the hand-ported `checkm8` USB exploit sequence was once replaced
  with a shell-out to `gaster`, on exactly this reasoning — and `gaster` was never
  made to work against an AppleTV3,2 on either Linux 7.1.x or macOS 26, so the
  hand-port came back as `blackb0x-pwn` and gaster is gone. "Someone else maintains
  it" is not the same as "it works on this hardware.")
- **Terse, single-source CLI output.** Every user-facing message flows through
  `Cli.cpp`'s rendering of `DeviceManager`'s `DeviceEventSink` callbacks
  (`onStatus`/`onProgress`/`onDeviceAdded`/`onDeviceUpdated`/`onDeviceRemoved`) in one
  consistent, unbracketed style — not raw `printf()` scattered through
  `DeviceManager.cpp`/`Patcher.cpp`. Real errors go to `stderr`. Don't reintroduce
  debug-dump printfs (hex keys/IVs, raw status codes) outside of an explicit verbosity
  flag.
- **Version-pin every historically-relevant vendored library to what the original app
  actually used**, not "whatever's current" — already done for every submodule that
  existed in the original app; don't re-point one of those at a different commit
  without redoing the OSINT (see `docs/HISTORY.md`'s "Version pinning" section for the
  method, if a pin ever needs revisiting).

## Repo layout

- `src/` — first-party code, flat, and nothing else: `main.cpp`, `Cli.hpp`/`.cpp`,
  `Console.hpp`/`.cpp`, `DeviceManager.hpp`/`.cpp`, `IPSW.hpp`/`.cpp`,
  `IPSWDownloader.hpp`/`.cpp`, `Patcher.hpp`/`.cpp`, `Personalize.hpp`/`.cpp`,
  `ResourcePath.hpp`/`.cpp`, `BakeRamdisk.hpp`/`.cpp`, `BakeFirmware.cpp`.
  The original Objective-C (`AppDelegate`, `MainView`,
  `Blackb0x.h`/`.m`, `TaskManager`, and the old `.h`/`.m`/`.mm` counterparts of the
  files above) has been fully ported and deleted — check `docs/HISTORY.md`/git
  history if you need to see what it looked like. `checkm8.h`/`SHAtter.h` are exploit
  payload byte arrays, `#include`d directly by `DeviceManager.cpp` — not leftover
  Cocoa, keep these.
- `src/Pwn/` — the standalone `blackb0x-pwn` exploit binary's own C sources
  (`main.c`, `Checkm8Pwn.c`), separate from the main CLI.
- `src/libraries/` — already-portable C kept in-tree and built directly by the
  root `CMakeLists.txt`: `xpwntool.c` (compiled against `third_party/xpwn`'s own
  headers — its private `libxpwntool/` header copies are deleted),
  `idevicerestore_img3.c`, `libplist_compat.c`.
  Both GPL patchers have moved OUT of here to their own submodules, built as
  separate executables and fork/exec'd rather than linked (see the table below):
  `CBPatcher` and `iBoot32Patcher`. `libbootkit/` and `libprerestore.h` used to
  sit here as dead code, linked into nothing and referenced by nothing; both are
  deleted.
- `src/tests/` — `RamdiskOverlayTests.cpp`, the one test target.
- `package/` — the `xyz.regulad.blackb0x` Debian package: `build.sh` (builds the
  `.deb` with Theos' `dm.pl` inside a container), `layout/` (its literal on-device
  file tree plus `DEBIAN/control` and `DEBIAN/postinst`), `packages.txt` (the apt
  package list templated into `postinstall.sh` at package-build time), and
  `local_only_debs.txt` (the packages bundled as a local apt repo at
  `/var/.blackb0x/local-debs`, for anything no live repo serves). The package
  needs no state beyond this tree — `bakeRamdisk()` builds it and installs the
  resulting `.deb` rather than hand-staging its contents.
- `debcache/` — the checked-in `.deb` cache (107 packages, ~58MB), at the repo
  root because it is a build input in its own right, not Blackb0x app data.
  The bake consumes it for both the bundled local repo and the apt cache staged
  onto the ramdisk; real package archives, not loose files mirroring a
  destination path. **Generated on Linux, consumed everywhere.**
  `scripts/build_deb_cache.py` grows it with a real apt-get dependency solve
  inside podman and hard-refuses to run off Linux; a GitHub Action is the
  intended way to run it. No bake ever calls it — every bake reads the
  committed bytes, resolving the closure over them with
  `scripts/build_deb_cache_experimental_no_container.py`, which only reads.
  It used to be `Blackb0x/Debs/`.
- `dist/` — bake-firmware's output (gitignored, not checked in): one
  `<device>_<buildID>-Ramdisk.dmg` per known firmware, plus
  `bootchain/<device>_<buildID>/` holding that firmware's patched
  iBSS/iBEC/kernelcache/devicetree. An entry that already
  exists is skipped; pass `--force` to rebuild. There is no staleness
  detection: a `.sum` sidecar holding a content hash of the bake inputs used
  to force re-bakes automatically, and it was removed as fragile, so **after
  changing anything that affects baked output, pass `--force`**.
- `keys/` — per-firmware IPSW decryption `.keys` files, read locally by
  `IPSW.cpp` only; never shipped to the device.
- `misc/` — bake-time assets that are neither code nor a package, four
  consumers total: `untether.bin` and `dirhelper` (staged onto the device),
  `apt/net.tihmstar.gpg` (staged as an on-device apt keyring),
  `prebake_package_blacklist.txt` (read by the preinstall-eligibility pass),
  and `firmware_versions.txt` (the `(device, buildID)` -> ProductVersion
  binding, regenerated by `scripts/generate_firmware_versions.py`).
  `apt/*.gpg.key` are the armored copies `scripts/build_deb_cache.py` feeds
  to gpgv. The `.list` files that used to live here are in `package/layout/`
  now. See `misc/README.md` — at 51KB it is the reverse-engineering writeup,
  not a directory index.
- `third_party/` — every vendored dependency (see table below).
- `docs/HISTORY.md` — the full debugging/decision log.

## Vendored dependencies

All built from source via `ExternalProject_Add`/`add_subdirectory` in `CMakeLists.txt`,
statically linked. **Forked** means: patched on our own branch, pushed, pointed to from
`.gitmodules` — not a local working-tree diff.

| Submodule | Source | Forked? |
|---|---|---|
| `libplist`, `libusbmuxd` | libimobiledevice/* | No — historically pinned |
| `libimobiledevice` | **regulad/libimobiledevice**@`legacy` | Yes — additive, `--with-ssl-implementation=wolfssl`-selectable SSL backend for `idevice.c` (real OpenSSL/GnuTLS untouched, still selectable) |
| `libirecovery` | **regulad/libirecovery**@`libusb-async-cancel-fix`, off synackuk/libirecovery | Yes — real stderr diagnostics on `irecv_send_buffer()`'s upload-failure paths, which previously returned `IRECV_E_USB_UPLOAD` silently unless built with `debug()` on; plus `irecv_usb_control_transfer_ex()`, which keeps the partial byte count on a stall/timeout that both ordinary wrappers discard. Built `--with-iokit` only. The branch also carries libusb-path fixes (a use-after-free, a double free and a non-terminating completion wait in `irecv_async_usb_control_transfer_with_cancel()`) that this project no longer compiles — the branch name is a leftover from when it did |
| `libimobiledevice-glue`, `libplist-modern` | libimobiledevice/* | No — current HEAD, not historically pinned (see HISTORY for why two `libplist`s) |
| `libfragmentzip` | **regulad/libfragmentzip**@`fix-cxx-stdbool-header` | Yes — one header fix (C++/`<stdbool.h>` collision) |
| `libgeneral` | tihmstar/libgeneral | No |
| `xpwn` | **regulad/xpwn**@`legacy` | Yes — a wolfSSL AES-CBC buffer over-read fix in `img3.c`, plus disabling the legacy-libusb-0.1-only `pwnmetheus2` subdirectory |
| `wolfssl`, `curl`, `libzip`, `libpng`, `bzip2`, `zlib` | upstream | No — current HEAD or latest stable tag; none of these existed in the original app |
| `CBPatcher` | zzanehip/CBPatcher (upstream, pinned) | No — plain upstream. **Built as a separate EXECUTABLE and fork/exec'd, never linked**, same GPL-3.0 reason as `iBoot32Patcher` below: it was a static library on blackb0x's own link line until that was noticed, which the in-tree copy's missing LICENSE file helped hide. No fork needed — upstream already ships a `main()` whose CLI (`<infile> <outfile> <version> [--nosb]`, nukesb defaulting to 1) is a drop-in for the `patch_kernel()` call this used to link. The old local delta (an `#ifdef __APPLE__` around CBPatch.c's Apple-only Mach-O includes, plus `portable_macho.h` standing in for them) existed only to build on Linux and died with Linux support |
| `iBoot32Patcher` | **regulad/iBoot32Patcher**@`blackb0x`, off zzanehip/iBoot32Patcher | Yes — two real bug fixes: `patch_kaslr()` fell off the end of a non-void function on every *successful* branch (garbage return read non-zero on x86_64, 0 on arm64, so a real macOS run treated a successful KASLR patch as a hard failure), and `iBootPatcher()` tested its `RSA` argument twice so the `debug` argument was dead and `patch_debug_enabled()` ran whenever the RSA patch was asked for. **Built as a separate EXECUTABLE and fork/exec'd, never linked** — it is GPL-3.0-or-later and blackb0x declares no license, so linking would make blackb0x a GPLv3 derivative. Do not "simplify" it back into a static library |

`src/libraries/xpwntool.c` (in-tree, not a submodule) is sourced from
`zzanehip/xpwntool-swift`, and compiles against `third_party/xpwn`'s headers
rather than the private copies it shipped with, with one local fix: `decrypt()`'s three error paths
(`cannot open infile` / `cannot open outfile` / `cannot duplicate file from provided
template`) each printed the diagnostic and then fell through to dereference the NULL
they had just reported, so any one of them was a SIGSEGV rather than a failure. They
bail out now. That is the root cause behind the "`decrypt()` a nonexistent file
corrupts the heap" hazard `Patcher.cpp` documents in several places; `decrypt()` is
still `void`, so callers detect failure by checking for a zero-byte output.

## Build & run

```
cmake -S . -B build && cmake --build build -j$(nproc)

# Once, in bulk, for every known firmware — NOT run by blackb0x itself.
# Writes dist/<device>_<buildID>-Ramdisk.dmg and dist/bootchain/<device>_<buildID>/:
./build/bake-firmware [--signed-only] [--device <model>] [--build <buildID>]
                      [--only bootchain|ramdisk] [--force] [--stop-early]

./build/blackb0x [--ecid <id> | --udid <id>] [--dry-run]
```

Two binaries (three counting `blackb0x-pwn`, which `blackb0x` spawns itself).
Nothing needs root any more: the privilege split existed for Linux's loop-mount and
raw-USB device nodes, and neither applies on macOS.

- **`bake-firmware`** is the single ahead-of-time baker. It was two binaries,
  `bake-all-ramdisks` and `bake-all-bootloaders`, split because the Linux ramdisk
  bake needed `CAP_SYS_ADMIN` for its loop mount while the bootchain half needed no
  root at all; with the loop mount gone that reason went with it, and the duplicated
  target-enumeration and filter code collapsed into one file. For every `.keys` file
  under `keys/` (i.e. every known device/firmware combination) it does
  two things:
  - **bootchain** — downloads and patches iBSS, iBEC, KernelCache and DeviceTree into
    `dist/bootchain/<device>_<buildID>/`. This is the only way to exercise
    `Patcher::patchiBSS()`/`patchiBEC()`/`patchKernel()` across every known firmware
    with no hardware attached. Each target runs in a forked child, deliberately: the
    vendored patch code does not survive being driven dozens of times in one process
    (see `BakeFirmware.cpp`'s own comment).
  - **ramdisk** — attaches an HFS+ image via `hdiutil` (no root, no mount helper),
    merges the overlay (the `xyz.regulad.blackb0x` package plus the debcache) into
    that firmware's `RestoreRamDisk`, and writes
    `dist/<device>_<buildID>-Ramdisk.dmg`. **This path has never been run on real
    macOS** — written with no Mac available; see `BakeRamdisk.cpp`'s caveat and
    `.claude/TODO.md` item 4a. Stays in-process, unlike the bootchain half, because
    the debcache and the entrypoint cross-compile are resolved once and shared across
    every target.

  `--signed-only` restricts the run to builds ipsw.me currently reports Apple as still
  signing (a small fraction of the total — what most real devices are actually on);
  `--device`/`--build` narrow it further. `--only bootchain` skips the entrypoint
  cross-compile and the debcache entirely, which is the fast loop for iterating on
  patch logic.

  The overlay is fully static (no per-device secrets get baked in), so each patched
  output is valid for every device on that firmware; there's no reason to re-derive
  it on every `blackb0x` run. `blackb0x` never invokes the bootchain half at all (it
  still patches those four components live — `.claude/TODO.md` item 5), and for the
  ramdisk `Patcher::patchRamdisk()` just checks whether the `dist/` entry it needs
  already exists. If it doesn't, `Cli.cpp` spawns `bake-firmware --only ramdisk` for
  that one tuple in the background. Re-running is cheap: any output that already
  exists is skipped unless you pass `--force`.
- **`blackb0x`** drives everything else (DFU discovery, the exploit, uploads, checking
  jailbreak status over AFC2). No `geteuid() != 0` gate in `Cli.cpp`, and none needed —
  a real permission failure surfaces clearly from `libirecovery`'s own
  `irecv_open_with_ecid()`. No install step; the binaries run from wherever they're
  built.
- **`scripts/push_authorized_keys.sh`** is separate from both: a standalone,
  no-root-needed script you run by hand, after `blackb0x` reports the jailbreak is
  running, to grant yourself SSH access (see "Runtime requirements" below for what
  it actually does).

**Build-time system dependencies, verified against a real fresh clone + build (see
README for the full list)**: a C/C++ toolchain, GNU make, CMake ≥3.16,
autoconf/automake/libtool/pkg-config (most of the tree is autotools-based) — Homebrew
bundles none of the four, so install them explicitly. (`xxd` used to be required too,
solely to embed `gaster`'s payload binaries as C arrays; with gaster gone, nothing in
the build shells out to it any more.)

**Runtime requirements beyond the build** (not just build-time deps):
- `hdiutil`/`diskutil`/`cp`/`tar` (invoked directly as subprocesses, no shell) —
  `bake-firmware`'s ramdisk half only, all built in to macOS.
- `python3` — `bake-firmware`'s ramdisk half shells out to
  `scripts/build_deb_cache_experimental_no_container.py` to resolve the debcache
  fresh on every bake.
- `arm-apple-darwin11-clang` and `ldid` on `$PATH` — `bake-firmware`'s ramdisk
  half cross-compiles `entrypoint/` on every bake (once per run, not per
  firmware). One-time setup in `entrypoint/README.md`; `brew install ldid`
  plus a `cctools-port` build against the iPhoneOS 6.1 SDK.
- Theos (`$THEOS`, default `~/theos`) and `dpkg-scanpackages` (`brew install dpkg`)
  — `package/build.sh` builds the `xyz.regulad.blackb0x` `.deb` with Theos's
  `dm.pl` on every bake.

**No container runtime is required anywhere.** Podman used to be a hard runtime
dependency of three separate steps, and in every case it existed to give a *Linux*
host something macOS already has natively: Apple's `ld64`/`as` (via `cctools-port`)
for `entrypoint/`, a box with Theos on it for the `.deb`, and `p7zip` for the
by-hand extraction helpers. `scripts/build_deb_cache.py` is the one thing that still
wants podman — it is a Linux-only maintenance tool, never invoked by a build or a
bake, kept only because growing `debcache/` needs a real apt-get solve. See its
own header.
- `usbmuxd` — macOS's own built-in daemon; Normal-mode discovery has nothing to talk
  to without it. (Linux additionally needed it run with `--no-preflight` via a systemd
  drop-in or discovery silently never fired; that requirement is gone with Linux.)
- The invoking user's own `~/.ssh/authorized_keys`, if SSH access is wanted —
  `blackb0x` itself no longer touches this at all; run
  `scripts/push_authorized_keys.sh` by hand once the jailbreak is confirmed running
  (it forwards a local TCP port to the device's real sshd — Cydia's own openssh
  package — over `usbmuxd`/`iproxy`, then pushes the file like a normal
  `ssh-copy-id`). Entirely optional, no root/sudo needed, and nothing else in
  `blackb0x` depends on it.
- `stdbuf` (GNU coreutils) — **required**, not optional:
  `runLineBufferedSubprocess()` (`DeviceManager.cpp`) checks for it on `PATH` before
  ever forking `blackb0x-pwn` and
  refuses to run the exploit at all if it's missing, rather than silently falling
  back to unbuffered output. An earlier version of this code did fall back silently
  — that's exactly the "blind the whole time" bug documented in `docs/HISTORY.md`,
  reintroduced by treating this as optional. Don't re-add that fallback.
- No kernel-module blacklisting, no udev rule, no systemd drop-in. All three were
  Linux-only requirements and are gone; see the README's "No one-time system setup".

## Install-time design (what ships on the ramdisk, and how)

Folded in from `.claude/NEO_FLOW.md`, which is deleted. That file was written as a
design doc for work not yet implemented; the work is done and has moved past the
plan in several places, so what follows is the as-built design. Where this and the
code disagree, the code is right.

This supersedes the pre-rewrite Objective-C tool's install flow entirely. Two
structural problems drove the rewrite: `postinstall.sh` hit the live network on
every single real-OS boot forever, with no offline fallback; and the base Cydia
filesystem was a flat, pre-extracted overlay with no corresponding `dpkg` database
entries, so `dpkg` never knew those files existed. The fix that shipped is a real
`dpkg`/`apt`-driven install, resolved once at bake time and run once per device at
first real boot — but **not** a fully network-disabled one. That specific piece of
the plan turned out to be impossible (see `postinstall.sh` below).

### How `/blackb0x` gets assembled

Nothing is copied into `/blackb0x` by hand at runtime. The whole tree is assembled
once at bake time by `BakeRamdisk.cpp`'s `stageBlackb0xTree()`, then blindly
replicated onto the real device by `entrypoint.c`'s `merge_tree()` at boot.

1. **Resolve the closure.** `scripts/build_deb_cache_experimental_no_container.py`
   walks `package/packages.txt` (real package *names*, not `.deb` filenames) into a
   transitive closure over the `.deb` bytes already committed in `debcache/`. It only
   reads; it cannot fetch. Growing `debcache/` is a separate, Linux-only, CI job —
   `scripts/build_deb_cache.py`, which does a real apt-get solve against the five
   configured repos inside podman and refuses to run off Linux. See that script's
   header and the `debcache/` entry in "Repo layout".
2. **Local-only fallback.** A few packages will never resolve through any live repo
   but do have a real recovered `.deb` (`essential`, currently the only one).
   `package/local_only_debs.txt` names them by filename. `package/build.sh` bundles
   exactly those into the package as a file-backed apt repo at
   `/var/.blackb0x/local-debs`, with a real `dpkg-scanpackages` index, reachable
   on-device through `sources.list.d/local.list`
   (`deb [trusted=yes] file:///var/.blackb0x/local-debs ./`). A `.deb` sitting in
   apt's cache with no matching `Packages` entry is invisible to apt's resolver —
   verified with a real minimal repro — which is why these need an index even though
   the main debcache doesn't.
3. **Decide bake-time force-install vs. real apt.**
   `computePreinstallEligibleFilenames()` walks the resolved closure against
   `misc/prebake_package_blacklist.txt`. Packages with a real, stateful,
   uninspectable or device-only postinst/preinst (`cydia`, `firmware-sbin`, `rtadvd`,
   `pam`, `pam-modules`, `essential`, the exploit-specific ones) are excluded, and so
   is anything that transitively depends on an excluded package — propagated to a
   fixpoint, not one level. An eligible package that still has a postinst gets it
   stripped between unpack and configure (safe: postinst only runs at `--configure`);
   one with a preinst gets its `.deb` rebuilt without that member before being
   unpacked at all (preinst runs *during* `--unpack`, so stripping after the fact is
   too late). Both decided per real `.deb` content, never hardcoded.
4. **Install the eligible set for real.** On Linux this ran inside a `debian:stretch`
   container for an era-appropriate `dpkg` 1.18.26: force-unpack everything
   (`--force-architecture --force-depends --unpack`, because this ecosystem has a
   genuine unbreakable circular Pre-Depends chain, `dpkg → tar → gzip/lzma → sed →
   dpkg`), then one `dpkg --force-depends --configure -a` to let dpkg's own solver
   break the cycle — the real debootstrap two-phase pattern. **macOS has no container
   runtime, so that path is gone**; `computePreinstalledPackages()` now extracts each
   eligible `.deb` with plain `ar`/`tar`, merges its payload, and hand-writes the
   matching dpkg status stanza and `info/` state. That is sound precisely because
   step 3 already proved no maintainer script ever executes on this path, which
   removes the ordering problem real dpkg was there to solve. `kPreinstallInnerScript`
   in `BakeRamdisk.cpp` is the old container script, kept as the record of what it
   did and executed by nothing.
5. **Cache it once per process.** `computeGlobalDebcacheOnce()` runs steps 1–4 exactly
   once per `bake-firmware` invocation (a static-local cache). The pipeline is
   firmware-independent; re-running it per target meant identical repeated work.
6. **Build and install the package.** `stageBlackb0xPackage()` runs
   `package/build.sh`, which produces a real `xyz.regulad.blackb0x` `.deb` with
   Theos's `dm.pl`, then installs it into the staged tree the same way
   `stageEtasonatv()` does — extract, merge payload, append a real dpkg status stanza.
   This replaced a dozen hand-rolled `stageFile()` calls (`.claude/TODO.md` item 11).
   `postinstall.sh`'s `__BLACKB0X_PACKAGES__` placeholder is templated by `build.sh`
   from `package/packages.txt` itself — deliberately *not* from this bake's resolved
   closure, so the package needs no bake state and stays independently buildable. It
   is also more correct on-device: bake-time resolution runs against a synthetic
   `firmware` package and a few entries legitimately fail there while resolving fine
   against real repos. Top-level `etc/`/`var/` are remapped to `private/etc/`,
   `private/var/` — the `.deb` ships them unprefixed (right for on-device dpkg, where
   they are symlinks), but `/blackb0x` is a flat mirror replicated literally.
7. **Stage the rest, per firmware.** The bake-time `apt-get update` lists cache
   (so on-device apt knows what every repo offered even with no network at install
   time), the non-preinstalled `.deb` bytes into apt's real cache directory, and
   exactly one of three persistence payloads picked by this firmware's real
   `ProductVersion`.
8. **Build the entrypoint binary once.** `BakeFirmware.cpp`'s `main()` calls
   `buildEntrypointBinary()` before its per-firmware loop, same pattern as the
   debcache cache, and passes the one built path into every `bakeRamdisk()` call.

### Persistence payloads

Picked by `stageVersionBranch()` off the firmware's real `ProductVersion`:

- **8.4.x** → `stageEtasonatv()`: the four loose payload files extracted directly
  from the real `net.tihmstar.etasonuntether` `.deb` in `debcache/` (not the old
  hand-assembled `tihmstar-untether.tar`, which is gone), with this project's own
  `misc/untether.bin` deliberately overriding the package's copy, held in dpkg so
  apt can never replace it. Plus the `/untether/expl.js` → `/--early-boot` symlink.
  The `rtbuddyd`→`jsc` swap is **not** done at bake time — see `entrypoint.c`'s
  `fixup_etasonuntether_rtbuddyd()`, which needs the real target volume. The only
  branch with a real untether.
- **7.x / 8.x (non-8.4)** → `stageIos7Tether()`: swaps a custom binary into
  `/usr/libexec/dirhelper` (`misc/dirhelper`). Genuinely tethered; no untether exists
  for this range. The technique traces cleanly to evasi0n6's published
  `dirhelper`-hijack mechanism (matching `remount()` call, confirmed against
  evasi0n6's archived source), but the compiled binary itself was never
  open-sourced by anyone — see `misc/README.md` for the full dead-end provenance.
- **6.1.4** → `stageP0sixspwn()`: p0sixspwn's own untether payload, extracted from
  the real vendored `.deb`.
- Anything else warns and stages common content only. No hard failure — a firmware
  with no persistence answer yet still gets everything else correctly.

### `entrypoint.c` replaces `/sbin/launchd`, not `/etc/rc.boot`

The one place this project's exploration went somewhere and came back. Real
disassembly of an AppleTV2,1 10B809 `RestoreRamdisk` showed `/etc/rc.boot` is itself
`LC_MAIN`-entered directly by the kernel, so the splice target moved there for a
while on the theory that injecting at the true first entry point is strictly better.
It didn't generalize: a real bake against AppleTV3,1/3,2 12H606 failed because that
firmware's ramdisk has no `/etc/rc.boot` at all (`/etc/` is nearly empty there,
confirmed by mounting it). Reverted to always targeting `/sbin/launchd`, the one
thing guaranteed to exist as real PID-1 across every firmware generation. The ad-hoc
signing identity matches: `com.apple.launchd`.

`do_install()` is unconditional apart from two guards — bail if
`/mnt1/Applications/AppleTV.app/AppleTV` is missing (not an Apple TV), and `panic()`
if `/mnt1/var/.blackb0x/install-done` already exists rather than clobber live dpkg
state. No version branching happens on-device at all; that decision was made at bake
time, so `entrypoint.c` never needs to know what firmware it is on.

- `merge_tree()` is a generic recursive merge (real `stat()` owner/mode, symlinks
  recreated verbatim, existing destination directories recursed into rather than
  replaced). It replaced the old
  `create_cydia_directories()`/`MYSTERY_DIR_MODE`/`clone_directory()` machinery
  entirely — every directory `/blackb0x` needs is already a plain staged entry in it.
- `panic()` deliberately never returns and never reboots. An automatic reboot on a
  genuine failure would re-run the same ramdisk into the same panic every cycle, with
  nothing to show a human debugging over console/serial.
- The mount target is `/mnt1`, not `/mnt` — `/mnt1` and `/mnt2` are real pre-existing
  empty mountpoints on a pristine ramdisk; `/mnt` never was.
- `set_auto_boot()` runs the pristine ramdisk's own `/usr/sbin/nvram auto-boot=1`
  before every `reboot(2)`. SecureROM/iBoot clears that variable once a real DFU
  payload has run; without it a plain reboot risks leaving the device at the iBoot/DFU
  prompt instead of continuing into the installed OS. This is the one thing salvaged
  from the ssh-rd-derived `rc.boot` this project once looked at (now deleted).

### `postinstall.sh` — one-shot, network-opportunistic

Runs once the device boots into its real OS
(`xyz.regulad.blackb0x.postinstall.plist`, `RunAtLoad`, root). `StandardOutPath` and
`StandardErrorPath` point at two *separate* log files — pointing both at one path is
a real, long-documented launchd bug.

**The original plan called for hard-disabling the network for the whole install
window. That is impossible in practice**: Kodi and a few other real packages are far
too big for this A4-era ramdisk's budget, so they are never staged locally at all
(`kNeverStageDebs`). What shipped uses the network opportunistically instead:

- All five real sources.list.d entries are always present; `apt-get update` may reach
  them if it can. That is the **one and only** command in the script allowed to fail
  (`|| true`). Everything else runs under a bare `set -ex`, so a real failure stops
  the script, `install-done` never gets written, and the next boot retries the whole
  install from scratch.
- With no network, apt still has the bake-time `apt-get update` lists cache staged at
  `/private/var/lib/apt/lists/`, so it knows what every repo offered as of bake time.
- Whatever `.deb` bytes did fit sit in apt's own cache
  (`/private/var/cache/apt/archives/`); apt finds them via its normal
  cache-before-download check, no `file://` source needed for the main debcache.

The sequence: exit if `install-done` exists (a real UTC timestamp, not an empty
touch); wait ≥60s since boot before touching apt, because this `RunAtLoad` job fires
before networking has necessarily associated and this old launchd has no portable
one-time-delay knob (`sleep` is not guaranteed present on a stock retail OS — dpkg's
own control file lists `bash` as a `Depends:`, so this ecosystem treats even a shell
as something it must supply; `coreutils-bin`'s cached `.deb` gets `dpkg -i`'d if
missing, though it is normally already bake-time-preinstalled); `apt-get update ||
true`; `apt-get install cydia` **on its own, first**, because its real postinst does
its own `/var/stash` relocation and nothing else may assume that environment exists;
then the templated package set; then `install -f`, plain `upgrade`, `autoremove`;
then write `install-done` last.

Plain `upgrade`, never `dist-upgrade`/`full-upgrade`: this on-device apt
(`apt7 0.7.25.3`) has no unified `apt` command and no `full-upgrade` at all, and
plain `upgrade` only touches already-installed packages, never demanding more from
the network than what is staged.

Worth knowing, because the pre-rewrite tool got this wrong: **the real `cydia`
package ships its own `/usr/libexec/cydia/startup`, `firmware.sh` and a real
`RunAtLoad` LaunchDaemon** (1.1.30, downloaded and inspected directly). The
synthetic-`firmware`-package declaration, GSC capability stanzas,
`Media/Cydia/AutoInstall` handling and `uicache` refresh all happen automatically
once `apt-get install cydia` runs through real dpkg. The old tool reimplemented all
of it by hand only because its flat-file-copy install never ran real dpkg, so
Cydia's own LaunchDaemon never got registered.

### Ramdisk sizing

`/blackb0x` does not fit in the pristine ramdisk's free space, and growing an HFS+
volume in place proved unreliable (xpwn's `grow_hfs()`, `libhfsp`, and the Linux
kernel driver's own resize path each have a confirmed bug in exactly that operation).
So the bake assembles the real final content — original ramdisk + spliced `launchd` +
`/blackb0x` — onto a generously oversized throwaway scratch volume, measures that
*mounted* volume's real disk usage (block-rounded; estimating from a plain host
directory undercounted by several MB in practice), then creates the shipped volume
sized from that number plus a margin and does one `cp -a`.

Both volumes are case-sensitive, matching the real iOS/tvOS root (HFSX) — a real bake
failure showed ncurses' terminfo tree needs genuinely distinct case-varying sibling
directories (`e`/`E`, `a`/`A`).

**A finished ramdisk over 64 MiB is a hard failure**, not a warning: the oversized
output is deleted and the bake returns false, because a ramdisk that big cannot be
uploaded and leaving it in `dist/` would get it silently reused.
`DEBUG_RAMDISK_LIMIT_MIB` overrides the ceiling; `-1` disables the check entirely and
turns it into a surfaced warning instead. (The ceiling was a 70MB rule-of-thumb
warning under the Linux design; the A4/A5 iBEC has no `ramdisk-size` variable to ask,
and 64 MiB was confirmed against a live AppleTV3,2 as at-or-very-near the real limit.)

## External references

- [ATV3 jailbreak writeup PDF](https://elhacker.info/Books/BOOKS%20PART%206/atv3_jb-.pdf)
  — potentially explains the `jsc` (JavaScriptCore) framework reference and the
  `/mnt/--early-boot` symlink found in the `etasonATV` branch of `entrypoint/`'s
  reverse-engineered install logic (see `misc/README.md`) — both were
  flagged there as "we replicate the action, not the underlying mechanism."

## Current status: macOS only

**Linux support is removed.** The build refuses to configure off Apple (`CMakeLists.txt`
fails fast rather than dying deeper in a vendored ExternalProject). Apple Silicon is the
tested host.

`checkm8` never worked on Linux, and that is why. The exploit sequence runs to
completion and the task-struct overwrite simply never lands. Four explanations were
proposed and each refuted by later measurement — a host-stack limitation, the overwrite
being refused, the bug-setup SETUP never arriving, and the consumed-byte count — and by
the end **every observable matched a working macOS run while the outcome still
differed**. `docs/HISTORY.md` records each dead end and why it died; the point of that
detail is that nobody re-runs those experiments.

Two findings from it are worth carrying forward regardless of platform:

- **The bug setup is non-deterministic.** The same cancel delay produced 0 and 64
  consumed bytes on identical hardware, and 32 of 35 attempts consumed bytes within a
  single run. Any measurement taking one sample per configuration is measuring noise.
- **`DFU_GETSTATUS` is not an oracle for the exploit precondition.** checkm8 *is* a bug
  in that state machine: the aborted `DFU_DNLOAD` leaks a buffer the state machine stops
  tracking, so a device reporting `dfuIDLE` while holding a dangling pointer is the
  vulnerability, not evidence against it.

What still works and is verified: normal-mode discovery (real `usbmuxd`, wolfSSL/SSLv3
lockdownd handshake, AFC), the full firmware download/decrypt/patch/re-encrypt pipeline,
`--dry-run`, and `blackb0x-pwn` against real AppleTV3,2 hardware on macOS.
**Only one physical unit has ever been available: an AppleTV3,2** — the `AppleTV2,1`
(SHAtter) and `AppleTV3,1` (external-hardware checkm8) paths are implemented from
protocol analysis only.

**The ramdisk baker's macOS path has never been run on real macOS.** It was written with
no Mac available (see `BakeRamdisk.cpp`'s own caveat and `.claude/TODO.md` item 4a). The
Linux loop-mount path that *was* verified is gone, so ramdisk baking is currently
unverified end to end. This is a known, accepted consequence of dropping Linux, not an
oversight.

The `DEBUG_`-prefixed knobs in `blackb0x-pwn` (`DEBUG_CANCEL_DELAY_US`,
`DEBUG_OVERWRITE_TIMEOUT_MS`, `DEBUG_RECONNECT_ATTEMPTS`, `DEBUG_KEEP_CONNECTION`,
`DEBUG_IGNORE_GROOM_ERRORS`, `DEBUG_TRACE_TRANSFERS`, `DEBUG_DFU_STATUS`) are kept.
Every default is the behaviour confirmed working on real hardware, so an unset
environment is the original path byte for byte. `DEBUG_DFU_STATUS` is the exception that
deliberately perturbs: `GETSTATUS` advances the DFU state machine, so a run with it set
is not evidence about a run without it.
