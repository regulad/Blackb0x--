# TODO

Open items not yet resolved. See `AGENTS.md` for repo conventions and
`docs/HISTORY.md`/`NEO_FLOW.md` for how we got here.

## 1. Pin down `untether.bin`'s exact build (mostly resolved)

`misc/tihmstar-untether.tar` — the last original tarball ever
checked into this repo — is gone now: the real, long-lost
`net.tihmstar.etasonuntether-1.3.1.deb` itself turned up (now in
`debcache/`), and `BakeRamdisk.cpp`'s `stageEtasonatv()` extracts the
8.4 untether payload straight out of that `.deb` at bake time instead of
the tarball. Direct provenance was already confirmed before this switch —
see `misc/README.md`'s `## etasonATV / tihmstar-untether
provenance` section, `### Direct provenance, found via the "Home Depot"
lead`: tihmstar's own Cydia repo (`repo.tihmstar.net`) is still live, and
`untether/expl.js`, `usr/bin/orphan_commander`, and `etc/rc.d/daemonload`
are all byte-identical (MD5-verified) to files inside his real, current
`net.tihmstar.etasonuntether-1.3.1.deb` — so this switch changes the
*source* of those files from a checked-in tarball to the real package,
not their content.

What's still open — narrower than before, but not closed:

- `untether/untether.bin` (43,680 bytes, now kept standalone at
  `misc/untether.bin` — see that file's own README section) still
  doesn't match either currently published build: not tihmstar's real
  `etasonuntether-1.3.1`'s `untether.bin` (35,677 bytes, dated 2021-04-04
  in that package), nor `untetherhomedepot`'s (33,600 bytes, 2017). **The
  "intermediate official release" hypothesis is now ruled out**: Wayback
  Machine's crawl of `repo.tihmstar.net` shows only two
  `net.tihmstar.etasonuntether` releases ever existed (`1.3.0`, first
  crawled 2020-09-16 but internally dated 2017-09-25, and `1.3.1`,
  2021-04-04) — their `untether.bin`s are byte-identical (`sha256` match)
  to each other, so there was never a third, differently-built official
  release. Also newly characterized: our build's embedded kernel-banner
  table is all SoC `S5L8947X` (the real Apple TV 3 SoC) across four
  sequential tvOS 8.4.x point releases, while the officially-packaged
  `1.3.1` deb's own table never once mentions `S5L8947X` at all — it's
  five entries for five *other* SoCs (iPhone4S/iPad2/iPad3/iPod5/iPhone5-
  class), matching that deb's own control file describing it as a generic
  multi-device payload (`Description: Untether for 8.4.1 32bit`), not
  something ever actually verified against a real Apple TV 3 kernel. It
  shares the harness's distinctive strings/log format and the exact
  `"Marijuan"` watermark from tihmstar's own public `jelbrekTime` source
  (`jailbreak.m`, internally codenamed **"v0rtex"**) — and that watermark
  turns out to be a standing signature technique across his toolchain more
  broadly, not a one-off: his currently-maintained `tihmstar/libpatchfinder`
  has a reusable `get_MarijuanARM_patch()`, including a 32-bit iOS 8
  kernel-patchfinder file dated "13.08.21" (about 3.5 months after this
  tarball's own timestamps) — circumstantial support that this is
  tihmstar's own private 2021-era dev tooling, never packaged, rather than
  a third party's build, but not proof. **Still genuinely unknown**: the
  actual origin of the base build itself — no second copy of this exact
  binary turned up anywhere on the web, GitHub, or Wayback.
- Our own `untether.bin` vs. our own `orig_untether.bin`'s 15-byte diff is
  now fully characterized (previous entry here was wrong — these are NOT
  Thumb branch immediates). 14 of the 15 bytes are an in-place ASCII
  string edit: one of the binary's four embedded `Darwin Kernel Version`
  banner strings changes from `...xnu-2784.40.6~50/RELEASE_ARM` (Nov 2016)
  to `...xnu-2784.40.6~93/RELEASE_ARM` (Jan 2021) — same length, hence
  patchable in place without relinking; the other three banner slots
  (`~86`/`~87`/`~92`) are untouched. Net effect: drop the oldest
  recognized kernel build, add the newest, keeping the table fixed at 4
  entries. The remaining 1 byte (`ADDS r6, #0x28` → `#0x45`, offset
  `0x3301`) is a real, separate code change elsewhere, still unexplained.
  Timestamps (`orig_` 2021-04-17, patched 2021-04-28) sit just 8-19 days
  after Apple's real `12H923` tvOS 8.4.x update (2021-04-09) — whoever
  patched this was tracking Apple's still-ongoing point releases in close
  to real time. *Who* did it (tihmstar himself vs. a third party) is still
  unknown.
- Whether tihmstar's own `v0rtex`-family source (beyond the `jelbrekTime`
  copy, which targets watchOS/armv7k, not this binary's iOS/tvOS armv7,
  and `libpatchfinder`, whose visible git history only goes back to a June
  2023 squash commit) was ever published anywhere closer to this exact
  target — checked `tihmstar/v1ntex`, `v3ntex` (unrelated, 64-bit
  iOS11/12-era) and `tihmstar/jbinit` (unrelated, no AppleTV/8.4.1/
  S5L8947X references) directly; both dead ends. No further leads found
  yet.

