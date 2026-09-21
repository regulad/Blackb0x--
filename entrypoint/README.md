# entrypoint/

Source for Blackb0x's first-boot installer binary. **Apple's `/sbin/launchd`
is left byte for byte alone on every firmware this project bakes** — that is
now true of both ramdisk generations, and it used to be true of neither.
Where the binary lands depends on which generation the ramdisk is, and
`BakeRamdisk.cpp`'s `installEntrypoint()` picks by probing the mounted image:

| generation | installed as | started by |
|---|---|---|
| LaunchDaemons — AppleTV3,x **12H1006** | **`/usr/sbin/blackb0x_entrypoint`** (new file) + `/System/Library/LaunchDaemons/xyz.regulad.blackb0x.entrypoint.plist` (new file) | Apple's real `launchd`, as a one-shot LaunchDaemon |
| `rc.boot` — AppleTV2,1 **11D258**; all three devices at **10B329a** if that fallback is taken | **`/etc/rc.boot`**, replacing Apple's own stub of that name | `launchd` → `/bin/launchctl` → `fwexec("/etc/rc.boot")` |

See "Run as a launchd unit, not as launchd" for the first, and "The `rc.boot`
generation gets `/etc/rc.boot`" for the second. **The legacy path is the only
one that replaces something Apple shipped**, and that difference is called
out rather than smoothed over.

**One binary, no compile-time role.** Every generation-specific decision is a
*runtime probe* — see "Runtime probes, not a build flag" below. The artifact
is identical on both generations: one build, one signature, one undefined-
symbol closure, and the bake-time closure check covers both at once.

It is **not** real launchd and does not masquerade as it — it's a standalone
one-shot installer that runs just long enough to recursively merge
`/blackb0x` (a new top-level directory `bakeRamdisk()` stages at bake
time — see `BakeRamdisk.cpp`'s `stageBlackb0xTree()` — as a flat mirror of
the real device's final layout, every entry already carrying its correct
final owner/mode) onto the real device filesystem (mounted at **`/mnt`**, a
directory the baker creates for us — see "Why our own `/mnt`" below; it used
to be `/mnt1`), then reboots
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

## Runtime probes, not a build flag

Three things differ between the two ramdisk generations, and `entrypoint.c`
settles all three by **asking the running system**, never by a `-D`, an
`argv` role switch, or anything else decided at compile time:

| question | probe | acts when |
|---|---|---|
| do I have stdout/stderr? | `fcntl(1, F_GETFD) == -1`, and the same for fd 2, tested **independently** | launchd gave us nothing — i.e. the `rc.boot` path |
| is the ramdisk root writable? | `statfs("/")`, test `MNT_RDONLY` | it came back read-only, whichever path we are on |
| will anything start `restored_external`? | `stat("/System/Library/LaunchDaemons/com.apple.restored_external.plist")` | that unit does not exist — i.e. the `rc.boot` path |

**Why not a build flag.** The generation is a property of the *ramdisk
image*, not of the device, and the baker only learns which one it has after
it mounts the image — long after this binary was compiled. Plumbing that
decision backwards into the build would encode, at link time, an assumption
about where the artifact will land; when the assumption is wrong the result
is a device that boots and silently does the wrong thing, which is the exact
failure class this project has spent weeks deleting. A probe asks the real
question instead of a proxy for it, costs a few lines, and is self-correcting
if we ever retarget.

It also keeps the artifact **identical on both generations**: one build, one
`ldid` identity, one undefined-symbol closure, and one `verifyEntrypointRuntimeClosure()`
run at bake time that covers both paths.

**On the `restored_external` probe specifically**, since it is the one where
a more direct signal would have been nicer. What we want to know is "is it
already running?", and the honest answer is that this userland cannot cheaply
tell us: there is no `ps`, no `/proc`, and no shell. Asking IOKit whether the
USB device controller has already been configured *would* be direct, and was
rejected on cost — it means linking `IOKit`, i.e. a second `LC_LOAD_DYLIB`
dragging in CoreFoundation, libobjc and libicucore, against a binary whose
one-name dylib closure is precisely what makes the bake-time closure check
tractable. The plist probe is the next best thing and is not merely a proxy
for the generation: it asks whether *the mechanism that would start it*
exists, so it stays correct on a ramdisk that has the directory but not that
unit.

## Why our own `/mnt`

The NAND is mounted at **`/mnt`** — a directory *we* create, at **bake**
time, in `BakeRamdisk.cpp`'s `createBlackb0xMountpoint()`, with the owner and
mode read off Apple's own `/mnt1` rather than assumed (0755 root:wheel on
both generations). `entrypoint.c` does no `mkdir` and has no fallback: a
missing `/mnt` makes the `mount(2)` fail loudly, which is the correct answer
for a ramdisk we did not bake.

Every earlier version of this project borrowed one of Apple's mountpoints.
The history is worth keeping, because each step was a real finding:

