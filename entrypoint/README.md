# entrypoint/

Source for the binary that gets spliced into the ramdisk's `/sbin/launchd`
at bake time (a brief mid-project detour spliced into `/etc/rc.boot`
instead — see this file's own "Status" section below for why that was
tried, and why it got reverted). It is **not** real launchd, and no longer
masquerades as it — it's a standalone first-boot installer that becomes
PID 1 on the patched restore ramdisk just long enough to recursively merge
`/blackb0x` (a new top-level directory `bakeRamdisk()` stages at bake
time — see `BakeRamdisk.cpp`'s `stageBlackb0xTree()` — as a flat mirror of
the real device's final layout, every entry already carrying its correct
final owner/mode) onto the real device filesystem (mounted at `/mnt1`, one
of the pristine ramdisk's own pre-existing empty mountpoints — confirmed
directly against a real, decrypted AppleTV2,1 10B809 RestoreRamdisk:
`/mnt1` and `/mnt2` both exist at the ramdisk root and are empty; reusing
one of these two is required rather than mkdir'ing a fresh `/mnt`, since
`bakeRamdisk()` touches nothing on the pristine ramdisk beyond
`/sbin/launchd`'s content and the new `/blackb0x` directory), then reboots
into the real OS. `entrypoint.c`
itself has no idea what firmware it's running on or what any of these
files are for anymore — which per-firmware persistence payload to stage,
and all of the old runtime install-state/version branching, moved
entirely into `BakeRamdisk.cpp`, since the target firmware is already
fully known at bake time. See
`misc/README.md` for the reverse-engineering writeup (strings, the
original `Patcher.mm`, a full Ghidra decompilation, and raw disassembly of
every syscall trampoline) this is built from.

This directory is a from-scratch reimplementation, not a decompilation —
Mach-O decompilation output is not something to build from directly. The
first pass matches the original's logic exactly (same syscalls, same file
lists, same quirks) so it's a verified-correct baseline before any actual
behavior changes (current pinned deb filenames, hardcoded firmware-version
branches, etc.) get made on top of it.

The original binary is genuinely freestanding — confirmed via its Mach-O
load commands (`LC_UNIXTHREAD`, zero `LC_LOAD_DYLIB` entries): no libSystem,
no dyld, every syscall made directly via `mov r12, #N; svc #128`, even
`strlen`/`memcpy`/`memset` hand-rolled. This is almost certainly load-bearing
— this binary *is* what runs as PID 1 at the earliest point of ramdisk boot,
before it's guaranteed dyld/libSystem are even functional — so `entrypoint.c`
replicates that: `-ffreestanding -nostdlib -static`, raw syscalls, no libc.

## Why it used to be containerized, and isn't now

This build ran inside podman for as long as the build host was Linux. The
reason was narrow and specific: producing a freestanding ARMv6 Darwin Mach-O
needs Apple's own `ld64`/`as`, and on Linux the only way to get those is
`cctools-port`, a *port* of them. (LLVM's own `lld` doesn't support the
`-static` linking this needs — verified, it warns "not yet implemented" and
still emits a dyld-dependent PIE binary.) That port, plus an ancient
iPhoneOS SDK, had no business being installed on an immutable rpm-ostree
host, so podman kept all of it out of the way.

On macOS `ld64` and `as` are simply the native tools, so the container has
nothing left to provide and is gone. What remains genuinely necessary is the
`arm-apple-darwin11-` prefixed cross-compiler and `ldid` — see the setup
below.

Theos was tried first and dropped — its `tool.mk` template assumes exactly
the opposite of what this binary needs (dynamic linking against `libSystem`,
`LC_MAIN`, a normal `main(argc,argv,envp)` fed by crt startup glue), and its
other conveniences (Logos, `.deb` packaging) don't apply to a binary that
gets spliced directly into a ramdisk rather than installed via dpkg.

## One-time setup

The `Makefile` expects two things on `$PATH`: `arm-apple-darwin11-clang` and
`ldid`. Nothing else, and nothing is vendored — this is the only binary in
the project that needs any of it.

1. **`ldid`**, for ad-hoc signing. In homebrew-core:
   ```
   brew install ldid
   ```
   (`ldid-procursus` also provides an `ldid` and works here; the two formulae
   conflict, so pick one.)

