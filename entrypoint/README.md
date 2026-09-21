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
`strlen`/`memcpy`/`memset` hand-rolled. `entrypoint.c` replicates that:
`-ffreestanding -nostdlib -static`, raw syscalls, no libc.

**The reason once given for that here was wrong, and is worth correcting
rather than deleting.** This file used to claim freestanding was "almost
certainly load-bearing" because dyld and libSystem might not be functional
that early in ramdisk boot. They are. Apple's own `/sbin/launchd` on both a
real AppleTV2,1 10B809 ramdisk and a real AppleTV3,1 12H606 ramdisk is
`LC_MAIN` with `LC_LOAD_DYLINKER`, linking `libSystem.B.dylib` and
`libobjc.A.dylib` — so the genuine PID 1 Apple ships for this exact boot is
dynamically linked, and dyld demonstrably works there.

Freestanding is still the right choice, for a plainer reason: it has no
dylib closure to satisfy. The ramdisk's `/usr/lib` is a fixed, minimal set
we do not control and which differs between firmwares, so every dynamic
dependency is a per-firmware compatibility risk for zero benefit in a binary
this small. See "Why this is a binary and not a shell script" below, where
that closure problem is what actually kills the obvious alternative.

## Why it used to be containerized and cross-compiled, and isn't now

This build ran inside podman for as long as the build host was Linux. The
reason was narrow and specific: producing a freestanding ARMv6 Darwin Mach-O
needs Apple's own `ld64`/`as`, and on Linux the only way to get those is
`cctools-port`, a *port* of them. (LLVM's own `lld` doesn't support the
`-static` linking this needs — verified, it warns "not yet implemented" and
still emits a dyld-dependent PIE binary.) That port, plus an ancient
iPhoneOS SDK, had no business being installed on an immutable rpm-ostree
host, so podman kept all of it out of the way.

On macOS `ld64` and `as` are simply the native tools, so the container has
nothing left to provide and is gone.

**Neither does the cross-toolchain.** This file used to require a
cctools-port build of `arm-apple-darwin11-clang`, which in turn required a
real iPhoneOS 6.1 SDK extracted from a 1.7GB archive.org copy of Xcode 4.6.
That whole chain is unnecessary on a Mac and has been dropped: cctools-port
exists to supply Apple's `ld64`/`as` to hosts that lack them. Apple's own
`clang` still has the ARM backend, and Apple's own `ld` still lists `armv6`
in `ld -v`'s supported-arch line, so plain `-arch armv6` produces exactly
the artifact needed. Verified on Apple clang 21 / ld-1267 against this
directory's real `entrypoint.c`: Mach-O `armv6`, `LC_UNIXTHREAD`, zero
`LC_LOAD_DYLIB`, `_entry` as the thread-state PC, `ldid`-signed as
`com.apple.launchd`.

**That last sentence used to read "the only remaining requirement is `ldid`",
and it no longer does.** The stock toolchain still works — it is
`XCODE_TOOLCHAIN=system` now — but it is not what this builds with by
default, because of the mechanism the next section measures. A pinned
era-appropriate Xcode is a real prerequisite again; see "Pinned toolchain".

### What actually holds this up: `ld-classic`, not the SDK

The sentence above — "Apple's own `ld` still lists `armv6`" — is true but
understates how that works, and the mechanism is the real long-term risk to
this build. Apple's *current* linker never implemented 32-bit ARM at all.
`ld` detects those architectures and silently delegates to **`ld-classic`**,
a frozen fork of the old linker still shipped inside Xcode. Measured on this
host (Apple clang 21.0.0, `ld-1267`, built June 2026):

```
$ ld -v
@(#)PROGRAM:ld  PROJECT:ld-1267
configured to support archs: armv6 armv7 armv7s arm64 ...
will use ld-classic for: armv6 armv7 armv7s i386 armv6m armv7k armv7m armv7em

$ clang -arch armv7 -marm ... -Wl,-v
@(#)PROGRAM:ld-classic  PROJECT:ld64-957.1
```

