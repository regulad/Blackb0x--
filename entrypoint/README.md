# entrypoint/

Source for Blackb0x's first-boot installer binary. On the current target
firmware it is installed on the patched restore ramdisk as
**`/usr/sbin/blackb0x_entrypoint`** and started by Apple's own real
`launchd` from
`/System/Library/LaunchDaemons/xyz.regulad.blackb0x.entrypoint.plist` — see
"Run as a launchd unit, not as launchd" below, which is the design change
and its evidence. It used to be spliced over `/sbin/launchd`'s content and
run as PID 1 (a brief mid-project detour spliced into `/etc/rc.boot`
instead — see this file's own "Status" section below for why that was
tried, and why it got reverted), and it still is on the older `rc.boot`
generation of ramdisk, which cannot load a LaunchDaemon plist at all.

It is **not** real launchd and does not masquerade as it — it's a standalone
one-shot installer that runs just long enough to recursively merge
`/blackb0x` (a new top-level directory `bakeRamdisk()` stages at bake
time — see `BakeRamdisk.cpp`'s `stageBlackb0xTree()` — as a flat mirror of
the real device's final layout, every entry already carrying its correct
final owner/mode) onto the real device filesystem (mounted at `/mnt1`, one
of the pristine ramdisk's own pre-existing empty mountpoints — confirmed
directly against real, decrypted AppleTV2,1 10B809, AppleTV2,1 11D258 and
AppleTV3,x 12H1006 RestoreRamdisks: `/mnt1` and `/mnt2` both exist at the
ramdisk root and are empty; reusing one of these two is required rather
than mkdir'ing a fresh `/mnt`, since `bakeRamdisk()` adds as little as it
possibly can to the pristine ramdisk), then reboots
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

## Run as a launchd unit, not as launchd

The current design, and a deliberate divergence from the original
NSSpiral/Blackb0x (which replaced `/sbin/launchd` wholesale) and from every
version of this port before it. Apple's real `/sbin/launchd` is left byte
for byte alone; our binary is a **new file** that Apple's launchd starts as
an ordinary one-shot LaunchDaemon.

| | |
|---|---|
| binary | `/usr/sbin/blackb0x_entrypoint`, 0755 root:wheel |
| unit | `/System/Library/LaunchDaemons/xyz.regulad.blackb0x.entrypoint.plist`, 0644 root:wheel |
| `Label` / `ldid -I` | `xyz.regulad.blackb0x.entrypoint` |

