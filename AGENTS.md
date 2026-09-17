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
  macOS support (`blackb0x`/`gaster` only, not the ramdisk baker yet) is actively
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
  maintain; a hand-ported `checkm8` USB exploit sequence was replaced with shelling
  out to `gaster` (the same implementation `palera1n` itself uses) once it became the
  more proven path. Default to this; don't reach for a rewrite first.
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
  `<device>_<buildID>-Ramdisk.dmg` per known firmware, plus a `.sum` sidecar per
  entry recording a `Blackb0x/ramdisk/` + `Blackb0x/Debs/` content hash at bake
  time (see `ResourcePath`'s `ramdiskOverlayContentHash()`/`sumFileFor()`) —
  re-running bake-all-ramdisks after editing either re-bakes anything whose
  sidecar no longer matches, and `Patcher::patchRamdisk()` refuses a `dist/`
  entry whose sidecar is stale rather than silently uploading old content.
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
| `gaster` | **regulad/gaster** (fork), `linux-reset-race` branch, off verygenericname/gaster | Yes — claims interface 0 (with `libusb_set_auto_detach_kernel_driver()`) instead of sending every DFU class request unclaimed, which is what the kernel's own "did not claim interface 0 before use" warning was about. A second change (skipping the post-`SETUP`/`SPRAY` reset) was tried and reverted after real hardware proved it load-bearing, not precautionary — see `docs/HISTORY.md`. Also carries `checkm8_stage_setup()` instrumentation: it logs the cancelled transfer's reported size (the only value the cancel delay actually feeds, and the reason sweeping `CANCEL_DELAY_US` changes nothing), and `GASTER_SETUP_FULL_PAD=1` pads as `blackb0x-pwn` does instead of trusting that size |
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
autoconf/automake/libtool/pkg-config (most of the tree is autotools-based), and
`xxd` — genuinely required, easy to miss, since it's only used once: embedding
`gaster`'s exploit payload binaries as C arrays at build time
(`add_custom_command(... COMMAND xxd -iC ...)` in `CMakeLists.txt`).

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
- `stdbuf` (GNU coreutils) — **required**, not optional: `runGaster()`
  (`DeviceManager.cpp`) checks for it on `PATH` before ever forking `gaster` and
  refuses to run the exploit at all if it's missing, rather than silently falling
  back to unbuffered output. An earlier version of this code did fall back silently
  — that's exactly the "blind the whole time" bug documented in `docs/HISTORY.md`,
  reintroduced by treating this as optional. Don't re-add that fallback.
- The in-tree `apple_mfi_fastcharge` kernel driver **must be blacklisted** (a
  `/etc/modprobe.d` drop-in — see the README's own setup section) or it fights
  `gaster` for the DFU-mode device mid-exploit; see `docs/HISTORY.md` for exactly
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

The actual `checkm8` exploit run has repeatedly hung/frozen the USB stack across
*multiple different Linux machines* — initially misdiagnosed (on a single machine) as
a host-controller-level USB limitation, since the symptoms (D-state hangs, corrupted
enumeration) looked hardware-specific. A live `dmesg` capture during a real hang found
the actual cause: the in-tree Linux `apple_mfi_fastcharge` driver auto-binds to the
Apple TV even in DFU mode (its product-ID match range, `0x1200`-`0x12ff`, includes this
device's real DFU PID `0x1227`) and independently issues its own `usb_reset_device()`
calls while `gaster`'s own raw, timing-sensitive control transfers are in flight — two
actors resetting the same device at once, which explains both the corruption and why it
reproduces on any Linux box with this common, usually-autoloaded kernel module present.
Fixed the same way as the `usbmuxd --no-preflight` requirement above — a one-time
manual system setup step documented in the README, not code in `blackb0x` itself: the
module needs to be blacklisted via `/etc/modprobe.d`, since a bare `modprobe -r` alone
was tried first and confirmed insufficient on real hardware (the kernel reloads it on
its own via `request_module()` on every one of gaster's stage-transition reconnects,
independent of anything either binary does in userspace); see the README's own setup
section for the exact commands.

**Tested against real hardware with the module actually blacklisted — confirmed
working as designed, but confirmed *not* the fix for the hang.** `apple_mfi_fastcharge`
genuinely stayed unloaded through the whole run (no competing driver anymore, confirmed
via `lsmod` and the kernel log's driver attribution), but the exact same corruption and
hang happened anyway — `gaster` still got stuck at the same point, and the device was
left in the same descriptor-corrupted state (`lsusb -v`: garbled `iManufacturer`/
`iProduct`, `Couldn't open device`) that only clears on a physical unplug/replug. So
`apple_mfi_fastcharge` was a real, additive conflict worth fixing, but not the (sole)
root cause — this pointed back at a genuine host-side (kernel/xHCI) limitation
reinitializing this device after `gaster`'s own reset, independent of any competing
driver.

`gaster` is now forked (**regulad/gaster**, `linux-reset-race` branch — see the vendored
dependencies table above). Two changes were tried; one was reverted after a real-hardware
test disproved it:

- **Tried and reverted**: skipping `gaster_checkm8()`'s post-stage `reset_usb_handle()`
  call after a successful `SETUP`/`SPRAY` stage (reasoning: unlike `RESET`/`PATCH`, those
  two never put the device into `DFU_STATE_MANIFEST_WAIT_RESET`, so the reset looked
  merely precautionary). **Confirmed wrong against real hardware**: with this change,
  `gaster` hung deterministically after `Stage: SETUP` with *no* corruption or kernel log
  activity at all — `ps` showed the process in real `D` state, and
  `sudo cat /proc/<pid>/stack` showed it blocked inside
  `usb_reset_configuration`/`usb_control_msg`/`usb_start_wait_urb` — the kernel side of
  `libusb_set_configuration()` in the next stage's reconnect, waiting on a
  `SET_CONFIGURATION` URB that never completes. `checkm8_stage_setup()`'s async-abort
  heap-race technique apparently leaves the device's own USB peripheral controller
  unresponsive at the hardware level until a real bus reset clears it — the reset was
  load-bearing, not precautionary. Reverted; `gaster_checkm8()` is back to upstream's
  unconditional post-stage reset for every stage.
- **Kept**: `wait_usb_handle()` now claims interface 0 (with
  `libusb_set_auto_detach_kernel_driver()` enabled first) before running any DFU class
  request against it, instead of sending every one of those requests unclaimed — the
  direct fix for the "did not claim interface 0 before use" kernel warning seen
  throughout this investigation, and a code-level handling of a conflicting kernel driver
  (like `apple_mfi_fastcharge`) that doesn't depend solely on it being blacklisted ahead
  of time.

**Tested against real hardware in this (reverted-reset, claim-fix-only) form: one real
improvement confirmed, the core hang still not fixed.** The unkillable `D`-state hang is
gone — `/proc/<pid>/stack` this time showed the process just sleeping normally in
`wait_usb_handle()`'s retry loop, not wedged in the kernel — but the device still fails
to reconnect cleanly after the post-`SETUP` reset: the identical `error -110`/`error -75`/
"config 1 has 0 interfaces" corruption as the very first capture, confirmed to never
self-recover. Three independent, real software causes have now each been ruled out in
turn (`apple_mfi_fastcharge`, the reset-skip theory, unclaimed interfaces) without fixing
the underlying hang, though two of the three are genuine, permanent improvements worth
keeping regardless. **This converges on the same conclusion the original single-machine
investigation reached, now corroborated by elimination of every specific software
mechanism tested plus the original report that this reproduces across multiple different
Linux machines: a genuine host-side (kernel/xHCI) limitation, not a userspace software
bug.** See `docs/HISTORY.md`'s checkm8/gaster section for the full evidence trail and the
six earlier real software bugs found and fixed getting here.

**That "host limitation" framing was re-examined instead of accepted outright, and a real
bug in the interface-claim fix itself turned up.** Rather than guess further, directly
validated the two things actually in question: whether libusb's Linux sysfs-based
matching (this project builds with `--disable-udev`) really finds the device, and whether
the device genuinely still exists while `gaster` is stuck. A standalone probe against the
exact same static `libusb-1.0.a`, run live while `gaster` was stuck waiting, found and
opened the device immediately and successfully called `libusb_set_configuration()` — the
same call previously found wedged in `D` state — ruling out "libusb can't find/open the
device" as an explanation. `libusb_claim_interface()` failed with
`LIBUSB_ERROR_INVALID_PARAM`, and `lsusb -v` confirmed why: a genuinely truncated
configuration descriptor (`bNumInterfaces 0`), stable for minutes with no exploit running
at all — real device-side descriptor corruption after `SETUP`'s heap-race. The actual bug:
`wait_usb_handle()` was *gating* success on that claim succeeding, but upstream `gaster`
never claims at all — with a permanently-zero-interface descriptor, the claim failed every
time and `wait_usb_handle()` never even reached `checkm8_check_usb_device()`'s own check,
turning a possibly-transient exploit-related state into an unconditional "not found."
Fixed: the claim is now attempted only when `libusb_get_active_config_descriptor()` shows
`bNumInterfaces > 0`, and `wait_usb_handle()` proceeds to `usb_check_cb()` regardless of
whether the claim happened — matching upstream's permissiveness for exactly the case
where claiming isn't possible. **Not yet re-verified against real hardware.** Still
unresolved; further blind changes to gaster's exploit-timing code are not recommended
without a new, specific mechanism to test.