So every 32-bit ARM link here runs through a ~3.5 MB binary at
`XcodeDefault.xctoolchain/usr/bin/ld-classic` that is pinned at `ld64-957.1`
and receives no development. Apple has called the explicit `-ld_classic` flag
deprecated since Xcode 15, yet the automatic delegation is still present three
years later. Neither `-arch armv6` nor `-arch armv7` currently emits any
warning or deprecation notice.

**The consequence for planning:** the day `ld-classic` is removed, this build
breaks — and so would any alternative that compiles ARM32 on a Mac, because
they all route through the same linker. In particular, **pinning an iPhoneOS
SDK would not protect against this**. An SDK is headers plus link stubs; it
contains no compiler and no linker. The exposure lives entirely in the
toolchain.

The mitigations that actually address it, in increasing order of effort:

1. **Commit the built `entrypoint`** (13 KB). A reproducible build is better,
   but a checked-in artifact means a dead toolchain cannot stop a release.
2. **Vendor `ld-classic`** — one 3.5 MB binary — or pin a whole Xcode.
   **This is what shipped.** See "Pinned toolchain" below; it is the default
   now, not an option.
3. **Keep `make CC=arm-apple-darwin11-clang` documented and working.**
   `cctools-port` is a *source* port of `ld64`/`as`, so it is immune to Apple
   removing anything from its own toolchain. This is the half of the deleted
   cross-toolchain dependency that genuinely bought insulation, which is why
   the `Makefile`'s `CC` override is kept rather than ripped out. It now lives
   behind `XCODE_TOOLCHAIN=system`, since `CC` is only consulted on that path.

Correction to this section's own history, for accuracy: it says the old
iPhoneOS 6.1 SDK came from "a 1.7GB archive.org copy of Xcode 4.6", implying
that route is now unofficial. It is not. Apple still serves these itself.
Xcode 6.4 — which carries the **iPhoneOS 8.4** SDK, matching the `12H1006`
migration target — is at:

```
https://download.developer.apple.com/Developer_Tools/Xcode_6.4/Xcode_6.4.dmg
```

(The `developer.apple.com/services-account/download?path=...` spelling of the
same file is the account-UI wrapper around that asset path — same download,
same requirement, not a fallback worth listing separately.)

**It needs an authenticated Apple ID session, so it cannot be fetched
unattended.** Verified: an anonymous request to that URL 302s to
`developer.apple.com/unauthorized/` and returns that page's HTML with a 200,
which means a naive `curl -f` or a `%{http_code}` check *succeeds* while
downloading 257 bytes of redirect and then an error page instead of a 2.6 GB
disk image. Any script that fetches this must verify the payload (size and
content type), never the status code. CI cannot fetch it at all without
credentials.

So availability is not the argument against using an SDK — it is obtainable,
officially, today. The arguments are the two above it: an SDK does not mitigate
the `ld-classic` exposure, because it contains no linker; and its link stubs
describe a *different* build from the one the binary will actually run on. See
`docs/HISTORY.md` for the measured symbol-delta evidence behind that second
point, and for why linking against dylibs extracted from the target ramdisk has
a structurally zero delta instead.

## Pinned toolchain

This is how `entrypoint` builds. It is the default and the documented path,
not an option for enthusiasts — the section above is the justification, and
the short form is that the stock toolchain's 32-bit ARM support is a silent
delegation to a frozen binary Apple has called deprecated since Xcode 15.
A pinned Xcode's own `ld64` carries `armv6`/`armv7`/`armv7s` as first-class
**native** architectures in one binary, with no delegation at all:

```
$ .../Xcode_6.4/.../usr/bin/ld -v
@(#)PROGRAM:ld  PROJECT:ld64-242.2
configured to support archs: armv6 armv7 armv7s arm64 i386 x86_64 ...
```

No `will use ld-classic for:` line. That is the entire point.

### Which Xcode, and for which device

| Xcode | SDK | `ld64` | Device | Target build |
|---|---|---|---|---|
| **6.4** | iPhoneOS8.4 | 242.2 | AppleTV3,1 / AppleTV3,2 | 12H1006 |
| **5.1.1** | iPhoneOS7.1 | 236.4 | AppleTV2,1 | 11D258 |

Both run on an Apple Silicon host under Rosetta 2 — verified, they are
x86_64 binaries and Rosetta handles them without complaint.

