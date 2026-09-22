# TODO

Open items not yet resolved. See `AGENTS.md` for repo conventions and
`docs/HISTORY.md`/`AGENTS.md's "Install-time design"` for how we got here.

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

`AGENTS.md` stated "CLI-only, Linux-only... dropped, not dual-maintained" as
a "don't re-litigate without asking" convention at the time this was written
— this entry reopened that by explicit request. (That quote is long stale:
AGENTS.md now says "CLI-only, macOS-only", and Linux support is gone
outright. Kept as written because it records what was true then.) Reopened a second time, more
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

**UPDATE — the real macOS link error arrived.** `bake-firmware` does not
link without CoreFoundation, Security and SystemConfiguration: wolfSSL's
`DoAppleNativeCertValidation`/`LoadSystemCaCertsMac` and curl's
`Curl_macos_init` reach into them unconditionally on Darwin. Those three
are linked now, taken from the actual undefined-symbol list. IOKit is
deliberately absent — this binary talks to no USB device, and adding it
for symmetry with `blackb0x` would link a framework nothing references.

**UPDATE — the `.deb`-reading half of this path now runs on real macOS**,
and needed two fixes nothing on Linux could have surfaced. `ar rc` builds
an archive dpkg rejects outright on macOS, because Apple's cctools ar
prepends a `__.SYMDEF SORTED` member ahead of `debian-binary`; `rcS`
suppresses it. And `tar --auto-compress` is create-only in bsdtar, so
`readDebControlInfo()`'s `-t` call was a hard error on every package.
Both fixed; the portable resolver now walks all 107 real archives and
resolves 60 top-level packages to 68 files. See AGENTS.md's "First real
macOS build" for the full list.

**UPDATE — `entrypoint/` builds on macOS too, and needs no cross-toolchain.**
This was briefly the blocker for reaching the `hdiutil` half at all, since
the bake builds `entrypoint/` before it gets that far and
`arm-apple-darwin11-clang` was not installed. It turns out nothing needs to
be: Apple's own `clang` keeps the ARM backend and Apple's own `ld` still
lists `armv6` in `ld -v`, so `entrypoint/Makefile` now passes `-arch armv6`
to the system compiler. Verified artifact: Mach-O armv6, `LC_UNIXTHREAD`,
zero `LC_LOAD_DYLIB`, `_entry` as the thread-state PC, `ldid`-signed
`com.apple.launchd`. The `cctools-port` build and the 1.7GB Xcode 4.6 SDK
extraction are both dropped from the documented path; `brew install ldid` is
the whole setup now.