## 2. Document & implement pushing SSH access to the device (resolved)

Previously: `DeviceManager::pushAuthorizedKeys()` hand-rolled an AFC2 write
of the invoking user's `~/.ssh/authorized_keys` to
`/private/var/root/.ssh/authorized_keys`, wired into `Cli.cpp` to run
automatically once the jailbreak was confirmed running. This piled up
exactly the "difficult flow" gaps this TODO originally listed (retry/timeout
semantics, `.ssh/` directory creation, permission bits, host-key
reconciliation, fatal-vs-warning), all needing to be gotten right inside
`main.cpp`'s dependency graph.

Replaced with `scripts/push_authorized_keys.sh`, run by hand, outside the
main binary entirely: it forwards a local TCP port to the device's real
sshd (Cydia's own openssh package, already running post-boot) via
`iproxy` (already built as part of the vendored `libusbmuxd`, no new
dependency), then pushes the keys file over that tunnel like a normal
`ssh-copy-id`. This sidesteps every gap above instead of solving it in C++:
- Mechanism is now just "read `scripts/push_authorized_keys.sh`" — plain
  `ssh`/`iproxy`, no bespoke AFC2 protocol code to document.
- Retry/timeout is a simple TCP-reachability poll loop in the script, not
  hand-rolled `waitForAFC2` state in `DeviceManager.cpp`.
- `.ssh/` creation and `chmod 700`/`600` permissions are one `ssh` command
  (`mkdir -p && chmod && cat > ... && chmod`) — the old AFC2 path never set
  permissions at all, a latent bug now fixed as a side effect.
- Host-key reconciliation is solved by not needing it: the script uses
  `UserKnownHostsFile=/dev/null` + `StrictHostKeyChecking=accept-new`, so a
  throwaway `127.0.0.1:<port>` tunnel never pollutes the user's real
  `~/.ssh/known_hosts`, and normal `ssh`/password-prompt UX (default Cydia
  openssh password `alpine`) handles first connect.
- Fatal-vs-warning is moot: it's an optional, separate, user-run step with
  its own exit code, not something `blackb0x`'s own run can fail on.
- Device/tether-combination coverage is now uniform by construction — the
  script only depends on Cydia's openssh already running post-boot, not on
  anything exploit-path-specific, so there's no separate path per device to
  validate.

`blackb0x` itself now only prints a one-line pointer to the script once
`jailbreakRunning` first flips to 1 (`Cli.cpp`'s `onDeviceUpdated`); see
`AGENTS.md`'s "Build & run" and "Runtime requirements" sections and the
top-level `README.md`'s "Steps to jailbreak" step 6 for the user-facing
writeup. Not yet re-verified against real hardware end-to-end (no unit
available this session) — the mechanism (usbmuxd TCP forwarding + a real
sshd behind it) is standard and low-risk, but flag if a real run surfaces
anything `iproxy`-specific (e.g. AppleTV 2,1's older tvOS/openssh build
behaving differently) worth recording here.

## 3. Reimplement p0sixspwn's postinst in `entrypoint.c`

Not attempted yet, and not worth chasing right now: nobody on this project
currently has the AppleTV2,1 hardware on 6.1.3-6.1.6 firmware this branch
targets to verify against, and `docs/HISTORY.md`'s own gaster/checkm8
history shows this kind of change can look correct on paper and still be
wrong in a way only real hardware would catch. See `BakeRamdisk.cpp`'s
`stageP0sixspwn()` for the specifics of what's currently staged vs. what
the real package's postinst does that isn't replicated anywhere yet.

## 4. Bring back macOS support for `blackb0x` (and, eventually, ramdisk baking too)

`AGENTS.md` currently states "CLI-only, Linux-only... dropped, not
dual-maintained" as a "don't re-litigate without asking" convention — this
entry reopens that by explicit request. Reopened a second time, more
urgently, after a long real-hardware debugging session on Linux
(AppleTV3,2, ECID 2685369898254) kept surfacing libusb/DFU-state races in
`DeviceManager.cpp`'s `boot_client()` — an intermittent "boots into the
real OS instead of continuing the exploit chain" failure that a conditional
reset (only reset when the device actually reports DFU state 8) mitigates
but hasn't been confirmed to fully resolve. Rather than keep chasing
timing-dependent libusb behavior on Linux, the plan is to get the real
chain running against gaster's and libimobiledevice's native, first-party
IOKit paths on macOS instead, where they're the actively-maintained
reference implementations rather than something inherited secondhand via
libusb's Darwin backend.

First cut was scoped to the `blackb0x`/`gaster` CLI path only — ramdisk
baking (`bake-firmware`, `BakeRamdisk.cpp`) stayed Linux-only for a
while (real loop-mounted HFS+, `CAP_SYS_ADMIN`), with a macOS build of
`blackb0x` consuming `dist/` output baked elsewhere in the meantime. macOS
support for the ramdisk baker itself has since landed too — see 4a below,
now resolved.

Real blockers, in the code today — **all four now addressed** (code
written and cross-checked to still build clean on Linux; none of it has
been compiled or run on actual macOS/Xcode yet, since no Mac was available
this session):

- ~~`target_link_options(blackb0x PRIVATE -Wl,--allow-multiple-definition)`~~
  — root-caused and fixed properly instead of worked around: diffed
  libusbmuxd's `common/collection.c` against libimobiledevice-glue's
  `src/collection.c` directly (byte-for-byte identical `struct collection`
  layout and function bodies; glue's is a strict superset, adding
  `collection_copy()`, which nothing in libusbmuxd calls). `libusbmuxd_ext`'s
  `INSTALL_COMMAND` (`CMakeLists.txt`) now runs `${CMAKE_AR} d
  ${DEPS_LIB}/libusbmuxd.a collection.o` right after `make install`,
  stripping libusbmuxd's copy of the object out of its installed archive so
  only libimobiledevice-glue's survives to link time. No more duplicate
  symbols to paper over, on any platform — the GNU-ld-only flag is gone
  entirely, not just made conditional. Confirmed: Linux build still links
  clean with it removed.
- ~~`ResourcePath.cpp`'s `resolveGasterPath()`~~ — now branches on
  `#if defined(__APPLE__)`: uses `_NSGetExecutablePath()`
  (`<mach-o/dyld.h>`) + `realpath()` to resolve symlinks (matching what
  `readlink("/proc/self/exe", ...)` already does implicitly on Linux),
  falling back to the existing `readlink`-based path otherwise.
- ~~`DeviceManager.cpp`'s `isUninterruptible()`~~ — now has a `#if
  defined(__APPLE__)` branch that shells out to `ps -o state= -p <pid>` via
  fork/exec+pipe (no `popen()`/`system()`, matching this file's existing
  no-shell convention) and checks for `U` (BSD ps's uninterruptible-wait
  code, same meaning as Linux's D-state), instead of reading
  `/proc/<pid>/status`.
- ~~`runGaster()`'s hard requirement on GNU `stdbuf`~~ — new
  `resolveStdbufBinary()` helper tries plain `stdbuf` first (works
  identically on Linux, and on macOS if the user has opted into Homebrew
  coreutils' "gnubin" PATH shim), then falls back to `gstdbuf` (Homebrew's
  default prefixed name) on Apple platforms. Went with the
  probe-for-both-names fix rather than the alternative
  `forkpty()`-backed rewrite floated here previously — smaller, more
  targeted change; the `forkpty()` rewrite remains on the table later if
  the `stdbuf`/`gstdbuf` dependency itself becomes a real pain point.
- New, beyond the four originally listed here: `gaster` was being built
  with `HAVE_LIBUSB` unconditionally regardless of target OS
  (`CMakeLists.txt`'s `add_executable(gaster ...)` block), which — per the
  next bullet's old wording — meant even a macOS build would've gone
  through libusb's Darwin backend rather than gaster's own real upstream
  IOKit implementation (gaster.c's `#else` branch, gated on `!HAVE_LIBUSB`:
  `<CommonCrypto/CommonCrypto.h>` + `<IOKit/usb/IOUSBLib.h>`, no vendored
  deps needed at all — both are always-present system frameworks). Now
  `if(APPLE) ... else() ... endif()`-gated: Apple builds skip `HAVE_LIBUSB`/
  `deps::usb`/`deps::wolfssl` entirely and link `-framework CoreFoundation
  -framework IOKit` instead; Linux keeps the exact libusb+wolfSSL build it
  already had. This directly answers the next bullet's old open question
  for gaster's own exploit step (no longer inheriting libusb's Darwin
  backend at all) — it doesn't touch `libimobiledevice`/`libirecovery`'s own
  USB transport, which normal-mode/DFU-mode device communication elsewhere
  in `blackb0x` still goes through; whether an IOKit-native path is
  available/needed there too is unexplored.