**Today the SDK column does not matter**, because this build is
`-ffreestanding -nostdlib` and links no SDK at all; either Xcode produces an
equivalent artifact. **It will matter shortly.** The conversion of
`entrypoint.c` to a dynamically linked binary (queued behind a pending
hardware run) does link an SDK, and the pairing is not interchangeable: an
8.4 SDK measured against a 7.1.2 device advertises **787 symbols the device
does not export** — the class that links clean and dies at load, which on this
hardware is a silent death with no console. So the mapping is real in the
`Makefile` now (`XCODE_FOR_AppleTV2,1` and friends) rather than left as a
comment, and `src/BakeRamdisk.cpp` carries a bake-time symbol-closure check
that turns that failure class into a loud bake failure.

### Getting them

```
https://download.developer.apple.com/Developer_Tools/Xcode_6.4/Xcode_6.4.dmg
https://download.developer.apple.com/Developer_Tools/xcode_5.1.1/xcode_5.1.1.dmg
```

Apple still serves both, officially, today. **An authenticated Apple ID
session is required, so neither can be fetched unattended.** An anonymous
request 302s to `developer.apple.com/unauthorized/` and returns that page's
HTML **with a 200**, so a naive `curl -f` or a `%{http_code}` check *succeeds*
while writing 257 bytes of error page instead of a multi-gigabyte disk image.

**Verify the payload, not the status code**, and not a published hash either:

- Check the size and that `file` calls it a disk image, not HTML.
- Check the Apple signature chain: `codesign -dvvv` on the mounted
  `Xcode.app` should report Apple's own authority chain.
- Do **not** gate on a published SHA-1. Xcode 6.4's matches its published
  value, but **5.1.1's does not** — Apple re-signed and re-served some DMGs in
  2019, so the archived hashes for those are stale. The signature chain is the
  check that still means something.

### The interface

Everything below is settable from the **environment** as well as the command
line, which is the whole plumbing story — see "Building entrypoint itself".

| Variable | Default | Meaning |
|---|---|---|
| `XCODE_TOOLCHAIN` | `auto` | `auto`, a path, or `system` (see below) |
| `XCODE_SEARCH_DIR` | `~/Downloads` | where `auto` looks |
| `XCODE_VERSION` | `6.4` | which Xcode `auto` looks for |
| `DEVICE` | *(unset)* | picks `XCODE_VERSION` from the table above |
| `LDID` | `ldid` | ad-hoc signer |
| `CC` | `clang` | consulted **only** under `XCODE_TOOLCHAIN=system` |

`XCODE_TOOLCHAIN` has three states:

- **`auto`** (the default) — discover a pinned Xcode under
  `$XCODE_SEARCH_DIR`. It accepts an already-extracted tree
  (`Xcode_6.4.app`, `Xcode_6.4`, `xcode_6.4`), an already-mounted volume
  (`/Volumes/Xcode_6.4`, `/Volumes/Xcode`), or a whole `.dmg`
  (`Xcode_6.4.dmg`, `xcode_6.4.dmg`), which it attaches `-nobrowse -readonly`
  for the build and detaches afterwards. Version is confirmed against the
  bundle's own `version.plist` where there is one, so an unrelated
  `/Volumes/Xcode` holding a current Xcode is rejected rather than used.