- **`/mnt1`, for the project's whole life.** Then:
  `/usr/local/bin/restored_external` is the only process on either ramdisk
  that contains mount code at all, and **`/mnt1` is its own system-partition
  mountpoint** — it carries `/sbin/mount`, `/sbin/mount_hfs`,
  `create_partition_mountpoints`, `libpartition, mounting '%s' at '%s'`, and
  hardcoded `/mnt1/private/var` and `/mnt1/usr/sbin/lsof`. It only mounts on a
  host `StartRestore`, already forbidden while we run, so the collision was
  conditional — but sharing that name bought nothing.
- **`/mnt2` next, and `/mnt4` rejected**, because the mountpoint set is not
  the same on both generations. Checked directly on both mounted volumes, all
  empty:

  | ramdisk | mountpoints present | `restored_external` references |
  |---|---|---|
  | 12H1006 | `/mnt1` `/mnt2` `/mnt3` `/mnt4` | `/mnt1`…`/mnt4` |
  | 10B329a | `/mnt1` `/mnt2` only | `/mnt1`, `/mnt2` |

  `/mnt4` does not exist on the legacy generation at all — and note the right
  column: **no borrowed mountpoint is un-referenced by `restored_external` on
  either generation.** There was no clean answer among Apple's.
- **Our own `/mnt`, which is where this landed.** It cannot be the one
  `restored_external` reaches for; it exists on both generations by
  construction, so there is no per-firmware branch and no "which mountpoints
  does this image happen to have" question to get wrong later.

**And created at bake time, not at runtime**, which is the subtle part: a
runtime `mkdir` would depend on the ramdisk root being writable at that
moment, which is exactly the thing `ensure_root_writable()` exists because we
cannot assume. The baker has no such problem — it already has the image
attached read-write to stage `/blackb0x` and the binary.

The minimal-footprint principle that used to justify borrowing is not
abandoned, just paid for explicitly. What the pristine ramdisk gains:

| | LaunchDaemons generation | `rc.boot` generation |
|---|---|---|
| adds | `/usr/sbin/blackb0x_entrypoint`, `/System/Library/LaunchDaemons/xyz.regulad.blackb0x.entrypoint.plist`, `/blackb0x/`, `/mnt/` | `/blackb0x/`, `/mnt/` |
| replaces | *nothing Apple shipped* | **`/etc/rc.boot`** (content only; mode, owner and mtime preserved) |
| leaves alone | everything else, `/sbin/launchd` included | everything else, `/sbin/launchd` included |

One empty directory is a trivial addition beside the overlay; saying so
plainly beats leaving a reader to wonder.

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
  `entrypoint` ran — on *both* generations, since that splice was universal
  until the `rc.boot` work. That is a complete, sufficient explanation for "nothing
  happens" that is independent of code signing, and it explains the
  stock-vs-ours asymmetry directly.

`com.apple.restored_external.plist` and the binary it runs are therefore
**load-bearing for observability**. Nothing in the bake touches either, and
nothing should. On the `rc.boot` generation there *is* no such plist, which
is why `entrypoint` forks `restored_external` itself there — same daemon,
same reasoning, started by us instead of by launchd; see "`fork`, not
`exec`, for `restored_external`". Its startup is also safe, checked rather than assumed: it
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

> **Amended.** This section describes the console side, which is still
> exactly as written below — but `/dev/console` turned out not to reach the
> **display**, so `entrypoint.c` now emits everything to a second
> destination as well: an on-screen console it draws itself. See
> "Seeing anything at all: the framebuffer console" for that half. The two
> destinations are fed by **one** call (`emit()` / `emit_err()`) that formats
> once into one buffer and hands that same buffer to both, so they cannot
> diverge and a call site cannot forget one of them. Everything below about
> *where the console stream goes* is unchanged; only the spelling of the call
> is (`printf` → `emit`, `fprintf(stderr, …)` → `emit_err`).

`entrypoint.c` has **one console stream and does not configure it**.
Everything it says reaches stdout (progress) or stderr (errors), and launchd
attaches both to `/dev/console` from the two
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
`/var/mobile/Media/blackb0x_install.log` lives under `/mnt`, i.e. on the
NAND. Worse, the failure would be silent rather than loud: launchd would
create the file on the **ramdisk**, under the still-empty `/mnt`
mountpoint, we would then mount the real volume over the top, and every byte
would land in a shadowed file that dies with the RAM disk.

The obvious alternative — point `StandardOutPath` at a path on the ramdisk
and copy the finished file onto the NAND once `/mnt` is mounted — was
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

**On the `rc.boot` generation nobody sets those descriptors up**, and that is
now handled rather than merely recorded. The kernel does not open
`/dev/console` for init there, `launchd` inherits nothing, `launchctl`
inherits nothing, and the `/etc/rc.boot` it `fwexec`s inherits nothing — which
is exactly why the original binary opened `/dev/console` by hand as its first
act (the disassembly shows the `open`/`dup2`). Without a guard every `printf`
would fail with `EBADF` on precisely the generation with the least other
observability.

`ensure_console_fds()` restores it in the shape this file previously
prescribed: a guard that fires **only** when nothing was provided.

```c
int needOut = (fcntl(1, F_GETFD) == -1);
int needErr = (fcntl(2, F_GETFD) == -1);
if (!needOut && !needErr) return;          /* launchd already attached both */
int fd = open("/dev/console", O_WRONLY);
```

