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
`com.apple.launchd`. The only remaining requirement is `ldid`.

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
3. **Keep `make CC=arm-apple-darwin11-clang` documented and working.**
   `cctools-port` is a *source* port of `ld64`/`as`, so it is immune to Apple
   removing anything from its own toolchain. This is the half of the deleted
   cross-toolchain dependency that genuinely bought insulation, which is why
   the `Makefile`'s `CC` override is kept rather than ripped out.

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

Theos was tried first and dropped — its `tool.mk` template assumes exactly
the opposite of what this binary needs (dynamic linking against `libSystem`,
`LC_MAIN`, a normal `main(argc,argv,envp)` fed by crt startup glue), and its
other conveniences (Logos, `.deb` packaging) don't apply to a binary that
gets spliced directly into a ramdisk rather than installed via dpkg.

## One-time setup

```
brew install ldid
```

That is the whole list. The `Makefile` needs `clang` and `ld` (Xcode Command
Line Tools, which the rest of this project already requires) plus `ldid` for
ad-hoc signing. `ldid-procursus` also provides an `ldid` and works here; the
two formulae conflict, so pick one.

Nothing is vendored and no SDK is needed. If you want to build with a
cctools-port cross-toolchain anyway, `make CC=arm-apple-darwin11-clang`
still works — it is simply no longer the documented path.

This was three steps until recently: `ldid`, an iPhoneOS 6.1 SDK, and a
cctools-port toolchain built against it. The last two are gone for the
reasons in the section above. `entrypoint/assets/README.md` still documents
how to produce `iPhoneOS6.1.sdk.tar.xz`, but nothing in the build reads it
any more — keep it for the record, not as a prerequisite.

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
one-time setup above (`ldid`, plus the Xcode Command Line Tools the rest of
the project already needs) — there's nothing to check in, since the
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

Done: `entrypoint.c` reverse-engineered and verified
disassembly-for-disassembly against the original `sbin/launchd` (six real
bugs found and fixed along the way — see `docs/HISTORY.md`), and building
with the stock macOS toolchain (Apple clang 21 / ld-1267, real build, real
artifact checks — see "One-time setup" above). Wired into `bakeRamdisk()`:
every bake builds this (cached in-process — see "Building entrypoint itself"
above) and splices it into `/sbin/launchd` in place of the real pristine
binary there.

The SDK and cross-toolchain this section used to list as prerequisites are
no longer needed at all; see "What was here before, and why it is gone".

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