**Real macOS hardware results are in, and they change the picture:**

- **gaster does not work on macOS, full stop.** Confirmed by real testing
  against AppleTV3,2 hardware: not intermittent, not something a workaround
  fixed — every attempt with gaster on macOS has failed. The IOKit-vs-
  libusb switch above (and everything else tried) did not change this.
  Given this, gaster is **not** the answer to "does the checkm8 chain work
  on macOS" — 4b's `blackb0x-pwn` is.
- **`blackb0x-pwn` (4b below) is known-good on macOS**, confirmed against
  real AppleTV3,2 hardware — this is now blackb0x's actual, working macOS
  checkm8 path, wired up as the default via `--pwntool` (`Cli.hpp`'s
  `CliOptions::pwnTool`, `DeviceManager::checkm8Attempt()`): `gaster` stays
  the unconditional-only option on Linux, `blackb0x-pwn` is the macOS
  default (override with `--pwntool gaster` to force the known-broken path
  anyway, e.g. to keep debugging gaster itself).
- **Apple Silicon tip, from real testing:** a plain (non-Thunderbolt) USB
  hub between the Mac and the Apple TV, instead of a direct connection, has
  made `blackb0x-pwn` reliable. Worth trying first if a direct connection
  is flaky — not yet understood why this matters, just observed.