Three details are deliberate. `fcntl(F_GETFD)` is the cheapest possible "is
this descriptor open?" — it touches no file and returns `-1`/`EBADF` exactly
when the fd is closed. The two descriptors are tested **independently**:
nothing guarantees that a process handed a stdout was handed a stderr, and
clobbering a live fd 2 to fix a dead fd 1 would be its own bug. And it is
`O_WRONLY`, not `O_RDWR` — this process never reads the console, and asking
for read access on a tty we do not own a session on is a needless way to
fail. It is called first thing in `main()`, before `setvbuf()`, since
`setvbuf()` on a closed fd 1 is a silent no-op followed by a run with no
output.

An unconditional re-open is still wrong and is still not done: it would fight
whatever launchd attached on the primary path.

Bonus, once `restored_external` is up: its own log primitive ends at
`fputs(msg, __stdoutp)`, and on the way it drains `syslogd`'s ASL store and
re-prints every record as `SYSLOG: %s`. So anything logged through ASL
reaches `/dev/console` too, courtesy of that daemon.

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
  `wait()` would have been ambiguous; on both current paths orphan reaping is
  Apple's launchd's problem again — and naming the pid matters more now than
  it did, because on the `rc.boot` path we have a *second* child, the
  `restored_external` we forked, which never exits. A bare `wait()` would
  block on the wrong one forever. The explicitly empty `envp` is likewise
  still right.
- **The teardown got tighter, and one real ordering bug went with it.**
  Nothing now sits between `do_install()` returning and the NAND being
  `sync()`ed and unmounted — not a status line, not a second thought. The
  reason is `restored_external`'s reboot-on-child-exit behaviour (below): the
  cheap, correct mitigation is to be unmounted before it can fire, not to add
  a lock or wait for anything. The old sequence also unmounted `/mnt1` *before*
  `/mnt1/private/var`, which is a submount of it, and then `rmdir`ed
  `/mnt1/private/var2` after `/mnt1` was already gone — i.e. against the
  pristine ramdisk's own empty mountpoint rather than the volume the directory
  was created on. It is innermost-first now, with `var2` removed while its
  volume is still mounted, and the `devfs`-failure path got the same fix
  (it used to leave the data partition mounted across a reboot).
- **`restored_external` reboots the device when its `-server` child exits, on
  BOTH generations.** An earlier revision of this file called that
  10B329a-specific; it is not. On 12H1006 too, its no-argument `main()` does a
  `sysctlbyname`, `fork()`s, the child `execl`s itself with `-server`, the
  parent `waitpid()`s, and on any exit the parent logs `restored exited
  normally with status %d - rebooting` (or the `0x%x` / `due to signal %d`
  variants) and reboots via `/sbin/reboot`. With no host attached that child
  blocks in `accept()` forever, so it is a tail risk rather than a likely one
  — which is why the mitigation is the short dirty window above and nothing
  heavier.
- **Nothing else on the ramdisk touches block devices**, so there is nothing
  to lock against: `launchd`, `launchctl`, `xpcproxy`, `syslogd` and
  `ReportCrash` contain zero references to `/mnt`, `disk0`, `rdisk` or any
  mount call. Two specifics worth recording so nobody re-investigates them:
  `syslogd`'s paths are all absolute ramdisk paths (`/var/log`, `/var/log/asl`,
  `/etc/asl.conf`, `/dev/klog`, `/var/run/syslog`) and `/var/log` and
  `/etc/asl.conf` do not even exist there — note also that `ASL_DISABLE=1` in
  its plist is read by `libsystem_asl` in *clients*, not by syslogd, so it
  makes syslogd skip logging to itself and does not disable the store. And
  `ReportCrash` is on-demand only (MachServices, no `RunAtLoad`), with
  `-r /private/var/logs/restored` keeping reports on the ramdisk, in RAM.

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
  `/bin/bash /usr/share/blackb0x/postinstall.sh`, and **there is no shell on
  either ramdisk** — `/bin` is exactly `cat expr launchctl ln mkdir mv rm`.
  `ProgramArguments` here must name a real binary.

There are **no ordering primitives**: this launchd has no
`Requires`/`After`/`Before`, and the boot-time load is a single directory
scan in which `RunAtLoad` jobs start in enumeration order. Anything that must
happen after the display is up has to poll or sleep. Nothing artificial is
added — `entrypoint`'s first action is already a wait loop on
`/dev/disk0s1s1` appearing, which staggers it naturally.

### It is per-firmware, and the older generation gets `/etc/rc.boot`

Measured on three real decrypted ramdisks. The boundary is the iOS 7 → 8
launchd rewrite:

| generation | firmwares | `/sbin/launchd` | init |
|---|---|---|---|
| LaunchDaemons | AppleTV3,x **12H1006** | 239,536 B, `com.apple.xpc.launchd` | scans `/System/Library/LaunchDaemons` (three plists: ReportCrash.restored, restored_external, syslogd); no `/etc/rc.boot` |
| `rc.boot` | AppleTV2,1 **11D258**, and all three devices at **10B329a** (the fallback target) | 149,296 B, `com.apple.launchd` | no `LaunchDaemons` anywhere, no `/Library`, no `launchd.conf`; spawns `/bin/launchctl`, which execs `/etc/rc.boot` |

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
   the whole life of the restore.