**Still unverified: the `hdiutil` volume-creation half.** The two spots
flagged below stand exactly as written. One incidental finding toward them:
attaching a bare HFS+ volume with no partition map requires
`-imagekey diskimage-class=CRawDiskImage`, or `hdiutil attach` fails with
"image not recognized" — confirmed by mounting two real decrypted
RestoreRamdisks by hand.

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
`--device`/`--build` filters (there was a `--signed-only` too; it has since
been removed outright — see item 13). `--only bootchain` is the
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
Worth a real full run (every known build, not just the handful spot-checked)
to catch tuple-specific breakage
the handful of spot-checked builds wouldn't — e.g. the AppleTV2,1 4.x
legacy-recreation case `bakeRamdisk()` currently refuses outright (see its
own early bail-out), older firmware branches' different persistence
payloads (`stageVersionBranch()`), or firmware-version-gated `Depends:`
resolution now that the synthetic `firmware` package is pinned to each
device model's newest known version instead of a single hardcoded
constant (see item 4a's own history, and item 8 below) — a bad pin for
one specific device model could silently break every tuple of that
model's dependency resolution without showing up anywhere else.

## 7. CI/CD system for prebuilt, patched firmware suite (BUILT, unrun)

**Done, in `.github/workflows/ci.yml`.** Two halves, matching the two build
targets:

- **`build`**, on every push and PR. `macos-15`, no root, no Theos, no bake.
  Builds `jailbreak` then `authoring` separately so a failure reads as one or
  the other, runs `ctest` plus the resolver's own tests, then asserts the
  invariants that actually broke during the macOS port: no `/opt/homebrew` or
  `/usr/local` in any shipped binary's `otool -L`, `entrypoint` still armv6 +
  `LC_UNIXTHREAD` + no dyld, and the vendored apt actually runs.
- **`bake`**, on every push plus the monthly schedule and manual dispatch.
  Needs root, Theos and the network -- exactly the requirements this exists to
  keep off an end user's machine. One runner per device, `fail-fast: false`,
  so one device's failure does not deny the other two their artifacts. Uploads
  `dist/` with 90-day retention. Skipped on `pull_request` only: it runs the
  branch's code under sudo, and a fork PR is untrusted by definition.

**Refreshed on the 23rd of every month** (`cron: "0 5 23 * *"`). An earlier
draft approximated a 45-day interval with a monthly cron plus a job that
checked artifact age and skipped when the newest was younger than 45 days;
that was dropped as more machinery than the problem needed. Monthly also stays
clear of GitHub disabling schedules after 60 days of inactivity.

**It bakes one build, not 95.** `kJailbreakTargetBuild` (`Cli.cpp`) pins
`10B329a` as the ramdisk vehicle for every real run regardless of device, and
all three devices have keys for it. Baking the full 95 would spend hours
producing artifacts nothing requests. Widen the `--build` filter if that pin
moves.

**Unrun.** Written and validated with `actionlint` (which caught a real bug: a
leading `!` on the dyld check disabled errexit, so a regression would have
passed silently), and every check it performs was run locally against the real
tree. But no GitHub runner has executed it. First real run is the test.

Still open from the original note: whether publishing prebuilt jailbreak
components anywhere public raises different considerations than this project's
current "you build it yourself" posture. The workflow publishes to Actions
artifacts, not a public release, which is the conservative end of that
question but not an answer to it.

Original note follows.

## 7 (original). CI/CD system for prebuilt, patched firmware suite

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

## 10. Additional ramdisk size shedding to get under the 64MiB watermark (LARGELY MOOT)

**The watermark is no longer the binding constraint.** A real finished bake of
AppleTV3,2 10B329a came out at **30.0 MiB against the 64 MiB ceiling**, once
the payload was decmpfs-compressed *before* the volume was sized (item 16).
This entry was written when the same bake was producing 70.9 MiB.

With 34.0 MiB of headroom, `kNeverStageDebs` shrank from five entries to two.
`odcctools` (1.04 MiB), `gettext` (0.73 MiB) and `curl` (0.26 MiB) are staged
again -- they had been excluded for size alone, and 2.03 MiB is nearly free
now. They install offline instead of needing a network at first boot.

What remains, and why:

| package | .deb size | why it stays |
|---|---|---|
| `org.xbmc.kodi-atv2` | 35.12 MiB | 30.0 + 35.12 = 65.1 MiB, about 1.1 MiB over the ceiling |

`com.nito.nitotv` came off this list too, because it was on the wrong one.
It has a real postinst that deletes live device files
(`/var/mobile/Media/Photos/seas0nTV.png`,
`/Library/MobileSubstrate/DynamicLibraries/apocalypsePony*`), which is exactly
what `prebake_package_blacklist.txt` exists for — and it was already listed
there. Having it in `kNeverStageDebs` as well protected nothing: the prebake
blacklist already stopped it being force-installed at bake time, so the second
listing only made it network-only for no reason. At 1.61 MiB it now ships, and
apt installs it on-device where its postinst can actually run.

**The list moved to `misc/never_stage_debs.txt`**, read by
`shouldSkipStagingDeb()`, so each entry can carry its own reasoning. That file's
header spells out the distinction the nitotv mistake turned on:

- `prebake_package_blacklist.txt` — "do not force-install at bake time"; the
  `.deb` is still staged, so apt can install it offline.
- `never_stage_debs.txt` — "do not ship this `.deb` at all"; purely a size
  decision.

A package with a real maintainer script belongs in the first, never the second.

Kodi is the only real size casualty left, and it misses by roughly a megabyte.
The one route to it is item 14's `nvram` removal, which would free 14.22 MiB of
pristine-tree dylibs -- a real boot-chain change, and not worth spending on
Kodi.

Note the unit that matters is **.deb archive size, not unpacked size**. These
are staged into apt's cache as archives and never extracted at bake time, so
odcctools' ~7.4 MiB unpacked footprint was never the cost; its 1.04 MiB archive
was. afsctool does not shrink them either -- they are already compressed.

Original note follows.

## 10 (original). Additional ramdisk size shedding to get under the 64MiB watermark

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

## 11. Convert blackb0x's own install content into a real `xyz.regulad.blackb0x` .deb (DONE)

**Done, in commit 2ff796c.** This entry said "Not started" long after it
shipped; corrected here against the actual code.

What exists now: `stageBlackb0xPackage()` (`BakeRamdisk.cpp`) runs
`package/build.sh` to produce a real `xyz.regulad.blackb0x` `.deb` with
Theos's `dm.pl`, extracts it, merges its payload into the staged tree
(remapping top-level `etc/`/`var/` to `private/`), and registers it with
`stageManualDpkgInstall()` carrying the real on-device paths — the
`regulad.list`/`regulad.gpg` pair, `postinstall.sh`, the
`xyz.regulad.blackb0x.postinstall.plist` LaunchDaemon and `/var/root/.profile`
included. So all of it is dpkg-tracked now, with real `Status:`/`.list`
entries, exactly as this item asked. `package/build.sh` templates
`postinstall.sh`'s `__BLACKB0X_PACKAGES__` from `package/packages.txt` rather
than from a bake's resolved closure, which is what keeps the package
independently buildable.

See AGENTS.md's "Install-time design" section, step 6, for the as-built
description.

Original note follows.

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

## 12. Kernelcache decrypt fails on some builds; the cause is NOT yet known

**CORRECTION, from a wider sample.** This entry used to claim the failures
correlate exactly with a `DATA` `dataSize` that is not a multiple of 16. That
was a small-sample artifact and is **false**. Counterexample, from a real
successful bake: `AppleTV3,2` 10B329a has `dlen=6004801`, which is 1 mod 16,
and it patches, decrypts and bakes into a working 30 MiB ramdisk.

Everything measured so far:

| tuple | dlen | dlen mod 16 | result |
|---|---|---|---|
| AppleTV3,1 12H606 | 8312768 | 0 | patches |
| AppleTV3,2 10B329a | 6004801 | 1 | **patches** |
| AppleTV3,1 10B329a | 6813386 | 10 | fails |
| AppleTV2,1 10B809 | 6284571 | 11 | fails |
| AppleTV3,2 12H606 | 7618269 | 13 | fails |
| AppleTV2,1 11D258 | 6812814 | 14 | fails |

An alternative rule -- that it fails when the tag's padded size
`((total - 12) / 16) * 16` lands short of `dlen` -- does not hold either:
AppleTV3,2 12H606 pads to 7618272, three bytes PAST its dlen of 7618269, and
still fails. So the determinant is genuinely unknown.

What is still solid, and worth keeping:

- The symptom is always `createAbstractFileFromComp()` (`lzssfile.c`) rejecting
  the stream, with `decompress_lzss()` returning ~50-70 bytes fewer than the
  `complzss` header claims.
- It is not a wrong key. The header parses, the sizes are plausible, and
  `length_compressed + 0x180` lands exactly on `dlen`. The bulk decompresses;
  only the tail is wrong.
- It is not the decrypt/encrypt length asymmetry in `setKeyImg3()` vs
  `closeImg3()`. Changing the decrypt side to match was tried and moves the
  decompressed length without reaching the claimed one.
- It is not a macOS regression; `docs/HISTORY.md` records 10B144b failing the
  same way on Linux.

**It now blocks two of the three devices.** At `kJailbreakTargetBuild`
(10B329a), `AppleTV2,1` and `AppleTV3,1` both fail on the kernel while their
iBSS, iBEC and DeviceTree all patch cleanly, so no complete suite can be built
for them. `.github/workflows/ci.yml`'s bake matrix is restricted to
`AppleTV3,2` for exactly this reason; the other two go back the moment this is
fixed.

The next concrete move is unchanged: diff a known-good decryption of one
failing kernelcache against xpwn's output over the last few hundred bytes,
which localises the defect to decrypt or to `decompress_lzss()` rather than
guessing at rules.

## 12 (original). Kernelcache decrypt fails on every IMG3 whose `DATA` size is not 16-aligned

Found by the first real macOS `bake-firmware --only bootchain` sweep, run at
the time over the five then-currently-signed builds. Three of the five currently-signed builds fail to patch their
kernelcache, while iBSS, iBEC and DeviceTree succeed for all five:

| device | build | kernel |
|---|---|---|
| AppleTV2,1 | 10B809 | FAIL |
| AppleTV2,1 | 11D258 | FAIL |
| AppleTV3,1 | 10B809 | ok |
| AppleTV3,1 | 12H606 | ok |
| AppleTV3,2 | 12H606 | FAIL |

**This is not a macOS regression** — nothing in the failing path is
platform-specific, and `docs/HISTORY.md` already records 10B144b failing the
same way during a Linux sweep. It is also not a crash: `Patcher::patchKernel()`
already detects the empty `decrypt()` output and skips the firmware cleanly,
which is why a whole sweep survives it.

The correlation is exact. Every failing build's IMG3 `DATA` tag has a
`dataSize` that is **not** a multiple of 16 (13, 11 and 14 trailing bytes
respectively); the passing `AppleTV3,1` 12H606 is exactly block-aligned.
`decrypt()` is not what reports the failure — `openAbstractFile2()` returns
NULL because xpwn's `createAbstractFileFromComp()` (`lzssfile.c`) rejects the
stream: `decompress_lzss()` returns ~50-70 bytes fewer than the `complzss`
header's own `length_uncompressed`.

Two things already ruled out, so nobody repeats them:

- **Not a wrong key.** The `complzss` header sits at offset 0 of the encrypted
  payload and parses correctly (right signature, plausible compressed and
  uncompressed sizes, and `length_compressed + 0x180` lands exactly on
  `dataSize`). A wrong key would produce noise there. The bulk of the stream
  decompresses; only the tail is wrong.
- **Not the decrypt/encrypt length asymmetry, at least not on its own.**
  `img3.c`'s `setKeyImg3()` decrypts
  `((header->size - sizeof(AppleImg3Header)) / 16) * 16` bytes while
  `closeImg3()` encrypts `(header->dataSize / 16) * 16` — genuinely different
  formulas. Changing the decrypt side to match the encrypt side was tried and
  **does not fix it**: the decompressed length moves (12894145 -> 12894142 on
  AppleTV3,2 12H606) without reaching the claimed 12894208. The padded form is
  probably the correct one anyway, since Apple pads the encrypted payload up to
  the block boundary and the tag is sized to hold exactly that.

Worth re-reading with this in hand: `IMG3_AES_OVERREAD_PAD`'s own comment
attributes an observed valgrind over-read to wolfSSL lookahead. That over-read
is the same ~11 bytes as the gap between `dataSize` and the padded tag size, so
the two may be the same phenomenon described twice. Confirming or refuting that
is a good first move.

Next step is probably to diff a known-good decryption of one failing kernelcache
(any independent AES-CBC implementation) against xpwn's output over the last few
hundred bytes, which localises the defect to decrypt or to `decompress_lzss()`
without guessing.

## 13. `--signed-only` removed from `bake-firmware` (DONE)

**Done.** The flag, its argument parsing, and its target-filter block are gone
from `src/BakeFirmware.cpp`.

It asked ipsw.me which builds Apple is still actively signing and baked only
those. That filter answers a question the baker has no stake in. What this
project can jailbreak is decided by which `(device, buildID)` tuples have a
`.keys` file under `keys/` and patches that work against them, and the live
path pins `kJailbreakTargetBuild` (`Cli.cpp`, currently `10B329a`) regardless.
Apple's signing window neither creates nor removes a bakeable target, so the
flag only ever hid targets that were perfectly valid to bake, at the cost of
one network round trip per device model. `--device`/`--build` remain as the
way to narrow a run.

**`signedBuildsForDevice()` (`IPSW.hpp`) stays, and is not dead.** After this
removal it has exactly two callers, both `--stock-*` diagnostic routes, both
of which genuinely need Apple's signing window because a real SHSH ticket can
only be issued inside it:

- `Personalize.cpp`'s `requestTSS()` — warns before a TSS request Apple is
  near-certain to refuse. Reached only via `DeviceManager.cpp`'s
  `personalizeIMG3Component()`, inside `if ((isATV31 || isATV32) &&
  stockSecurerom)`.
- `Cli.cpp`'s `--stock-recovery` build selection — prefers a still-signed
  build blackb0x has local keys for over blindly resolving `"latest"`.
  Guarded by `if (options.stockSecurerom || options.stockRecovery)` and then
  narrowed by `if (!options.stockSecurerom)`, so `--stock-recovery` alone.

One real user-facing bug fixed on the way out: `Patcher.cpp`'s no-baked-ramdisk
PANIC message told the user to re-run `bake-firmware --signed-only`, a flag
that no longer exists. It now prints the exact `--device`/`--build` invocation
for the tuple that is actually missing.

Do not wire signing status back into the baker.

## 14. Deleting Apple's pristine ramdisk content for space (measured, rejected for now)

Considered as a size lever for item 10 and rejected on measurement, recorded so
nobody re-derives it.

`entrypoint.c` is freestanding, so almost nothing Apple ships on the restore
ramdisk is referenced at boot. Its whole dependency on the pristine tree is:

- `/dev` — only needs to exist as an empty directory; the kernel mounts devfs
  onto it, which is where `/dev/console` and `/dev/disk0s1s1` come from.
- `/mnt1` — a mountpoint, already empty on every pristine ramdisk.
- `/usr/sbin/nvram` — `execve`'d by `set_auto_boot()`.

Both filesystem mounts go through `sys_mount()` directly, so `mount_hfs`, `fsck`,
`mount` and the rest are unused, and nothing reads `/bin`,
`/System/Library/LaunchDaemons`, `restored_external`, `asr` or `sed`.

**Why deleting the rest is not worth it: `nvram`'s closure is the ramdisk.**
Measured against a real AppleTV3,1 12H606 RestoreRamdisk, its transitive dylib
closure is 43 files totalling **14.22 MiB of a 15 MiB volume** — CoreFoundation
(4.5 MB), libobjc (2.1 MB), libicucore (2.1 MB), IOKit, dyld and all of
`/usr/lib/system`. Keeping one 36 KB binary means keeping essentially everything,
so the saving is under a megabyte, against a real risk of deleting something
load-bearing that would only show up as a device that will not boot.

**The version that WOULD pay: drop `nvram` itself.** It exists only for
`set_auto_boot()`, and the host can do that job instead — `DeviceManager.cpp`
already sends `setenv auto-boot false` + `saveenv` to iBEC in
`sendStockRestoreTail()`, so issuing the `auto-boot=1` equivalent before `bootx`
moves it off the device entirely and makes the whole 14.22 MiB closure deletable.
That roughly doubles the `/blackb0x` budget under the 64 MiB ceiling. Not done:
it is a real behavioural change to the boot chain, unverifiable without hardware,
and a mistake in it presents as an exploit failure rather than a size problem.

**A larger, zero-risk saving is already available** without deleting anything —
see item 15. The current bake inflates Apple's 15 MiB to 34 MiB by destroying
HFS+ compression, so fixing the mechanism recovers more than deleting the content
would.

## 15. `bakeRamdisk()` grows the original volume instead of rebuilding it (DONE)

**Done.** The macOS branch now resizes, attaches with `-owners on`, splices
`/sbin/launchd` and copies `/blackb0x` straight onto the mounted original, then
First real bake attempt got through the whole bootchain, the debcache
resolution, the preinstall-eligibility pass and the staging decisions, then died
in `package/build.sh` with an empty stderr. Cause: **`package/build.sh` was
committed mode 0644**, and `stageBlackb0xPackage()` `execvp()`s it directly, so
exec failed with EACCES. It was invisible because `runCommand()`'s child did a
bare `_exit(127)` after `execvp()` and never said why. Both fixed: the script
(and `scripts/push_authorized_keys.sh`, run directly per the README) are 0755 in
git now, and `runCommand()` reports the real `strerror(errno)` for a failed
`chdir`/`execvp` and names a signal if the child was killed. Worth remembering as
a class: anything this project fork/execs out of the repo needs its mode tracked
in git, not just locally.

Second failure, one step further on: `package/build.sh` **refused to run as
root**, while `bakeRamdisk()` **requires** root and calls it via
`stageBlackb0xPackage()`. A mutual deadlock that predates all of this work and
was only reachable once the bake got far enough to invoke the package builder.
Both checks were individually well-reasoned. `dm.pl` picks ownership off its
own real uid (confirmed by reading `$THEOS/bin/dm.pl`): non-root forces
`root:wheel`, root preserves what is on disk. Fixed by making the root branch
converge instead of refusing — `build.sh` now does `chown -R 0:0` on the
staging tree when euid is 0, so `dm.pl`'s preserve branch records the same
`root:wheel` its non-root branch forces. Same `.deb` either way; the non-root
path was re-verified to still stamp `root:wheel`. Note this is exactly the
correction the old containerized build already applied, so the refusal's claim
that "natively there is nothing to correct" was the one wrong part.

detaches. `readVolumeLabel()`, `copyVolumeHeaderMetadata()` and
`unwrapUDIFIfPresent()` all existed only to make a synthesized volume resemble
the original and are deleted; `<plist/plist.h>` went with them, since
`readVolumeLabel()` was this file's only plist consumer. Not yet run end to end
(needs root plus a real bake). The rationale below is kept as the record.

The macOS branch currently attaches the original read-only, `cp -a`s its whole
content out to a plain host directory, stages `/blackb0x` and the spliced
`launchd` there, and synthesizes a brand-new volume with
`hdiutil create -srcfolder`. That is backwards, in two directions at once.

**Upstream did it the other way.** `origin/main:Blackb0x/Source/Patcher.mm`'s
`patchRamdisk:ssh:` runs `hdiutil resize -size 60MB <decryptedDMG>`, then
`hdiutil attach`, then writes new content straight into the mounted original with
`tar -xvf ... -C <mountpoint>`, then detaches. `hdiutil create -srcfolder` appears
exactly once, inside `if([path containsString:@"AppleTV2,1_4."])`, for the oldest
firmware where there is genuinely nothing to preserve. This port generalized that
one legacy special case into the main mechanism — and then refuses the 4.x case,
which is the one place it was right.

**What the round trip costs, all measured on real decrypted ramdisks:**

- **Ownership, silently.** The attach passes no `-owners on`, so macOS maps every
  file to the invoking user. Same image, two attaches: with the current flags
  `/sbin/launchd` reads as `regulad:staff`; with `-owners on` it reads `root:wheel`.
  The function requires root so its `chown()`s work, then copies the wrong uid
  anyway. Every file of Apple's tree would bake in owned by uid 501.
- **HFS+ compression, worth 19 MiB.** Apple's content is decmpfs-compressed.
  `du` on the mounted original: **15 MB**. After `cp -a` to a staging dir:
  **34 MB**. `/sbin/halt` goes from `compressed` to nothing. Against a 64 MiB hard
  ceiling this is the single largest waste in the whole bake.
- **Hard links.** `/sbin/halt` and `/sbin/reboot` are one inode, nlink 2. BSD
  `cp -R` writes two copies.
- **Volume identity.** A new volume means new CNIDs, a new UUID and a new header,
  which is the only reason `copyVolumeHeaderMetadata()` exists. Growing in place
  makes that function unnecessary.

**The replacement, verified standalone on a real ramdisk:**

```
hdiutil resize -sectors <computed> -imagekey diskimage-class=CRawDiskImage <img>
hdiutil attach -nobrowse -owners on -imagekey diskimage-class=CRawDiskImage \
    -mountpoint <mp> <img>
# write /blackb0x and splice launchd directly into <mp>
hdiutil detach <mp>
```

Confirmed on AppleTV3,1 12H606: the image grew 16.6 MB -> 60 MB, the filesystem
grew with it (`df`: 60Mi total, 44Mi free), `/sbin/launchd` kept `root:wheel` and
its 2016 timestamp, and new files wrote fine. `-imagekey
diskimage-class=CRawDiskImage` is REQUIRED on both commands for a bare HFS+ image
with no partition map; without it `attach` fails with "image not recognized".

Keep the port's dynamic sizing (a real improvement over upstream's fixed 60MB) and
feed the computed size to `resize` instead of `create`. Leave the AppleTV2,1 4.x
bail-out alone; nothing has ever tested that path.