- **Linux, meanwhile, does not work reliably either** — confirmed on two
  different real PCs (Intel 11th-gen, AMD Zen 2), both showing
  non-deterministic USB behavior during the exploit sequence. This
  reopening was originally prompted by exactly that Linux flakiness (see
  this section's own opening paragraph); switching development focus to
  macOS did produce a working tool, but on a *different* implementation
  (blackb0x-pwn, not gaster) than the one this reopening started out
  trying to fix on Linux, and Linux's own root cause is still unresolved.
  Whether that's fixable in this project's own code, or a property of the
  specific USB controllers/host stacks tested, is still open.
- Whether the `--no-preflight` usbmuxd workaround this repo needs on Linux
  is even relevant against macOS's built-in `usbmuxd` (probably not, since
  it's Apple's own reference daemon) — a docs question, not code. Still
  open, unrelated to the above.
- Whether libusb's Darwin backend can claim a checkm8/DFU-mode Apple TV
  without the system's own `usbmuxd`/`MobileDevice` stack interfering, for
  the libirecovery-mediated parts of the chain (`DeviceManager.cpp`'s
  `sendiBSS`/`sendiBEC`/etc., `irecovery` itself) that still go through
  libusb on every platform even with `--pwntool blackb0x-pwn` (only
  checkm8 itself moved to blackb0x-pwn's IOKit-native libirecovery build —
  see 4b). Given `blackb0x-pwn checkm8` is confirmed working, whatever
  happens next (`sendiBSS`/`sendiBEC`/etc.) is presumably also fine in
  practice — but not independently confirmed bullet-by-bullet.

Low-risk, expected to already work: `libusb_ext`'s `--disable-udev` is a
no-op on Darwin (that configure branch is Linux-only, backend is
autodetected); no other GNU-ld-only flags exist in the vendored
`ExternalProject_Add` blocks; the autotools-based vendored deps
(`libimobiledevice`, `libirecovery`, `wolfssl`, `curl`, `libzip`,
`libpng`, `bzip2`, `zlib`) all build on Darwin routinely elsewhere, given
Xcode CLT + Homebrew's autoconf/automake/libtool/pkg-config in place of
the Linux build-deps list. `Cli.cpp`'s udev/sudo help text and
`IPSWDownloader.hpp`'s `ipswDataRoot()` XDG-only fallback are both
cosmetically Linux-flavored but not blockers.

### 4c. gaster removed entirely (resolved)