So `/etc/rc.boot` is the only hook that generation reaches — and **that is
what we install there now.** `bakeRamdisk()` probes the mounted image
(`installEntrypoint()`): `/System/Library/LaunchDaemons` present → the unit;
otherwise `/etc/rc.boot` present → replace it; neither → the bake fails
outright rather than guessing at an init hook.

This **replaced the splice over `/sbin/launchd`**, which every bake did on
this generation until now and which no bake does anywhere any more. That
splice meant Apple's launchd never ran, so `launchctl` never ran, so
`restored_external` never ran — no display, no USB enumeration, a working
boot indistinguishable from a dead device. It was already documented as not
an endorsement; this is the fix.

### Standing in for Apple's `rc.boot`

Replacing a file means inheriting its duties. Apple's stub is 8,832 bytes on
11D258 and 8,880 on 10B329a (armv7 Mach-O, ad-hoc, `com.apple.rc`), and it is
small enough to state completely — five imports (`_getfsfile`, `_mount`,
`_umask`, `_execl`, `_reboot`) and four strings:

```c
f = getfsfile("/");
if (!f) reboot(0);
if (mount(f->fs_vfstype, "/", 0x10001, &args{f->fs_spec})) reboot(0);
umask(0);
for (p in {restored_external, restored_update, restored, ramrod})
    execl(p, p, NULL);
reboot(0);
```

Three corrections to how that has been described here before, all read off
the real binary:

1. **`0x10001` is `MNT_UPDATE|MNT_RDONLY`, not "read-write".** Apple's
   `rc.boot` *demotes* the ramdisk root to read-only; it does not promote it.
   The root arrives from the kernel writable and Apple gives that up before
   handing off.
2. **`getfsfile()` never reads `/etc/fstab` here.** There is no `/etc/fstab`
   on either ramdisk, and the legacy ramdisk's own `libsystem_c` contains the
   string `fstab` *nowhere at all* — it imports `_getfsstat`/`_statfs`/
   `_fstatfs` and synthesises the entry from the live mount table. (Confirmed
   independently on a modern host, which also has no `/etc/fstab`:
   `getfsfile("/")` returns a real synthesised entry.) So Apple's stub is
   asking the kernel what `/` actually is, with `getfsfile()` as a middleman.
3. **The `execl` list always lands on the first entry.** `/usr/local/bin` is
   exactly `restored_external` and `ioflashstoragetool` on *both* generations;
   `restored_update`, `restored` and `ramrod` do not exist on either.

What `entrypoint.c` does instead, and why each differs:

- **`ensure_root_writable()` — `statfs("/")`, and promote only if
  `MNT_RDONLY` is set.** `statfs` asks the same question `getfsfile` asks,
  directly, and hands back the device (`f_mntfromname`), the type
  (`f_fstypename`) and the flags in one call — no dependence on a libc
  fallback that is invisible in the headers. We promote rather than demote
  because we are not the last thing to run the way Apple's stub is: it execs
  and is gone, we keep running with `restored_external` forked alongside us,
  and a read-only root would turn any future need to write there into a
  silent failure on the generation with the least observability. Nothing
  writes to the ramdisk root today, so this is a safety net, and it announces
  itself either way.
- **`umask(0)` — Apple's, verbatim.** It is also what our LaunchDaemon plist
  declares (`Umask` 0, copied from `com.apple.restored_external.plist`), so
  setting it here makes the two paths behave identically instead of leaving
  the `rc.boot` path with whatever `launchctl` handed down. `merge_tree()`
  `chmod`s everything it creates, so a nonzero umask would not survive
  anyway; what this removes is the window between create and chmod. It is
  inherited by the `restored_external` we fork, exactly as under Apple's stub.
- **`start_restored_external()` — `fork`, not `execl`.** This is the
  load-bearing decision and it gets its own section below.

### `fork`, not `exec`, for `restored_external`

`restored_external` is why the `rc.boot` branch was worth doing at all. It is
the only binary on either ramdisk that calls `IOUSBDeviceControllerCreate` /
`IOUSBDeviceDescriptionCreateFromDefaults` /
`IOUSBDeviceControllerSetDescription`, and on this hardware the USB device
stack stays **off the bus until a userspace process does exactly that** — the
DeviceTree's `usb0-device` says `configuration-string = "standardMuxOnly"`
while the in-kernel auto-configurator personality matches only
`ConfigurationType = "standardBringup"`, so it never fires. It is also what
draws the display (`IOMobileFramebuffer`, `IOSurface`, `/usr/share/progressui`).

Apple's stub `execl`s it, which **replaces the calling process image**. For us
that would mean the install never happens. Exec'ing at the *end* instead is
worse than it sounds: the display and USB would come up only after the merge,
seconds before `reboot(2)` tears them down, so the entire window we are trying
to make observable would already be over.

So: `fork()`, the child `execve`s it, the parent carries on installing. The
device enumerates as `05ac:12a7` **while the merge is in flight**, and
`AppleUSBDeviceMux` (prelinked into both kernelcaches) makes anything
listening reachable from the host.