`/usr/sbin` rather than `/usr/local/bin`: it is the conventional home for a
privileged system-administration binary, which is exactly what this is — it
runs as root at boot and mounts filesystems — and it sits beside `asr` and
`nvram`, the two Apple tools this install path is most analogous to, one of
which (`nvram`) `entrypoint` actually execs. The plist's ownership and mode
are load-bearing, not cosmetic: launchd refuses a plist that is not
root-owned and `0644` ("Caller specified a plist with bad
ownership/permissions").

### Why: replacing PID 1 meant nothing else on the ramdisk ever ran

That is not a side effect, it is the whole behaviour of the old design, and
it was costing two things:

- **The display.** `/usr/local/bin/restored_external` is what draws it — not
  launchd, and not the kernel. It links `IOMobileFramebuffer` and
  `IOSurface`, owns `/usr/share/progressui`, the `applelogo` and
  progress-bar assets, and carries its own `Display Info: width=%zu
  height=%zu` and `attempting to power on display port` diagnostics.
  (iBoot's `setpicture` paints the *earlier* logo, before the kernel; that
  one is already ours, via the RestoreLogo the install path sends.)
- **USB, which is the bigger one.** On this hardware the USB device stack
  stays **off the bus until a userspace process configures it**. The
  DeviceTree node `/device-tree/arm-io/usb0-complex/usb0-device` carries
  `configuration-string = "standardMuxOnly"` on both 10B329a and 12H1006,
  while the in-kernel auto-configurator personality
  (`IOUSBDeviceConfigurator`) matches only `ConfigurationType =
  "standardBringup"`, so it never fires here; `IOUSBDeviceFamily`'s own
  diagnostic says the controller just idles ("cable connected, but don't
  have device configuration yet"). The one binary on the ramdisk that calls
  `IOUSBDeviceControllerCreate` /
  `IOUSBDeviceDescriptionCreateFromDefaults` /
  `IOUSBDeviceControllerSetDescription` is `restored_external`, and that
  call is what brings the device on-bus as `05ac:12a7` with the
  `AppleUSBMux` interface.

  So by replacing `/sbin/launchd` and exec'ing nothing else, the old design
  **guaranteed the device would never enumerate**, no matter how perfectly
  `entrypoint` ran. That is a complete, sufficient explanation for "nothing
  happens" that is independent of code signing, and it explains the
  stock-vs-ours asymmetry directly.

`com.apple.restored_external.plist` and the binary it runs are therefore
**load-bearing for observability**. Nothing in the bake touches either, and
nothing should. Its startup is also safe, checked rather than assumed: it
sets an `IOPMUBootStage` property, starts a gas-gauge thread, creates a
listen socket, disables the watchdog, calls `enable_usb_connections()`, then
blocks in an accept loop — it mounts nothing and erases nothing. Every
destructive primitive it contains (`WipeStorageDevice`, `clean_NAND`,
`FormatForLwVM`, `partition_nand_device`, `asr`) sits behind a host
`StartRestore` message. **Do not point `idevicerestore`, or any other
restore client, at the device while this unit is running** — our job mounts
the NAND filesystems read-write and reboots.

### What this does NOT buy

Stated plainly, because an earlier draft of this change was sold on exactly
the claim that turns out to be false:

- **It does not dodge AMFI, and it is not a weaker check.** There is no
  PID-1-special code-signing path. The kernel's `load_init_program` →
  `execve` and launchd's `posix_spawn` (through `xpcproxy` on 12H1006) both
  land in `mac_vnode_check_signature` / AMFI's execve hook. If anything a
  LaunchDaemon is the **more** enforced position, since `load_init_program`
  runs very early in `bsd_init`. The baked boot-args (`src/Patcher.hpp`'s
  `bootargs`) remain the only thing that disables enforcement.
- **It therefore does not settle "rejected and never ran" vs "ran and died
  silently".** If AMFI is refusing us we will now see Apple's logo and then
  nothing — a *new* ambiguity, not a resolved one. Do not read a logo as
  proof our binary started.
- **"Ad-hoc signed" is not the discriminator either.** Apple's own ramdisk
  binaries are all ad-hoc signed too — `launchd`, `launchctl`,
  `restored_external` and `rc.boot` all carry flags `0x2` and no
  entitlements. There is no `amfid` on either ramdisk, so whatever admits
  them is purely in-kernel, and the obvious static-trust-cache hypothesis was
  tested and failed: none of their CDHashes appear in the kernelcache, not
  even as an 8-byte prefix. The mechanism is genuinely unknown.

What it does buy: a live `/dev/console` for our own output
(`StandardOutPath`/`StandardErrorPath`, copied from Apple's plist; with `-v`
in the baked boot-args iBoot puts the framebuffer in text-console mode, so
`entrypoint`'s `printf` lands on HDMI verbatim), plus proof that `md0`
mounted and real launchd ran, plus USB enumeration — which is what makes
anything on the device reachable at all, since `AppleUSBDeviceMux` is
prelinked into both kernelcaches and does TCP-over-USB in-kernel.

### Where output goes, and why the install log is not it

`entrypoint.c` now has **one output stream and does not configure it**.
Everything it says is a plain `printf` (progress) or `fprintf(stderr, …)`
(errors), and launchd attaches both to `/dev/console` from the two
`Standard*Path` keys. The two functions that used to do this by hand are
gone: `console_print()`, which opened `/dev/console` itself and `dup2`'d it
onto fds 1 and 2 from `main()`, and `log_to_file()`, a second and entirely
independent writer that `open`/`append`/`write`/`close`/`chown`/`chmod`'d
the on-NAND log on *every message*. With one writer, **message ordering is
now guaranteed by construction**; two writers to two destinations could
interleave arbitrarily, and which sink a given message used was inherited
verbatim from the original binary's own inconsistencies rather than from
anything meaningful.

**The plist cannot point `StandardOutPath` at the on-NAND log**, and this is
the one real constraint in the design. launchd opens that path when it
*spawns* the job, long before `entrypoint` has mounted anything;
`/var/mobile/Media/blackb0x_install.log` lives under `/mnt1`, i.e. on the
NAND. Worse, the failure would be silent rather than loud: launchd would
create the file on the **ramdisk**, under the still-empty `/mnt1`
mountpoint, we would then mount the real volume over the top, and every byte
would land in a shadowed file that dies with the RAM disk.

The obvious alternative — point `StandardOutPath` at a path on the ramdisk
and copy the finished file onto the NAND once `/mnt1` is mounted — was
**considered and rejected**, for two reasons:

1. It assumes the ramdisk root is mounted **read-write** at the moment
   launchd spawns us. Nothing in this project establishes that. If it is
   read-only, launchd's `open()` fails, the job gets `/dev/null` for stdout,
   and we are totally blind with no diagnostic at all — precisely the
   failure this whole redesign exists to stop producing. `/dev/console` is
   the one sink *proven* to work on this exact volume, because Apple's own
   `restored_external` uses it there.
2. It is in RAM until the copy happens, so every failure **before** the
   mount — the disk-wait loop spinning forever, a `mount(2)` refusal, a
   `panic()` — leaves nothing behind after a power cycle. Those are exactly
   the failures currently under investigation.

So the on-NAND log is a **separate, deliberate artifact rather than a
transcript**: there is no transcript to copy, since the live stream goes to
a character device nothing reads back. `write_install_record()` is the whole
of it — called once at the end of `do_install()` with the merge's real
result, and once from `panic()`, both after the volume is mounted. The
fingerprint a later boot reads is unchanged in path
(`/var/mobile/Media/blackb0x_install.log`) and owner (501:20,
`mobile:staff`).

Two fixes went in with it:

- **Mode is 0644 now, not 0755.** The old code `chmod`'d the log 0755 on
  every write. That was the original binary's behaviour, faithfully
  reproduced; an executable bit on a text file read by a later boot has no
  meaning to grant.
- **A missing `/var/mobile/Media/` no longer swallows the artifact.** The
  old `open(…, O_CREAT)` ignored its result, so a freshly-erased NAND
  produced no log and no complaint. The directory (and its parent) is
  created if genuinely absent — never touched if it already exists, so a
  real device keeps its own ownership — and every failure along the way is
  reported on stderr.

**Known consequence on the legacy `rc.boot` generation**, recorded rather
than silently accepted. Those ramdisks cannot load a plist, so
`installEntrypoint()` still splices this binary over `/sbin/launchd` there
and it runs as PID 1 — with nothing to set up its fds, because the kernel
does not open `/dev/console` for init. (That is exactly why the original
binary opened it by hand; the disassembly shows the `open`/`dup2` as its
first act.) So on that generation stdout and stderr are closed and every
`printf` fails with `EBADF`. That path is already the one documented as
producing a dark, un-enumerated device with no `restored_external` and no
display, and it is explicitly not endorsed — so what is lost is a
diagnostic channel that was already of little use there. If it is ever
wanted back, the honest shape is a guard that fires **only** when launchd
did not provide a stream (`if (fcntl(1, F_GETFD) == -1) { … }`), never an
unconditional re-open that would fight the plist on the primary path.

`main()` sets both streams **unbuffered** (`setvbuf(_IONBF)`). That is what
replaces `console_print()`'s original reason for existing: stdio
line-buffers on a tty and *fully* buffers if `/dev/console` ever fails the
`isatty()` test, and this process has two paths that never return —
`panic()`'s sleep loop and `reboot(2)` — where a buffered tail is a lost
tail.

### What changed meaning now that we are not PID 1

- **`panic()` got strictly better.** Hanging as PID 1 froze the machine:
  launchd never ran, so nothing else on the ramdisk did either, and the
  device was dark and off the USB bus. Hanging as an ordinary LaunchDaemon
  leaves Apple's launchd and `restored_external` alive, so the display stays
  lit and the device stays enumerated while the process sits there. A panic
  is now observable rather than indistinguishable from a brick, and
  `KeepAlive` is `false`, so launchd will not respawn into a loop.
- **Returning from `main()` is no longer catastrophic.** An early
  `return -1` used to mean init exiting, which takes the system with it. Now
  it is an ordinary job exit with an ordinary status; launchd stays up and
  the failure is something a host can look at. The error paths keep their
  original return values — nothing downstream reads them, and inventing an
  exit-code scheme with no reader would be noise.
- **`reboot(0)` is the one place this makes us ruder, not safer.** Apple's
  launchd and `restored_external` are alive now and get no `SIGTERM`. That
  is accepted deliberately: `restored_external` is idle in an `accept()`
  loop, holds no mount of its own and writes nothing (every destructive
  primitive it has sits behind a host `StartRestore` message), and both NAND
  filesystems are unmounted and `sync()`'d before the call. A
  `launchctl reboot`-style shutdown would need `/bin/launchctl` and a live
  bootstrap port and would buy nothing. What is *not* acceptable is a
  host-side restore client being connected while this runs — a usage rule,
  not something this code can defend against.
- **`set_auto_boot()` is unchanged, and that was checked rather than
  assumed.** It waits with `wait4(pid, …)`, naming its one child. As PID 1
  this process was also the reaper of every orphan on the system, so a bare
  `wait()` would have been ambiguous; as an ordinary daemon there is exactly
  one child and orphan reaping is Apple's launchd's problem again. The
  explicitly empty `envp` is likewise still right.

### The unit, key by key

Modelled on `com.apple.restored_external.plist`, read directly off a real
decrypted 12H1006 ramdisk.

- **`RunAtLoad`** `true` — the whole point; there is no other trigger.
- **`KeepAlive`** `false`, stated explicitly. A `RunAtLoad` job that exits is
  *not* respawned without `KeepAlive`, so plain omission would have been
  correct too; this documents the intent on a plist whose whole reason to
  exist is that a respawn loop here would be invisible on a device with no
  console. (If `KeepAlive` were ever set true, `ThrottleInterval` defaults to
  10s, so a one-shot that exits immediately would respawn every ten seconds
  forever.)
- **No `OnDemand`.** It is launchd 1.0's legacy spelling of the same bit
  (`KeepAlive` sets `ondemand = !value`, `OnDemand` sets `ondemand = value`,
  so the two are consistent and dictionary order cannot matter), but
  12H1006's launchd is the XPC-based launchd 2.0 where it is deprecated and
  buys nothing `KeepAlive` does not already say.
- **`POSIXSpawnType`** `Interactive`, copied from `restored_external`. Darwin
  maps this to a scheduling/jetsam role; a `Background` role carries a
  throttled I/O tier, which is the wrong thing for a job whose entire work is
  a large recursive file merge.
- **`Umask`** `0`, also copied. `merge_tree()` chmods explicitly afterwards,
  so a nonzero umask would not survive anyway — but the window between create
  and chmod is real, and `0` removes it.
- **No `UserName`.** LaunchDaemons run as root by default. Setting it would
  make launchd resolve `"root"` through `getpwnam()`, and these ramdisks ship
  `/etc/master.passwd` with no `/etc/passwd` and no `opendirectoryd` running.
  Not a question worth having. (`package/layout/`'s
  `xyz.regulad.blackb0x.postinstall.plist` *does* set it; that one runs on a
  real, fully booted OS.)
- **Both output paths at `/dev/console`**, which looks like it contradicts
  this repo's own note that pointing `StandardOutPath` and
  `StandardErrorPath` at one path is a long-documented launchd bug. That bug
  is about two independent file descriptions on a *regular file*, whose
  independent offsets clobber each other. `/dev/console` is a character
  device with no offset, and Apple's own `restored_external.plist` on this
  very ramdisk points both at it.
- **XML, not a binary plist**, even though all three of Apple's own plists
  here are binary. launchd parses both, every jailbreak package in this
  ecosystem ships XML, this project's own on-device plist is XML, and it
  keeps `plutil` off the bake's runtime-dependency list.
- **Not modelled on `xyz.regulad.blackb0x.postinstall.plist`.** That one runs
  `/bin/bash /var/.blackb0x/postinstall.sh`, and **there is no shell on
  either ramdisk** — `/bin` is exactly `cat expr launchctl ln mkdir mv rm`.
  `ProgramArguments` here must name a real binary.

There are **no ordering primitives**: this launchd has no
`Requires`/`After`/`Before`, and the boot-time load is a single directory
scan in which `RunAtLoad` jobs start in enumeration order. Anything that must
happen after the display is up has to poll or sleep. Nothing artificial is
added — `entrypoint`'s first action is already a wait loop on
`/dev/disk0s1s1` appearing, which staggers it naturally.

### It is per-firmware, and the older generation cannot use it

Measured on three real decrypted ramdisks. The boundary is the iOS 7 → 8
launchd rewrite:

| generation | firmwares | init |
|---|---|---|
| LaunchDaemons | AppleTV3,x **12H1006** | `launchd` scans `/System/Library/LaunchDaemons` (three plists: ReportCrash.restored, restored_external, syslogd); no `/etc/rc.boot` |
| `rc.boot` | AppleTV3,2 **10B329a**, AppleTV2,1 **11D258** | no `LaunchDaemons` anywhere, no `/Library`, no `launchd.conf`; `/etc/rc.boot` instead |

**A plist dropped on an `rc.boot`-generation ramdisk reaches nothing**, for
two independent reasons read out of the real binaries:

1. That generation's `/sbin/launchd` contains **zero** occurrences of the
   string `LaunchDaemons` — it has no daemon-directory loader at all and
   shells out to `/bin/launchctl`. `launchctl`'s only reference to
   `/System/Library/LaunchDaemons` is an `opendir` loop that `strncmp`s each
   entry against `com.apple.jetsamproperties.` — the jetsam-properties
   lookup, not a job loader.
2. Even if it were a job loader, `launchctl`'s `system_specific_bootstrap()`
   `fwexec()`s `/etc/rc.boot` first (`vfork` + `waitpid`) and blocks there for
   the whole life of the restore. `/etc/rc.boot` (8832 bytes on 11D258, 8880
   on 10B329a; armv7 Mach-O, ad-hoc, `com.apple.rc`) `getfsfile()`s the root
   entry, `mount()`s it, `umask(0)`, then `execl`s each of
   `/usr/local/bin/restored_external`, `restored_update`, `restored` and
   `/usr/libexec/ramrod/ramrod` in turn — replacing its own process image on
   the first that exists, which is always `restored_external`. It never
   returns.

So those firmwares keep the old splice over `/sbin/launchd`, unchanged, with
a loud bake-time warning. That is explicitly **not** an endorsement — it is
the configuration known to produce a dark, un-enumerated device — but it is
what they have always had, and regressing them to "bake fails" would be
worse. `bakeRamdisk()` chooses per firmware (`installEntrypoint()`), and
fails outright on a ramdisk that has neither hook.

**The right fix for that generation**, when someone takes it on, is to
replace `/etc/rc.boot` with a binary that does our work and then `execl`s
`/usr/local/bin/restored_external` itself, preserving the display and USB
bring-up. Two things to know first: the "Status" section below records an
`rc.boot` attempt being tried and reverted, and that warning **does not
apply** — it was reverted because 12H606's `/etc` has no `rc.boot`, which is
precisely the firmware that has `LaunchDaemons` instead. The two generations
want opposite mechanisms and each is wrong on the other. And on 10B329a the
parent `restored_external` reboots the device when its `-server` child exits.

## Freestanding, and why it isn't any more

The original binary is genuinely freestanding — confirmed via its Mach-O
load commands (`LC_UNIXTHREAD`, zero `LC_LOAD_DYLIB` entries): no libSystem,
no dyld, every syscall made directly via `mov r12, #N; svc #128`, even
`strlen`/`memcpy`/`memset` hand-rolled. `entrypoint.c` replicated that for a
long time — `-ffreestanding -nostdlib -static`, raw syscalls, no libc — and
**no longer does. It is a dynamically linked armv7 binary now.**

Two arguments were made for freestanding here over time. The first was
simply wrong and is worth correcting rather than deleting: this file used to
claim freestanding was "almost certainly load-bearing" because dyld and
libSystem might not be functional that early in ramdisk boot. They are.
Apple's own `/sbin/launchd` on both a real AppleTV2,1 10B809 ramdisk and a
real AppleTV3,1 12H606 ramdisk is `LC_MAIN` with `LC_LOAD_DYLINKER`, linking
`libSystem.B.dylib` and `libobjc.A.dylib` — the genuine PID 1 Apple ships
for this exact boot is dynamically linked, and dyld demonstrably works there.

The second was that freestanding has no dylib closure to satisfy, and the
ramdisk's `/usr/lib` is a fixed, minimal set we do not control. That one was
true but has been measured rather than feared, and the measurement is what
changed the answer:

- **The ramdisks carry a real, complete libc, not stubs.** `libsystem_c` has
  ~487 KB of `__text` on 6.1 and ~364 KB on 8.4, 1275 exports each, real
  prologues under `otool -tV`. Every syscall this file used to hand-roll is
  exported on both firmwares, all 30 checked by name.
- **The closure is ONE name.** `/usr/lib/libSystem.B.dylib`, present on every
  iOS ever shipped. Its `LC_REEXPORT_DYLIB` targets were walked against both
  mounted ramdisks: 22/22 and 32/32 present, zero missing transitive deps.
  There is no dyld shared cache on either ramdisk, so the individual dylibs
  are linkable directly.
- **Matched-SDK correspondence is exact.** iPhoneOS 7.1 SDK against the
  11D258 ramdisk is 0 phantom / 0 device-only across 6640 symbols; iPhoneOS
  8.4 against 12H1006 is 0 phantom. See "Pinned toolchain" for what happens
  when the pairing is crossed.

And the decisive argument is the plainest one: **a dynamically linked PID 1
is MORE similar to the stock `/sbin/launchd`, which demonstrably works.**
`blackb0x --stock-ramdisk` boots on real hardware. Our freestanding static
binary was the unusual artifact, not the conservative one. The conversion is
convergence on a known-good configuration.

It also deleted the layer where every ABI bug this project has had to hunt
down lived — the carry-flag error convention, `reboot(2)`'s real two-argument
arity, and `fork`'s child detection via r1 — plus two latent instances of the
same class that were still in the file: a `STAT_*_OFFSET` block hard-coding
the 96-byte pre-64-bit-inode layout the raw trap returns (libc's `stat()`
here IS `stat64`, so the real 108-byte `struct stat` is correct and the
offsets evaporated), and a console writer that issued one `write(2)` per
byte with a `sync()` after.

**The honest cost**: freestanding failed only in ways we wrote. Dynamic adds
failure modes that happen *before* the first instruction here runs, where
even a working console shows nothing. Those are closed at bake time rather
than hoped away — see "The bake-time symbol-closure check" below.

See "Why this is a binary and not a shell script" below for the closure
problem that still kills the obvious alternative: a staged `bash` needs
`libreadline`/`libhistory`/`libncurses`, none of which exist on either
ramdisk, whereas this binary needs only the one library that always does.

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
in `ld -v`'s supported-arch line, so plain `-arch armv6` produced exactly
the artifact needed. Verified on Apple clang 21 / ld-1267 against this
directory's `entrypoint.c` *as it then was*: Mach-O `armv6`,
`LC_UNIXTHREAD`, zero `LC_LOAD_DYLIB`, `_entry` as the thread-state PC,
`ldid`-signed as `com.apple.launchd` (the identifier of the era, when this
binary still replaced `/sbin/launchd`'s content; it is
`xyz.regulad.blackb0x.entrypoint` now).

(That paragraph is history, and the architecture in it is stale: the build is
`-arch armv7` now, because every dylib on both ramdisks is armv7 **thin** and
there is no armv6 slice to link against. The old build got away with armv6
precisely because it linked nothing. The `ld-classic` mechanism the next
section describes applies identically to both archs.)

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

1. **Commit the built `entrypoint`** (~52 KB now that it is dynamic; it was
   13 KB freestanding). A reproducible build is better,
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
downloading an error page instead of a multi-gigabyte disk image. Any script
that fetches this must verify the payload, never the status code. CI cannot
fetch it at all without credentials.

Two refinements, both measured, because the obvious checks do not work:

- **Do not size-check against a fixed number.** An earlier revision of this
  file said the unauthorized page was 257 bytes. It is now ~83 KB. It is a
  normal web page and Apple may change it again, so any threshold written down
  here is a latent false pass. Check the content type, or that the payload
  begins like a DMG, not that it is "bigger than N".
- **Do not probe the URL to test whether a version exists.** A deliberately
  invented, nonexistent path returns the same unauthorized page, so an
  anonymous probe discriminates nothing at all — a real version and a fictional
  one are indistinguishable without credentials. Use Apple's release index to
  establish what exists.

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

**The SDK column is load-bearing.** This build links against that SDK's
`libSystem.B.dylib` stub, and the pairing is not interchangeable:

| SDK vs device | phantom (SDK-only) | device-only |
|---|---|---|
| **7.1 SDK vs 11D258 (7.1.2)** | **0** | **0** |
| **8.4 SDK vs 12H1006 (8.4)** | **0** | 1 |
| 8.4 SDK vs 11D258 (7.1.2) | **787** | 61 |
| 7.1 SDK vs 12H1006 (8.4) | 61 | 788 |

A phantom is a symbol the SDK advertises that the device does not export: it
links clean and fails to bind at `dyld` load, which on this hardware is a
silent death with no console. So `DEVICE=<model>` is not optional decoration
— it selects `XCODE_VERSION` (`XCODE_FOR_AppleTV2,1` and friends), which
selects the deployment target (`IOS_MIN_FOR_6.4` = 8.4, `IOS_MIN_FOR_5.1.1` =
7.1), which selects the SDK found inside that toolchain. One chain, one place
to change it.

`src/BakeRamdisk.cpp` carries a bake-time symbol-closure check against the
real mounted ramdisk that turns any residual instance of this failure class
into a loud bake failure. See below.

(If one Xcode ever had to serve both branches, the right pick is **5.1.1**,
not 6.4 — 61 phantoms, none of them a libc primitive, versus 787. That is the
opposite of the intuitive answer, and it is only a fallback; the per-device
mapping is what ships.)

### Getting them

```
https://download.developer.apple.com/Developer_Tools/Xcode_6.4/Xcode_6.4.dmg
https://download.developer.apple.com/Developer_Tools/xcode_5.1.1/xcode_5.1.1.dmg
```

Apple still serves both, officially, today. **An authenticated Apple ID
session is required, so neither can be fetched unattended.** An anonymous
request 302s to `developer.apple.com/unauthorized/` and returns that page's
HTML **with a 200**, so a naive `curl -f` or a `%{http_code}` check *succeeds*
while writing an error page instead of a multi-gigabyte disk image. Note the
page is ~83 KB, not the 257 bytes an earlier revision of this file claimed, and
it may change again — so do not write a size threshold down anywhere.

**Verify the payload, not the status code**, and not a published hash either:

- Check that `file` calls it a disk image rather than HTML. (Size alone is a
  weak test for the reason above; the type is the honest one.)
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
| `IOS_MIN` | from `XCODE_VERSION` | deployment target / which SDK to look for (6.4 → 8.4, 5.1.1 → 7.1) |
| `IPHONEOS_SDK` | *(unset)* | an SDK path, used verbatim; normally found inside the resolved toolchain |
| `LDID` | `ldid` | ad-hoc signer |
| `CC` | `clang` | consulted **only** under `XCODE_TOOLCHAIN=system` |

The SDK is found inside whatever toolchain was resolved, which is what keeps
the compiler and its headers from ever coming out of different Xcodes. Both
layouts are probed: `<root>/SDKs/iPhoneOS<ver>.sdk` (a bare extracted
toolchain root, the shape CI hands over) and
`<root>/.../Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS<ver>.sdk` (an
`Xcode.app` bundle or a mounted DMG). `IPHONEOS_SDK` overrides that and is
required under `XCODE_TOOLCHAIN=system`, which has no pinned Xcode to look
inside.

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
make -C entrypoint clean all DEVICE=AppleTV3,2     # ~/Downloads, Xcode 6.4,  iPhoneOS 8.4
make -C entrypoint clean all DEVICE=AppleTV2,1     # ~/Downloads, Xcode 5.1.1, iPhoneOS 7.1
make -C entrypoint clean all                       # no DEVICE -> defaults to 6.4 / 8.4
make -C entrypoint clean all XCODE_TOOLCHAIN=/opt/xcode-6.4-toolchain
make -C entrypoint clean all XCODE_SEARCH_DIR=/mnt/toolchains
make -C entrypoint clean all XCODE_TOOLCHAIN=system IPHONEOS_SDK=/path/to/iPhoneOS8.4.sdk
```

Theos was tried first and dropped. Its `tool.mk` conveniences (Logos, `.deb`
packaging) don't apply to a binary that gets written straight onto a ramdisk
rather than installed via dpkg, and it has no notion of the device → Xcode →
SDK pinning this build turns on. (The original objection to it was sharper —
`tool.mk` assumes dynamic linking against `libSystem` and `LC_MAIN`, which
was "exactly the opposite of what this binary needs" while it was
freestanding, and is now exactly what it produces. That half of the argument
is gone; the rest still holds.)

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

Nothing is vendored, and **the pinned Xcode's SDK is used as well as its
compiler and linker** — headers and the `libSystem.B.dylib` link stub both.
This used to say "no SDK is needed", which was true while the binary was
freestanding and is not any more.

**`XCODE_TOOLCHAIN=system` needs one extra thing now.** A current Xcode's
`iPhoneOS.sdk` is arm64e-only (`targets: [arm64e-ios]`), so there is nothing
in it to link an armv7 binary against. Pass `IPHONEOS_SDK=<path>` pointing at
a real armv7-bearing iPhoneOS SDK alongside it; without one the build fails
and says so, rather than quietly producing something for the wrong
architecture. The same variable is how a cctools-port toolchain is fed an
SDK.

If you deliberately want the stock Xcode Command Line Tools instead, that is
`make XCODE_TOOLCHAIN=system IPHONEOS_SDK=<sdk>` — it still produces a correct
artifact, it just routes through `ld-classic`. A cctools-port cross-toolchain
still works too:
`make XCODE_TOOLCHAIN=system CC=arm-apple-darwin11-clang`.

This was three steps once: `ldid`, an iPhoneOS 6.1 SDK, and a cctools-port
toolchain built against it. An SDK is a requirement again — but it arrives
inside the pinned Xcode rather than as a separately-extracted archive, and it
is the SDK matched to the device rather than whichever one happened to be
obtainable. The cctools-port toolchain is still optional.
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

So if the goal is to BE PID 1, targeting `/sbin/launchd` is correct, and
correct for every firmware generation — not merely the safer of two options.

**That is no longer the goal**, and this section is kept because the
underlying facts are still load-bearing rather than because the conclusion
still applies. Being PID 1 turned out to be the problem, not the prize: it
means Apple's launchd never runs, so nothing else on the ramdisk does either
— no `restored_external`, therefore no display and no USB enumeration. See
"Run as a launchd unit, not as launchd" at the top. What this section still
settles, and what the new design depends on, is that **Apple's own launchd
really is what the kernel starts**, so leaving it in place is enough to get
the rest of the ramdisk running.

**One alternative, still not implemented, and now much less attractive.**
That `/sbin/launchd` string is ordinary C string data at a known file offset,
and blackb0x already patches this kernelcache. Overwriting it with a path of
13 bytes or fewer (keeping the NUL in place) would make the kernel exec our
binary directly, leaving Apple's launchd file untouched. It was recorded here
as a cleaner alternative to the splice — but it has the same fatal property
the splice had: whatever the kernel execs is the only thing that runs, so
Apple's launchd would still never start and the device would still be dark
and un-enumerated. It also costs a new per-firmware kernel patch across all
95 known tuples. Recorded as an option; not a recommendation, and a worse one
than it looked.

## Why this is a binary and not a shell script

A shebang would be much less machinery than a compiled ARM binary, so
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
one binary built in one compiler invocation, whose entire dylib closure is
`/usr/lib/libSystem.B.dylib` — the one library that is present and complete
on every ramdisk — for a 2009 shell plus a hand-placed three-dylib closure
plus a coreutils closure, all running as PID 1 on a shell-less ramdisk under
a patched kernel. The binary stays. (This argument survived the conversion to
dynamic linking intact: the problem with the shell was never "it links
things", it was *which* things.)

## Building entrypoint itself

`bakeRamdisk()` (`src/BakeRamdisk.cpp`, see `buildEntrypointBinary()`) runs
`make clean all` in this directory automatically on every bake, using the
one-time setup above (`ldid`, plus the pinned Xcode) — there's nothing to
check in, since the build is cached in-process (see `buildEntrypointBinary()`
— identical for every firmware target, so it only actually runs once per
`bake-firmware` invocation, not once per firmware) and written straight onto
the mounted volume as `/usr/sbin/blackb0x_entrypoint` alongside its
LaunchDaemon plist (`installEntrypointUnit()`). On the older `rc.boot`
generation of ramdisk it is still spliced over `/sbin/launchd`'s content
instead (`spliceFileContentInPlace()`, preserving that file's existing
permissions from the pristine Apple ramdisk) — `installEntrypoint()` picks,
see "It is per-firmware" above.

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

**OPEN, AND NOW A REAL DEFECT: `buildEntrypointBinary()` still runs once for
the whole invocation and does not set `DEVICE`.** That was correct while this
was freestanding — no SDK, one equivalent artifact for every target. It is
not correct now. `BakeFirmware.cpp`'s `main()` calls it once before its
per-firmware loop, so a multi-device `bake-firmware` run splices ONE binary,
built against ONE SDK, into every device's ramdisk — and for the devices that
do not match, that is the 787-phantom-symbol case. The call has to move
inside the per-firmware loop with `DEVICE=<model>` set in the child's
environment (`setenv()` is enough; `runCommand()` is `execvp()` and make
reads the environment). Both `BakeFirmware.cpp` and `BakeRamdisk.cpp` carry
comments marking the exact lines. Until that lands:

- **CI is safe.** Each `bake` leg is one device and exports `DEVICE` into the
  job environment, so make gets the right Xcode and SDK.
- **A local `bake-firmware` narrowed to one device is safe** if `DEVICE` is
  exported, and `bake-firmware --device <model>` does not export it.
- **A local unnarrowed `bake-firmware` is not safe** across devices, and
  `verifyEntrypointRuntimeClosure()` is what will catch it — loudly, at bake
  time, naming the symbols.

The `Makefile` already carries and honours the mapping, so this is a caller
change rather than a redesign.

For standalone development/testing without going through a full bake, it is
just the Makefile:

```
make -C entrypoint clean all DEVICE=AppleTV3,2
make -C entrypoint clean all DEVICE=AppleTV2,1
```

`DEVICE` matters here — it is what selects the matched SDK. Without it the
`Makefile` defaults to `XCODE_VERSION=6.4` / iPhoneOS 8.4, which is right for
the two AppleTV3,x models and wrong for AppleTV2,1.

Output lands at `entrypoint/entrypoint`, a dynamically linked armv7 Mach-O
(`LC_MAIN`, `LC_LOAD_DYLINKER` → `/usr/lib/dyld`, one `LC_LOAD_DYLIB` →
`/usr/lib/libSystem.B.dylib`), signed with
`ldid -S -Ixyz.regulad.blackb0x.entrypoint` — matching the LaunchDaemon's
`Label` and sitting under the same reverse-DNS prefix as the rest of the
project (the Debian package is `xyz.regulad.blackb0x`, and `package/layout/`
already ships `xyz.regulad.blackb0x.postinstall.plist`), rather than ldid's
default (the binary's own filename). It used to be `com.apple.launchd`,
because it used to replace that file's content; it no longer does, and the
identifier should not keep claiming otherwise. **This changes nothing about
code signing** — the binary is still ad-hoc signed, and so, for what it is
worth, is every Apple binary on these ramdisks.

### The bake-time symbol-closure check

Every bake, with the real ramdisk mounted, `verifyEntrypointRuntimeClosure()`
(`src/BakeRamdisk.cpp`) proves the binary just spliced over PID 1 can actually
load on *that* volume: every `LC_LOAD_DYLIB` and the `LC_LOAD_DYLINKER` target
must exist on the ramdisk, and every undefined symbol must be exported by
something under `/usr/lib` or `/usr/lib/system`. A miss fails the bake with
the symbol names listed.

**It does real work now.** It was deliberately landed while `entrypoint` was
still freestanding — zero undefined symbols, zero `LC_LOAD_DYLIB`, no
`LC_LOAD_DYLINKER`, so it could not fail — specifically so that it would be
exercised by real three-device bakes before the conversion could need it.
Since the conversion it checks **38** undefined symbols against one
`LC_LOAD_DYLIB` and `/usr/lib/dyld`. It was 31 until the console/log rework
above replaced the hand-rolled writers with stdio: `_dup2` left with the
console fd, and eight names arrived — `___stdoutp`, `___stderrp`,
`_fprintf`, `_fwrite`, `_puts`, `_setvbuf`, `_strerror` and
`___snprintf_chk` (clang lowers the constant-string `printf`s to
`puts`/`fwrite`). All are ordinary `libsystem_c` exports, and
`___strcat_chk` — already in the verified 31 — establishes that the
`_FORTIFY_SOURCE` `_chk` family is present on both firmwares. The earlier
by-hand verification against the extracted roots for both firmwares
(**zero unresolved**, matched SDK to matched device) covered the 31; the
eight additions have been confirmed to link against each matched SDK, and
the SDK↔device correspondence for those pairings is measured at 0 phantom
symbols, but they have not themselves been re-checked against a mounted
ramdisk. This check is what will say so at bake time if that is
ever wrong.

It is not in CI because the bake has something CI does not: the exact
libraries that device will boot, per device and per build, with no stored
snapshot to drift. It is also the backstop for the open
`buildEntrypointBinary()` defect described above.

## Status

Done: `entrypoint.c` reverse-engineered and verified
disassembly-for-disassembly against the original `sbin/launchd` (six real
bugs found and fixed along the way — see `docs/HISTORY.md`). Wired into
`bakeRamdisk()`: every bake builds this (cached in-process — see "Building
entrypoint itself" above) and installs it per firmware — as
`/usr/sbin/blackb0x_entrypoint` plus its LaunchDaemon plist on the
LaunchDaemons generation, spliced over `/sbin/launchd` on the older
`rc.boot` one (`installEntrypoint()`).

Building goes through the **pinned toolchain** by default, and now links that
toolchain's matched iPhoneOS SDK. Verified on this host against the real
`entrypoint.c`, both devices, with the resulting invariants: `Mach-O
executable arm_v7`, `LC_MAIN` (no `LC_UNIXTHREAD`), exactly one
`LC_LOAD_DYLIB` = `/usr/lib/libSystem.B.dylib`, `LC_LOAD_DYLINKER` =
`/usr/lib/dyld`, `LC_VERSION_MIN_IPHONEOS`, none of
`LC_DYLD_CHAINED_FIXUPS`/`LC_DYLD_EXPORTS_TRIE`/`LC_BUILD_VERSION`, ARM (not
Thumb) encodings at `_main`, `ldid`-signed `xyz.regulad.blackb0x.entrypoint`:

| Device | Xcode / `ld` | SDK | size | undefined symbols resolved |
|---|---|---|---|---|
| AppleTV3,1 / AppleTV3,2 | 6.4, `ld64-242.2` native armv7 | iPhoneOS 8.4 | 52,288 | 38 undefined, 31 of them checked 31/31 against the 12H1006 ramdisk |
| AppleTV2,1 | 5.1.1, `ld64-236.4` native armv7 | iPhoneOS 7.1 | 52,336 | 38 undefined, 31 of them checked 31/31 against the 11D258 ramdisk |

(It was 13,184 / 13,152 bytes freestanding, and 51,920 / 51,968 before the
console/log rework — the +368 is stdio. The growth over freestanding is
expected and is almost entirely the dynamic-linking metadata plus the SDK's
own hardened string helpers. See "The bake-time symbol-closure check" for
which eight symbols are new and what is and is not verified about them.)

**Nothing here has been booted on a device** — not the pin, and not the
dynamic conversion. The pin is a supply-chain hedge against Apple removing
`ld-classic`. The conversion is convergence on the configuration Apple's own
PID 1 uses on these exact ramdisks, which is the argument for it; but the
only things measured are that it links, signs, and resolves every symbol
against the real firmware's real dylibs. Whether it *boots* is untested, and
if the device goes quiet after this change, this change is the obvious
suspect.

The cross-toolchain this section used to list as a prerequisite is still not
needed; see "What was here before, and why it is gone". An SDK is needed
again, but it comes out of the pinned Xcode — see "Pinned toolchain".

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