**Root is still required, and that is not fixable.** Verified as a normal user:
`hdiutil attach`/`resize` need no privilege, but `chown()` to root:wheel returns
EPERM, writing into the pristine root-owned `/sbin/launchd` returns EACCES, and
new files land as the invoking user. Staging real ownership goes through the VFS
and is root-only on Darwin. (`bake-firmware` used to print "No root needed"
unconditionally while `bakeRamdisk()` hard-failed without it; that contradiction
is fixed.) Note also that macOS mounts disk images `nosuid`, which does not matter
here: `chmod 4755` still records the bit on disk, it just is not honoured on the
build host.

## 16. decmpfs-compressing our own staged content (DONE)

**Ordering correction, learned the hard way.** The first implementation
compressed the payload AFTER copying it onto the mounted volume, then tried to
shrink. That saved almost nothing -- a real bake went 70.9 MiB -> 67.8 MiB --
for two separate reasons, both now fixed:

- **In-place compression cannot be reclaimed.** HFS+ shrinks only down to its
  highest allocated block, and compressing in place frees blocks scattered
  through the volume without relocating anything. Measured: usage fell 62 MiB ->
  31 MiB and the minimum achievable image size stayed at 73 MiB. Compress the
  staging tree on the host FIRST, then grow the volume to fit the compressed
  size. Growing the right amount once is the only arrangement that pays.