- **Exec failure is handled, not assumed away.** The child reports the failure
  on the console it inherited and `_exit(127)`s; the parent is untouched and
  finishes the install. A failed `fork()` is likewise reported and
  non-fatal — no display is bad, no install is worse.
- **We do not wait for it.** It never exits (it blocks in `accept()`), so a
  `wait4()` would hang the install forever. It is reparented to launchd when
  we exit, which is launchd's ordinary business.
- **Known tail risk**, recorded rather than defended against: on **both**
  generations its no-argument `main()` forks a `-server` child and
  `waitpid()`s, and reboots the device via `/sbin/reboot` when that child
  exits for any reason. Mid-merge that means a reboot with our mount dirty.
  With no host attached the child blocks in `accept()` forever, so it is a
  tail risk — and the mitigation is the short dirty window (`sync()` +
  unmount the instant the merge returns), not a lock. There is nothing to
  lock against: `launchd`, `launchctl`, `xpcproxy`, `syslogd` and
  `ReportCrash` contain zero references to `/mnt`, `disk0`, `rdisk` or any
  mount call.

### The signing identifier does not change for this path

`entrypoint` is signed `xyz.regulad.blackb0x.entrypoint` on both generations,
including when it is installed as `/etc/rc.boot`. Apple's `rc.boot` is signed
`com.apple.rc`, and matching it was considered and rejected:

- **Nothing establishes that the identifier matters to AMFI here.** Apple's
  own ramdisk binaries are *all* ad-hoc signed — `launchd`, `launchctl`,
  `restored_external` and `rc.boot` all carry flags `0x2` and no entitlements
  — there is no `amfid` on either ramdisk, and the obvious static-trust-cache
  hypothesis was tested and failed (none of their CDHashes appear in the
  kernelcache, not even as an 8-byte prefix). Whatever admits them is
  in-kernel and unknown. Picking an identifier to please it would be
  cargo-culting a mechanism nobody has identified.
- **It would be a lie about what the binary is**, the same reason
  `com.apple.launchd` was dropped when the splice went away. This is not
  Apple's `rc.boot`; it is our installer standing in the same slot.
- **It would fork the artifact.** The identifier is applied by `ldid` at build
  time, and which generation a ramdisk is is not known until the baker mounts
  the image. Making the identity depend on it would mean either two builds or
  a re-sign inside the bake — real machinery, bought with no established
  benefit, against the "one binary, one signature" property that keeps the
  bake-time closure check covering both paths at once.

If evidence ever turns up that the identifier *is* load-bearing, this is a
one-line change in `entrypoint/Makefile` — but it should be made on evidence.

## Seeing anything at all: the framebuffer console

`entrypoint/fbtext.h` (font + blitter, device-agnostic) and
`entrypoint/screen.h` (finding a surface to draw on) put this binary's output
**on the TV**. They are the reason the dylib closure is three names instead
of one.

### Why: `/dev/console` is wired up and does not reach the display

This was measured, not assumed. A diagnostic ramdisk (`DiagBinary`) carrying
this binary and its plist and nothing else was built to answer exactly one
question, and it answered it:

| image | result on AppleTV3,2 at 12H1006 |
|---|---|
| stock | Apple logo, stays |
| `DiagRepack` (re-sealed, nothing added) | logo + progress bar, stays |
| `DiagOverlay` (full ~26 MB overlay, no binary, no plist) | logo + empty progress bar, **stays** |
| `DiagBinary` (binary + plist, no overlay), stay-resident build | **system stays up** |

Three findings, and the third is this section's:

- **Size is exonerated.** The full overlay boots.
- **Our binary runs**, and the reboots previously chased were caused by our
  job *exiting* — this launchd evidently treats that as a reason to reboot.
  Hence the stay-resident no-overlay path in `main()`.
- **Nothing ever appeared on screen.** The stay-resident build prints a
  heartbeat to stdout every ten seconds, which launchd routes to
  `/dev/console` exactly as it does for Apple's own daemon, and the TV showed
  the Apple logo and nothing else for as long as anyone cared to watch. So
  `/dev/console` on this ramdisk is a live *stream* but not a live *display*:
  once `restored_external` points the display pipe at its own surfaces, the
  boot framebuffer the kernel console draws into is off-screen. **Drawing
  pixels ourselves is the only remaining way to get output off this device.**

### How: look the surface up by ID; never open IOMobileFramebuffer

`/usr/local/bin/restored_external` creates exactly **three** IOSurfaces at
display-init time, each with `kIOSurfaceIsGlobal = kCFBooleanTrue`, BGRA,
write-combined, stride `(width * 4 + 63) & ~63`, and programs them as
compositor layers with src and dst rects both `{0,0,width,height}`:

```
layer 0 <- surface[2]              opaque background, one solid colour
layer 1 <- surface[0] / surface[1] alternating, bzero'd => ALPHA 0
layer 2 <- NULL
```