`gaster` is gone from the tree — submodule, CMake target, `--pwntool` flag,
and the `regulad/gaster` fork. **It never pwned an AppleTV3,2 on either
Linux 7.1.x or macOS 26**, which is the whole reason. `blackb0x-pwn` is now
the only pwntool and is built unconditionally on every platform, so
everything above that describes a *choice* between the two (item 4's
`--pwntool` wording, 4b's `if(APPLE)` gating) is superseded — kept as
written because it records what was believed at the time.

See `docs/HISTORY.md`'s "gaster is removed" entry for what the fork carried,
what the twelve commits bought, and why deleting the code does not delete
the one finding gaster contributed (the two-implementation control that
disproved "Linux cannot do checkm8").

### 4a. macOS support for the ramdisk baker (`bake-firmware`/`BakeRamdisk.cpp`) (resolved)

Ported. As anticipated below, it ended up *simpler* on macOS than Linux's
loop-mount flow, not harder: `bakeRamdisk()` now has a `#if
defined(__APPLE__)` branch that mounts the original volume once with
`hdiutil attach -nobrowse -readonly`, reads its label via `diskutil info
-plist` (parsed with the already-vendored libplist), copies the content
out to a plain staging directory, splices in `/sbin/launchd` + `/blackb0x`
there, and builds the final correctly-sized volume in one step with
`hdiutil create -srcfolder ... -fs "Case-sensitive HFS+" -format UDRW
-layout NONE` — one real mount instead of Linux's three, no
`CAP_SYS_ADMIN`/loop-mount mechanism needed at all. `copyVolumeHeaderMetadata()`
(pure byte-level work on the raw image) is unchanged and shared by both
platforms. `MountGuard`'s destructor, `st_atim`/`st_mtim` vs
`st_atimespec`/`st_mtimespec`, and the vendored `hfsplus.h`'s `register`
keyword (bracketed with `#define`/`#undef` around the one include that
pulls it in, not editing the vendored header) are all now handled per the
same `#if defined(__APPLE__)` convention used elsewhere in this project.

The `podman`+real-`apt-get` dependency (`scripts/build_deb_cache.py`,
and `BakeRamdisk.cpp`'s own podman-based `computePreinstalledPackages()`
dpkg run) turned out to have no viable macOS path at all — not even
through `podman machine`'s Linux VM, since no containerization was
available in the environment this was ported from, and real Debian
`apt-get` has no native Apple Silicon path either (`dpkg` alone has a
MacPorts port, but not `apt-get`; Fink carries the genuine apt/dpkg
codebase but is unmaintained since Feb 2022 and explicitly unsupported on
Apple Silicon, fink/fink#232). Resolved with a new, deliberately
experimental, portable replacement instead of either container path:
`scripts/build_deb_cache_experimental_no_container.py` (a plain
transitive-closure dependency-group walk over already-vendored
`debcache/`, no version-constraint comparison, no network/root/
container use at all — see its own module docstring for the documented
gaps versus `build_deb_cache.py`, and
`scripts/test_build_deb_cache_experimental_no_container.py` for its 18
fully-portable tests) plus a portable, no-dpkg preinstall step in
`BakeRamdisk.cpp` (extracts each bake-time-eligible `.deb`'s `data.tar.*`
directly and hand-assembles the `dpkg-state/status` stanza, relying on
`computePreinstallEligibleFilenames()`'s existing guarantee that no
bake-time-eligible package has a real preinst/postinst to run in the
first place). `computeGlobalDebcacheOnce()` now picks between the real
podman-based script and this one via `#if defined(__APPLE__)`; the Linux
path is untouched. Building the experimental resolver surfaced (and
fixed, in both the Python script and the identical pre-existing bug in
`BakeRamdisk.cpp`'s own `readDebControlInfo()`) two real bugs: 14
vendored `org.tihmstar.*.deb`s store their control member as plain
`control` rather than the assumed `./control` inside `control.tar.*`,
and `rtadvd`'s bare `Depends: firmware` needs the same synthetic
`firmware` package declaration the podman script and
`kPreinstallInnerScript` already had.

`CMakeLists.txt`'s `add_executable(bake-firmware ...)` block no
longer wraps in `if(NOT APPLE)` — it builds unconditionally now, with no
Apple frameworks linked pre-emptively (matching how `blackb0x`'s own
`if(APPLE)` framework block was arrived at from a real link error, not a
guess).

**Not yet real-hardware/real-macOS verified** — everything above was
built and tested on Linux only (the experimental resolver's 18 tests
pass there; both `blackb0x` and `bake-firmware` build clean with
every `#if defined(__APPLE__)` branch necessarily uncompiled). Two
specific spots are flagged in `BakeRamdisk.cpp`'s own comments as
unverified pending a real Mac: the exact `-fs "Case-sensitive HFS+"`
argument string for `hdiutil create`, and whether `hdiutil create
-format UDRW -layout NONE`'s real output is genuinely flat raw bytes or
carries extra UDIF structure (defensively mitigated by an
`unwrapUDIFIfPresent()` safety net, itself untested). The actual
motivating question behind this whole port — whether baking a ramdisk
natively on the real Mac test machine, rather than copying one over from
elsewhere, resolves the persistent post-`bootx` boot failure documented
elsewhere in this file/`docs/HISTORY.md` — is also still open, only
answerable by running it there.

### 4b. `blackb0x-pwn`: Blackb0x's own original checkm8/SHAtter, standalone and IOKit-only

Added: a new `if(APPLE)`-gated executable target (`CMakeLists.txt`,
`src/Pwn/`), independent of both the main `blackb0x` binary's
gaster-based `checkm8Attempt()` and of gaster itself. Ports
`DeviceManager.m`'s original `+SHAtter:`/`+checkm8:` (deleted from the
tree in `907b64b`, recovered from git history — see `Checkm8Pwn.c`'s own
header comment for the exact `git show` invocation) byte-for-byte to C,
same "translate, don't improve" rule the main binary's own `SHAtter()`
port already follows.

"IOKit-only" is a real, verified build-time guarantee, not just "happens
to run on macOS": `third_party/libirecovery` has its own genuine upstream
`--with-iokit` native Darwin backend (confirmed by reading
`configure.ac`/`libirecovery.c` directly, not assumed) as an alternative
to libusb. `blackb0x-pwn` gets its own separate libirecovery build
(`libirecovery_iokit_ext`, own install prefix, built from a private
`git ls-files`-copied source tree — see that `ExternalProject_Add`'s own
comment in `CMakeLists.txt` for why a copy, not the shared checkout, is
required) with `--with-iokit` passed explicitly, so its configure step
fails loudly if IOKit isn't available rather than silently falling back to
libusb. Confirmed: no `deps::usb` anywhere in this target's link line at
all.

**Status: known-good, confirmed on real macOS + real AppleTV3,2
hardware.** `blackb0x-pwn checkm8` runs and succeeds; `blackb0x` itself now
defaults to it via `--pwntool` (see item 4's own update above) rather than
gaster, which is confirmed *not* to work on macOS at all. Also confirmed
working with the exact same real-hardware caveat item 4 records: a plain
(non-Thunderbolt) USB hub between an Apple Silicon Mac and the Apple TV,
rather than a direct connection, made it reliable.

Before real-hardware confirmation, the only verification available was
compiling and linking `Checkm8Pwn.c` *on Linux* against the existing
libusb-backed libirecovery headers/library (`-Wall -Wextra` clean,
undefined symbols only `irecv_*` + libc) to confirm it's genuinely
backend-agnostic C — real signal, but not a substitute for the real run
that's now happened.

## 5. Pre-patch firmware components ahead of time, instead of after device enumeration

**Partly done** — the non-ramdisk half now has a tool:
`bake-firmware` (`src/BakeFirmware.cpp`) downloads
and patches iBSS/iBEC/KernelCache/DeviceTree ahead of time for every
`(device, buildID)` under `keys/`, writing
`dist/bootchain/<device>_<buildID>/`. It shares `Patcher.cpp` with
`blackb0x` itself, so the two cannot drift — which also makes it the only
way to exercise `patchiBSS()`/`patchiBEC()`/`patchKernel()`, and therefore
the `iBoot32Patcher` binary they fork/exec, across every known firmware
with no hardware attached.

This started life as `bake-all-bootloaders`, a deliberately separate binary
from `bake-all-ramdisks` because it needed no root while the Linux ramdisk
bake needed `CAP_SYS_ADMIN` for its loop mount. With the loop mount gone
(the ramdisk bake is `hdiutil` now, also rootless) that reason went with it
and the two were merged into the single `bake-firmware`, which had been
carrying two verbatim copies of the target enumeration and the
`--signed-only`/`--device`/`--build` filters. `--only bootchain` is the
fast iteration loop the old split used to provide: it skips the entrypoint
cross-compile and the debcache entirely.

Still to do for this item: `blackb0x` itself does not yet *consume*
`dist/bootchain/`. The live path still downloads and patches inside the
window the device is sitting in pwned DFU. Wiring the consumption side up
is the remaining work, and it is the part that needs the real-hardware
timing question below answered first.

Original note follows.

Not started. Today's flow (`Cli.cpp`, ported from `MainView.m`'s
`jailbreakClick`/`checkExploit`/`downloadComponentsForBuildID`/
`componentsReady` chain) only starts downloading and patching iBSS/iBEC/
ramdisk/kernelcache/devicetree for the target firmware *after* a device is
already connected, identified, and the exploit (`checkm8`/`SHAtter`) has
already run — i.e. network I/O and CPU-bound patching both happen inside
the same window the device is expected to already be sitting in pwned DFU
waiting for the next component. Since the target device model/firmware is
knowable ahead of time in the common case (a specific device the user is
about to plug in, or — for `bake-firmware`-style bulk runs — every
firmware this project already knows about), there's no hard reason this
has to be done live: downloading and patching components for the
identified/likely firmware set could happen speculatively before (or
concurrently with) device enumeration/the exploit itself, so that by the
time the device is actually pwned and ready, sending components is just a
local file copy with no network/patch latency in between. Worth
scoping against real hardware timing data (is the current live-patch
window actually a problem in practice, or just a theoretical one) before
committing to the added complexity of a speculative/AOT patch cache.

## 6. Test building all possible ramdisk configurations

Not started. `bake-firmware` now builds on both Linux and macOS (see
item 4a above), but only ever a handful of individual `(device, buildID)`
tuples have actually been baked and checked in either environment this
session — never a full run across every one of the 95 known tuples under
`keys/` (`AppleTV2,1`/`AppleTV3,1`/`AppleTV3,2` combined).
Worth a real `--signed-only`-less full run on each platform (every known
build, not just currently-signed ones) to catch tuple-specific breakage
the handful of spot-checked builds wouldn't — e.g. the AppleTV2,1 4.x
legacy-recreation case `bakeRamdisk()` currently refuses outright (see its
own early bail-out), older firmware branches' different persistence
payloads (`stageVersionBranch()`), or firmware-version-gated `Depends:`
resolution now that the synthetic `firmware` package is pinned to each
device model's newest known version instead of a single hardcoded
constant (see item 4a's own history, and item 8 below) — a bad pin for
one specific device model could silently break every tuple of that
model's dependency resolution without showing up anywhere else.

## 7. CI/CD system for prebuilt, patched firmware suite

Not started. Every patched component this project produces (`dist/*.dmg`
ramdisks, and whatever else `Patcher`'s patch* functions touch) is built
locally, on-demand, by whoever happens to be running `blackb0x`/
`bake-firmware` at the time — there's no automated build producing and
publishing a versioned, prebuilt set of patched firmware components
anyone could just download instead of baking their own. Worth scoping
once item 6 above has actually exercised every known configuration
end-to-end: what a CI pipeline would build (presumably the same full
`bake-firmware` sweep), where prebuilt output would be published, how
staleness/re-bakes get triggered when this project's own patches change
without the underlying Apple firmware changing, and whether publishing
prebuilt jailbreak components anywhere public raises different
considerations than this project's current "you build it yourself"
posture.

## 8. Author a script to generate the (identifier, build) → firmware version binding (DONE)

**Done.** `scripts/generate_firmware_versions.py` regenerates
`misc/firmware_versions.txt` on demand, so it tracks
`keys/` instead of being a stale hand-collected snapshot. The
sourcing logic that was previously only described after the fact is now
checked in and auditable:

- ipsw.me primary, one GET per device model
  (`/v4/device/<model>?type=ipsw`), reading `buildid`/`version` from the
  `firmwares` array — 86 of the 95 tuples.
- AppleDB fallback (`/ios/Apple%20TV%20Software;<buildID>.json`) for the
  internal/beta builds ipsw.me does not carry, reading **`iosVersion`, not
  `version`** — AppleDB's `version` is a marketing number (build 11B553:
  `version` 6.0.2 vs a real ProductVersion of 7.0.4).
- `--verify` proves that rather than trusting it: resolves the build's real
  IPSW URL from AppleDB's own `sources` and range-fetches just that IPSW's
  `BuildManifest.plist` (HTTP Range against the remote zip's central
  directory, no full download) to read the real ProductVersion. Reuses
  `fetch_firmware_component.py`'s existing `HTTPRangeFile`/`fetch_zip_member`
  rather than reimplementing them.
- `--check` exits nonzero on drift between the checked-in file and freshly
  resolved data, comparing data lines only — usable from CI (item 7).

Verified on a real run: all 95 data lines came out byte-identical to the
hand-collected file, and `--verify` independently confirmed 8 of the 9
AppleDB-sourced tuples against real BuildManifest.plists. The 9th,
`AppleTV3,2 12B401`, has no IPSW or OTA source archived anywhere in AppleDB,
so it stays `iosVersion`-only and the generated header now says so per-tuple
instead of that caveat living in a hand-written comment.

One gotcha worth keeping: AppleDB's CDN 403s urllib's default
`Python-urllib/3.x` User-Agent, which presents exactly like "that build does
not exist". The script sends a real one.

Original note follows.

Not started. `bake-firmware` currently resolves the version that
drives persistence-payload selection (`stageVersionBranch()`) and the
synthetic `firmware` dpkg package's pin via a live `newestVersionForDevice()`
call per device model at bake time (`IPSW.cpp`, memoized per run in
`BakeFirmware.cpp` — see `docs/HISTORY.md`/this session's own fix for
why it has to be the newest-known-per-model version, not a given tuple's
own `ProductVersion`). `misc/firmware_versions.txt` (one
`<device> <buildID> <version>` line per known `keys/` tuple)
already exists as a similar, related binding, but was hand-collected by
an agent doing one-off research (ipsw.me queries, range-fetched
`BuildManifest.plist`s, AppleDB cross-checks for builds ipsw.me doesn't
carry) — not reproducible by running a script. Worth authoring a real
script (under `scripts/`, matching this project's existing
`build_deb_cache*.py` house style) that regenerates that same binding on
demand, so it can be refreshed as new builds/tuples get added to
`ImageKeys/` instead of staying a stale, manually-produced snapshot, and
so the exact sourcing logic (ipsw.me primary, `BuildManifest.plist`/AppleDB
fallback for builds ipsw.me doesn't list) is checked-in and auditable
rather than only described after the fact.

## 9. Change the saurik repo version when not building against an iOS 8 version

Not started. `misc/apt/saurik.list` is a static, checked-in file
(`deb http://apt.saurik.com/ ios/8.0 main`) staged verbatim onto every
baked ramdisk regardless of target OS
(`BakeRamdisk.cpp:stageBlackb0xTree()`'s plain `stageFile(...,
resolveMiscPath("apt/saurik.list"), ...)` call) — same class of bug as
item 8's persistence-payload/firmware-deb fix: apt.saurik.com hosts
separate, version-specific `ios/X.0` dist branches (Cydia's repo has
historically been split this way per iOS era), and pinning every bake to
`ios/8.0` is only correct for 8.x-era targets. Once bake-time knows the
real target version to use (the same "newest known version for this
device model" value item 8's fix and this session's bug fix already
thread through for `stageVersionBranch()`/the synthetic `firmware`
package), `saurik.list` needs to pick its dist path from that same value
instead of a fixed string — likely templated the same way
`stagePostinstallScript()` already substitutes `postinstall.sh`'s
`__BLACKB0X_PACKAGES__` placeholder, rather than a plain static
`stageFile()` copy. Needs the actual mapping from OS era to saurik's real
`ios/X.0` dist names confirmed against apt.saurik.com before implementing
(not all of this project's supported OS eras necessarily have their own
distinct saurik dist branch — worth checking which do).

## 10. Additional ramdisk size shedding to get under the 64MiB watermark

**The hard-fail half is DONE.** A finished ramdisk over the limit now fails
the bake instead of warning: `bakeRamdisk()` returns false and removes the
oversized output, so no known-unusable entry is left in `dist/` for
`bake-firmware` to silently reuse on the next run (it skips targets whose
output already exists). Warning-and-succeeding only moved the failure to real
hardware, where it costs a DFU cycle to find and looks like an exploit problem
rather than a size problem.

`DEBUG_RAMDISK_LIMIT_MIB` overrides the limit, following the `DEBUG_`
convention the pwn binaries use: unset is the hardware-confirmed 64 MiB,
`-1` disables the failure and warns only (what the size-shedding work below
needs, to measure real per-tuple sizes including tuples that are over), and
`N` sets an N MiB limit. An unparseable or zero value is rejected rather than
silently falling back, since that would be indistinguishable from the knob
working.

**Still open: whether `kNeverStageDebs` is complete.** That is the actual
size-shedding work, and it still needs item 6's full bake sweep to measure
real per-tuple sizes first.

Original note follows.

Not started. `BakeRamdisk.cpp`'s `kMaxRamdiskSize` (64MiB) is a real,
hardware-confirmed tripwire — its own comment cites a real USB bulk
short-write at exactly byte offset 0x4000000 (64MiB) on a 68.9MiB ramdisk,
and `DeviceManager.cpp`'s `warnIfRamdiskExceedsDeviceLimit()` queries a
given device/firmware's own real `ramdisk-size` getenv value right before
upload as the authoritative per-device check. Exceeding `kMaxRamdiskSize`
today only warns (`outSizeWarning`/`bake-firmware`' own "OK (with size
warning...)" summary line) rather than failing the bake, since some
devices/firmwares may tolerate more or less — but a bake that's already
over 64MiB has no margin left at all for whichever device's real limit
turns out to be at or below that. `kNeverStageDebs`
(`shouldSkipStagingDeb()`, `BakeRamdisk.cpp` ~line 798) already excludes
the largest known offenders — org.xbmc.kodi-atv2 (~40MB), odcctools
(~7.7MB), gettext (~3.2MB), curl (~0.7MB), com.nito.nitotv (~1.65MB) —
each already proven safe to drop via a real dependency-closure audit
(`ar`/`tar` control-file inspection against every vendored `.deb`'s own
Depends:/Pre-Depends:, checked to confirm nothing in `postinstall.sh`'s
real bootstrap closure needs them). That audit is done and closed for
this list — nothing here calls it back into question. What's actually
open: whether this list is complete. Worth a fresh pass once item 6's
full bake sweep exists to measure real per-tuple final sizes across
every known `(device, buildID)` combination (not just whichever were
spot-baked so far), identify which tuples are actually closest to or
over the watermark, and — only if some still are even with the current
exclusion list — run the same audit technique against whatever's left to
find further, currently-unexamined candidates safe to exclude or move to
network-only (staged in `apt-lists/` but not
`private/var/cache/apt/archives/`, same mechanism `kNeverStageDebs`
already uses).

A separate, currently-unexplored avenue: `computePreinstallEligibleFilenames()`
(`BakeRamdisk.cpp` ~line 1099) decides what gets unpacked and marked
installed at BAKE time (i.e. its real file content lands in the baked
ramdisk image) purely on "not in `prebake_package_blacklist.txt`,
transitively" — not on whether it's actually needed before
`postinstall.sh` can run. `postinstall.sh`'s own real bootstrap closure is
already known and much smaller (the same ~20 packages item 10's existing
audit above already enumerated — bash, dpkg, coreutils(-bin), apt7(-lib),
and what THEY pull in). Any currently-prebaked package outside that
closure only needs to be prebaked if nothing else about it requires it at
first boot; once confirmed unneeded by anything that runs before/during
`postinstall.sh`'s own bootstrap, it could instead be left as a plain
cached `.deb` (`private/var/cache/apt/archives/`) + `apt-lists/` entry —
still available for `postinstall.sh`'s own live `apt-get install` to pull
in on-device (network already required there regardless), just not
unpacked and consuming ramdisk space at bake time. Needs the same kind of
real, per-package assertion the existing `kNeverStageDebs` audit already
used (not guessed) before moving anything: confirm nothing in the
bootstrap closure itself, and nothing that runs before `postinstall.sh`
gets a working apt, actually `Depends:`/`Pre-Depends:` on it.

## 11. Convert blackb0x's own install content into a real `xyz.regulad.blackb0x` .deb

Not started. `stageBlackb0xTree()` (`BakeRamdisk.cpp`) currently stages
blackb0x's own install content as plain loose files via `stageFile()`,
untracked by dpkg at all — unlike literally everything else this ramdisk
installs, which goes through the real dpkg/apt mechanism
(`computePreinstalledPackages()`/`stageDebcache()`). Specifically:
`private/etc/apt/sources.list.d/regulad.list` +
`private/etc/apt/trusted.gpg.d/regulad.gpg` (the repo/key pair pointing
at this project's own apt source, staged the exact same way as
saurik/awkwardtv/bigboss/xbmc/net.tihmstar's — see item 9's saurik.list
entry above for that same list/gpg pairing pattern), `postinstall.sh`
itself (templated into `var/.blackb0x/postinstall.sh` by
`stagePostinstallScript()`), and
`System/Library/LaunchDaemons/xyz.regulad.blackb0x.postinstall.plist` (the
LaunchDaemon that actually runs it — already named in this project's own
reverse-DNS style, just not packaged as one). Wrapping these into one real
`xyz.regulad.blackb0x` `.deb` — built at bake time the same way
`stagePostinstallScript()` already templates `postinstall.sh`'s
`__BLACKB0X_PACKAGES__` placeholder, then packaged rather than staged
loose — would let dpkg/apt actually track blackb0x's own install content
like every other package on the device (real `Status:`/`.list`/`.md5sums`
entries, upgradeable/removable through normal apt instead of being
permanently-invisible loose files), instead of being the one piece of
this ramdisk's own content dpkg has no record of at all.