- **`hdiutil resize -limits` lies for a bare raw HFS+ image.** Its first field
  reports the image's ORIGINAL size (19440 sectors on a real AppleTV3,2
  ramdisk) no matter how much is allocated, and resizing to it fails EINVAL.
  The code shrank to that, failed, warned, and carried on with a full-size
  image. Use `-size min` (which does the arithmetic itself) or `-alllimits`
  (which reports the real per-image minimum). Shrink itself works fine -- the
  earlier conclusion that it was unusable was wrong, and came from using the
  wrong flag.

**`ditto`, not `cp -a`.** decmpfs does not survive `cp`, which reads through the
VFS and gets decompressed bytes. Measured on the same tree, APFS staging ->
HFS+ volume: ditto lands 3.3 MB still flagged `compressed`, `cp -a` lands 10 MB
with the flag gone. ditto also preserves mode, owner, group, xattrs and ACLs,
which is what `cp -a` was there for. A `tar` pipe works too.

**`directoryContentSize()` now measures `st_blocks`, not `fs::file_size`.** A
compressed file still reports its full uncompressed length through `stat()`, so
sizing from the logical length would size the volume for the uncompressed tree
and undo the whole exercise.

**Verified end to end on a realistic tree** (all 63 non-`kNeverStageDebs`
package payloads): 52 MiB staged uncompressed, 22 MiB after
`afsctool -c -T ZLIB`, volume grown to 35 MiB, `ditto` preserved every
compressed file, `-size min` brought the finished raw image to 31.7 MiB. All
decmpfs entries are type 3 or 4; zero LZVN.