2. **Get an iPhoneOS SDK.** See `entrypoint/assets/README.md` for exactly how
   `assets/iPhoneOS6.1.sdk.tar.xz` was produced (extracted from the real
   "Xcode 4.6" installer, archived on archive.org). Not checked into git
   (Apple's copyrighted material); regenerate it locally per that README if
   it's missing.

   This is still required even though the host is macOS: current Xcode SDKs
   dropped 32-bit ARM entirely, so nothing shipping with Xcode can target
   armv6.

3. **Build the `arm-apple-darwin11-clang` cross-compiler** against that SDK,
   using `cctools-port`'s own `usage_examples/ios_toolchain/build.sh`:
   ```
   git clone --depth 1 --branch cctools-877.8-ld64-253.9-1 \
     https://github.com/tpoechtrager/cctools-port.git
   cd cctools-port/usage_examples/ios_toolchain
   ./build.sh /path/to/entrypoint/assets/iPhoneOS6.1.sdk.tar.xz armv6
   ```
   This produces `target/bin/arm-apple-darwin11-clang` (plus `lipo`,
   `dsymutil`). Put `target/bin` on `$PATH`.

   The tag is pinned deliberately: its bundled cctools/ld64 versions
   (877.8 / 253.9) most closely match what BigBoss's historical "iOS
   Toolchain" Cydia package shipped (877.5 / 253.3, per
   https://theapplewiki.com/wiki/Dev:On-device_toolchains) — the same lineage
   of tooling the original jailbreak-scene binaries this project replaces
   were almost certainly built with.

   **UNVERIFIED on a macOS host.** cctools-port describes itself as a port
   "for Linux and \*BSD", and `ios_toolchain/build.sh` is documented only for
   those hosts — reasonably, since macOS already has native cctools and the
   script's whole purpose is to supply them where they're missing. It has not
   been run here. If it doesn't build cleanly, the plain-cctools route
   (`cd cctools-port/cctools && ./configure --target=arm-apple-darwin11 &&
   make && make install`) is the documented fallback, and Xcode's own `clang`
   can do the compiling as long as that `ld` does the linking.

## Building entrypoint itself

`bakeRamdisk()` (`src/BakeRamdisk.cpp`, see `buildEntrypointBinary()`) runs
`make clean all` in this directory automatically on every bake, using the
one-time toolchain setup above — there's nothing to check in, since the
build is cached in-process (see `buildEntrypointBinary()` — identical for
every firmware target, so it only actually runs once per `bake-firmware`
invocation, not once per firmware) and spliced directly into
`/sbin/launchd` on the mounted volume (`spliceFileContentInPlace()`),
preserving that file's existing permissions from the pristine Apple ramdisk.

For standalone development/testing without going through a full bake, it is
just the Makefile:

```
make -C entrypoint clean all
```

Output lands at `entrypoint/entrypoint`, a freestanding ARMv6 Mach-O, signed
with `ldid -S -Icom.apple.launchd` — that specific identifier because this
binary replaces `/sbin/launchd`'s content, so it's signed under launchd's
own well-known identifier rather than ldid's default (the binary's own
filename).

## Status

Done: SDK acquired, toolchain built, `entrypoint.c` reverse-engineered and
verified disassembly-for-disassembly against the original `sbin/launchd`
(six real bugs found and fixed along the way — see `docs/HISTORY.md`).
Wired into `bakeRamdisk()` as of this writing: every bake builds this
(cached in-process — see "Building entrypoint itself" above) and splices it
into `/sbin/launchd` in place of the real pristine binary there.

This went through a detour and back: real disassembly of an AppleTV2,1
10B809 RestoreRamdisk showed `/etc/rc.boot` is itself `LC_MAIN`-entered
directly by the kernel on that firmware, so for a while this spliced into
`rc.boot` instead, on the theory that injecting at the true first entry
point is strictly better than injecting at `launchd`. That didn't
generalize: a real bake against AppleTV3,1/AppleTV3,2 12H606 failed because
that firmware's ramdisk has no `/etc/rc.boot` at all (`/etc/` is nearly
empty there — confirmed by mounting it directly). Reverted to always
targeting `/sbin/launchd`, the one thing guaranteed to exist and be real
PID-1 across every known firmware generation — see `docs/HISTORY.md`'s
"Entrypoint injection point" entry for the full account.