- **a path** — used verbatim. `$XCODE_SEARCH_DIR` discovery is **completely
  bypassed**, so a CI runner never goes looking in a home directory that will
  not exist there. Three shapes resolve, and they are checked in this order:

  | Shape | Probe |
  |---|---|
  | bare extracted toolchain root (also an unpacked `.xctoolchain`) | `<path>/usr/bin/clang` |
  | an `Xcode.app` bundle | `<path>/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang` |
  | a directory *containing* `Xcode.app` (a mounted DMG's volume root) | `<path>/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang` |

  The bare root is the shape CI is expected to use — it is a few tens of MB
  rather than a 2.6 GB app bundle.

- **`system`** — explicit opt-out: build with `$(CC)` off `$PATH`, the way
  this used to build by default. A deliberate escape hatch.

**There is no silent fallback, ever.** If a pinned toolchain is asked for and
cannot be resolved, the build fails and prints the path (or search directory)
it tried, every shape it looked for, and how to fix it. Falling back to the
system compiler on a bad path would mean a green build that quietly used the
wrong toolchain — worse than a red one, and the exact failure the pin exists
to prevent. `system` is the only way to reach the stock compiler, and you have
to type it.

The build knows **nothing about how the toolchain got there**: no Git LFS, no
split parts, no reassembly, no fetching, no cache. It takes an Xcode that is
already in one piece — a directory or a whole `.dmg` — and builds. Assembling
one from parts is a CI concern and lives in CI.

### Examples

```sh
make -C entrypoint clean all                       # ~/Downloads, Xcode 6.4
make -C entrypoint clean all DEVICE=AppleTV2,1     # ~/Downloads, Xcode 5.1.1
make -C entrypoint clean all XCODE_TOOLCHAIN=/opt/xcode-6.4-toolchain
make -C entrypoint clean all XCODE_SEARCH_DIR=/mnt/toolchains
make -C entrypoint clean all XCODE_TOOLCHAIN=system   # stock compiler, on purpose
```

Theos was tried first and dropped — its `tool.mk` template assumes exactly
the opposite of what this binary needs (dynamic linking against `libSystem`,
`LC_MAIN`, a normal `main(argc,argv,envp)` fed by crt startup glue), and its
other conveniences (Logos, `.deb` packaging) don't apply to a binary that
gets spliced directly into a ramdisk rather than installed via dpkg.

## One-time setup

```
brew install ldid
```

…plus **`Xcode_6.4.dmg` and `Xcode_5.1.1.dmg` in `~/Downloads`**. Those two
are a real prerequisite, not a nicety: the build uses a pinned toolchain by
default and fails rather than falling back to the system compiler. See
"Pinned toolchain" above for which device each serves, where to get them, and
how to point the build somewhere other than `~/Downloads`
(`XCODE_TOOLCHAIN=<dir>` or `XCODE_SEARCH_DIR=<dir>`).

`ldid-procursus` also provides an `ldid` and works here; the two formulae
conflict, so pick one.

Nothing is vendored and **no SDK is needed** — this binary is freestanding,
so only the pinned Xcode's compiler and linker are used, never its headers or
link stubs. (That changes when `entrypoint.c` goes dynamic; the pin is already
sized for it.)

If you deliberately want the stock Xcode Command Line Tools instead, that is
`make XCODE_TOOLCHAIN=system` — it still produces a correct artifact, it just
routes through `ld-classic`. A cctools-port cross-toolchain still works too:
`make XCODE_TOOLCHAIN=system CC=arm-apple-darwin11-clang`.

This was three steps until recently: `ldid`, an iPhoneOS 6.1 SDK, and a
cctools-port toolchain built against it. The SDK is still not needed, and the
cctools-port toolchain is optional — but the pinned Xcode above replaced
"nothing at all" as the toolchain requirement, deliberately.
`entrypoint/assets/README.md` still documents how to produce
`iPhoneOS6.1.sdk.tar.xz`, but nothing in the build reads it any more — keep it
for the record, not as a prerequisite.

## What actually runs as PID 1

Established by disassembling real decrypted firmware, because this has been
guessed wrong here twice. Two decrypted kernelcaches (AppleTV3,1 10B809 and
AppleTV3,1 12H606) and two mounted RestoreRamdisks (AppleTV2,1 10B809 and
AppleTV3,1 12H606) all agree:

- **The kernel execs `/sbin/launchd`, and only that.** It is the sole entry
  in XNU's `init_programs[]`, a compile-time constant. In the 12H606
  kernelcache the string sits in `__TEXT,__cstring` in a contiguous run with
  `-s`, `/dev/null`, `stack_guard=` and `malloc_entropy=` — `load_init_program()`
  building its argv and apple vector, verbatim.
- **No boot-arg redirects it.** There is no `launchdsuffix`, no
  `launchd.debug`, no `launchd.development`; those exist only in
  DEVELOPMENT/DEBUG kernels and these are RELEASE. Booting `-s` only sets
  `RB_SINGLE`, which is useless here because the ramdisk has no shell to
  drop into.
- **`/etc/rc.boot` is never touched by the kernel.** Neither kernelcache
  contains that string at all. It is real on 10B809, and it really is an
  `LC_MAIN` Mach-O, which is what the earlier claim here got right — but the
  only binary that references it is `/bin/launchctl`, and `rc.boot` itself is
  an 8880-byte dyld-linked stub whose entire string payload is four paths:
  `restored_external`, `restored_update`, `restored`, `ramrod`. The real
  chain is kernel -> launchd -> launchctl -> rc.boot -> restored. On 12H606
  `rc.boot` is gone entirely and
  `/System/Library/LaunchDaemons/com.apple.restored_external.plist` does that
  job instead.

So targeting `/sbin/launchd` is correct, and correct for every firmware
generation — not merely the safer of two options.

**One alternative this opens up, not implemented.** That `/sbin/launchd`
string is ordinary C string data at a known file offset, and blackb0x already
patches this kernelcache. Overwriting it with a path of 13 bytes or fewer
(keeping the NUL in place) would make the kernel exec our binary directly,
leaving Apple's launchd untouched and removing the splice entirely. The cost
is a new per-firmware kernel patch to maintain across all 95 known tuples,
where the splice is firmware-independent today. Recorded as an option, not a
recommendation.

## Why this is a binary and not a shell script

A shebang would be much less machinery than a freestanding ARM binary, so
this was investigated properly rather than assumed. **The kernel side works.**
The 12H606 kernelcache contains XNU's `execsw[]` dispatch strings contiguously
— `Mach-o Binary`, `Fat Binary`, `Interpreter Script` — so `exec_shell_imgact`
is compiled in, and PID 1 reaches it through the same `execve` path as
everything else. A `#!` line on `/sbin/launchd` would genuinely be honored.

**The interpreter is what kills it.** Neither ramdisk has a shell anywhere.
The complete contents of `/bin` on both is `cat`, `expr`, `launchctl`, `ln`,
`mkdir`, `mv`, `rm`. No `sh`, no `cp`, no `chmod`, no `chown`, no `find`, no
`test`.

Pointing the shebang at a shell we stage ourselves (`/blackb0x/bin/bash`, which
the bake does put on the ramdisk) gets closer, and still does not clear it.
Cydia's `bash_4.0.44-16` is dynamically linked with absolute install names:

| bash needs | on the ramdisk? |
|---|---|
| `/usr/lib/libSystem.B.dylib` | present |
| `/usr/lib/libgcc_s.1.dylib` | present |
| `/usr/lib/libreadline.6.0.dylib` | **missing** |
| `/usr/lib/libhistory.6.0.dylib` | **missing** |
| `/usr/lib/libncurses.5.dylib` | **missing** |

Our copies of those three land at `/blackb0x/usr/lib/`, not `/usr/lib/`, and
there is no dyld shared cache on either ramdisk to satisfy them another way.
As PID 1 there is no parent process to set `DYLD_FALLBACK_LIBRARY_PATH`.
Coreutils is friendlier — every binary in `coreutils-bin` needs only
`libSystem.B`, `libgcc_s` and `libiconv.2` — but that is still one more
dylib to place.

It is fixable, by staging those dylibs into the ramdisk's real `/usr/lib/`
or rewriting install names at bake time. It is just not cheaper: it trades
one self-contained static binary, built by the stock toolchain in one
compiler invocation, for a 2009 shell plus a hand-placed dylib closure plus
a coreutils closure, all running as PID 1 on a shell-less ramdisk under a
patched kernel. The binary stays.

## Building entrypoint itself

`bakeRamdisk()` (`src/BakeRamdisk.cpp`, see `buildEntrypointBinary()`) runs
`make clean all` in this directory automatically on every bake, using the
one-time setup above (`ldid`, plus the pinned Xcode) — there's nothing to
check in, since the build is cached in-process (see `buildEntrypointBinary()`
— identical for every firmware target, so it only actually runs once per
`bake-firmware` invocation, not once per firmware) and spliced directly into
`/sbin/launchd` on the mounted volume (`spliceFileContentInPlace()`),
preserving that file's existing permissions from the pristine Apple ramdisk.

**How the toolchain choice reaches the `Makefile`: the environment, and
nothing else.** `runCommand()` is `fork()`/`execvp()`, which inherits
`environ`, and make imports the environment as make variables — and every
toolchain knob in the `Makefile` is `?=`. So

```sh
XCODE_TOOLCHAIN=/opt/xcode-6.4-toolchain sudo -E ./build/bake-firmware ...
```

reaches the build untouched. There is deliberately **no `bake-firmware`
flag** for this: it would only re-spell an interface that already works, and
would then have to be threaded through every caller. A CI step sets one
environment variable and needs no other setup.

`buildEntrypointBinary()` runs once for the whole invocation, so it does not
set `DEVICE` today — correct, because a freestanding binary links no SDK and
every target gets an equivalent artifact. When `entrypoint.c` goes dynamic
that call moves inside `BakeFirmware.cpp`'s per-firmware loop and starts
setting `DEVICE=<model>`; the `Makefile` already honours it.

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

### The bake-time symbol-closure check

Every bake, with the real ramdisk mounted, `verifyEntrypointRuntimeClosure()`
(`src/BakeRamdisk.cpp`) proves the binary just spliced over PID 1 can actually
load on *that* volume: every `LC_LOAD_DYLIB` and the `LC_LOAD_DYLINKER` target
must exist on the ramdisk, and every undefined symbol must be exported by
something under `/usr/lib` or `/usr/lib/system`. A miss fails the bake with
the symbol names listed.

It is a **no-op today** — freestanding means zero undefined symbols, zero
`LC_LOAD_DYLIB`, no `LC_LOAD_DYLINKER` — and that is why it landed now rather
than alongside the dynamic conversion: it gets exercised by the existing bakes
on all three devices while it still cannot fail. It is not in CI because the
bake has something CI does not: the exact libraries that device will boot,
per device and per build, with no stored snapshot to drift.

## Status

Done: `entrypoint.c` reverse-engineered and verified
disassembly-for-disassembly against the original `sbin/launchd` (six real
bugs found and fixed along the way — see `docs/HISTORY.md`). Wired into
`bakeRamdisk()`: every bake builds this (cached in-process — see "Building
entrypoint itself" above) and splices it into `/sbin/launchd` in place of the
real pristine binary there.

Building now goes through the **pinned toolchain** by default. Verified on
this host against the real `entrypoint.c`, with both pinned Xcodes and with
the stock one, all three producing the same invariants — `Mach-O executable
arm_v6`, `LC_UNIXTHREAD` with `_entry` as the thread-state PC, zero
`LC_LOAD_DYLIB`, zero `LC_LOAD_DYLINKER`, `ldid`-signed `com.apple.launchd`:

| Toolchain | `ld` | size |
|---|---|---|
| Xcode 6.4 (`DEVICE=AppleTV3,x`) | `ld64-242.2`, native armv6 | 13,184 |
| Xcode 5.1.1 (`DEVICE=AppleTV2,1`) | `ld64-236.4`, native armv6 | 13,152 |
| stock (`XCODE_TOOLCHAIN=system`) | `ld-1267` → `ld-classic` `ld64-957.1` | 13,200 |

They are functionally equivalent but not byte-identical, which is worth
knowing when attributing a behaviour change.

**Nothing here has been booted on a device.** The pin is a supply-chain hedge
against Apple removing `ld-classic`; it fixes nothing that is currently
broken, and it changes no behaviour that has ever been observed on hardware.

The SDK and cross-toolchain this section used to list as prerequisites are
still not needed; see "What was here before, and why it is gone". The pinned
Xcode is a different requirement, added deliberately — see "Pinned toolchain".

This went through a detour and back. For a while it spliced into
`/etc/rc.boot` instead of `/sbin/launchd`, on the theory that `rc.boot` was
the true first entry point. That reverted for a good reason — a real bake
against AppleTV3,1/AppleTV3,2 12H606 failed because that firmware's ramdisk
has no `/etc/rc.boot` at all — but the theory behind the detour was also
simply false, which was only established later. **The kernel never execs
`/etc/rc.boot` on any firmware**; `rc.boot` is `LC_MAIN`, which is what the
original disassembly correctly observed, but it is launched by `launchctl`,
several steps downstream of PID 1. See "What actually runs as PID 1" above
for the evidence, and `docs/HISTORY.md`'s "Entrypoint injection point" entry
for the original account.