One scanning gotcha worth knowing: `getxattr()` follows symlinks by default,
and these package trees contain absolute symlinks (`/bin/more` ->
`/usr/bin/less`). Auditing compression types without `XATTR_NOFOLLOW` reads the
HOST's own LZVN-compressed system binaries and reports phantom type-8 files.

Not done. Worth doing, with one real constraint that makes it non-trivial.

Item 15 stopped the bake from *destroying* Apple's HFS+ compression. The
obvious follow-up is to *add* compression to the content this project stages
itself, which is currently written uncompressed.

**The payoff is large.** `build/blackb0x` (5,329,096 bytes) compresses to 1.9 MB
on a real HFS+ volume via `ditto --hfsCompression`, a 64% saving. The staged
`.deb` bytes in apt's cache will not shrink (already compressed), but the
bake-time-preinstalled package payloads are ordinary binaries and libraries and
should compress comparably.

**The constraint: the compression type must be ZLIB, and modern macOS will not
pick it for you.** decmpfs type support is per-kernel, and this project's
firmware range spans 2012 to 2016. Measured directly on real decrypted
ramdisks:

| firmware | decmpfs types present |
|---|---|
| AppleTV2,1 10B809 (tvOS 6.1, xnu-2107) | 3 and 4 only (ZLIB xattr / ZLIB resource fork) |
| AppleTV3,1 12H606 (tvOS 8.4.2, xnu-2784) | 3, 4, and 8 (LZVN resource fork) |

So ZLIB is safe across the whole range; LZVN is only safe on the 8.4.x targets.
And `ditto --hfsCompression` on macOS 26 produces **type 8 (LZVN)** — verified
by reading the `com.apple.decmpfs` xattr off a file it wrote. Using it naively
would bake a ramdisk that boots on 8.4.x and fails on 6.1.x, discoverable only
on hardware and presenting as an exploit failure.

`ditto` exposes no type selector. Options, none free:

- `afsctool -c` can target zlib, but is a third-party dependency this project
  does not have and would have to vendor.
- Write decmpfs by hand: deflate the data, set `com.apple.decmpfs` (magic
  `fpmc`, type 3 inline or 4 in the resource fork, plus the uncompressed size)
  and set `UF_COMPRESSED`. Fully documented format, no new dependency, but easy
  to corrupt silently.
- Compress only when the target is 8.4.x and skip otherwise. Cheapest to build,
  but makes the output firmware-dependent in a new way.

**Two things already confirmed safe**, so they need not be re-tested:

- Splicing into a decmpfs-compressed file works. The pristine `/sbin/launchd`
  is compressed on every firmware checked, and `open(O_WRONLY|O_TRUNC)` +
  write leaves the file byte-identical to what was written, with the
  `compressed` flag and the `com.apple.decmpfs` xattr both cleared.
- Compression on the ramdisk does not leak onto the device. `merge_tree()`
  (`entrypoint.c`) reads through the VFS with plain `read()`, which decompresses
  transparently, so `/mnt1` receives ordinary uncompressed files. The saving is
  purely ramdisk-side, which is exactly where the 64 MiB ceiling binds.


## 17. Replacing the experimental resolver with real apt (VENDORED AND BUILDING; not yet wired in)

