# AGENTS.md — Blackb0x portable port

Short and operational: what an agent needs to move around this repo and build it
correctly, right now. For the *why* — every bug hunt, dead end, and decision's
evidence — see **[`docs/HISTORY.md`](docs/HISTORY.md)**. If something here seems to
need more justification than it gives, it's almost certainly explained there.

## What Blackb0x is

A jailbreak tool for 2nd/3rd-gen Apple TV (A1378/A1427/A1469) via the checkm8/SHAtter
DFU-mode boot exploit, which then side-loads Cydia + Kodi. A portable CLI port of an
original macOS Cocoa/Objective-C app (the `.m`/`.mm`/`.h` files still in
`Blackb0x/Source/` are that original — reference-only, not built).

## Conventions (don't re-litigate without asking)

- **CLI-only.** No GUI — dropped, not dual-maintained. Primary target is Linux;
  macOS support (`blackb0x`/`blackb0x-pwn` only, not the ramdisk baker yet) is actively
  being brought up — see `.claude/TODO.md` item 4 for exactly what's done vs. still
  needs real macOS hardware to verify.
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

- `Blackb0x/Source/` — the ported C++, and nothing else: `main.cpp`, `Cli.hpp`/`.cpp`,
  `DeviceManager.hpp`/`.cpp`, `IPSW.hpp`/`.cpp`, `IPSWDownloader.hpp`/`.cpp`,
  `Patcher.hpp`/`.cpp`, `ResourcePath.hpp`/`.cpp`, `BakeRamdisk.hpp`/`.cpp`,
  `BakeAllRamdisks.cpp`. The original Objective-C (`AppDelegate`, `MainView`,
  `Blackb0x.h`/`.m`, `TaskManager`, and the old `.h`/`.m`/`.mm` counterparts of the
  files above) has been fully ported and deleted — check `docs/HISTORY.md`/git
  history if you need to see what it looked like. `checkm8.h`/`SHAtter.h` are exploit
  payload byte arrays, `#include`d directly by `DeviceManager.cpp` — not leftover
  Cocoa, keep these.
- `Blackb0x/Libraries/` — already-portable C kept in-tree and built directly by the
  root `CMakeLists.txt`: `CBPatcher.c`/`libcbpatcher/`, `libiboot32patcher.c`/
  `libiboot32patcher/`, `xpwntool.c`. `libbootkit/` is dead code, linked into nothing.
- `Blackb0x/ramdisk/` — the ramdisk overlay payload shipped to the jailbroken Apple TV
  itself, checked in as loose files (no `.tar`/`.tgz`) mirroring their destination
  paths, merged onto the mounted ramdisk via one `cp -a` (single unconditional
  tree — no more SSH-only vs full-Cydia split, so no reason to split the tree
  itself either; it used to be `common/`+`cydia/` subdirectories for exactly that
  now-gone distinction). Host key generation and SSH access are no longer baked in
  here at all — SSH access is granted post-boot, by hand, via
  `scripts/push_authorized_keys.sh` (see "Build & run" below). A
  large chunk of `ramdisk/files/cydia/` and `ramdisk/files/p0sixspwn/` is actually
  pre-extracted content that originally came from real Cydia `.deb` packages
  (identifiable via the dpkg `.list` manifests still present under
  `private/var/lib/dpkg/info/` in each) — the eventual goal is to source those
  packages for real and build this tree from them at bake time instead of
  checking in the already-extracted result (unowned by `mobile`, permissions not
  representative of a real device). Not done yet. Needs no porting.
- `Blackb0x/Debs/` — loose `.deb` packages that `setup.sh` `dpkg -i`'s at first
  boot, merged into `/files/` on the ramdisk alongside (but separately from) the
  `ramdisk/` tree by `bakeRamdisk()` — kept apart because these are real package
  archives, not loose files mirroring a destination path. Same eventual goal as
  `ramdisk/files/cydia/` above: source these from real Cydia repos rather than
  checking in the `.deb` bytes.