Because they are **global**, they can be re-opened by ID from any process
with `IOSurfaceLookup()`, and the display pipe scans out of them
continuously — so storing into their pixels changes the screen with no swap,
no compositor call, and no cooperation from the process that owns them. The
ramdisk's own `IOSurface` binary exports every entry point needed
(`IOSurfaceLookup`, `IOSurfaceGetID`, `IOSurfaceGetWidth`/`Height`/
`BytesPerRow`/`PixelFormat`/`BaseAddress`, `IOSurfaceLock`,
`IOSurfaceUnlock`), verified with `nm -gU` against the decrypted ramdisks
rather than an SDK stub.

**IOMobileFramebuffer is never opened.** Not once. Its exclusive-access
behaviour could not be settled by reading it, and "probably it lets a second
client in" is not a thing to find out on a device whose only failure signal
is a black screen. Lookup-by-ID needs none of it.

**Which surface: the opaque background** (`surface[2]`, layer 0), identified
by reading pixel `(0,0)` — the background reads alpha `0xFF`, the two
progress-layer buffers read `0x00000000`. The layer above it is transparent,
so our text shows through; and the progress buffers are the ones
`restored_external` rewrites on every update, whereas the background is
redrawn only on display init and HDMI hot-plug. Its swap routine has exactly
two callers in `restored_external` — a progress update and an image blit —
and no timer, animation or polling. While it sits in `accept()` with no host
attached, which is the field state, it performs zero swaps and zero pixel
writes, so what we store stays up.

**Ordering is polled, not assumed.** This ramdisk's launchd unit graph has
no `Requires`/`After`/`Before`, so `restored_external` may not have run when
we start — and on the `rc.boot` generation *we* are the one who forks it, so
it definitely has not. `screen_init()` therefore does not assume; it opens a
bounded 30-second window and retries.

### The rules that keep it from becoming a new way to fail

It is a diagnostic aid on a device we are already struggling to see. An
install that died because the debug channel could not attach would be
strictly worse than no debug channel. So:

- **Every failure is a `printf` and a `return`.** There is no path from
  `screen.h` to `exit()`, `reboot()` or `panic()`.
- **The poll never blocks.** `screen_init()` tries once and returns; later
  attempts are piggy-backed on output calls, at most one per second, until
  the window closes — then it says so on stdout once and goes quiet. Nothing
  waits on the display, so the install is never delayed by a diagnostic.
- **Nothing is lost by not blocking.** Lines emitted before a surface is
  found are held in a backlog (48 × 112 bytes of BSS) and replayed the moment
  one is. A blocking wait would buy latency and nothing else.
- **Writes happen inside an `IOSurfaceLock` window, and the base address is
  re-fetched inside it every time**, so a mapping is never dereferenced on
  the assumption that it is still there. The lock is deliberately **not**
  held across the run: holding another process's surface lock for the length
  of an install is the sort of thing that wedges that process.
- **Geometry from another process is bounds-checked** before a single store
  (pixel format, min/max width and height, stride within one cache line of
  `width * 4`), and `fbtext.h` clips every pixel to the reported extents.

### The console itself

Top-left, newest work appended downward, wrapping back to the top. No word
wrap: a long line is cut off at the right edge, which is the right trade for
progress lines and `errno` strings, where the informative part is at the
front.

- **Scale 3, i.e. 24-pixel glyphs.** Two constraints pull against each other:
  ten-foot legibility guidance puts the comfortable minimum at roughly 1/30
  of screen height, and our messages want columns. `scale = width / 416`
  solves for ~52 columns, which on the 1280×720 output this device drives is
  3 — a 24-pixel glyph is exactly 1/30 of the frame, and ~50 columns survive
  after the inset. It is the smallest scale that clears the legibility floor,
  so it is the one that keeps the most text. 1920 lands on 4, 720 clamps
  to 2.
- **A ~3% overscan inset.** The top-left corner is precisely what a TV that
  overscans eats first. Apple TV over HDMI normally maps 1:1 and would not
  need this; "normally" is not a property we can check from here, and the
  cost is a few dozen pixels.
- **A non-scrolling status row** below the scroll region. The stay-resident
  heartbeat fires six times a minute against a ~28-row screen; an ordinary
  line per tick would wrap the console and erase every boot line within five
  minutes — destroying the only thing that run exists to show a human. So the
  full line goes to the console, where scrollback is free, and the screen
  gets the same text rewritten in place.
- **`panic()` turns the console red** (`screen_alert()`) so a panic is
  distinguishable at TV distance from the progress lines above it.
- **Opaque, no blending, no read-modify-write.** The mapping is
  write-combined: stores are fast, reads are very slow. Every glyph cell is
  written in full, foreground where the bit is set and background where it is
  not — which also means the output is legible whichever layer the surface
  turns out to be on. The single exception is the one 32-bit `(0,0)` read per
  candidate surface that identifies the background layer.
- **Non-ASCII collapses to `-`.** This file's prose uses real em-dashes and
  the 8×8 font is printable ASCII only; a UTF-8 em-dash would otherwise
  render as three `?` glyphs and eat three of about fifty columns.
- The font is `font8x8_basic`, public domain (Marcel Sondaar / Daniel
  Hepper).

### `fbtext.h` knows nothing about the device, on purpose