**apt now builds and solves natively on macOS.** `third_party/apt` is a real
submodule (`regulad/apt`@`blackb0x`, off Debian's 2.9.4 tag) and produces
`apt 2.9.4 (darwin-arm)` from a clean checkout.

What it took, so nobody rediscovers it:

- **apt7 itself is unobtainable.** `git.saurik.com` is down, telesphoreo.org is
  gone, and the GitHub user `apt7` is an unrelated person's computer-graphics
  coursework (`bezier.cpp`, `cohentoviewport.cpp`), not Debian apt. Do not go
  looking again.
- **Homebrew's `apt` formula cannot work on Darwin.** It pulls `libcap`,
  `systemd` and `util-linux`, which are Linux-only by nature. Confirmed by a
  real install attempt failing on `libcap`. Note apt itself needs none of the
  three: its own CMake marks systemd optional and never mentions libcap or
  libmount.
- **Procursus is the real precedent.** They build this exact apt version for
  Darwin, and their `build_patch/apt` (nine diffs) plus `build_patch/apt-macos`
  (`apt-key.diff`) apply cleanly to 2.9.4. Their `makefiles/apt.mk` also moves
  `private-output.cc` and `algorithms.cc` to `.mm` (both reach into Foundation;
  as plain C++ every Objective-C declaration in `NSObjCRuntime.h` fails) and
  copies `memrchr.cc` into `ftparchive/`.
- **One fix beyond their set was needed**, for a toolchain newer than theirs:
  `cacheset.h`'s `Container_iterator` arithmetic operators had to become
  `const`. See that commit for the reasoning.

Build requirements (host tools, not vendored libraries — apt is never linked
into anything this project ships): Homebrew `berkeley-db@5`, `openssl@3`,
`xxhash`, `lz4`, `xz`, `gettext`, `dpkg`; `Dpkg.pm` on `PERL5LIB` (it lives in
dpkg's `libexec/lib/perl5`, not a default `@INC` path); and julian-klode's
`triehash` on `PATH`. CMake needs `BERKELEY_INCLUDE_DIRS`/`BERKELEY_LIBRARIES`
passed explicitly, since apt's `FindBerkeley.cmake` does not know Homebrew's
keg-only layout. `USE_NLS=1`, not 0 — with NLS off, `apti18n.h`'s stub
declarations collide with the system headers' exception specifications.

**Proven to actually solve**, against the real `debcache/`:

- `apt-ftparchive packages debcache` indexes all 107 packages.
- With a synthetic root (`Dir::State`, `Dir::Cache`, `Dir::Etc`, `Dir::Log`,
  `Dir::Bin::methods` pointing at the build tree) `apt-get update` succeeds off
  a `file://` source.
- `apt-get -s install cydia openssh` resolves **31 packages** with real version
  constraints and `Provides:` handling.

Three configuration facts that cost time:

- `APT::System "Debian dpkg interface"` must be set explicitly, or apt exits
  with "Unable to determine a suitable packaging system type".
- `Dir::Bin::methods` must point at the build tree's `methods/`, or every fetch
  fails with "The method driver .../file could not be found".
- The synthetic `firmware` package must be declared installed in the status
  file at the target's real version, exactly as the existing scripts already
  do. Without it `com.saurik.patcyh` fails on `Depends: firmware (>= 5.3)`.

**Steps 1 and 2 are DONE.** `CMakeLists.txt` has an `apt_ext` ExternalProject
and an `apt` target, and `triehash` is vendored at `third_party/triehash` (a
single MIT-licensed Perl script; apt wants it on `PATH` under the bare name, so
the build creates a small symlink shim directory). Homebrew prefixes are
resolved at configure time via `brew --prefix`, not hardcoded, because these
are keg-only formulae whose paths differ by architecture.

`apt` is **`EXCLUDE_FROM_ALL`** and must be asked for by name:

```
cmake --build build --target apt
```

That is deliberate. It is a large C++ build most work here never touches, and
it needs Homebrew formulae the rest of the project does not — an ordinary
`cmake --build build` failing on a machine without `berkeley-db@5` would be a
bad trade for a tool only the ramdisk bake uses. Verified: a normal build is
completely unaffected.

Output lands in one predictable place rather than an ExternalProject prefix:

```
build/apt-tools/{apt-get,apt-cache,apt-config,apt-ftparchive}
build/apt-tools/methods/...
```

`methods/` has to come along, because apt shells out to `methods/file` for a
`file://` source. Verified end to end from the staged copies alone:
`apt-ftparchive` indexes debcache, `apt-get update` succeeds, and
`apt-get -s install cydia openssh` resolves 31 packages.

**Remaining work to actually replace the resolver:**

3. Replace `computeGlobalDebcacheOnce()`'s call to the experimental resolver
   with an apt invocation, parsing the "The following NEW packages will be
   installed:" block.
4. Handle the one expected failure mode: apt reports
   "Couldn't configure grep, probably a dependency cycle" because this
   ecosystem has a genuine circular `Pre-Depends` chain (`dpkg -> tar ->
   gzip/lzma -> sed -> dpkg`), which this project already documents. That is an
   *ordering* failure, not a resolution failure — the package set is printed
   correctly before it. Parse the list and ignore the ordering error, or pass
   `-o APT::Immediate-Configure=false`.
5. Only then delete `scripts/build_deb_cache_experimental_no_container.py` and
   its tests.

Original note follows.

Open. `scripts/build_deb_cache_experimental_no_container.py` is what every bake
uses to resolve `package/packages.txt` into a `.deb` closure, and it is
deliberately naive: a transitive walk by bare package name, with no version
constraint comparison and no real `Provides:`/`Conflicts:`/`Breaks:` semantics.
Its own docstring lists the gaps. The goal is to delete it in favour of a real
solver.

**Homebrew's `apt` formula is not the answer.** It exists (`apt` 3.3.3,
keg-only) but its dependency list includes `libcap`, `systemd` and
`util-linux`, and a real install attempt fails because `libcap` does not build
on macOS. Those are Linux-only by nature, not packaging accidents. Confirmed,
not assumed.

Better lead: **the era-appropriate apt is already proven to build for Darwin,
because it is what runs on the device.** The on-device package manager is
`apt7 0.7.25.3` (saurik's Telesphoreo port), an ARM Darwin build of real
Debian apt. That version predates apt's dependency on libcap/systemd/libmount
entirely. Vendoring that source as a `third_party/` submodule and building it
for the host fits this project's existing convention exactly ("every
third-party dependency is a git submodule, built from source"), and has a
second advantage over modern apt: bake-time resolution would run the *same
solver version* the device itself runs, so the two cannot disagree.

Worth checking before committing to it: whether saurik's apt7 source is still
retrievable, how much of its build assumes an iOS cross-toolchain rather than a
plain host build, and whether a host build can be pointed at a synthetic root
(`-o Dir::State=`, `-o Dir::Cache=`, `-o Dir::Etc=`) over `debcache/` without
needing a real dpkg database. The resolution job itself is architecture-
independent -- it is reading `Packages` indices and control fields, not
executing ARM binaries -- so nothing requires the solver to be an ARM build.

Until then the experimental resolver stays, and its output is at least
load-bearing enough to have produced a closure that assembles into a real
ramdisk.


## 18. `package/packages.txt` entries that cannot all coexist (RESOLVED)

**Both fixed. `packages.txt` is 60 entries now and real apt solves it cleanly
for every firmware, with zero drops.** The `--allow-drops` machinery stays in
`build_deb_cache_apt.py` as a guard, but nothing trips it any more.

- `apt7-ssl` removed. It is superseded by `apt7-lib`, which declares
  `Provides:`/`Conflicts:`/`Replaces:` on it -- the standard idiom for "this
  absorbed that". Nothing in the 107-package debcache depends on it, it is
  `Priority: optional` against apt7-lib's `required`, and it would have dragged
  in `curl`, which `kNeverStageDebs` deliberately excludes for size. **The
  `.deb` stays in `debcache/`**; only the list entry is gone.
- `net.tihmstar.etasonuntether` and `com.ih8sn0w-squiffy-winocm.p0sixspwn`
  removed. They are mutually exclusive by firmware (`firmware (= 8.4.1)` and
  `firmware (< 7.0)`), so no flat list could ever be right for both. They never
  needed to be there: `stageVersionBranch()` picks between them off the real
  ProductVersion, and `stageEtasonatv()`/`stageP0sixspwn()` open their `.deb`
  by hardcoded filename straight out of `debcache/`, never via the picklist.
  Both still register real dpkg state through `stageManualDpkgInstall()`, so
  the device still sees them installed -- that path is untouched.

Result: 60 top-level packages resolve to 67 `.deb` files, identically for 6.1.3
and 8.4.1, with the era-correct persistence payload staged by the bake rather
than by apt.

Original note follows.

## 18 (original). `package/packages.txt` has entries that cannot all coexist

Surfaced by switching to real apt (item 17). The list is one flat,
firmware-independent set, but three of its 63 entries are neither flat nor
firmware-independent, and real apt refuses the whole solve over them:

- **`apt7-ssl`: FIXED, removed from the list.** It and `apt7-lib` genuinely
  `Conflicts:` each other and both were listed; the old resolver had no
  `Conflicts:` handling, so it staged both, which cannot have been installable
  on-device. `apt7-ssl` is unambiguously the one to drop, and the evidence is
  stronger than "apt complained about it":
  - `apt7-lib` (0.7.25.3-**16**) declares `Provides: apt7-ssl`,
    `Conflicts: apt7-ssl` **and** `Replaces: apt7-ssl`. That triple is the
    standard Debian idiom for "this package has absorbed and superseded that
    one" — apt7-lib *is* apt7-ssl's functionality now, and anything depending
    on the name is still satisfied through the `Provides:`.
  - The versions agree: apt7-lib is at -16, apt7-ssl stalled at -3.
  - Nothing in the entire 107-package debcache depends on `apt7-ssl` (checked
    every `Depends:`/`Pre-Depends:` field directly).
  - `apt7-ssl` is `Priority: optional` against apt7-lib's `required`, and it
    `Depends: curl` — which is in `kNeverStageDebs` as too big for this
    ramdisk. Keeping it would have dragged a deliberately-excluded package
    back in.
- **The two persistence payloads are mutually exclusive by firmware.**
  `net.tihmstar.etasonuntether` is `Depends: firmware (= 8.4.1)` and
  `com.ih8sn0w-squiffy-winocm.p0sixspwn` is `Depends: firmware (< 7.0)`. At
  most one is installable for any target. The old resolver stripped version
  constraints and picked *neither*.

`scripts/build_deb_cache_apt.py --allow-drops` works around this by dropping
the offending TOP-LEVEL entries one at a time and reporting each, which is how
`bakeRamdisk()` invokes it. That is a workaround, not a fix: the list still
claims things that are false, and the warning fires on every bake.

**What is left is only the persistence pair**, and it may not be a bug so much
as a list that should be shorter. Confirmed by reading the code: both payloads
are loaded by HARDCODED FILENAME straight out of `debcache/` --
`stageEtasonatv()` opens `net.tihmstar.etasonuntether-1.3.1.deb` and
`stageP0sixspwn()` opens `com.ih8sn0w-squiffy-winocm.p0sixspwn_1.4-1_iphoneos-arm.deb`
directly, with `stageVersionBranch()` choosing between them off the real
`ProductVersion`. Neither goes through the picklist at all.

So their presence in `packages.txt` controls only two things: whether their
`.deb` is staged into the on-device apt cache, and whether `postinstall.sh`
tries to `apt-get install` them on-device. For the wrong-era payload both are
undesirable, and apt now correctly excludes it. For the right-era one, it is
already installed at bake time, so the on-device install is at best redundant.
That argues for removing both entries — but it changes `postinstall.sh`'s
behaviour, which is the half that cannot be tested without hardware, so it is
left alone. The `--allow-drops` path produces the correct staged set either
way; the cost is one warning line per bake.

Worth knowing before touching it: `packages.txt` feeds TWO consumers, the
bake-time closure and `postinstall.sh`'s own on-device `apt-get install` list
(templated by `package/build.sh`). A fix has to be right for both, and the
on-device half is the one that cannot be tested without hardware.

Evidence that the new resolver gets this right, from real runs against the
real debcache:

| target | dropped (after the apt7-ssl fix) | persistence chosen |
|---|---|---|
| 6.1.3 | `net.tihmstar.etasonuntether` (`firmware (= 8.4.1)`) | p0sixspwn |
| 8.4.1 | `com.ih8sn0w-squiffy-winocm.p0sixspwn` (`firmware (< 7.0)`) | etasonuntether |

Both still resolve to 68 `.deb` files, and exactly one entry is dropped per
target rather than two.

## 19. Update the on-device CA trust store; restore Cydia stashing and frontrow picture behavior

Three fresh items, added 2026-09-22 after `docs/HISTORY.md`'s "RESOLVED:
sshd is alive" entry — the jailbreak works on real hardware now, and this
project moves from "make it boot" work to install-experience polish.

**Update the on-device CA trust store.** `apt-get update` against
HTTPS-only apt sources (`apt.awkwardtv.org` currently; `ios.regulad.xyz`
was worked around by switching it to plain HTTP instead — see
`docs/HISTORY.md`'s "SSL errors against `apt.awkwardtv.org` and
`ios.regulad.xyz`" entry) reliably fails with SSL errors on real hardware.
Best-supported theory, not fully confirmed: this OS-era device's factory
root CA trust store doesn't trust whatever CA issued these hosts' current
certificates (e.g. Let's Encrypt's `ISRG Root X1` postdates this OS by
years).

Confirmed directly against the real decrypted stock rootfs for
`AppleTV3,2 12H1006` (already cached locally at
`~/.local/share/blackb0x/ipsw-rootfs/rootfs.raw` — mount with `hdiutil
attach -readonly -imagekey diskimage-class=CRawDiskImage`):

- There is no `/System/Library/Keychains/SystemRootCertificates.keychain`
  at all on this OS. The built-in trust store is
  `Security.framework/certsTable.data` + `certsIndex.data` — a compiled
  binary format baked into an Apple-signed framework bundle (whose real
  code lives inside `System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7`,
  not as a standalone file). Not something to hand-edit or replace —
  undocumented format, Apple-signed, high blast radius if gotten wrong.
- `SecTrustStoreSetTrustSettings` genuinely exists as an exported symbol in
  this exact build's `Security.framework` (confirmed via `strings` on the
  real dyld shared cache). This is Apple's own supplementary/admin
  trust-store API — the same one a Configuration Profile uses to add a
  custom trusted root without touching the built-in system store at all.
  Purely additive, no Apple-signed file modified.

**DONE — confirmed working on real hardware.** `apt-get update` now pulls
`apt.awkwardtv.org` normally. Built as `cainjector/` plus the
`xyz.regulad.blackb0x.cainjector` package (`package/cainjector-layout/`,
`package/build_cainjector.sh`), prebaked by `stageCainjectorPackage()` in
`BakeRamdisk.cpp` — a package whose job is to make HTTPS apt work cannot be
something apt has to fetch over HTTPS.

It installs the whole Debian `ca-certificates` store (121 Mozilla roots,
provenance in `cainjector/README.md`), not just the one chain awkwardtv
needed, so this should hold for years rather than until the next repo moves.

Three findings from getting it working, all recorded in
`docs/HISTORY.md`:

- **`modify-anchor-certificates` is required**, or every call returns
  `-34018` (`errSecMissingEntitlement`). The name is a literal in this
  firmware's own `/usr/libexec/securityd`; the ad-hoc signature carries it
  because the untether patches AMFI.
- **Two of the hand-written prototypes were wrong**, and the tool's own
  `SecTrustStoreContains` verification is the only reason that surfaced as a
  diagnosis rather than a silent no-op. Settled against Apple's real
  `SecTrustStore.h`.
- **OpenSSL and curl are separate trust stores** and were handled too: the
  PEM goes to `/usr/lib/ssl/cert.pem` (OpenSSL's compiled-in `OPENSSLDIR`),
  and curl — which never consults OpenSSL's defaults — gets
  `CURL_CA_BUNDLE` via `/etc/profile.d/blackb0x-cainjector.sh` and the
  postinstall daemon's own `EnvironmentVariables`.

Still open, and deliberately NOT solved by this: **TLS protocol support.**
`openssl 0.9.8zg` tops out at TLS 1.0 and no trust work moves that ceiling,
so `curl`/`openssh` remain limited to hosts that still accept TLS 1.0. Only
the CFNetwork/SecureTransport consumers (which is what apt uses) get the
full benefit.

**Restore Cydia's own stashing behavior — DONE.** Investigated with a
dedicated agent: `cydia.postinst` is a compiled Mach-O binary (armv6 +
arm64), not a script, and its real strings show it relocates only
`/Applications` into `/var/stash` itself — `/usr/include`/`/usr/share`
were never its job (that part of the original comment above the `cydia`
install line was wrong; still unknown which other package relocates
those two, if anything currently does). `move.sh` ships in cydia's own
payload as a separate, generic "stash any given directory" helper, already
called successfully by `pam`/`pam-modules` on their own paths — cydia's
postinst does not call it itself.

Confirmed on real hardware that `postinst` genuinely doesn't create the
stash: running `/var/lib/dpkg/info/cydia.postinst configure` by hand exits
without producing `/var/stash`. Not worth reverse-engineering which
internal check is failing inside a compiled binary — `move.sh`'s own
`shift_()` function has the identical shape of silent-no-op gate (a
`du`/`df` arithmetic check that just does nothing, no error, if it doesn't
come out as expected), so postinst likely has an analogous environment
assumption silently unmet on this device.

Fixed by calling the same real, already-shipped `move.sh` helper directly
from `postinstall.sh`, right after the `cydia` install line, guarded
exactly the way `shift_()` guards itself (`/Applications` exists and isn't
already a symlink) so it's a no-op wherever postinst does work correctly.
Not a reimplementation — reuses the identical mechanism `pam`/`pam-modules`
already depend on.

**Confirmed working on real hardware.** `move.sh /Applications` actually
runs and stashes it — `/Applications` now symlinks to
`/var/stash/_.CqctQL/Applications` (the random `_.XXXXXX` component is
expected: real Cydia's own compiled `postinst` uses the identical
`mktemp -d`-style naming, confirmed from its own strings, so nothing
should ever depend on that path being fixed — everything is supposed to
follow the `/Applications` symlink instead). This item is closed.

**Restore frontrow picture behavior — the premise was wrong, and the real
problem is much bigger than icons.**

This entry originally assumed icon placement had regressed. It had not.
Placement, permissions, the `Appliances/` symlink, ad-hoc signing and
reboots were all ruled out one at a time on real hardware, and the actual
cause is that **this firmware has no `.frappliance` mechanism at all**:

```
$ strings -a /Applications/AppleTV.app/AppleTV | grep -ci frappliance
0
```

Confirmed on the device. Nothing in the shipped binary ever looks at the
directory both Kodi and nitoTV install into. `BRApplianceManager
_loadAppliances` gets its list from a bound merchant list — synthesized by
`BRMerchant.appDefinitions` from Apple's remote vendor bag
(`_updateMerchantsWithVendorBags:`, `ATVMerchantCoordinator`) — not from any
directory scan. Apple replaced the AppleTV2-era drop-in model with a
store/account-driven one, and both packages encode a convention this 2022
build no longer implements.

What IS still live is the `.appliance` *format*: `+[BRApplianceInfo
infoForApplianceDescription:]` still takes a plain `NSDictionary` of `FR*`
keys, and Apple's own `Computers.appliance` etc. remain in the bundle as
now-unread plists in exactly that shape. So the mechanism can be driven
from outside.

In progress: `appliancetvtweak/`, a MobileSubstrate tweak that hooks
`_loadAppliances`, `dlopen`s each `.frappliance`, adds the `BRAppliance`
protocol at runtime (neither bundle declares it, which is the second gate —
both already implement `initWithApplianceInfo:`), and feeds the result back
through `_loadApplianceWithInfo:`. It must be listed in
`misc/prebake_package_blacklist.txt` for the same reason `cydia` is: it
depends on MobileSubstrate infrastructure that does not exist in the
bake-time bootstrap container.

The real unknown is not loading but **survival** — these 2010-2015 classes
call BackRow API against a 2022 build, so they may load, conform, init, and
then crash on since-changed internals. Load one by hand and watch for a
crash before investing further. If that turns out to be a dead end,
launching Kodi/nitoTV over SSH or from a LaunchDaemon is a legitimate
fallback, and the main menu is simply not available on this firmware.