- `dist/` — bake-all-ramdisks' output (gitignored, not checked in): one
  `<device>_<buildID>-Ramdisk.dmg` per known firmware. An entry that already
  exists is skipped; pass `--force` to rebuild. There is no staleness
  detection: a `.sum` sidecar holding a `ramdisk/`+`Debs/` content hash used
  to force re-bakes automatically, and it was removed as fragile, so **after
  changing anything that affects baked output, pass `--force`**.
- `Blackb0x/ImageKeys/` — per-firmware IPSW decryption `.keys` files, read locally by
  `IPSW.cpp` only; never shipped to the device.
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
| `libirecovery` | **regulad/libirecovery**@`libusb-async-cancel-fix`, off synackuk/libirecovery | Yes — `irecv_async_usb_control_transfer_with_cancel()`'s libusb branch had a use-after-free, a double free and a non-terminating completion wait; it was unreachable until `blackb0x-pwn` was built for non-Apple. Plus real stderr diagnostics on `irecv_send_buffer()`'s upload-failure paths, which previously returned `IRECV_E_USB_UPLOAD` silently unless built with `debug()` on |
| `libimobiledevice-glue`, `libplist-modern` | libimobiledevice/* | No — current HEAD, not historically pinned (see HISTORY for why two `libplist`s) |
| `libfragmentzip` | **regulad/libfragmentzip**@`fix-cxx-stdbool-header` | Yes — one header fix (C++/`<stdbool.h>` collision) |
| `libgeneral` | tihmstar/libgeneral | No |
| `xpwn` | **regulad/xpwn**@`legacy` | Yes — a wolfSSL AES-CBC buffer over-read fix in `img3.c`, plus disabling the legacy-libusb-0.1-only `pwnmetheus2` subdirectory |
| `wolfssl`, `curl`, `libusb`, `libzip`, `libpng`, `bzip2`, `zlib` | upstream | No — current HEAD or latest stable tag; none of these existed in the original app |
| `iBoot32Patcher` | **regulad/iBoot32Patcher**@`blackb0x`, off zzanehip/iBoot32Patcher | Yes — two real bug fixes: `patch_kaslr()` fell off the end of a non-void function on every *successful* branch (garbage return read non-zero on x86_64, 0 on arm64, so a real macOS run treated a successful KASLR patch as a hard failure), and `iBootPatcher()` tested its `RSA` argument twice so the `debug` argument was dead and `patch_debug_enabled()` ran whenever the RSA patch was asked for. **Built as a separate EXECUTABLE and fork/exec'd, never linked** — it is GPL-3.0-or-later and blackb0x declares no license, so linking would make blackb0x a GPLv3 derivative. Do not "simplify" it back into a static library |

`Blackb0x/Libraries/xpwntool.c` (in-tree, not a submodule) is sourced from
`zzanehip/xpwntool-swift`, with one local fix: `decrypt()`'s three error paths
(`cannot open infile` / `cannot open outfile` / `cannot duplicate file from provided
template`) each printed the diagnostic and then fell through to dereference the NULL
they had just reported, so any one of them was a SIGSEGV rather than a failure. They
bail out now. That is the root cause behind the "`decrypt()` a nonexistent file
corrupts the heap" hazard `Patcher.cpp` documents in several places; `decrypt()` is
still `void`, so callers detect failure by checking for a zero-byte output. `Blackb0x/Libraries/libcbpatcher/` is likewise
in-tree, recovered from `zzanehip/CBPatcher` but with real local portability work on
top (see `portable_macho.h`).

## Build & run

```
cmake -S . -B build && cmake --build build -j$(nproc)

# Once, in bulk, for every known firmware — NOT run by blackb0x itself.
# Writes dist/<device>_<buildID>-Ramdisk.dmg per firmware (gitignored):
sudo ./build/bake-all-ramdisks [--signed-only]

# Pre-patches iBSS/iBEC/KernelCache/DeviceTree for every known firmware.
# Needs NO root, unlike bake-all-ramdisks. Writes dist/bootchain/<device>_<buildID>/:
./build/bake-all-bootloaders [--signed-only] [--device <model>] [--build <buildID>]

./build/blackb0x [--ecid <id> | --udid <id>] [--dry-run]
# (needs sudo instead, unless a udev rule already grants your own user raw
# USB access to the device in DFU/Recovery/WTF mode — see README)
```

Two binaries, deliberately separated by privilege:

- **`bake-all-ramdisks`** is the only piece of this tool that needs `CAP_SYS_ADMIN`/
  `CAP_CHOWN` (loop-mounting a real HFS+ image — see `BakeRamdisk.hpp`'s header
  comment for why an in-process, no-mount approach isn't viable). For every
  `.keys` file under `Blackb0x/ImageKeys/` (i.e. every known device/firmware
  combination), it downloads that firmware's `RestoreRamDisk` component and merges
  the `Blackb0x/ramdisk/` overlay into it, writing each result to
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
  jailbreak status over AFC2) and needs root *by default* only for raw DFU/
  Recovery/WTF-mode USB access — there's no hard `geteuid() != 0` gate in
  `Cli.cpp` (there used to be; removed as a genuine correctness fix, not a
  relaxation for its own sake — a udev rule can hand a normal user that same
  access, see README's own setup section, and a real permission failure surfaces
  clearly from `libirecovery`'s own `irecv_open_with_ecid()` either way). No
  install step — both binaries run from wherever they're built.
- **`scripts/push_authorized_keys.sh`** is separate from both: a standalone,
  no-root-needed script you run by hand, after `blackb0x` reports the jailbreak is
  running, to grant yourself SSH access (see "Runtime requirements" below for what
  it actually does).

**Build-time system dependencies, verified against a real fresh clone + build (see
README for the full list)**: a C/C++ toolchain, GNU make, CMake ≥3.16,
autoconf/automake/libtool/pkg-config (most of the tree is autotools-based).
(`xxd` used to be required too, solely to embed `gaster`'s payload binaries as C
arrays; with gaster gone, nothing in the build shells out to it any more.)

**Runtime requirements beyond the build** (not just build-time deps):
- `mkfs.hfsplus`/`fsck.hfsplus` (`hfsprogs` package) and a kernel with `hfsplus`
  support (`CONFIG_HFSPLUS_FS`) — needed by `bake-ramdisk`, not `blackb0x` itself.
- `mount`/`umount`/`blkid`/`cp` (invoked directly as subprocesses, no shell) — also
  `bake-ramdisk` only. Assumed present on any mainstream distro, not called out as a
  separate install step.
- `python3` and `podman` — `bake-all-ramdisks` shells out to
  `scripts/build_deb_cache.py` (via `python3`, which shells out to `podman`
  itself) to resolve the debcache picklist fresh on every bake; see
  `BakeRamdisk.cpp`'s `buildPicklist()`. Same `$SUDO_USER`/`runuser`
  re-invocation as the existing `entrypoint/` podman calls, for the same
  reason (podman's own rootless storage belongs to the real invoking user,
  not root's).
- `usbmuxd` itself must be installed (a separate requirement from the point below —
  most distros package it separately, e.g. `usbmuxd`), and **must run with
  `--no-preflight`** (a systemd drop-in — `/etc/systemd/system/usbmuxd.service.d/
  override.conf` — is the documented way; see `docs/HISTORY.md` for exactly why) or
  Normal-mode device discovery silently never fires. Both of these have to ship in
  the end-user README.
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
- The in-tree `apple_mfi_fastcharge` kernel driver **must be blacklisted** (a
  `/etc/modprobe.d` drop-in — see the README's own setup section) or it fights
  the pwntool for the DFU-mode device mid-exploit; see `docs/HISTORY.md` for exactly
  why. Same pattern as `usbmuxd --no-preflight` above: a documented one-time manual
  step, not something `blackb0x` checks or fixes for you at runtime.

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
The `checkm8` exploit run itself is **the open problem**, and it is the reason this
branch exists. It has never succeeded on Linux. Two independent implementations have
been tried against the one physical AppleTV3,2 available:

- **`gaster`** (verygenericname/gaster, the tool palera1n's `legacy` branch shells
  out to) — vendored, then forked to `regulad/gaster@linux-reset-race` for a long
  instrumentation and bug-fixing campaign. **Never pwned the device on either Linux
  7.1.x or macOS 26.** It is now removed from the tree entirely, fork included.
- **`blackb0x-pwn`** — this project's own original hand-ported checkm8, recovered
  from git history and built as a standalone binary on every platform. This is the
  implementation confirmed working on real AppleTV3,2 hardware **on macOS**, and it
  is now the only pwntool. On Linux it fails at a specific, well-characterised point
  (see below).

Along the way three real, independent software causes were each found, fixed, and
ruled out as *the* cause — all three fixes are genuine improvements worth keeping
regardless:

1. **`apple_mfi_fastcharge`** auto-binds to the Apple TV even in DFU mode (its
   product-ID match range `0x1200`-`0x12ff` covers this device's DFU PID `0x1227`)
   and issues its own `usb_reset_device()` calls mid-exploit. Must be blacklisted via
   `/etc/modprobe.d` — a bare `modprobe -r` is confirmed insufficient, since the
   kernel reloads it via `request_module()` on every stage-transition reconnect. Real
   conflict, **not** the root cause: with it genuinely unloaded the hang was identical.
2. **Unclaimed interfaces.** DFU class requests were being sent without claiming
   interface 0, which is what the kernel's "did not claim interface 0 before use"
   warning was about. Fixed — and a second bug inside that very fix turned up: it
   *gated* success on the claim succeeding, but with a device-side truncated config
   descriptor (`bNumInterfaces 0`, confirmed stable for minutes via `lsusb -v`) the
   claim fails forever, turning a possibly-transient state into an unconditional
   "device not found."
3. **The reset-skip theory** — skipping the post-`SETUP`/`SPRAY` USB reset, on the
   reasoning that unlike `RESET`/`PATCH` those stages never enter
   `DFU_STATE_MANIFEST_WAIT_RESET`. **Disproved on real hardware**: the device wedged
   in true `D` state inside `usb_reset_configuration`/`usb_control_msg`. The reset is
   load-bearing, not precautionary. Reverted.

**The "Linux host-stack limitation" conclusion that followed those three was itself
wrong, and has been retracted.** A `usbmon` capture established that the resets work,
that the partial-transfer mechanism is precise and predictable, and that large
malformed control writes *are* delivered by this host. What actually fails is
narrower: the device refuses the overwrite transfer for `blackb0x-pwn` specifically,
and the acceptance window is structurally impossible to hit — acceptance needs the
groom size above 320 bytes, while fitting all 1660 payload bytes needs it at or below
388, and the device treats anything from 1472 up as a continuation of the same
2048-byte DFU buffer. No cancel delay reconciles those. See `docs/HISTORY.md` for the
full evidence trail, the measured tables, and the dead ends worth not re-trying.

The decisive outstanding measurement is still **what that same transfer does on a
working macOS run** — but the way to get it has changed. The `XHC20`/tcpdump route
`docs/HISTORY.md` originally prescribed is a **dead end**: macOS hides the USB capture
interfaces unless SIP is fully disabled (confirmed by Apple DTS), and reports say the
method fails on macOS 15.6.1+ even with SIP off. `analyze_usbmon_checkm8.py`'s
`DLT_USB_DARWIN` path is consequently unverified *and* unreachable — don't invest in
it. The measurement has to come from instrumenting `blackb0x-pwn` itself and diffing
its output across platforms; see `docs/HISTORY.md` for what is and isn't recoverable
that way.

All investigation knobs are `DEBUG_`-prefixed and every default is the
macOS-confirmed behaviour, so an unset environment is the original path byte for
byte: `DEBUG_CANCEL_DELAY_US` (100), `DEBUG_OVERWRITE_TIMEOUT_MS` (100),
`DEBUG_RECONNECT_ATTEMPTS` (30), `DEBUG_KEEP_CONNECTION` (unset),
`DEBUG_IGNORE_GROOM_ERRORS` (unset).