It is handed a base pointer, a width, a height and a stride, and it stores
32-bit words. That separation is load-bearing: it lets the drawing logic be
compiled and **checked on the host**, where the output can actually be looked
at, before it is ever run on a device we cannot see. The check renders to a
PPM, dumps an ASCII-art view of the top-left, and asserts the things that are
easy to break — the scale rule, the text-area geometry, that nothing is
written outside the surface or outside the column span (a poison-filled
stride pad catches a `width * 4` mistake, which would otherwise show up as a
diagonal smear rather than a crash), that the wrap returns to row 0, and that
the status row is not part of the scroll. It lives in the scratch repo
(`blackb0x-scratch/fbtext/repocheck/`), not here, because it is a
verification harness for a 2012 device and not a unit test of this project's
CLI.

### What this cost in linkage

`entrypoint` loaded exactly **one** dylib and now loads **three**:

| dylib | why |
|---|---|
| `/usr/lib/libSystem.B.dylib` | as before |
| `/System/Library/PrivateFrameworks/IOSurface.framework/IOSurface` | the nine lookup/geometry/lock entry points |
| `/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation` | `CFRelease` alone — `IOSurfaceLookup()` returns a CF object, and the ID sweep would otherwise leak (and keep mapped) every surface it probes. It is a runtime dependency of IOSurface regardless, so this declares something that was going to be loaded anyway. |

Verified against the **real firmware roots**, not SDK stubs, on all three
ramdisk generations this project can bake:

| ramdisk | IOSurface entry points | transitive closure | unresolved |
|---|---|---|---|
| 12H1006 (AppleTV3,x) | 9/9 | 42 entries | 0 |
| 11D258 (AppleTV2,1) | 9/9 | 38 entries | 0 |
| 10B329a (fallback) | 9/9 | 33 entries | 0 |

Both device branches build an **identical undefined-symbol set of 58**
(44 before; +9 IOSurface, +`CFRelease`, +`___vsnprintf_chk`, +`_fputs`,
+`_fputc`, +`_time`), and all 58 resolve on all three ramdisks. Both SDKs
ship a linkable `IOSurface.framework` stub with the needed exports —
iPhoneOS 8.4 (Xcode 6.4) and iPhoneOS 7.1 (Xcode 5.1.1) — so the feature does
not have to degrade on the older branch, and `entrypoint/Makefile` fails with
a named error rather than a link error if an SDK ever lacks one of the three.

CI asserts the **whole set by name** rather than a count plus a grep, which
is strictly stronger than the old "exactly one `LC_LOAD_DYLIB`" check: an
unexpected fourth dependency fails, and so does a substitution that keeps the
count the same.

### Neither weak linking nor a compile-time gate is needed, and that was measured

The obvious worry is that linking is a **build-time** decision while the
generation is a property of the **ramdisk**, so a library present on one
generation and absent on another would mean a binary that fails to load
entirely on the other — a failure no runtime probe can rescue. Two escapes
were considered, and neither is necessary, because the premise is false:

```
12H1006: IOSurface 9/9  CFRelease present  install-name /System/Library/PrivateFrameworks/IOSurface.framework/IOSurface  armv7
11D258:  IOSurface 9/9  CFRelease present  install-name  (identical)                                                     armv7
10B329a: IOSurface 9/9  CFRelease present  install-name  (identical)                                                     armv7
```

All three generations ship the framework, at the **same install name**, as
**armv7 thin**, exporting all nine entry points as real text symbols. So:

- **Weak linking (`-weak_framework` / `LC_LOAD_WEAK_DYLIB`) was not adopted.**
  It buys tolerance of an absent library, and no ramdisk this project can bake
  lacks it. Adding it would trade a link-time guarantee for a runtime NULL
  check with nothing bought — and a weak reference that silently resolves to
  NULL is a worse failure mode here than a bake that refuses to proceed.
- **A compile-time gate (`-DIS_LAUNCHD_UNIT` or similar) was not adopted**,
  and would have been wrong even if the library had been missing. The
  generation is a function of *(device, build)*, not device: under the
  10B329a fallback **all three devices become the `rc.boot` generation**, so
  a `DEVICE`-keyed flag would be silently wrong the day that fallback is
  taken. It would also fork the artifact, giving up the "one binary, one
  signature, one undefined-symbol closure" property that lets the bake-time
  check cover both paths at once.

The same reasoning applies to the console descriptors, and the existing
runtime probe there stays: `fcntl(fd, F_GETFD) == -1` asks the real question
("did I get usable descriptors?") rather than a proxy for it. See "Runtime
probes, not a build flag".

