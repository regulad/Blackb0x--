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
  `ResourcePath.hpp`/`.cpp`, `BakeRamdisk.hpp`/`.cpp`, `BakeAllRamdisks.cpp`,
  `BakeAllBootloaders.cpp`. The original Objective-C (`AppDelegate`, `MainView`,
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
- `Blackb0x/Debs/` — loose `.deb` packages the bake consumes: the source for both
  the bundled local repo and the apt debcache staged onto the ramdisk. Real
  package archives, not loose files mirroring a destination path. The eventual
  goal is to source these from real Cydia repos rather than checking in the
  `.deb` bytes.
- `dist/` — bake-all-ramdisks' output (gitignored, not checked in): one
  `<device>_<buildID>-Ramdisk.dmg` per known firmware. An entry that already
  exists is skipped; pass `--force` to rebuild. There is no staleness
  detection: a `.sum` sidecar holding a content hash of the bake inputs used
  to force re-bakes automatically, and it was removed as fragile, so **after
  changing anything that affects baked output, pass `--force`**.
- `Blackb0x/ImageKeys/` — per-firmware IPSW decryption `.keys` files, read locally by
  `IPSW.cpp` only; never shipped to the device.
- `Blackb0x/Misc/` — bake-time data that is neither code nor a package:
  `firmware_versions.txt`, `prebake_package_blacklist.txt`, the prebuilt `apt`
  binaries, `dirhelper`, and `untether.bin` (see `Blackb0x/Misc/README.md`).
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
# Writes dist/<device>_<buildID>-Ramdisk.dmg per firmware (gitignored):
./build/bake-all-ramdisks [--signed-only]

# Pre-patches iBSS/iBEC/KernelCache/DeviceTree for every known firmware.
# Needs NO root, unlike bake-all-ramdisks. Writes dist/bootchain/<device>_<buildID>/:
./build/bake-all-bootloaders [--signed-only] [--device <model>] [--build <buildID>]

./build/blackb0x [--ecid <id> | --udid <id>] [--dry-run]
```

Three binaries. Nothing needs root any more: the privilege split existed for Linux's
loop-mount and raw-USB device nodes, and neither applies on macOS.

- **`bake-all-ramdisks`** attaches an HFS+ image via `hdiutil` (no root, no mount
  helper). **This path has never been run on real macOS** — written with no Mac
  available; see `BakeRamdisk.cpp`'s caveat and `.claude/TODO.md` item 4a. For every
  `.keys` file under `Blackb0x/ImageKeys/` (i.e. every known device/firmware
  combination), it downloads that firmware's `RestoreRamDisk` component and merges
  the overlay (the `xyz.regulad.blackb0x` package plus the debcache) into it,
  writing each result to
  `dist/<device>_<buildID>-Ramdisk.dmg`. `--signed-only` restricts this to builds
  ipsw.me currently reports Apple as still signing (a small fraction of the total —
  what most real devices are actually on). The overlay is fully static (no
  per-device secrets get baked in), so each patched output is valid for every
  device on that firmware; there's no reason to re-derive it on every `blackb0x`
  run, so `blackb0x` itself never invokes this — `Patcher::patchRamdisk()` just
  checks whether the `dist/` entry it needs already exists, and tells you to run
  `bake-all-ramdisks` if not. Re-running is cheap: any `dist/` entry that already
  exists is skipped.
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
  `bake-all-ramdisks` only, all built in to macOS.
- `python3` and `podman` — `bake-all-ramdisks` shells out to
  `scripts/build_deb_cache.py` (via `python3`, which shells out to `podman`
  itself) to resolve the debcache picklist fresh on every bake; see
  `BakeRamdisk.cpp`'s `buildPicklist()`. Same `$SUDO_USER`/`runuser`
  re-invocation as the existing `entrypoint/` podman calls, for the same
  reason (podman's own rootless storage belongs to the real invoking user,
  not root's).
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

## External references

- [ATV3 jailbreak writeup PDF](https://elhacker.info/Books/BOOKS%20PART%206/atv3_jb-.pdf)
  — potentially explains the `jsc` (JavaScriptCore) framework reference and the
  `/mnt/--early-boot` symlink found in the `etasonATV` branch of `entrypoint/`'s
  reverse-engineered install logic (see `Blackb0x/Misc/README.md`) — both were
  flagged there as "we replicate the action, not the underlying mechanism."

## Current status

Builds clean; fully statically linked. Normal-mode discovery (real `usbmuxd`,
wolfSSL/SSLv3 lockdownd handshake, AFC), the full firmware download/decrypt/patch/
re-encrypt pipeline, and `--dry-run` are all verified working against real hardware.
**Only one physical unit has ever been available to test against: an AppleTV3,2** —
`AppleTV2,1`(SHAtter)/`AppleTV3,1` (external-hardware checkm8) paths are implemented
from protocol analysis only, unverified.
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