> **Known gap, in `src/` and therefore not fixed here.**
> `verifyEntrypointRuntimeClosure()` in `src/BakeRamdisk.cpp` builds its
> "defined" set by scanning `*.dylib` under `/usr/lib` and `/usr/lib/system`
> only. Framework binaries live at
> `/System/Library/…Frameworks/X.framework/X` — no `.dylib` extension, other
> directories — so the ten new IOSurface/CoreFoundation symbols will read as
> unbound and **the bake will fail** until that scan also covers the binary's
> own `LC_LOAD_DYLIB` paths. The structural half of that function is already
> fine: it `fs::exists()`-checks each load command's path verbatim, and all
> three resolve on all three ramdisks.

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
the rest of the ramdisk running. It is also what makes `/etc/rc.boot` a
legitimate install target on the older generation rather than a repeat of
the reverted detour: the chain there is kernel → launchd → launchctl →
`rc.boot`, so installing at `rc.boot` leaves every link above it intact,
which is exactly what the splice destroyed.

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
generation it is written over `/etc/rc.boot` instead
(`spliceFileContentInPlace()`, preserving that file's existing permissions
from the pristine Apple ramdisk) — `installEntrypoint()` picks by probing the
mounted image; see "It is per-firmware" above. **`/sbin/launchd` is not a
target on either path.** The artifact is the same either way: the generation
is settled by runtime probe, not at build time.

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
(`src/BakeRamdisk.cpp`) proves the binary just installed can actually load on
*that* volume: every `LC_LOAD_DYLIB` and the `LC_LOAD_DYLINKER` target must
exist on the ramdisk, and every undefined symbol must be exported by
something under `/usr/lib` or `/usr/lib/system`. A miss fails the bake with
the symbol names listed. Because the binary is identical on both generations,
this one check covers both install paths.

**It does real work now.** It was deliberately landed while `entrypoint` was
still freestanding — zero undefined symbols, zero `LC_LOAD_DYLIB`, no
`LC_LOAD_DYLINKER`, so it could not fail — specifically so that it would be
exercised by real three-device bakes before the conversion could need it.
It checks **43** undefined symbols against one `LC_LOAD_DYLIB` and
`/usr/lib/dyld`. The history of that count: 31 freestanding-era → 38 when the
console/log rework replaced the hand-rolled writers with stdio (`_dup2` left,
and `___stdoutp`, `___stderrp`, `_fprintf`, `_fwrite`, `_puts`, `_setvbuf`,
`_strerror` and `___snprintf_chk` arrived — clang lowers constant-string
`printf`s to `puts`/`fwrite`) → **43** with the `rc.boot` work, which added
five: `_fcntl` and `_dup2` (the console guard), `_statfs` (the root-writability
probe), `_umask` (Apple's `rc.boot` duty), and `_printf` (the first format
string with a conversion in it, so clang can no longer lower it). No `_memset`
appeared — clang inlines the 44-byte mount-args zero.

All five are ordinary `libsystem_kernel`/`libsystem_c` exports, and all 43
were re-checked against **real firmware, not SDK stubs**: zero unresolved on
the 12H1006 extracted root (7,477 exports), the mounted 12H1006 ramdisk
(24,396), the 10B329a extracted root (6,136) and the mounted 10B329a ramdisk
(15,743) — i.e. both SDK branches and both generations. The 11D258 ramdisk was
not mounted for that pass; its generation is covered by 10B329a and its SDK
branch by the 7.1-matched build, and this check is what will say so at bake
time if that ever turns out not to be enough.

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
LaunchDaemons generation, and over `/etc/rc.boot` on the older `rc.boot`
one (`installEntrypoint()`). **`/sbin/launchd` is not touched on either.**

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
| AppleTV3,1 / AppleTV3,2 | 6.4, `ld64-242.2` native armv7 | iPhoneOS 8.4 | 52,544 | 43 undefined (42 + `dyld_stub_binder`), **0 unresolved** against both the 12H1006 extracted root and the mounted 12H1006 ramdisk |
| AppleTV2,1 | 5.1.1, `ld64-236.4` native armv7 | iPhoneOS 7.1 | 52,576 | 43 undefined (42 + `dyld_stub_binder`), **0 unresolved** against both the 10B329a extracted root and the mounted 10B329a ramdisk |

(It was 13,184 / 13,152 bytes freestanding, 51,920 / 51,968 before the
console/log rework, and 52,288 / 52,336 before the `rc.boot` work. The growth
over freestanding is expected and is almost entirely the dynamic-linking
metadata plus the SDK's own hardened string helpers. See "The bake-time
symbol-closure check" for which symbols are new and what is and is not
verified about them — including that the 11D258 ramdisk itself was not
available to mount for the latest pass.)

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

This went through a detour, a revert, and then back to `/etc/rc.boot` for
different and better reasons. For a while it spliced into `/etc/rc.boot`
instead of `/sbin/launchd`, on the theory that `rc.boot` was the true first
entry point. That reverted for a good reason — a real bake against
AppleTV3,1/AppleTV3,2 12H606 failed because that firmware's ramdisk has no
`/etc/rc.boot` at all — and the theory behind it was also simply false, which
was only established later. **Neither objection applies to the current
design**: the baker now *probes* for the hook rather than assuming a
universal one, so a ramdisk with no `rc.boot` takes the LaunchDaemons path
and one with neither fails loudly; and the current justification is not "it
is the first entry point" (it is not) but "it is the only hook that
generation's `launchctl` ever execs". **The kernel never execs
`/etc/rc.boot` on any firmware**; `rc.boot` is `LC_MAIN`, which is what the
original disassembly correctly observed, but it is launched by `launchctl`,
several steps downstream of PID 1. See "What actually runs as PID 1" above
for the evidence, and `docs/HISTORY.md`'s "Entrypoint injection point" entry
for the original account.
