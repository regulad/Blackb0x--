# HISTORY.md — Blackb0x portable port debugging log

This is the full narrative log of the macOS→Linux port (and, later, the reopened
Linux→portable work — see "Reopening macOS support" below): every bug hunt, every dead end,
every decision and the evidence behind it, in the order it happened. `AGENTS.md` (repo
root) is the short version — what an agent needs to navigate the repo and build it right
now. Come here when you need the *why* behind something AGENTS.md just states as fact, or
when picking up a specific unresolved thread (search for its name — every major
investigation has its own heading). Not a design doc — see `CMakeLists.txt`'s own inline
comments for terse rationale on each individual build fix.

## What Blackb0x is

A jailbreak tool for 2nd/3rd-gen Apple TV (A1378/A1427/A1469) via the checkm8/SHAtter
DFU-mode boot exploit, which then side-loads Cydia + Kodi. Originally a macOS
Cocoa/Objective-C app (Xcode project). This repo is being ported to a Linux CLI.

## Decisions already made (do not re-litigate without asking)

- **CLI-only.** No GTK/Qt GUI port — a plain command-line tool replaces AppDelegate/MainView.
- **Linux-only.** macOS/Xcode/AppKit support is being dropped entirely, not dual-maintained.
  (Superseded — see "Reopening macOS support" below: this was reopened by explicit
  request, scoped first to the `blackb0x`/`gaster` CLI path, not the ramdisk baker.)
- **Surgical conversion, not a rewrite.** Keep already-portable C libraries as-is; convert
  Objective-C/Foundation files to C/C++ as each is touched, in the phase order below.
- **CMake**, not Theos (Theos cross-compiles *for* Apple platforms; it can't produce a
  Linux binary or replace AppKit/IOKit, so it was ruled out early).
- **Git submodules** under `third_party/` for every externally-sourced dependency, per
  explicit user instruction — not FetchContent, not system packages where avoidable.
- **Version-pin every vendored library to what the original app actually used**, via OSINT
  on the original prebuilt binaries recovered from git history — not "whatever's current."
  See "Version pinning" below. This was explicitly requested and took significant effort;
  don't casually re-point a submodule at a different commit without redoing the legwork.

## Repo layout after Phase 0/1 restructuring

- `Blackb0x/Source/` — `main.cpp` + `Cli.hpp`/`.cpp` (Phase 6), `DeviceManager.hpp`/`.cpp`
  (Phase 3), `IPSW.hpp`/`.cpp` + `IPSWDownloader.hpp`/`.cpp` (Phase 4), and `Patcher.hpp`/
  `.cpp` (Phase 5) are all done C++ ports — see below for each phase's detail.
  `ResourcePath.hpp`/`.cpp` is a small shared utility (resolves paths under
  `Blackb0x/Files/`) used by Phases 4, 5, and 6. `AppDelegate.h/.m`, `MainView.h/.m`,
  `Patcher.h/.mm`, `TaskManager.h/.m`, `Blackb0x.h/.m`, `DeviceManager.h/.m`,
  `IPSW.h/.mm`/`IPSWDownloader.h/.mm` are the **original, now-fully-superseded
  Objective-C** — kept in-tree for reference/provenance only, not deleted, not built.
  `checkm8.h`/`SHAtter.h` (payload byte arrays) are `#include`d directly by
  `DeviceManager.cpp` instead of the old `.m`.
- `Blackb0x/Libraries/` — the already-portable C kept in-tree: `CBPatcher.c`,
  `libiboot32patcher.c`, `xpwntool.c` (+ their header subdirs), all built directly by the
  root `CMakeLists.txt`. `libbootkit/` is dead code (dead `#include` in `DeviceManager.m`,
  no symbols actually called) — left alone, not linked into anything.
- `Blackb0x/Files/` — payload data (Cydia tarball, keys, `setup.sh`). Unchanged; `setup.sh`
  runs *on* the jailbroken Apple TV post-install, not on the host, so it needs no porting.
- `third_party/` — everything vendored from scratch (see pin table below).
- Removed entirely: `Blackb0x.xcodeproj/`, `MainMenu.xib`, `Blackb0x.entitlements`,
  `Info.plist`, `Blackb0x.pch`, `Assets.xcassets/`, icon PNGs, `main.m`, and every prebuilt
  macOS `.dylib`/`.a` (all rebuilt from source now).

## Version pinning: how each library's version was determined

The user asked to pin every vendored library to the exact commit the original developer
used, not to whatever's current upstream — "this'll take some OSINT." Method:

1. Recovered the original prebuilt macOS binaries from git history (`git show
   85e2d8b:Blackb0x/Libraries/<file>` — the initial commit; `libcbpatcher.a`/
   `libxpwntool.a` were updated once more a week later in commit `21a2f65`, everything
   else was frozen from the start and never touched again).
2. Ran `strings`/`nm`/`file` on them via WSL (Windows Git Bash has no `strings`).
3. Found three kinds of hard evidence:
   - **Exact embedded version strings.** `libusbmuxd.dylib` contains the literal string
     `libusbmuxd 2.0.1`. `libfragmentzip.0.dylib`/`libfragmentzip.dylib` contain their own
     git-describe strings (`0.60-120447d0f4...` and `0.59-542a470d7b...` respectively —
     two different builds were bundled; the higher/newer one was taken as authoritative).
   - **An embedded debug path.** `libirecovery.a`/`libirecovery.3.dylib` contain
     `/Users/spiral/Desktop/Jailbreak/Tools/synackuk-libirecovery/...` — the developer
     built from **synackuk's fork** of libirecovery, not `libimobiledevice/libirecovery`.
   - **A toolchain fingerprint.** `libimobiledevice.a`/`libirecovery.a` both embed
     `Apple clang version 11.0.0 (clang-1100.0.33.16)` → Xcode 11.3.1 (released
     2020-01-28).
4. Cross-referenced Homebrew's `homebrew-core` formula git history (via `gh api
   repos/Homebrew/homebrew-core/commits?path=Formula/<name>.rb`) and found
   `libimobiledevice`, `libusbmuxd`, and `libplist` were all bumped to new versions by
   Homebrew **on the same day, 2020-06-15**. The bundled binaries predate that bump
   (`libusbmuxd 2.0.1` was superseded that day), landing the whole toolchain in a
   **Nov 2019 – 2020-06-15** window — consistent with the Xcode 11.3.1 fingerprint. This
   window is what led to the *initial* `libimobiledevice` pin (tag `1.2.0`) — later
   superseded by a much more precise method, see point 6.
5. For `xpwntool.c`/`libxpwntool/*.h` (already in-tree, not a submodule): the README
   itself credits **zzanehip** — "updated xpwntool (Created by planetbeing)". Found
   `zzanehip/xpwntool-swift` on GitHub and confirmed by direct content diff: `decrypt()`'s
   body and several headers are byte-identical to Blackb0x's copies (only include paths /
   a header comment / one debug printf differ). Pinned by evidence to commit `58b69c595c`
   (2020-09-13, the repo's last commit). No file changes were needed — this just confirms
   provenance of code already in-tree.
6. **Exact header content diffing beats date-proximity inference.** The original vendored
   headers (`Blackb0x/Libraries/libimobiledevice/*.h`, deleted in Phase 1 but recoverable
   via `git show 85e2d8b:...`) are the real ground truth for what API surface the app
   compiled against — independent of which binary got linked. When Phase 3's actual code
   (`DeviceManager.m`) turned out to call `idevice_new_with_options()`, which doesn't exist
   in tag `1.2.0`, the fix was: extract the original header from git history, then binary-
   search forward through `libimobiledevice`'s upstream commit log (using `git log -S
   '<newly-needed-symbol>'` to find candidate commits, then `diff` the original header
   against each) until finding a **byte-for-byte exact match**. This landed on commit
   `e52ef95` (2020-02-20) — a full four months more precise than the Homebrew-based guess,
   and the one actually used going forward. Apply this method first, before Homebrew/date
   inference, whenever a specific API call is in question.
7. **Not everything is recoverable — `libirecovery`'s upstream history was rewritten.**
   Once `DeviceManager.cpp` was written and needed `checkm8`'s
   `irecv_async_usb_control_transfer_with_cancel()`, the commit that (per current GitHub
   history) *introduces* that function in `synackuk/libirecovery` is dated **2025-11-27** —
   impossible, since `DeviceManager.m` (Sept 2020) already depends on it. The fork's git
   history was evidently squashed/rebased upstream at some point, discarding whatever
   commit actually introduced checkm8 support. There is no way to recover the true original
   commit from the current repo. Resolution: pinned to `e7aadfe`, the *oldest surviving*
   commit in current history that has the needed function — an honest "best available,"
   not a genuine date-accurate pin. Its API surface then cascaded: it needs
   `libimobiledevice-glue` (re-added, current HEAD — also not date-accurate, same reason),
   which itself needs `libplist >= 2.3.0` — well past the historically-pinned `libplist
   2.1.0`, which is genuinely correct for `libusbmuxd`/`libimobiledevice`. Rather than
   compromise the good pin, added a **second** `libplist` submodule
   (`third_party/libplist-modern`, current HEAD) used only to satisfy `glue`'s
   configure-time version check — see `CMakeLists.txt` for the mechanics.

### Final pins

| Submodule | Source | Pinned commit | Evidence |
|---|---|---|---|
| `third_party/libplist` | libimobiledevice/libplist | `3df02d4d...` (tag `2.1.0`) | Homebrew formula history, 2020-06-15 bump |
| `third_party/libusbmuxd` | libimobiledevice/libusbmuxd | `e97cebea...` (tag `2.0.1`) | Exact embedded version string |
| `third_party/libimobiledevice` | **regulad/libimobiledevice** (fork), `legacy` branch | `e52ef95...` (2020-02-20) is still the base commit — pin evidence unchanged (see point 6) | Forked later to add wolfSSL as an additional, `--with-ssl-implementation=wolfssl`-selectable SSL backend for `idevice.c`'s connection-layer TLS (real transport/`usbmuxd_*` calls untouched) — see the "libimobiledevice was forked" bullet further down for the full story. Not a re-pin: the `legacy` branch is that exact commit plus a small, additive patch (4 files, +237/-27). |
| `third_party/libirecovery` | **synackuk/libirecovery** (not upstream!) | `e7aadfe...` | Embedded debug path names this fork explicitly; exact commit is **unrecoverable** — upstream history was rewritten (see point 7). This is the oldest surviving commit with the checkm8 API Blackb0x needs, not a date-accurate pin. |
| `third_party/libimobiledevice-glue` | libimobiledevice/libimobiledevice-glue | current HEAD (**not historically pinned**) | Required transitively by the `libirecovery` pin above; didn't exist as a separate library in 2020 |
| `third_party/libplist-modern` | libimobiledevice/libplist | current HEAD (**not historically pinned**) | Exists ONLY to satisfy `libimobiledevice-glue`'s `libplist >= 2.3.0` configure check — not linked into anything itself |
| `third_party/libfragmentzip` | tihmstar/libfragmentzip | `120447d0...` (v0.60, 2020-01-04) | Exact embedded git-describe string |
| `third_party/libgeneral` | tihmstar/libgeneral | `e499458c...` (2020-01-04, 18 min before the libfragmentzip pin) | Re-added after initially (and wrongly) being judged unnecessary — see below |
| `third_party/xpwn` | **regulad/xpwn** (fork), `legacy` branch | `20c32e5...` (planetbeing/xpwn current HEAD at fork time) | No surviving evidence for the original pin; see rationale in `CMakeLists.txt` — this supplies capability the original app never had (DMG building), not a port of something it used. Forked later (still that same commit, two files' worth of changes) to fix a real wolfSSL AES buffer over-read in `img3.c` and to durably keep the pre-existing `pwnmetheus2`-disable edit — see the "first real end-to-end attempt" bullet further down for both |

`Blackb0x/Libraries/xpwntool.c` (in-tree, not a submodule): confirmed sourced from
`zzanehip/xpwntool-swift @ 58b69c595c` by content diff. No changes needed.

### Submodules added later for SSLv3 support / full static linking (not historically pinned)

None of these existed in the original macOS app — they supply capability it never needed
(a modern static-linking-friendly build, and a TLS library that still supports SSLv3 for
talking to this era of Apple TV lockdownd). Pinned to current HEAD or the latest stable
release tag, same as `xpwn`/`libimobiledevice-glue` above.

| Submodule | Source | Pinned commit | Why not a release tag |
|---|---|---|---|
| `third_party/wolfssl` | wolfSSL/wolfssl | current HEAD | Needs `--enable-sslv3`/`WOLFSSL_STATIC_RSA` build flags either way; no reason to lag |
| `third_party/curl` | curl/curl | current HEAD | Built against wolfSSL, not OpenSSL, specifically for this project |
| `third_party/libusb` | libusb/libusb | current HEAD | Vendored only because no static `libusb-1.0.a` exists anywhere on this system |
| `third_party/libzip` | nih-at/libzip | current HEAD | **Supersedes** the earlier "system package" decision noted above — vendored once static linking became the goal |
| `third_party/libpng` | pnggroup/libpng | `v1.6.58` (release tag) | `master` tracks an unstable `1.8`-dev series (`PNGLIB_ABI_VERSION` → `libpng18.a`, not `libpng16.a`) |
| `third_party/bzip2` | libarchive/bzip2 | current HEAD | Upstream `sourceware.org/bzip2` has no CMake support at all; this is the common CMake-buildable fork |
| `third_party/libfragmentzip` | **regulad/libfragmentzip** (fork) | `fix-cxx-stdbool-header` branch, off `120447d0` | Same commit as the historical pin above, plus one C++-compatibility fix that kept getting lost to `git reset --hard` as an in-tree patch — see the "`third_party/libfragmentzip` is forked" bullet below for the full story |

### Dependencies that were added, then removed, then one re-added

- `libimobiledevice-glue` and `libtatsu` were added early (the *current* upstream
  `libimobiledevice`/`libusbmuxd`/`libirecovery` configure scripts require them) but
  **removed** once pinned to the historical versions above — checked each pinned
  version's own `configure.ac` directly and confirmed neither dependency existed yet at
  that point in history.
- `libgeneral` was removed by the same reasoning (`libfragmentzip`'s pinned `configure.ac`
  has no `PKG_CHECK_MODULES` for it) — but this was **wrong** and had to be reverted:
  `libfragmentzip.c` at that exact commit already `#include <libgeneral/macros.h>` in the
  source even though the build system hadn't caught up yet. Lesson: check the `.c` source
  directly, not just `configure.ac`, before concluding a dependency is unneeded.
- `libzip` was added as a **system** package (`apt libzip-dev` + `pkg_check_modules`) once
  `libfragmentzip`'s pinned `configure.ac` revealed it as a real dependency. This also
  retroactively explains why `libzip.dylib` existed in the original app's bundled binaries
  — a loose end from the initial investigation, now closed.
- `libimobiledevice-glue` and `libtatsu` were removed a second time for the same reason as
  the first, then **`libimobiledevice-glue` had to be re-added** once `libirecovery` got
  re-pinned to `e7aadfe` (see "Version pinning" point 7) — that specific commit genuinely
  needs it. `libtatsu` stayed removed; nothing in the final pin set needs it.

## Build-system fixes needed to compile 2019–2020-era code on a 2025 toolchain

All of these are documented inline in `CMakeLists.txt` next to the relevant
`ExternalProject_Add`/`add_subdirectory` call; this is the summary:

- **CRLF line endings**: every submodule ships its own `.gitattributes` (`* text=auto`),
  which on Windows checkout used `core.eol=native` → CRLF, breaking every `#!/bin/sh`
  shebang. Fixed via **local, repo-scoped** git config on each submodule —
  `core.autocrlf false` + `core.eol lf` — followed by a forced clean re-checkout
  (`git clean -xfdq` then `reset --hard`, or delete-then-checkout for a fresh submodule
  add). This was an explicit user preference: no `.gitattributes` edits, no CMake-side
  normalization logic. **Apply this immediately after adding or re-pinning any submodule,
  before ever running its `autogen.sh`/`configure`.**
- **`getconf LFS_CFLAGS` returns empty** on this modern glibc (64-bit `off_t` is already
  the default), but `libimobiledevice`'s old `common/Makefile.am` unconditionally
  concatenates it into `-D_FILE_OFFSET_BITS=`, which is syntactically invalid and breaks
  `<features.h>`'s own version check. Fixed by overriding `LFS_CFLAGS=` at `make` time.
- **Broken vendored `autogen.sh`** in `libimobiledevice` and `libfragmentzip` at these old
  commits — both fail with `required file './ltmain.sh' not found` on a modern
  autoconf/libtool. Fixed by substituting `autoreconf -fi && ./configure` in their
  `CONFIGURE_COMMAND` instead of calling the vendored script.
- **`libfragmentzip` needs `libgeneral` with no `PKG_CHECK_MODULES` mechanism** to learn
  its location — passed `CPPFLAGS=-I${DEPS_INCLUDE} LDFLAGS=-L${DEPS_LIB}` explicitly.
- **`libfragmentzip.h`'s `bool`/`true`/`false` enum conflicts with `<stdbool.h>`** now
  being pulled in transitively by a newer system `curl.h` (didn't happen in 2016 when the
  header was written). This is the **one vendored-source patch** made this session
  (everywhere else uses compile flags) — verified upstream's own later fix was exactly
  this (`#include <stdbool.h>`, delete the manual enum) and applied the same fix.
- **`SSLv3_method()` was removed entirely in OpenSSL 3.x** (not just deprecated/hidden —
  genuinely gone from the linkable ABI on this distro). Patched
  `third_party/libimobiledevice/src/idevice.c` to call `TLS_method()` instead (the modern
  version-flexible replacement; matches the direction upstream itself eventually took,
  though not the same commit — that fix isn't even in this tag's ancestry, likely due to
  a git history rewrite/import upstream at some point).
- **`libimobiledevice`'s bundled CLI tools** (`idevicebackup2`, `ideviceinfo`, etc., under
  `tools/`) fail to link for reasons unrelated to the library itself and are not used by
  Blackb0x — skipped by overriding `make`'s `SUBDIRS=common src include`.
- **`xpwn`'s `pwnmetheus2` subdirectory** hard-requires the legacy libusb-0.1 API
  (`<usb.h>`) on non-Apple platforms — an unrelated GUI pairing tool. Disabled by
  commenting out its `add_subdirectory` line in `third_party/xpwn/CMakeLists.txt`.
- **`xpwn`'s vendored `minizip`** predates zlib's `z_crc_t` typedef, and **most of `xpwn`
  is circa-2010 C** that trips several diagnostics GCC ≥14 turned from warnings into
  hard errors by default (implicit-int, implicit-function-declaration,
  incompatible-pointer-types, int-conversion, return-mismatch) plus GCC ≥10's
  `-fno-common` default causing "multiple definition" link errors. Fixed with a blanket
  loop over every target `xpwn` defines, relaxing those specific diagnostics — not by
  patching 2010-era vendored source file-by-file.
- **`xpwn` itself declares `cmake_minimum_required(VERSION 2.6)`**, which CMake ≥4 refuses
  outright. Wrapped its `add_subdirectory` call in
  `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)` / `unset(...)` rather than editing the vendored
  `CMakeLists.txt`.
- **Wrong imported-library filenames** were guessed more than once when wiring up
  `add_deps_imported_lib` (e.g. assumed `-2.0`/`-1.0` suffixes that these older library
  versions don't use, or vice versa). Ground truth is always `ls build/deps/lib/*.a`
  after a successful `ExternalProject_Add` install — don't guess, check.
- **`AC_CHECK_MEMBER`/`AC_TRY_COMPILE` probes run before `PKG_CHECK_MODULES` CFLAGS are
  folded into `CPPFLAGS`.** `libimobiledevice`'s `configure.ac` probes whether the
  installed `usbmuxd.h` is "new enough" using a raw compile test that only sees the
  *default* include path — `PKG_CHECK_MODULES(libusbmuxd, ...)` earlier in the same file
  does not itself add `-I` flags to the environment these probes run in. Without an
  explicit `CPPFLAGS=-I${DEPS_INCLUDE}`, the probe can't find our built `usbmuxd.h` at all
  and fails with a **misleading** message ("libusbmuxd is not up-to-date; missing
  conn_type member") that's actually "header not found" — don't take such messages at
  face value; check whether the header is even reachable first.
- **A `.pc` file's own transitive `Requires:` can cross a prefix split.**
  `libimobiledevice-glue-1.0.pc` (built against `libplist-modern`) declares `Requires:
  libplist-2.0`. Any *other* autotools project that does `pkg-config
  libimobiledevice-glue-1.0` (i.e. `libirecovery`) needs `libplist-2.0.pc` on its OWN
  `PKG_CONFIG_PATH` too, even though it never uses libplist directly — pkg-config resolves
  `Requires:` chains transitively. Whenever a dependency spans the `deps`/`deps-modern`
  prefix split, every consumer up the chain needs both prefixes on its `PKG_CONFIG_PATH`.
- **`extern "C"` linkage must match between a `friend` declaration and its definition.**
  When `DeviceManager.hpp` declared `friend void some_c_callback(...)` with implicit C++
  linkage, but `DeviceManager.cpp` defined the same function as `extern "C"`, GCC treats
  them as conflicting declarations of different functions — the friendship silently didn't
  apply, and calls to private members from that "friend" failed with "is private within
  this context". Fix: declare the function's `extern "C"` prototype once (e.g. right after
  the `extern "C" { #include ... }` block), then have the `friend` declaration reference
  that exact (already C-linkage) name.
- **Newly-linked static libraries can need transitively-implied system libraries you never
  needed before.** Adding `DeviceManager.cpp` (which pulls in `libimobiledevice.a`, which
  uses OpenSSL's `SSL_*` functions internally for device connection encryption) needed
  `OpenSSL::SSL` added to `blackb0x`'s link line — `OpenSSL::Crypto` alone (all that was
  needed for the stub) wasn't enough. `undefined reference to 'SSL_*'` at final link,
  not at compile time, is the signature of this class of problem.
- **Stale `ExternalProject_Add` stamp files**: these live under
  `build/<name>-prefix/src/<name>-stamp/`, separate from the submodule source directory.
  Cleaning a submodule's source tree (`git clean -xfdq`) does **not** invalidate a stale
  "configure already succeeded" stamp from a prior failed attempt, causing `make` to run
  against a source tree with no generated `Makefile`. **Rule: do a full `rm -rf build`
  any time you change a submodule's pinned commit, patch a vendored file, or edit an
  `ExternalProject_Add`'s configure/build commands — don't rely on incremental stamps.**

## Phase 5 — `Patcher.mm` → `.hpp`/`.cpp` (done, but with one unresolved bug)

`patchiBSS`/`patchiBEC`/`patchKernel` ported near-verbatim — thin wrappers around the
already-portable `decrypt()`/`iBootPatcher()`/`patch_kernel()`, unchanged behavior.
`patchRamdisk` (the one method built around `hdiutil`/`tar` shell-outs) was substantially
rewritten; see `Patcher.hpp`'s header comment for the pipeline summary. Notes beyond what's
in that comment:

- **`blackb0x_xpwntool` naming.** The new static library wrapping `xpwntool.c` had to be
  named `blackb0x_xpwntool`, not `xpwntool` — `third_party/xpwn`'s own `CMakeLists.txt`
  already defines an `add_executable(xpwntool xpwntool.c)` target, and CMake target names
  are global.
- **`CBPatcher.c`/`libiboot32patcher.c` had no implementations in-tree.** Same pattern as
  the DMG/HFS+ code in Phase 0: only headers were checked in (`CBPatch.h`, `finders.h`,
  `functions.h`, `patchers.h`, etc.) because the original app linked prebuilt `.a` files.
  This only surfaced once `Patcher.cpp` became the first code to actually call
  `iBootPatcher()`/`patch_kernel()`. Recovered the missing sources from **zzanehip's**
  forks — `zzanehip/CBPatcher` (`CBPatch.c`, `CBPUtils.c`) and `zzanehip/iBoot32Patcher`
  (`finders.c`, `functions.c`, `patchers.c`) — confirmed by near-exact header match against
  the headers already in-tree before pulling in anything, same provenance-first method used
  everywhere else in this port. The iBoot32Patcher sources needed a mechanical
  `sed -i 's|<include/|<libiboot32patcher/|g'` to match Blackb0x's own include-path
  convention; no other changes.
- **`CBPatch.c`'s Mach-O headers don't exist on Linux**, but the actual usage is pure
  static binary analysis of a downloaded ARMv7 kernelcache buffer (confirmed via grep: no
  `dlopen`/`mmap` calls anywhere in the file, despite including `<dlfcn.h>`/`<sys/mman.h>`).
  Since Mach-O is a stable, publicly documented file format (not a private macOS runtime
  API), the fix was a new header — `Blackb0x/Libraries/libcbpatcher/portable_macho.h` —
  defining just the classic 32-bit structs actually used (`mach_header`, `load_command`,
  `segment_command`, `section`, `symtab_command`, `nlist`), included via
  `#ifdef __APPLE__ ... #else #include "portable_macho.h" #endif` around the original
  `<mach-o/*.h>` includes. `mac_policy.h` additionally needed an explicit `#include
  <stdint.h>` it had previously gotten transitively from a removed Apple header.
- **UDIF-vs-raw-HFS+ format auto-detection** (added only after real-world testing, not
  planned upfront). The port plan and `third_party/xpwn`'s own reference tool
  (`ipsw-patch/main.c`) both assume every image `decrypt()` produces is UDIF-wrapped
  (trailing "koly" magic). Downloading and decrypting a real AppleTV2,1 build 11D258
  `RestoreRamdisk.dmg` and inspecting it with `xxd` showed it decrypts straight to a **raw**
  HFS+ partition — "H+" signature at offset `0x400`, no "koly" trailer anywhere in the file.
  Fixed by probing the last 512 bytes of the decrypted file for the "koly" magic at runtime
  and branching: only genuinely UDIF-wrapped images go through xpwn's `extractDmg()` to
  unwrap to a raw HFS+ image first; the common (older-hardware) case is read directly as
  raw bytes. The final write-back mirrors this: `buildDmg()` only if the input was UDIF,
  otherwise a plain raw write — matching whatever `decrypt()` actually produced.
- **Deliberately unimplemented: `AppleTV2,1_4.x` ramdisk recreation.** The original's
  `"AppleTV2,1_4."`-prefixed branch doesn't patch an existing ramdisk at all — it rebuilds
  one from scratch via `hdiutil create ... -format UDRW`, for the oldest ATV2 4.x-era
  firmware. This port explicitly does **not** implement that branch — `patchRamdisk` prints
  an error to stderr and returns `false` for that path prefix rather than silently doing
  the wrong thing. Building a fresh empty HFS+ volume from nothing is a materially
  different problem from growing/injecting into an existing one and was out of scope for
  this pass.
- **In-memory HFS+ manipulation, no mount/loopback anywhere — first design, later abandoned.**
  The raw (or UDIF-unwrapped) bytes were loaded into a heap buffer, wrapped as an
  `AbstractFile`/`io_func` via xpwn's `createAbstractFileFromMemoryFile()`, then opened
  directly as a `Volume*` via `openVolume()`. All injection used xpwn's own `add_hfs()`/
  `grow_hfs()`/`hfs_untar()` primitives against that in-memory volume. This matched the
  original port plan's explicit intent to avoid any OS-level mount — see "RESOLVED" below
  for why it was abandoned anyway, in favor of a real loop mount via the Linux kernel's own
  `hfsplus` driver.
- **`hfs_untar()` has no destination-prefix concept** — it always extracts each tar
  member to the literal path baked into the tar, with no way to redirect e.g. `ATV-Cydia.tgz`'s
  contents under `/files/cydia/`. Wrote a small wrapper, `hfsUntarWithPrefix()` (mirrors
  `hfs_untar()`'s own tar-parsing loop but prepends a caller-supplied prefix to every member
  path before calling `add_hfs()`/`newFolder()`), plus `ensureHfsDirExists()` (a `mkdir -p`
  equivalent: walks `getRecordFromPath3()` up the path components, calling `newFolder()` for
  any that don't already exist) used by both the wrapper and by `addLocalFileToVolume()`.
  **(Superseded — these helpers no longer exist; see "RESOLVED" below. Kept here for the
  historical record, since the compressed-file fix immediately below was real, hard-won
  debugging work independent of which design ultimately won.)**
- **The compressed-file crash and its two-attempt fix.** Real functional testing (not just
  compiling) against the downloaded 11D258 ramdisk hit a SIGSEGV partway through
  `hfsUntarWithPrefix()`. `gdb -batch -ex run -ex bt` (via WSL) showed the crash was in
  `free()` ← `closeHFSPlusCompressed()` ← `writeToHFSFile()` ← `add_hfs()` — xpwn's own code
  crashing while overwriting a file that already has the `UF_COMPRESSED` flag set (HFS+
  transparent decmpfs compression) from a prior extraction pass over the same path.
  - **First attempt (wrong): `clearHfsCompressionFlagIfSet()`.** Looked up the existing
    record via `getRecordFromPath3()`, cleared `UF_COMPRESSED` in
    `fileRecord->permissions.ownerFlags`, called `updateCatalog()` to persist the cleared
    flag, then let `add_hfs()` proceed to overwrite. This avoided the segfault, but running
    the same real-data test further revealed a **new** failure: `hfs_panic("BTree
    inconsistent!")`. Directly mutating a catalog record's flags out from under the B-tree
    without going through its normal record-removal path left something inconsistent.
  - **Second attempt (correct): `removeExistingFileIfPresent()`.** Instead of trying to
    salvage the existing record, look it up and — if it's a file record — call xpwn's own
    `removeFile()` on it before `add_hfs()` runs, exactly matching the "replacing %s" branch
    already present in xpwn's own `hfs_untar()` for this exact situation (confirmed by
    reading that function's source first). This is the precedent-following fix, not a novel
    one. Called from both `addLocalFileToVolume()` and `hfsUntarWithPrefix()`'s regular-file
    branch. **Confirmed working**: re-ran the real-data test and `ssh.tar`, `RamdiskBins.tar`,
    and the *entire* `ATV-Cydia.tgz` `bin/` directory (many compressed executables) all
    extracted without crashing.
- **RESOLVED: the "BTree inconsistent!" catalog-growth bug, and everything that followed
  from chasing it.** After the compressed-file fix above, the identical
  `hfs_panic("BTree inconsistent!")` error still occurred later in the same real-data test,
  partway through `Debs.tar` extraction into `/files/`. A second `gdb -batch -ex run -ex
  'break hfs_panic' -ex bt` backtrace showed this was unrelated to compression — it's inside
  xpwn's own catalog B-tree **growth** path: `growBTree` → `getNewNode` → `splitNode` →
  `addRecord` (recursing ~3 levels) → `addToBTree` → `newFile` → `add_hfs`, ultimately
  failing inside `allocate` → `writeExtents` → `removeExtents` → `search` → `searchNode` →
  `hfs_panic`, triggered when `lastRecordDataOffset == 0`. Reading `grow_hfs()`'s full
  implementation confirmed the root cause: it only resizes the **overall volume bitmap**,
  not the **catalog file's own B-tree extents** — so once enough new catalog entries have
  been injected that the catalog B-tree needs to grow past its originally-allocated extent,
  that organic-growth code path (not exercised by xpwn's own more modest reference use
  cases) has a genuine, pre-existing bug in this ~2010-era from-scratch HFS+ writer. Not a
  bug introduced by the Blackb0x port — a real limitation of `third_party/xpwn` itself, hit
  only because Blackb0x's ramdisk payload (six tarballs plus a dozen loose files) pushes the
  catalog B-tree further than xpwn's own tools ever have.

  This was reported to the user as a genuine open question rather than guessed at further,
  which led to a second research pass — evaluating alternative Linux-side HFS+ *write*
  implementations rather than trying to fix xpwn's B-tree code blind:
  - **libhfsp (Debian's `hfsplus`/`libhfsp-dev` package) — ruled out, worse than xpwn.**
    Direct testing (`hpmount`/`hpmkdir`/`hpcopy`, the package's own CLI tools) found a single
    `hpmkdir()` call — no file copy at all — on a freshly-`mkfs.hfsplus`'d volume already
    corrupted an extent entry into the reserved alternate-volume-header block, confirmed via
    `fsck.hfsplus` and by checking that the same volume was clean immediately after
    `mkfs.hfsplus` and before any libhfsp write. Root-caused directly in libhfsp's own
    source (`libhfsp/src/volume.c`, cloned from `deepin-community/hfsplus` for inspection):
    all three block-allocation functions (`volume_allocated`/`volume_allocate`/
    `volume_deallocate`) contain a bounds check against the volume's reserved region that
    exists in the source **only as commented-out dead code**. Corroborated by the project's
    own `ChangeLog`, which calls the 1.0.1 release (2000) "a stable, readonly implementation"
    and never claims the write path reached that bar through the final 1.0.4 release (2002).
  - **The Linux kernel's own, in-tree, actively-maintained `hfsplus` driver — this is what's
    actually used now.** Requires a real loop mount, which the original port plan explicitly
    wanted to avoid — but real functional testing (loop-mounting a blank `mkfs.hfsplus`
    volume and copying the *entire* real payload set onto it, including `Debs.tar`, via
    ordinary `tar`/`cp`) confirmed it handles exactly the catalog growth that broke both
    designs above, with a clean `fsck.hfsplus` verdict and a byte-for-byte content match
    against the source tarballs. (Testing this required a real Fedora/Bluefin Linux host —
    WSL2's own kernel ships with `CONFIG_HFSPLUS_FS` disabled and building an out-of-tree
    module for it turned out to not be a genuinely supported WSL2 configuration either; see
    "Environment notes" below.)
  - **Resizing an *existing* volume in place — investigated and confirmed unsupported on
    Linux, not just "no tool found."** `patchRamdisk()` needs to GROW an existing,
    already-populated Apple-built HFS+ image, not just write into a volume already sized
    correctly by `mkfs.hfsplus`. There is no `resize2fs`-equivalent CLI tool for HFS+ on
    Linux, but there IS a real library that implements HFS+ resize —
    `libparted-fs-resize` (what GParted's HFS+ support uses under the hood; split out of
    libparted core in parted-3.0 into this separate add-on library specifically because
    HFS+/FAT resize had no free alternative at the time). Built a throwaway C program
    directly against it (`ped_file_system_open`/`get_resize_constraint`/`resize`, via a
    Fedora toolbox container for the `parted-devel` headers, keeping the host's immutable
    base image untouched) and ran it against a real, populated HFS+ volume: the library
    reported its own resize constraint capping `max_size` at the volume's *current* size,
    and calling `ped_file_system_resize()` to grow past that returned, verbatim, **"No
    Implementation: Sorry, HFS+ cannot be resized that way yet."** Growing (as opposed to
    shrinking) HFS+ is an explicit, acknowledged non-implementation in the one library that
    does implement HFS+ resize on Linux — not an untested edge case, and not something worth
    waiting on upstream for.

  **The actual fix**, implemented in `Patcher.cpp`'s `patchRamdisk()`: don't grow the
  original volume at all. `mkfs.hfsplus` a brand-new volume at the final target size (same
  40MB/60MB sizing as before), copy the original volume's entire tree across via a real loop
  mount + `cp -a` (preserving permissions/ownership/setuid bits/symlinks — this needs
  `CAP_CHOWN`, so `patchRamdisk()` now requires the calling process to run as root, e.g.
  `sudo blackb0x ...`), copy a deliberately narrow set of the original's volume-header
  identity fields across too (`finderInfo`, `createDate`, `lastMountedVersion` — patched at
  both the primary and backup header offsets via plain fixed-size byte copies, no B-tree
  interaction at all; `attributes` and every size/count field are deliberately *not* copied,
  since blindly copying them risks importing stale state that doesn't match the
  freshly-built volume's actual, correct state), and only then inject the new payloads via
  ordinary `tar -x`/file copies into the *new*, already-correctly-sized volume. All of the
  bespoke xpwn-Volume helpers described above (`ensureHfsDirExists`, `parentOf`,
  `removeExistingFileIfPresent`, `addLocalFileToVolume`, `hfsUntarWithPrefix`,
  `untarFileIntoVolume`, `gunzipFile`, `untarGzipFileIntoVolume`) are gone, replaced by a
  handful of `fork`/`execvp` subprocess helpers (`runCommand`, `runCommandCapture`,
  `makeTempDir`, a `MountGuard` RAII type, `copyLocalFileIntoMount`, `readVolumeLabel`,
  `copyVolumeHeaderMetadata`) invoking `mount`/`umount`/`mkfs.hfsplus`/`blkid`/`cp`/`tar`
  directly (argv arrays, no shell). **Verified end-to-end against the same real, live-
  downloaded AppleTV2,1 11D258 `RestoreRamdisk.dmg` used throughout this investigation**:
  `patchRamdisk(ssh=false)` returns `true`, the resulting image passes `fsck.hfsplus` clean
  ("The volume ramdisk appears to be OK"), `file(1)` confirms the original's real 2014
  creation date survived the metadata copy, and mounting the result confirms all 8 spot-
  checked injected paths (`/ssh.tar`, `/sbin/launchd`, `/files/setup.sh`, `/files/cydia`,
  `/files/p0sixspwn`, `/files/etasonATV`, `/files/rtbuddyd.bin`, `/files/kodi.png`) are
  present, with 916 files under `/files` total. Phase 5 is done.
- **Real-data test methodology** (not part of the permanent build — written, run, and
  deleted each time, per this session's convention of leaving no scratch files behind):
  download a real AppleTV2,1 11D258 `RestoreRamdisk.dmg` + its firmware keys via the
  already-verified Phase 4 `IpswFetch`/`FragmentDownloader`, run it through
  `Patcher::patchRamdisk()`, and inspect/gdb the output. This is the only way Phase 5's
  HFS+ code has ever been exercised — there is no offline synthetic-fixture test for it —
  but by the final (kernel-mount) design it's a genuine pass/fail signal: `fsck.hfsplus` on
  the output plus a mount-and-spot-check of injected paths, not just "didn't crash."

## Environment notes

- **The dev environment moved partway through Phase 5, from Windows+WSL2 to a real Linux
  box (Fedora Silverblue/Bluefin, immutable/rpm-ostree-based, hostname `longitude`).** This
  wasn't incidental — WSL2's own kernel ships with `CONFIG_HFSPLUS_FS` disabled entirely
  (`# CONFIG_HFSPLUS_FS is not set` in `/proc/config.gz`), and building it as an
  out-of-tree module for WSL2 turned out to not be a clean, genuinely-supported path either:
  Microsoft's own `.wslconfig` `kernelModules` mechanism exists and is documented, but it's
  designed around pairing a full custom-built replacement kernel with a matching modules
  VHDX, not bolting one module onto the stock inbox kernel, and Microsoft's own support
  guidance calls building/loading kernel modules this way "not officially supported."
  Testing the kernel-mount approach for `patchRamdisk()` (see above) needed a real kernel
  with `hfsplus` support, which a real Fedora box has out of the box — this single
  constraint is what actually decided Phase 5's final design between the sessions.
- **This machine is accessed over SSH with no GUI available**, which shaped how privileged
  (root-requiring) test commands got authenticated:
  - `sudo` normally needs a TTY for the password prompt, which a non-interactive tool-call
    shell doesn't have — `sudo -v`/`sudo -S` from an agent's own shell will not work here.
  - `pkexec` (polkit's graphical auth agent) was tried next, since the D-Bus session bus
    address was reachable even without `DISPLAY`/`WAYLAND_DISPLAY` set in the calling
    shell — but this pops a GUI dialog on the machine's own physical screen, useless to a
    user who's only ever connected over SSH. Confirm what the user is actually looking at
    before assuming a GUI auth prompt is reachable.
  - **What actually worked**: a scoped `/etc/sudoers.d/` rule the *user* adds themselves via
    `sudo visudo -f /etc/sudoers.d/<name>` (never generated/written by the agent — sudoers
    edits are exactly the kind of hard-to-reverse, security-relevant action that needs the
    user's own hands), granting `NOPASSWD` for only the specific binaries/argument patterns
    a task needs (e.g. `/usr/bin/mount -t hfsplus *`, `/usr/bin/mkfs.hfsplus *`, `/usr/bin/
    umount /tmp/*`) — never a blanket `ALL=(ALL) NOPASSWD: ALL`. Confirmed scoping actually
    works by checking that `sudo -n <allowed-command>` succeeds while `sudo -n true`
    (something NOT in the rule) still demands a password. For anything not covered by such a
    rule (e.g. running an arbitrary one-off test binary that itself needs to run *as root*,
    rather than just needing to shell out to a couple of allowed commands), the simplest
    honest path is just asking the user to run that one command themselves via Claude Code's
    `!` prefix, rather than trying to route around a real authentication boundary.
- **Podman/toolbox on this host needed the system binary, not linuxbrew's.** `podman` was
  resolved from `/home/linuxbrew/.linuxbrew/bin/podman` by default (earlier in `$PATH`),
  which isn't configured for this host's rootless-container setup and fails with `configure
  storage: mkdir /run/containers: permission denied`. Fixed by explicitly using
  `/usr/bin/podman`/`/usr/bin/toolbox` (`PATH=/usr/bin:$PATH ...`, or a small shim directory
  with a symlink prepended to `PATH`, since `toolbox` itself shells out to a bare `podman`
  it resolves via its own inherited `$PATH`). A toolbox container's `/tmp` and `$HOME` are
  both bind-mounted from the host by default — confirmed by writing a marker file from one
  side and reading it from the other — which made it possible to build a throwaway C test
  program inside a toolbox (for `parted-devel` headers, without layering anything onto the
  host's immutable base image via `rpm-ostree`) while operating on image files created by
  the host's own already-working `mkfs.hfsplus`/`mount`.
- Before that move: dev machine was Windows; there was no local Linux toolchain outside
  WSL. Build/test
  everything through the `regulad-ubuntu` WSL distro (`wsl -d regulad-ubuntu -- bash -lc
  "..."`), which has cmake, gcc/g++, git, pkg-config, autoconf, automake, and linuxbrew's
  libtool/m4 already installed. The repo is reachable from WSL at
  `/mnt/d/repositories/Blackb0x`.
- Extra system packages installed in that WSL distro for this build:
  `libusb-1.0-0-dev`, `libzip-dev` (both via `apt-get install`).
- When a `git checkout <commit>` on a submodule aborts because a locally-modified tracked
  file (e.g. a source patch you made) would be overwritten, do **not** work around it with
  a `find ... -exec rm -rf` + re-checkout combo run unconditionally after the failed
  command — if the checkout aborted, HEAD didn't move, and the follow-up commands still
  execute (no `set -e`), leaving a confusing mixed-content working tree that matches
  neither commit. `git -C <submodule> reset --hard HEAD` first, *then* checkout cleanly.
- **PowerShell → `wsl.exe` argument passing is unreliable for anything with shell
  variables or complex quoting** — `$f` in a `for` loop got silently dropped/mangled more
  than once this session even inside single-quoted PowerShell strings. When a WSL
  one-liner needs real shell logic (loops, variable expansion), write it to a `.sh` file
  in the repo first and invoke `wsl -d regulad-ubuntu -- bash /mnt/d/repositories/Blackb0x/somefile.sh`
  instead of trying to inline it through PowerShell.
- `wsl -l -v`'s own output comes through mangled (UTF-16LE double-decoding, one space
  between every letter) when captured — read past the garbling rather than assuming the
  command failed.
- The build's true exit code is easy to lose: piping through `tail`/`tee` without
  `pipefail` reports the pipeline's last command's exit status, not the actual build's.
  Always `grep` the log for `Error 1\|Error 2\|error:\|configure: error` rather than
  trusting a reported exit code.
- **Known machine where a real end-to-end jailbreak attempt did NOT succeed**: this same
  dev machine (hostname `longitude`, see above) — Dell Latitude 7420 (2022), 11th Gen
  Core/Tiger Lake-LP, Iris Xe, two xHCI USB host controllers (one routed through the
  Thunderbolt 4 controller, one a plain USB 3.2 Gen 2x1 controller, **no legacy EHCI
  controller at all**), Bluefin 44, kernel `7.1.8-200.fc44.x86_64`. Full hardware probe:
  <https://linux-hardware.org/?probe=bd45f480d3>. Tried repeatedly against a real
  AppleTV3,2 in DFU mode. **Power-cycling/rebooting alone does not fix it** — this is the
  expected result, not a mystery: see the long "First real end-to-end attempts" bullet in
  the libimobiledevice-fork section above for the full investigation, but the short
  version is that six real software bugs surfaced and were fixed along the way (DFU
  remote instructions, curl/wolfSSL CA chain validation, the kernelcache version-string
  and patch-failure heap corruption, a wolfSSL AES-CBC buffer over-read, and a
  Linux/libusb vs. macOS/IOKit error-code translation gap in the hand-ported checkm8
  code) — and after all of them were fixed and the exploit backend was even swapped
  entirely for `gaster` (the same implementation `palera1n` itself uses, confirmed to
  support this exact chip/firmware), the exploit payload itself was confirmed to
  genuinely execute (`Stage: PATCH` / `ret: true`) on this machine — but the
  device-initiated USB reset that checkm8 triggers afterward unreliably hangs (a real
  uninterruptible-sleep kernel wait, confirmed via `ps`/`/proc/<pid>/status`) or fails to
  re-enumerate, at inconsistent points in the sequence, on two independent
  implementations. That, combined with this machine's Thunderbolt-only/no-EHCI USB
  topology, is the actual, current, well-evidenced conclusion: **a host-controller-level
  USB limitation for checkm8-class device-initiated resets, not a software bug in this
  project** — rebooting the host was never going to fix that.
  **The USB-hub mitigation was tried too, twice, and also did not help**:
  <https://linux-hardware.org/?probe=101c2e8278> (Microchip USB2807/USB5807 hub
  chipsets — actually a Dell docking station, given the RTL8153 Ethernet/webcam/audio
  devices riding alongside) and <https://linux-hardware.org/?probe=44f0d4852a>
  (Terminus Technology `1a40:0101` + VIA Labs `2109:2817` — genuine plain USB hub
  chipsets, the closer match to the "route through a dumb hub" workaround this was
  meant to test). Neither got any further than connecting directly. With the direct,
  Thunderbolt-routed, and two structurally different hub-routed paths all hitting the
  identical device-reset/reconnect failure, a topology-specific quirk (e.g. "needs a
  hub in the path") is no longer a plausible explanation — this reads as this specific
  machine's USB stack (kernel/xHCI driver/controller firmware) genuinely not handling
  checkm8's device-initiated reset reliably, full stop. **Next step is different
  hardware, not more mitigations on this one** — ideally something with a legacy EHCI
  controller (older/cheaper laptops, many desktop motherboards) or at minimum a
  non-Thunderbolt-routed xHCI implementation from a different vendor generation, since
  every avenue on this specific Tiger Lake-LP/Thunderbolt setup has now been tried.
  **SUPERSEDED** — see "`blackb0x-pwn` on Linux" at the end of this file. Running a
  second, independent checkm8 implementation here with a usbmon capture shows the
  resets completing cleanly every time; the failure is elsewhere, and the
  host-controller-limitation conclusion above does not hold.

## Current state / how to build

```
cmake -S . -B build && cmake --build build -j$(nproc)
./build/blackb0x
```

(The commands above assume a normal Linux dev box. If still working from the old
Windows+WSL2 setup for some reason, prefix each with
`wsl -d regulad-ubuntu -- bash -lc "cd /mnt/d/repositories/Blackb0x && ...`", but see
"Environment notes" above for why that setup was abandoned partway through Phase 5.)

This builds and runs the **real CLI** now — `blackb0x --help` prints usage and exits 0;
run without root it correctly refuses with a clear error; run as root (`sudo ./build/
blackb0x`) it prints the startup banner and begins waiting for a device. The entire pinned
dependency stack (libplist ×2, libusbmuxd, libimobiledevice, libimobiledevice-glue,
libirecovery, libgeneral, libfragmentzip, xpwn, cbpatcher, iboot32patcher) **and**
`DeviceManager.cpp`/`IPSW.cpp`/`IPSWDownloader.cpp`/`Patcher.cpp`/`Cli.cpp` (Phases 3–6,
all done) compile cleanly and link successfully. Phases 4 and 5 were both functionally
verified end-to-end with throwaway test programs (built, run, and deleted each time —
never part of the permanent build, per this session's convention): Phase 4 against a live
`api.ipsw.me` query, a real `.keys` file parse, and a full round-trip download + plist-parse
of a real `BuildManifest.plist` from a live IPSW on Apple's CDN; Phase 5 (and its SSH-key
security fix) against multiple full `patchRamdisk()` runs on a live-downloaded AppleTV2,1
11D258 `RestoreRamdisk.dmg`, verified via a clean `fsck.hfsplus` and a mount-and-spot-check
of the injected payload and SSH configuration (see the "RESOLVED" writeup and the Phase 6
entry below for exact detail). This is real, verified functionality, not aspirational.
Phase 6's device-selection/DFU-wait/exploit/upload flow itself is **not yet
hardware-verified** — see that phase's entry below. **`blackb0x` runs entirely as root**
(decided during Phase 6 — see that entry for why) — always invoke it via `sudo`.

**New runtime (not just build-time) dependency from Phase 5's final design**: the target
Linux system needs `mkfs.hfsplus`/`fsck.hfsplus` (the `hfsprogs` package on Fedora/Debian/
Ubuntu — a port of Apple's own `diskdev_cmds`) and a kernel with `hfsplus` support
(`CONFIG_HFSPLUS_FS`, either built-in or as a loadable module — standard on virtually every
mainstream distro kernel; see "Environment notes" for the one dev-environment exception,
WSL2, that doesn't have it). `mount`/`umount`/`blkid`/`cp`/`tar` are assumed present on any
Linux system as a matter of course and aren't called out as new dependencies.

## Remaining phases (see original plan for full detail; ordering matters)

1. ~~**Phase 3 — `DeviceManager.m` → `.hpp`/`.cpp`**~~ **Done.** The checkm8/SHAtter
   exploit byte-level USB control transfer logic was ported **verbatim** (same sequence,
   buffer sizes, sleep durations — exploit-critical hardware-timing code, not "improved"
   during conversion). `AppleTVIcon`'s Cocoa rendering became a plain `AppleTVDevice`
   struct; `dispatch_async`/`MainView` mutation became `DeviceEventSink` callbacks (direct,
   synchronous calls — there's no GUI event loop to marshal onto); `NSDictionary`-based
   plist handling became direct libplist C API calls extracting just the 5 fields actually
   used; the device list became a `std::deque<AppleTVDevice>` (deque specifically, not
   vector — push_back must not invalidate pointers a background jailbreak-check thread may
   be holding). `arrangeIcons()` (pure icon-grid layout) and the original `newDevice()`'s
   "is this the currently-GUI-selected device, should we auto-continue" logic were dropped
   entirely — the latter is a CLI-front-end decision to make in Phase 6, not
   `DeviceManager`'s job. **Not yet wired up** — `main.cpp` doesn't construct a
   `DeviceManager` or call anything in it; that's Phase 6.
2. ~~**Phase 4 — `IPSW`/`IPSWDownloader` → `.hpp`/`.cpp`**~~ **Done.** `IpswFetch` replaces
   `IPSW_Fetch`: `NSURLSession` → a small `httpGet()` via libcurl (the IPSW.me API
   response is plain text, not JSON, confirmed by how the original consumed it — no JSON
   parser needed); `NSDictionary`-based `.keys` parsing → direct libplist dict/array
   iteration into `std::map<std::string, FirmwareKeyPair>`. `FragmentDownloader` replaces
   the original's fixed 5-slot design (`dlone`..`dlfive`, which only existed to drive 5
   parallel `NSProgressIndicator` widgets) with one instance per in-flight download and a
   `std::function<void(unsigned int)>` progress callback — since
   `fragmentzip_download_file`'s callback is a bare C function pointer with no user-data
   parameter, routing to the callback uses a `thread_local` trampoline (safe because the
   call is synchronous/blocking on the calling thread); this scales to any number of
   concurrent downloads (each on its own thread) rather than being capped at 5. The
   original's hardcoded `/Users/<name>/Documents/Blackb0x` save path became
   `ipswDataRoot()` (`$XDG_DATA_HOME/blackb0x` or `~/.local/share/blackb0x`). Verified
   functionally, not just compiled — see "Current state" above.
3. ~~**Phase 5 — `Patcher.mm` → `.hpp`/`.cpp`**~~ **Done.** `patchiBSS`/`patchiBEC`/
   `patchKernel` ported verbatim. `patchRamdisk` replaces the `hdiutil`/`tar` shell-outs
   with: `mkfs.hfsplus` a new right-sized volume, loop-mount it via the real Linux kernel
   `hfsplus` driver alongside a read-only mount of the original, `cp -a` the original's
   entire tree across (plus a narrow, deliberate copy of its volume-header identity fields),
   then inject every payload with ordinary `tar -x`/file copies. Getting here took two
   earlier designs that both hit real, reproducible data-corrupting bugs under real firmware
   payloads (an in-memory `xpwn` `Volume` writer, then `libhfsp`), plus a confirmed dead end
   trying to resize the *existing* volume in place instead of building a right-sized
   replacement (`libparted-fs-resize` explicitly doesn't implement growing HFS+, only
   shrinking) — see the "RESOLVED" writeup above for the full evidence trail. **Verified
   end-to-end** against a real, live-downloaded AppleTV2,1 11D258 `RestoreRamdisk.dmg`:
   `patchRamdisk(ssh=false)` returns `true`, output passes `fsck.hfsplus` clean, and every
   spot-checked injected path is present after mounting the result. Needs the calling
   process to run as root (see "Current state" above). The `AppleTV2,1_4.x` from-scratch
   ramdisk-rebuild branch is deliberately unimplemented (a materially different problem —
   building an empty volume with no pre-existing content to base it on — and out of scope
   for this pass), not forgotten.
4. ~~**Phase 6 — CLI framework**~~ **Done.** `Cli.hpp`/`Cli.cpp` replace
   `AppDelegate`/`MainView`/`Blackb0x.h/.m`/`main.m` (the Objective-C originals are still
   in-tree for reference, per the repo-layout note above) — a single linear session:
   wait for + select a device (`--ecid`/`--udid` flags or an interactive numbered menu),
   wait for DFU mode if needed (an actual blocking poll loop, replacing `spawnDFUHelper`'s
   popup — a CLI has no button to re-click once the user notices it), run the
   model-appropriate exploit (`checkExploit`'s per-model branching ported verbatim,
   including the literal "plug in an Arduino" message for `AppleTV3,1`), download+patch
   firmware components for the right build (`10B329a` hardcoded for a fresh jailbreak
   install — verbatim from the original, a real firmware-compatibility constraint, not a
   placeholder), and send them to the device (`componentsReady:`'s jailbroken-vs-fresh
   `iBECDowngrade`/`iBECBoot` branching, also verbatim). `TaskManager.h/.m` is dropped
   entirely — it only ever drove `NSProgressIndicator` widgets, no logic of its own.
   Downloads are sequential, not parallel like the original's fire-and-forget
   `dispatch_async` — a deliberate simplification, see `Cli.cpp`'s header comment.
   **Decided**: `blackb0x` just runs as root, full stop — no scoped sudoers/polkit rule, no
   udev-group juggling for non-root USB access either, since the DFU/USB layer needs raw
   device access anyway and the user explicitly didn't want to maintain two separate
   privilege-elevation stories (one for USB, one for mount/mkfs) when "run the whole thing
   as root" already covers both. `patchRamdisk()` itself deliberately doesn't embed `sudo`
   calls (see its doc comment) precisely so this choice stays open for later reconsideration.
   **Not yet hardware-tested** — compiles and runs its non-hardware-dependent paths
   (`--help`, the root check, option parsing) cleanly; the actual device-selection/DFU-wait/
   exploit/upload flow needs a physical Apple TV 2/3, the primary hardware-gated checkpoint
   for the whole port (see below). One real device was available briefly during this phase,
   but only in Normal mode, and its lockdown pairing failed — root-caused to the *system's*
   `usbmuxd` (not this port's code: `go-ios`, an independent from-scratch reimplementation,
   failed identically) refusing the device's ancient SSL/TLS handshake during its own
   preflight step (`journalctl -u usbmuxd`: `lockdown error -5`, i.e. `LOCKDOWN_E_SSL_ERROR`)
   — a real environment incompatibility between 2010s-era embedded lockdownd and a modern
   OpenSSL 3.x-linked usbmuxd, not something to chase further here since it doesn't block
   DFU-mode operation at all (`checkm8`/`SHAtter`/every `send*` call talk raw USB via
   `libirecovery`, no lockdownd/SSL/usbmuxd involved).
   - **Security fix made during this phase**: `Blackb0x/Files/ssh.tar` — a static, checked-
     into-this-public-repo resource — shipped fixed SSH host keys (RSA, DSA, and the legacy
     SSH-1 format) identical on every device anyone has ever jailbroken with this tool, which
     defeats SSH host-key verification entirely (anyone with a copy of this repo can already
     impersonate any Blackb0x-jailbroken device). Fixed two ways: (1) the host keys were
     surgically removed from `ssh.tar` itself via `tar --delete` (confirmed byte-identical
     otherwise — no metadata drift on the remaining members), and `patchRamdisk()` now
     generates a fresh RSA host key per patching run via the host's own `ssh-keygen` (RSA
     only — this old sshd predates ed25519, and modern `ssh-keygen` can no longer generate
     the SSH-1/DSA formats it would otherwise also need); (2) `patchRamdisk()` requires (hard
     error if missing, not a soft warning) the invoking user's own `~/.ssh/authorized_keys` —
     resolved via `$SUDO_USER` since the tool runs as root, so plain `$HOME` would otherwise
     resolve to root's home — and copies it onto the device's root account, so the device is
     reachable with keys the user already controls rather than any shared default. This tool
     deliberately never generates an SSH *client* keypair on the user's behalf — if they don't
     have one, the error message tells them to run `ssh-keygen` themselves and add the result
     to `~/.ssh/authorized_keys`, two distinct steps. Verified end-to-end against real
     downloaded firmware: legacy/DSA host key files absent from the patched image, a fresh
     RSA host key with its own unique fingerprint present, `authorized_keys` byte-identical to
     the invoking user's own file with correct `0600`/`0700` permissions, and `fsck.hfsplus`
     still clean.
   - **Tried and abandoned: an in-process reimplementation of `usbmuxd`'s raw
     TCP-over-USB transport, entirely bypassing the system daemon.** The
     `lockdown error -5` finding above first looked like "usbmuxd's SSL is too
     modern," but direct testing showed the system usbmuxd doesn't expose this
     device to any client at all once its own preflight step fails — not even the
     raw device listing — so at the time this looked like the system daemon could
     never be used for this hardware, for anything. In response, `third_party/usbmuxd`
     was vendored as a submodule and its real, unmodified `src/usb.c`/`src/device.c`
     (a genuine mini-TCP stack: `struct mux_header` wrapping a real `struct tcphdr`,
     full SYN/SYN-ACK/ACK connection states) were built into an in-process transport
     (`Blackb0x/Source/InProcessMux/`), with `preflight.c` stubbed out and `client.c`
     replaced by a single-process mutex/condvar byte-buffer client instead of a
     Unix-socket-per-client model. This was fully debugged (including a real,
     non-obvious bug: inbound device data only becomes visible to a reader when
     `device_client_process()` is driven with `POLLOUT`, an event a real `poll()`
     loop over client sockets normally supplies and which the in-process design had
     to generate itself via a pump thread) and **verified end-to-end against real
     hardware** — a plaintext lockdownd `QueryType` round-trip matched the expected
     `com.apple.mobile.lockdown` response exactly. It was abandoned anyway once
     further investigation (below) found that the system usbmuxd's transport layer
     is actually protocol-agnostic and gates devices in exactly one place
     (`preflight.c`'s own internal SSL probe), which a documented `--no-preflight`
     daemon flag disables outright — making the entire vendored transport
     unnecessary. `third_party/usbmuxd` has been deregistered as a submodule and
     `Blackb0x/Source/InProcessMux/` deleted; see the "libimobiledevice forked"
     section below for the architecture that replaced it.
     - **wolfSSL vendored for the actual SSLv3 layer** (submodule, pinned to current
       HEAD — no historical pin applies, this supplies capability the original app never
       needed). mbedTLS and GnuTLS were ruled out first by direct research: mbedTLS
       dropped SSLv3 in 3.0.0 (so only an EOL 2.x release would have it), GnuTLS dropped
       it in 3.4.0 (2015) — wolfSSL is the one currently-maintained library that still
       supports it, via an explicit `--enable-sslv3` build flag, specifically because it
       targets embedded/legacy-compatibility use cases. Built via the same
       `ExternalProject_Add` pattern as the other autotools deps, with
       `--enable-sslv3 --enable-arc4 --enable-md5` (the latter two, both off by default,
       anticipating that a genuinely ancient SSLv3 peer will only offer legacy
       RC4-MD5-family cipher suites — not yet confirmed against the real device, since
       the handshake itself isn't implemented yet). Deliberately uses wolfSSL's own
       native `wolfSSL_*` API, never its OpenSSL-compatibility layer, so it cannot
       collide with the real OpenSSL 3.x symbols already linked elsewhere in this
       project.
     - **A from-scratch lockdownd+AFC client (`LegacyLockdownClient`/`AfcClient`/
       `CertGen`/`PairingStore`) was built, fully verified against real hardware, and
       then deliberately deleted** once a better architecture was found — see the
       "libimobiledevice forked instead of hand-maintaining a lockdownd/AFC client"
       section below for what replaced it and why. It's gone from the tree, but three
       real bugs it surfaced along the way are still load-bearing today (the fixes
       live on in the fork/wolfSSL build, not in deleted code), so the debugging trail
       is worth keeping:
       1. `wolfSSL_CTX_use_certificate_buffer` failed with `-463`
          (`WOLFSSL_BAD_FILE`, an unhelpfully generic code). wolfSSL's own
          `--enable-debug` trace pinpointed the actual cause: the cert generator gave
          *every* generated cert — including the host leaf cert loaded directly into
          wolfSSL — a serial number of `0`. wolfSSL enforces RFC 5280 §4.1.2.2
          (positive serials) and rejects serial `0` on any cert that isn't a
          self-signed CA (`wolfcrypt/src/asn.c`: "Error serial number of 0 for
          non-root certificate") — and real libimobiledevice's own
          `common/userpref.c` does the exact same thing on its OpenSSL code path
          (`ASN1_INTEGER_set(sn, 0)` on root/host/device certs alike), so this wasn't
          a bug specific to the from-scratch generator. Rather than patching
          `userpref.c` to diverge from upstream, the fix landed in wolfSSL's own
          build instead: `EXTRA_CFLAGS=-DWOLFSSL_ASN_ALLOW_0_SERIAL` (wolfSSL's own
          documented escape hatch for exactly this, confirmed directly in
          `wolfcrypt/src/asn.c`'s guard around the same check) — one line, and
          `userpref.c` needed no changes at all.
       2. Even after the cert loaded, the very first real handshake attempt got an
          immediate TCP RST from the device, ~2ms after a suspiciously tiny 48-byte
          ClientHello. Root cause: wolfSSL compiles out the entire classic
          static-RSA-key-exchange cipher suite family (`RC4-MD5`, `RC4-SHA`, etc.) by
          default as a security hardening measure — and those are the *only* suites a
          genuine SSLv3 peer from this era can offer (SSLv3 predates
          ephemeral DHE/ECDHE key exchange entirely). With none compiled in, the
          ClientHello's cipher-suite list was effectively empty, and the device
          correctly rejected it outright. Fixed by rebuilding wolfSSL with
          `EXTRA_CFLAGS=-DWOLFSSL_STATIC_RSA` (there's no dedicated
          `--enable-staticrsa` configure flag — the only place this macro appears in
          `configure.ac` is bundled into the unrelated `--enable-qt-test` path, which
          *also* forces `-DOPENSSL_NO_SSL3`, the opposite of what's needed) and
          explicitly calling `wolfSSL_CTX_set_cipher_list(ctx, "RC4-MD5")` at the SSL
          setup site rather than trusting wolfSSL's default suite list — both fixes
          are still exactly what the libimobiledevice fork's own
          `idevice_connection_enable_ssl()` does today.
       3. With the handshake itself finally succeeding, the *first* `GetValue` request
          worked, but every one after it failed or returned garbage ("implausible
          response length"). Root cause: the from-scratch client's 4-byte length-prefix
          read did a single non-looping `wolfSSL_read(..., 4)` call and treated any
          return other than exactly 4 as a hard failure — but a short read (`n=1`,
          `n=3`) is normal stream behavior, not an error, and the un-consumed remainder
          of that prefix stayed in the stream, permanently desyncing every request
          after the first. Fixed with a small loop-to-completion helper at the time;
          real libimobiledevice's own `property_list_service.c` already loops
          correctly for this exact reason, so the fork inherits correct behavior here
          for free.
       Also found by inspecting a real successful pairing response: **this device's
       `Pair` response omits `"Result": "Success"` entirely on success** (`Result` is
       only ever present to signal failure). Real libimobiledevice's own
       `lockdown.c` already handles this correctly and has for years — its
       `lockdownd_check_result()` has a comment reading `/* iOS 5: the 'Result' key is
       not present anymore. But we need to check for the 'Error' key. */` — so this
       needed no fix at all once the fork's transport could reach the device; it was
       only a real problem for the from-scratch client, which didn't know the quirk
       going in.
     - **Project-wide OpenSSL removal.** After the cert-serial bug above, and per an
       explicit decision to stop linking real OpenSSL at all: every OpenSSL use in
       `xpwn`/`dmg`'s vendored `.c` files (`filevault.c`, `8900.c`, `img3.c`,
       `pwnutil.c`) now resolves through wolfSSL's OpenSSL-compatibility shim headers
       (`wolfssl/openssl/*.h`, enabled via `--enable-opensslall`) instead of real
       OpenSSL, via `OPENSSL_INCLUDE_DIR`/`OPENSSL_LIBRARIES`/`CRYPTO_LIBRARIES`
       CACHE-variable overrides forced before `add_subdirectory(third_party/xpwn)`.
       This wasn't a drop-in swap — three real build problems had to be solved:
       - wolfSSL's shim headers aren't self-contained the way real OpenSSL's are; they
         need `wolfssl/options.h` processed first. Fixed with a per-target
         `-include .../wolfssl/options.h` force-include on every xpwn/dmg target, plus
         `${DEPS_INCLUDE}` on the include path for `options.h`'s own
         `#include <wolfssl/wolfcrypt/settings.h>`.
       - `--enable-opensslall`'s shim headers textually collide with real OpenSSL
         headers if both ever land in the same translation unit (both declare the same
         bare symbol names). This same constraint is why the libimobiledevice fork's
         `idevice.c` (native wolfSSL, for the actual TLS session — see below) never
         `#include`s any OpenSSL-shim header, while `common/userpref.c` in that same
         library (real OpenSSL/wolfSSL-shim, for cert generation) is left completely
         unpatched in its own separate translation unit — the same two-files-never-
         mixed pattern this project used earlier for its own now-deleted
         `CertGen.cpp`/`LegacyLockdownClient.cpp` split.
       - linuxbrew's own real OpenSSL headers happen to live under
         `/home/linuxbrew/.linuxbrew/include`, the same prefix PNG/BZip2's own
         `include_directories()` calls inside xpwn's vendored CMakeLists.txt pull in
         incidentally — and without `BEFORE`, that path won the `-I` order race ahead
         of wolfSSL's shim dir, so `#include <openssl/aes.h>` silently resolved to the
         real header, compiled fine, and only failed at *link* time with undefined
         references to bare `AES_cbc_encrypt`/`SHA1_Init` (confirmed via `nm` on the
         resulting `.o`). Fixed with `target_include_directories(... BEFORE PRIVATE
         ${DEPS_INCLUDE}/wolfssl)` on every affected target.
       Verified via `nm -D` on the final `blackb0x` binary: zero `SSL_*`/`EVP_*`/
       `X509_*` symbols anywhere, confirming no real OpenSSL linkage survives.
     - **Full static linking.** Per an explicit decision that every third-party
       dependency should be a static archive (only glibc/libstdc++/libgcc_s/libm stay
       dynamic — deliberately *not* full `-static`, since glibc's NSS/`getpwnam`
       is unreliable when statically linked, and was needed at the time for
       `$SUDO_USER` resolution in code since superseded — see the libimobiledevice
       fork section below), five more submodules were vendored specifically because no
       static archive for them exists anywhere on this system: `curl` (built against
       wolfSSL, not OpenSSL — `CURL_USE_WOLFSSL=ON`, `-DUSE_LIBIDN2=OFF` since the
       system's `libidn2` is only available as a `.so` and IDN support isn't needed for
       Apple's ASCII-only hostnames), `libusb` (`--disable-udev`, verified its
       netlink-based Linux backend, `linux_netlink.c`, is a real first-class
       non-udev path per `configure.ac`, not a degraded fallback), `libzip`,
       `libpng` (pinned to the `v1.6.58` release tag — its `master` branch tracks an
       unstable `1.8`-dev series that produces a differently-named `libpng18.a`), and
       `bzip2` (`libarchive/bzip2`, a common CMake-buildable fork — upstream bzip2 has
       no CMake support at all). Real build-system problems solved along the way:
       - `CMAKE_INSTALL_LIBDIR` defaults to `lib64` on this Fedora-based system for
         CMake-based `ExternalProject_Add` targets (`curl`, `libzip`), while every
         autotools-based one installs to plain `lib` — pinned
         `-DCMAKE_INSTALL_LIBDIR=lib` explicitly on both to match.
       - `libusb`'s public header installs to `include/libusb-1.0/libusb.h`, not
         directly under `include/`, but usbmuxd's own `usb.c` does a bare
         `#include <libusb.h>` — the imported `deps::usb` target's
         `INTERFACE_INCLUDE_DIRECTORIES` needed that subdirectory added explicitly
         (and the directory pre-created via `file(MAKE_DIRECTORY ...)` at configure
         time, since CMake validates an IMPORTED target's include paths before any
         `ExternalProject` has actually built anything).
       - Two separate vendored xpwn files (`minizip/CMakeLists.txt`,
         `dmg/CMakeLists.txt`) each hardcode a bare `target_link_libraries(... z)`
         *in addition to* their own correct `${ZLIB_LIBRARIES}` reference — a bare
         `-lz` makes the linker do a fresh library search that prefers the `.so` over
         a static `.a` sitting in the same `-L` directory, silently reintroducing a
         dynamic `libz.so` dependency. Fixed post-hoc from the top-level
         `CMakeLists.txt` (not by editing the vendored files) — overwriting
         `minizip`'s `LINK_LIBRARIES`/`INTERFACE_LINK_LIBRARIES` outright (since
         `target_link_libraries()` only appends, so calling it again can't undo the
         bad entry), and surgically removing just the `z` item from `dmg`'s list with
         `list(REMOVE_ITEM ...)`.
       - `bzip2_ext`/`libpng_ext` need to exist as CMake targets *before* the loop that
         calls `add_dependencies(${xpwn_executable} bzip2_ext libpng_ext)` on every
         executable xpwn's subdirectories define (needed because `xpwn`, the static
         library, re-exports `${BZIP2_LIBRARIES}`/`${PNG_LIBRARIES}` transitively to
         every consumer) — CMake silently drops an `add_dependencies()` call whose
         target doesn't exist yet instead of erroring, which produced a real "No rule
         to make target 'deps/lib/libpng16.a'" build failure until both
         `ExternalProject_Add` blocks were moved earlier in the file.
       Verified via `ldd` on the final `blackb0x`: only
       `libstdc++.so.6`/`libm.so.6`/`libgcc_s.so.1`/`libc.so.6`/`ld-linux-x86-64.so.2`
       remain — no `libssl`/`libcrypto`/`libcurl`/`libusb`/`libzip`/`libpng`/`libbz2`/
       `libudev` anywhere in the dynamic dependency list.
     - **`third_party/libfragmentzip` is forked**, not patched in-place: its
       `libfragmentzip.h` needs a small C++-compatibility fix (`#include <stdbool.h>`
       instead of a hand-rolled `typedef enum{false=0,true=1}bool;` that collides with
       C++'s built-in `bool` keyword) that a plain in-tree patch lost *twice* to
       `git reset --hard` on that submodule during this session. Fixed for good by
       forking to `github.com/regulad/libfragmentzip`, committing the fix on a
       `fix-cxx-stdbool-header` branch off the exact commit already in use, and
       pointing `.gitmodules`'s `url`/`branch` at the fork — the fix is now upstream of
       any future reset instead of living as a repeatedly-reapplied local patch.
     - **libimobiledevice was forked instead of hand-maintaining a lockdownd/AFC
       client, and `DeviceManager.cpp` is wired up to it through the real system
       `usbmuxd`.** The from-scratch `LegacyLockdownClient`/`AfcClient`/`CertGen`/
       `PairingStore` above were fully verified against real hardware first — full
       pairing, `StartSession`, the wolfSSL SSLv3 handshake, and `GetValue` all
       returned correct data end-to-end against a real Apple TV 3,2
       (`8.4.4`/`12H1006`) — but implementing AFC's full feature set (and every
       *other* libimobiledevice service: installation_proxy, notification_proxy,
       diagnostics_relay, mobile_image_mounter, ...) from scratch one operation at a
       time was never going to be a good use of time, when real libimobiledevice
       already implements all of it correctly.
       - **The question that changed the architecture**: given that
         `idevice_connection_enable_ssl()` (not `usbmuxd`) is what terminates SSL for
         a libimobiledevice client, could the system `usbmuxd` be used again instead
         of vendoring a whole transport? Reading `usbmuxd`'s own real source
         (`src/device.c`/`usb.c`/`client.c`, fetched directly from GitHub) confirmed
         those three files are a pure byte-shuttle mux-TCP-over-USB engine with zero
         SSL/TLS awareness anywhere in them (`grep -in "ssl\|tls"` on all three: no
         matches). The *only* place the daemon itself touches SSL is `preflight.c` —
         an internal, best-effort probe that opens its own lockdown session (using
         the daemon's own linked libimobiledevice) purely to warm a pairing-record
         cache, called *before* `client_device_add()` (the call that actually makes a
         device visible to any client's `usbmuxd_get_device_list()`). Several of
         `preflight.c`'s failure paths `goto leave` without ever reaching
         `client_device_add()` — that's the entire mechanism hiding this hardware
         from the system daemon, not anything in the daemon's real transport. And
         `main.c` has a real, documented `-p`/`--no-preflight` flag that skips this
         probe entirely and calls `client_device_add()` unconditionally. So the
         system usbmuxd works fine for this hardware's raw transport — it just needs
         to be told not to preflight it with a TLS stack that can't negotiate SSLv3
         either. **This is why the in-process transport above was abandoned**: once
         `--no-preflight` was confirmed to work, vendoring `usbmuxd`'s own engine was
         no longer buying anything, only extra code to maintain.
       - **The remaining problem is exactly the one the in-process attempt already
         solved**: libimobiledevice's own `idevice_connection_enable_ssl()` hardcodes
         real OpenSSL/GnuTLS with `SSL_CTX_set_min_proto_version(ssl_ctx,
         TLS1_VERSION)` — a TLS 1.0 floor that can never negotiate SSLv3 regardless
         of backing library, the same reason the system daemon's own preflight probe
         fails. Since libimobiledevice has no pluggable transport/TLS layer
         (`internal_connection_send`/`internal_connection_receive` call
         `usbmuxd_send`/`usbmuxd_recv` directly on a real fd, and SSL setup is
         inlined into `idevice_connection_enable_ssl()`), this still requires a fork
         — but a much smaller one than before, since the real `usbmuxd_*` transport
         calls no longer need replacing, only the SSL call site.
       - **Fork**: `github.com/regulad/libimobiledevice`, branch `legacy`, off the
         same `e52ef95` commit this project was already pinned to (see "Final pins"
         above) — not a new pin, that exact commit with `configure.ac`,
         `src/Makefile.am`, `src/idevice.h`, and `src/idevice.c` patched.
       - **The SSL backend is additive and configure-time selectable, not a hard
         replacement of OpenSSL/GnuTLS**: a new `--with-ssl-implementation=
         auto|openssl|gnutls|wolfssl|mbedtls` autoconf option (`configure.ac`,
         mirroring the existing `--with-cython` pattern) defines exactly one of
         `LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_{OPENSSL,GNUTLS,WOLFSSL}` (naming
         matches this project's own `LIBIMOBILEDEVICE_*` config-macro convention),
         completely independent of the pre-existing `HAVE_OPENSSL`/`--disable-openssl`
         flag, which continues to govern only `common/userpref.c`'s cert generation,
         unchanged. `mbedtls` is a recognized value that errors cleanly at configure
         time — mbedTLS dropped SSLv3 support in 3.0.0, so it fundamentally can't do
         what this option exists for. This project's own build passes
         `--with-ssl-implementation=wolfssl` (see `CMakeLists.txt`'s
         `libimobiledevice_ext`), but the fork itself still builds and works as
         plain upstream against real OpenSSL/GnuTLS if configured that way.
       - **`src/idevice.h`**: `struct ssl_data_private` is now a 3-way branch on
         `LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_{WOLFSSL,GNUTLS}`/plain-OpenSSL, holding
         `WOLFSSL*`/`WOLFSSL_CTX*` in the new branch and the original fields
         unchanged in the other two. `struct idevice_connection_private`/
         `struct idevice_private` are **completely unchanged** from upstream — `data`
         still holds the real fd `usbmuxd_connect()` returns, `mux_id` is still real
         — there is no opaque bridge type anywhere in this version of the patch.
       - **`src/idevice.c`**: every transport function (`idevice_new_with_options`,
         `idevice_connect`, `internal_connection_send`/`_receive`/`_receive_timeout`,
         `idevice_get_device_list[_extended]`, `idevice_event_subscribe`, ...) is
         **byte-for-byte unchanged from upstream** — real `usbmuxd_*` calls
         throughout, real push-based hotplug notification via
         `idevice_event_subscribe()`. Only `idevice_connection_enable_ssl()` (plus
         `internal_idevice_init`/`_deinit` for `wolfSSL_Init`/`Cleanup`, and cleanup
         in `internal_ssl_cleanup`) gained a new
         `#elif defined(LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_WOLFSSL)` branch, using
         `wolfSSL_set_fd()` against the real fd (no custom I/O callbacks needed, since
         a real fd exists again), `wolfSSLv3_client_method()`,
         `wolfSSL_CTX_set_cipher_list(ctx, "RC4-MD5")`, and the pair record's **host**
         certificate/key (`USERPREF_HOST_CERTIFICATE_KEY`/`_HOST_PRIVATE_KEY_KEY`) —
         not the **root** cert/key upstream's OpenSSL branch uses, which targets
         iOS 5+/TLS1.0-era devices and was never verified to also work for HOST vs
         ROOT on hardware this old; HOST is what the from-scratch client verified
         correct above. `common/userpref.c` — real cert generation, using real
         OpenSSL/GnuTLS — is completely unpatched; its serial-0 certs work as-is
         because of the `WOLFSSL_ASN_ALLOW_0_SERIAL` wolfSSL build flag (see the
         bug-history bullet above), not because anything in this fork was changed
         for it. Verified via `nm build/deps/lib/libimobiledevice.a | grep -c
         wolfSSL_connect` (real wolfSSL symbols compiled in, not silently falling
         back) and by inspecting the generated `config.h` (`HAVE_OPENSSL` and
         `LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_WOLFSSL` both set, independently, as
         designed). Diff against upstream `e52ef95`: 4 files, +237/-27 lines — kept
         deliberately small and clean (`git reset --soft` + a targeted re-patch onto
         a pristine checkout, then `git push --force origin legacy`, after an earlier,
         much larger version of this same fork branch — the one that also replaced
         the transport layer for the in-process mux attempt above — was abandoned).
       - **A real, known ecosystem conflict, still present and still fixed the same
         way**: `libusbmuxd.a` (needed transitively — `common/userpref.c`'s
         `usbmuxd_read_buid`/`usbmuxd_read_pair_record`/etc, a daemon-side
         pairing-record cache path separate from the direct-filesystem
         `/var/lib/lockdown` storage) and `libimobiledevice-glue-1.0.a` each vendor
         their own separate copy of `common/collection.c` with external linkage — a
         hard "multiple definition" link error once a real binary exercises both.
         Both copies implement the same generic collection ADT, so
         `-Wl,--allow-multiple-definition` on `blackb0x`'s own link flags is the
         standard practical workaround, not a correctness compromise.
       - **Requires the system `usbmuxd` to be run with `--no-preflight` (or `-p`)**
         — without it, this hardware's preflight probe still fails exactly as
         described above and the device is never exposed to `blackb0x` at all, fork
         or no fork. **This must ship as a systemd drop-in and be documented in the
         end-user README**: a unit override
         (`/etc/systemd/system/usbmuxd.service.d/override.conf`, or the
         distro-appropriate equivalent) adding `--no-preflight` to `usbmuxd`'s
         `ExecStart` — e.g.
         ```
         [Service]
         ExecStart=
         ExecStart=/usr/sbin/usbmuxd -U usbmuxd --no-preflight
         ```
         (the empty `ExecStart=` first is required to clear the original
         `ExecStart` before overriding it — systemd drop-ins append by default), then
         `systemctl daemon-reload && systemctl restart usbmuxd`. This is a real,
         permanent change to how the *system's* usbmuxd behaves for every device, not
         just this project's target hardware — worth calling out plainly in the
         README rather than leaving it as an implicit prerequisite, since without it
         normal-mode discovery silently never fires.
       - **Verified against real hardware, through the real, unmodified `afc.c`**
         (not the deleted from-scratch `AfcClient`), talking through the real system
         `usbmuxd` run with `--no-preflight`: a checkpoint program calling the real
         `idevice_new`/`lockdownd_client_new_with_handshake`/`lockdownd_get_value`/
         `lockdownd_start_service`/`afc_client_new`/`afc_read_directory` returned
         correct `ProductType`/`ProductVersion`/`BuildVersion`/`DeviceName`/
         `UniqueDeviceID`, correctly reported `com.apple.afc2` unreachable (device
         isn't jailbroken yet), and produced a genuine AFC directory listing
         (`. .. DCIM Downloads Photos iTunes_Control`) via libimobiledevice's actual
         AFC protocol implementation. Also confirmed directly via
         `ls -la /var/lib/lockdown/`: the pairing record landed at the genuine
         libimobiledevice storage path (`<udid>.plist`, owned `usbmuxd:usbmuxd`),
         not the `$XDG_CONFIG_HOME/blackb0x` location the now-deleted `PairingStore`
         used.
       - **Pairing-record persistence is entirely delegated to the daemon, on
         purpose — blackb0x never writes into that directory itself.**
         `common/userpref.c`'s `userpref_save_pair_record()`/`_read_pair_record()`/
         `_delete_pair_record()` are just thin wrappers around
         `usbmuxd_save_pair_record_with_device_id()`/`usbmuxd_read_pair_record()`/
         `usbmuxd_delete_pair_record()` (`libusbmuxd`) — confirmed by reading both
         files end to end — which only ever send a `SavePairRecord`/`ReadPairRecord`/
         `DeletePairRecord` message over the usbmuxd control socket; there is no
         fallback direct-filesystem write path anywhere in this fork or upstream.
         The actual `<udid>.plist` file is created by the daemon process itself, so
         even though blackb0x runs entirely as root, the file lands owned by
         whatever unprivileged user the daemon runs as (`usbmuxd:usbmuxd` here) with
         mode `644` — exactly the ownership/permissions the directory (`/var/lib/
         lockdown` on this system; some distros use `/var/lib/usbmuxd` instead, same
         idea) needs, for free, without blackb0x having to chown/chmod anything
         itself. **If any future code path ever needs to write a file into that
         directory directly** (bypassing the daemon's own IPC), it must explicitly
         `chown`/`chmod 644` it to match the directory's owner — root would
         otherwise leave the daemon unable to read its own pairing-record store.
       - **`LegacyLockdownClient`/`AfcClient`/`CertGen`/`PairingStore` are deleted**
         (all four files, entirely), per the explicit decision to not maintain two
         parallel implementations of the same thing. `DeviceManager.cpp`'s
         `plistInfoForDeviceUUID()`/`isJailbroken()`/`isJailbreakRunning()` are back
         to essentially their *original* pre-fork form (real `idevice_new`/
         `lockdownd_client_new_with_handshake`/etc calls), and its constructor is
         back to a single `idevice_event_subscribe(blackb0x_idevice_event_cb, ...)`
         call — real push-based hotplug discovery through the real daemon, no
         polling thread. None of this higher-level code needed to change at all;
         only the library underneath it did — the whole point of this approach is
         that it doesn't know or care that it's talking to hardware the system
         usbmuxd would otherwise refuse. `third_party/usbmuxd` (the vendored daemon
         submodule) and `Blackb0x/Source/InProcessMux/` (the whole in-process
         transport, bridge, and verbosity-scheme directory) are both gone from the
         tree — no protocol-level or transport-level code left to maintain outside
         the fork itself.
   - **Tried and abandoned: Fil-C (fil-c.org) as this project's whole-repo build
     toolchain.** Fil-C is a memory-safe C/C++ clang fork (fat pointers + a
     capability-checked runtime) — tried on the theory that a root-required
     binary this size, written entirely in C/C++, is worth hardening against
     its own memory-safety bugs. Fil-C requires the ENTIRE dependency graph to
     be compiled by its own compiler (no interop with normal "Yolo-C" objects
     at all), so every `ExternalProject_Add(..._ext)` needed `CC`/`CXX`
     (autotools) or `-DCMAKE_C_COMPILER`/`-DCMAKE_CXX_COMPILER` (CMake) pointed
     at Fil-C's `clang`/`clang++` explicitly, on top of setting
     `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` before `project()` for every
     native target this file defines directly.
     - **Most of the dependency graph genuinely compiled and linked clean**
       with nothing but the compiler swapped — no source changes: wolfSSL
       (real crypto/ASN.1/RSA/AES, ~70MB of object code), libplist/libplist++,
       libgeneral, libimobiledevice-glue, bzip2, libusb, libzip, curl,
       libimobiledevice, libusbmuxd, libirecovery, and xpwn's own
       `hfs`/`dmg`/`minizip`/`common` static libraries.
     - **zlib had to stop being the one system/Homebrew-provided, non-vendored
       dependency** (see the ZLIB comment near the top of `CMakeLists.txt`) —
       Fil-C's "no interop, full stop" rule doesn't make an exception for
       small/simple libraries, and pulling in the real system `zlib.h`
       (`ZLIB_INCLUDE_DIR=/usr/include`, found by the old `find_package(ZLIB)`)
       broke immediately with "unknown type name '__gnuc_va_list'" the moment
       any translation unit's `#include` chain reached `<stdio.h>` — glibc's
       header assumes gcc/glibc-internal builtin cooperation Fil-C's own
       clang+musl doesn't independently provide when parsing a foreign libc's
       headers directly. This was the single largest class of errors (~6000,
       nearly all identical) in the first whole-repo attempt. **`zlib_ext` is
       kept vendored even after reverting off Fil-C** — the user's explicit
       call, and also more consistent with this project's own "every
       third-party dependency is a static archive built from source" goal
       (see "Full static linking" above) than reintroducing the one exception
       would have been.
     - **The actual wall: a genuine Fil-C compiler crash, not a config
       problem.** `third_party/xpwn`'s own CLI tools (`ipsw-patch/libxpwn.c`,
       `dmg/dmg.c`, `hdutil/hdutil.c`, `hfs/hfs.c`) are circa-2010 C that
       declares the same globals in multiple translation units without
       `extern` — a legal, if deprecated, pre-C99 idiom, which is exactly why
       this project already passes `-fcommon` to every xpwn target (a
       pre-existing fix for an unrelated GCC≥14 diagnostics change). Fil-C's
       own instrumentation pass can't represent a "common linkage" global at
       all (its whole memory-safety model needs a global's size/type resolved
       unambiguously, which common linkage deliberately defers to link time)
       and hits a hard assertion instead of a clean diagnostic:
       ```
       clang: /fil-c/llvm/.../FilPizlonator.cpp:12459: Assertion
       `G.getLinkage() != GlobalValue::CommonLinkage' failed.
       ```
       (Fil-C version 0.684.) **Checked for an existing upstream issue before
       stopping**: none found for this exact assertion. The closest is
       [pizlonator/fil-c#251](https://github.com/pizlonator/fil-c/issues/251)
       ("Compiler crashes when building Protobuf"), which hits the same
       `lockDownLinkage()` function's assertions but for `AppendingLinkage`,
       not `CommonLinkage` — a related but genuinely different case. Worth
       filing upstream if this gets revisited, but wasn't filed as part of
       this session.
     - **Two smaller, separate build-script bugs found and fixed along the
       way** (both real, independent of Fil-C, but only surfaced by trying
       it): `ExternalProject_Add(... BUILD_COMMAND make -j SUBDIRS="common src
       include")` — writing the quotes literally into an *unquoted* CMake
       argument doesn't group it the way a shell would; CMake splits on the
       embedded spaces regardless, producing three broken argv tokens
       (confirmed directly: `/bin/sh: line 21: cd: "common: No such file or
       directory`). The fix is a single CMake-quoted token,
       `"SUBDIRS=common src include"`, matching the pre-existing
       `libimobiledevice_ext` pattern (`SUBDIRS=${LIBIMOBILEDEVICE_LIB_SUBDIRS}`,
       a variable substituted whole into one already-CMake-quoted argument).
       Also: `xpwn/hfs/hfscompress.c` does a bare `#include <zlib.h>` with no
       `include_directories()` call anywhere in `xpwn/hfs`'s own
       `CMakeLists.txt` (unlike `minizip`/`ipsw-patch`, which do call that) —
       invisible under a normal host compiler (always has `/usr/include` on
       its default search path regardless) but a hard failure once zlib.h
       wasn't implicitly reachable; fixed with an explicit
       `target_include_directories(hfs PRIVATE ${DEPS_INCLUDE})` +
       `add_dependencies(hfs zlib_ext)`, both kept after reverting off Fil-C
       since they're correct (and arguably more correct — using the vendored
       zlib.h/libz.a consistently) regardless of which compiler builds it.
     - **Reverted via `git diff` review, not a hard reset**: every
       Fil-C-specific piece (the `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER`
       override and its propagation into every `_ext` sub-build, plus the
       `--disable-examples-build`/`--disable-tests-build`/`-DBUILD_OSSFUZZ=OFF`/
       `SUBDIRS=...`-restriction changes made solely to dodge Fil-C's
       CRT/`main`-symbol collisions in `libusb`/`libzip`/`libusbmuxd`/
       `libirecovery`'s own unused test/example/tool binaries) was removed;
       zlib vendoring and the two build-script fixes above were kept.
       Verified with a full clean rebuild afterward: gcc again (via this
       system's `ccache` wrapper), `ldd build/blackb0x` still shows only
       libc/libstdc++/libm/libgcc_s.
   - **First real end-to-end attempts against the user's actual AppleTV3,2,
     with a real DFU-mode remote handoff, surfaced five more real, unrelated
     bugs — all five now fixed — plus real, repetitive CLI noise worth
     cleaning up once actually watched scroll by on a live run:**
     - **The remote's two-stage DFU handoff isn't obvious and the CLI's own
       instructions didn't explain it.** Holding Menu+Down until the LED
       blinks fast only reaches Recovery Mode — which blinks identically to
       DFU mode, so it looks like it worked — DFU itself needs a *second*
       stage right after: release both buttons completely, then hold
       Menu+Play until it blinks fast again. `Cli.cpp`'s `runCli()` DFU-wait
       message now spells out both stages explicitly (plus a note that a bad
       micro-USB cable or a missed IR button-edge during the handoff are the
       two most common silent failure modes), instead of the original
       single-stage "hold DOWN and MENU" instruction that was accurate for
       Recovery Mode but not DFU.
     - **`IPSW.cpp`'s `httpGet()` (used to resolve firmware URLs from
       `api.ipsw.me`) failed with curl's `CURLE_SSL_CACERT` — "Problem with
       the SSL CA cert."** Root-caused with a throwaway `wolfSSL_connect()`
       test program (`wolfSSL_Debugging_ON()` made this fast) rather than
       guessing: the CA bundle itself
       (`/etc/ssl/certs/ca-certificates.crt`) loaded fine and genuinely
       contains the right root (Google Trust Services' `GTS Root R4`,
       confirmed present, which is what `api.ipsw.me`'s real chain —
       `CN=ipsw.me` -> `Google Trust Services WE1` -> `GTS Root R4` —
       chains to) — the actual failure was `ASN_NO_SIGNER_E` (-188) during
       chain validation itself: wolfSSL's default chain verifier requires
       each certificate's issuer to exactly match the next certificate's
       subject in a single, exact path all the way to a trusted root, and
       gives up rather than trying alternate paths the way OpenSSL does.
       This is common enough with real-world CAs (cross-signing, multiple
       valid paths to different roots) that wolfSSL ships a dedicated,
       documented macro for it: `WOLFSSL_ALT_CERT_CHAINS` ("Allow
       non-validated intermediate CAs", `src/internal.c`) — added to
       `wolfssl_ext`'s `EXTRA_CFLAGS` alongside `WOLFSSL_STATIC_RSA`/
       `WOLFSSL_ASN_ALLOW_0_SERIAL` (no dedicated configure flag for this one
       either). Verified fixed with the same throwaway test program
       (`wolfSSL_connect()` now succeeds against `api.ipsw.me`) and directly
       through `blackb0x`'s own static curl+wolfSSL stack (a minimal
       `curl_easy_perform()` against the real `api.ipsw.me` URL blackb0x
       itself calls returns `CURLE_OK`). This is unrelated to the Apple
       TV/`idevice.c` SSLv3 story entirely — it's curl's own modern
       TLS 1.2/1.3 connection to a normal HTTPS API, not the ancient
       lockdownd handshake.
     - **`Patcher::patchKernel()`'s version string was always empty, silently,
       on every single run — a real bug present since this method was first
       written, never previously exercised on real hardware.** The deleted
       `versionString(path)` helper derived a device/version pair by
       splitting `path`'s *parent directory name* on `_` — an assumption
       matching a single flat `"<device>_<version>"` cache-directory layout
       that was never actually true for this port: `Cli.cpp`'s
       `downloadAndPatchComponents()` builds `workDir` as
       `ipswDataRoot()/deviceModel/buildID` — two nested segments, neither of
       which contains a version string at all (confirmed directly: real
       output was `Device: 10B329a - Version:` — the "device" slot actually
       held the *build ID*, and "version" was always empty). Fixed by
       sourcing the version from where it actually lives —
       BuildManifest.plist's own top-level `ProductVersion` key (confirmed
       present via a direct manual fetch+parse of a real manifest:
       `ProductVersion` = `"6.1.3"` for this exact 10B329a build) — added to
       `Cli.cpp`'s `ManifestInfo`/`parseManifest()` and threaded through as
       an explicit parameter,
       `Patcher::patchKernel(path, productVersion)`, replacing the deleted
       path-based guess entirely.
     - **That empty version, plus a second bug — `patchKernel()` never
       checked `patch_kernel()`'s return value — combined into real, live
       heap corruption (`malloc(): corrupted top size`, a crash) on every
       kernel-patch attempt, not just a wrong result.** `CBPatcher.c`'s
       `patch_kernel()` correctly rejects an unsupported/unparseable version
       ("This version of CBPatcher does not support iOS 0 kernels") and
       returns nonzero *without ever writing its output file* — but the
       calling code proceeded to `decrypt()` that nonexistent file anyway.
       Confirmed by direct, isolated reproduction (a throwaway harness
       linking this project's own built `libcbpatcher.a`/`libxpwn.a`/
       `libwolfssl.a` against a real kernelcache fetched from the same real
       IPSW, replaying the exact key/iv from a live run): with `version=""`,
       `patch_kernel()` fails cleanly as designed, and the *following*
       `decrypt()` call — third_party/xpwn's own `xpwntool.c`, given a
       missing input file — is what actually corrupts the heap (`error:
       cannot open infile` immediately followed by `malloc(): corrupted top
       size`). Fixed by checking `patch_kernel()`'s return value and
       aborting `patchKernel()` cleanly (no second `decrypt()`, no
       `outputs_.kernel` set) on failure — a real robustness gap independent
       of the version bug above, since patch_kernel() can fail for other
       reasons on other builds too.
     - **A third, deeper bug — this one survives both fixes above and is
       specific to the kernelcache, never hit by iBSS/iBEC — turned out to
       be a real buffer over-read/over-write inside wolfSSL's own optimized
       AES-CBC path, not xpwn's control flow.** With the *correct* version
       (`"6.1.3"` -> `getRealVersion()` -> `"6.0"`), `patch_kernel()` now
       genuinely succeeds (`[CBPatch] Found ...` signature matches, "Kernel
       patched successfully") — but the *second* `decrypt()` call
       (re-encrypting the patched kernel back into the original IMG3
       container, using the original file as a template — the same
       operation iBSS/iBEC also do, successfully, every run) hit a
       *different* corruption: `double free or corruption (!prev)`.
       Hand-instrumenting copies of `third_party/xpwn/ipsw-patch/{xpwntool.c,
       img3.c,lzssfile.c}` with `fprintf(stderr, ...)` tracing first
       narrowed it to `info->root->free(info->root)` inside
       `duplicateAbstractFile2()`/`duplicateAbstractFile()`'s recursive
       signature-sniffing of the template's nested container structure
       (kernelcache payloads are IMG3-wrapping-a-Comp/LZSS-container,
       confirmed via the "match: ..." trace already visible during the
       *first* `decrypt()` call — iBSS/iBEC are plain, uncompressed IMG3
       with no nested Comp layer, exactly why they never exercise this
       path) — but pinning the *exact* line needed a real debugger.
       `valgrind` (installed by the user partway through this
       investigation, specifically for this) nailed it precisely on the
       first run: `Invalid write of size 4/8` inside wolfSSL's
       `AesDecrypt_C`/`AesDecrypt_preFetchOpt` (`wolfcrypt/src/aes.c`),
       called via `wolfSSL_AES_cbc_encrypt` -> `setKeyImg3`, reading/writing
       up to ~11 bytes **past the end of** `readImg3Element()`'s
       `malloc(header->dataSize)` for the kernelcache's DATA tag
       (`dataSize` = 6,004,801 — an odd, non-16-aligned length). This code
       was written against real OpenSSL's `AES_cbc_encrypt`, which touches
       exactly the requested length; wolfSSL's optimized decrypt path
       (`AesDecrypt_preFetchOpt`, used because this project links wolfSSL's
       OpenSSL-compat shim instead of real OpenSSL — see the "Project-wide
       OpenSSL removal" bullet above) does a small fixed-size lookahead past
       the last full block for performance. iBSS/iBEC never hit this
       because they're smaller/laid out differently, so the same overrun
       happened to land inside other live heap data instead of triggering a
       glibc-detected corruption — not because the underlying bug wasn't
       there.
       - **Fixed in a fork, not a local patch**: `third_party/xpwn`
         (planetbeing/xpwn) had no fork yet, unlike libimobiledevice/
         libfragmentzip — forked to `github.com/regulad/xpwn`, branch
         `legacy`, off the exact pinned commit (`20c32e5`). Two changes on
         that branch: (1) a pre-existing *uncommitted* local edit this repo
         was already carrying (disabling `pwnmetheus2`, which hard-requires
         the unavailable legacy libusb-0.1 API) — folded into the fork so it
         stops being a silently-droppable local diff, same reasoning as the
         other two forks; (2) the actual fix — every buffer that gets
         AES-CBC'd in place (`readImg3Element`'s DATA/KBAG allocations,
         `writeImg3`'s realloc-grown write buffer) now gets one extra,
         explicitly-zeroed AES block (`IMG3_AES_OVERREAD_PAD = 16`,
         `calloc`/`memset` instead of `malloc`/bare `realloc`) past its
         logical length. This only pads the underlying allocation —
         `dataSize`/`size` and everything derived from them, and therefore
         the actual on-disk IMG3 format this code produces, are completely
         unaffected; only how much slack exists in memory after the
         logical end changes. Zeroing (not just padding) the extra bytes
         matters too, confirmed by valgrind's *next* complaint once the
         overrun itself was fixed: "Use of uninitialised value" from the
         same AES lookahead reading real-but-never-written pad bytes —
         harmless to correctness (never incorporated into output within
         `dataSize`) but worth silencing properly rather than leaving a
         valgrind-dirty result.
       - **Verified valgrind-clean end to end**, not just "doesn't crash":
         same isolated repro harness (this project's own built
         `libcbpatcher.a`/the fixed `libxpwn.a`/`libwolfssl.a`, a real
         kernelcache fetched from the same real IPSW, the exact key/iv from
         a live run) under `valgrind --error-exitcode=99`: `ERROR SUMMARY: 0
         errors from 0 contexts`, `All heap blocks were freed -- no leaks
         are possible`, exit 0.
     - **A fifth bug, found on the next real `--dry-run` attempt (device in
       DFU, all four bugs above already fixed): `getRealVersion()`'s
       "real firmware version -> iOS-equivalent kernel-signature family"
       bucketing — ported byte-for-byte from the original `Patcher.mm`
       (confirmed directly: identical switch/case, still present, dead code,
       there today) — maps this tool's actual target (10B329a / real
       `ProductVersion` `"6.1.3"`) to `"7.0"`, and `patch_kernel()` finds
       **zero** matching signatures for `"7.0"` against a real AppleTV3,2
       6.1.3 kernelcache (`"[CBPatch] One or more patches not found"`,
       hard failure — this is the return-value check from the previous bug
       correctly catching a real failure, not a regression). Passing the
       raw `productVersion` straight through instead — relying on
       `kernPat()`'s/`kernPatOld()`'s own internal major-version truncation
       (`versionInt < 8 -> versionFloat = (float)versionInt`, `CBPatcher.c`)
       — finds and applies every expected patch (tfp0, the AMFI/memcmp
       bypass, the sandbox policy patch, ...) and reports success, against
       this exact real kernelcache. Most likely explanation, not a claim
       the original bucketing logic was conceptually wrong: this project's
       `libcbpatcher.a` was lost and rebuilt from a third-party source fork
       (see the libcbpatcher/iboot32patcher recovery story elsewhere in this
       file), whose `"7.0"`-family signatures apparently don't exactly match
       what the original 2020 binary's signatures did. `Patcher::
       patchKernel()` now passes `productVersion` directly and no longer
       calls `getRealVersion()` — which is kept, unused, as a preserved
       reference in case the signature drift for the `"7.0"` bucket ever
       gets fixed upstream, not deleted, since it may still be correct for
       firmware families (4.x/5.x/7.x+) this tool has no way to test against.
     - **Also cleaned up real, repetitive CLI noise surfaced by actually
       watching a full real run scroll by**: `Patcher.cpp`'s
       `patchiBSS`/`patchiBEC`/`patchKernel`/`patchRamdisk` each printed a
       raw file-path + hex AES key/IV dump on every single call (no value
       once the wolfSSL/decrypt path is trusted; useful during the original
       key/IV debugging, not for normal operation) — replaced with one
       `"Patching iBSS/iBEC/kernelcache/ramdisk..."` line each. The download
       progress callback (`Cli.cpp`'s `downloadAndPatch`) printed every
       `"NN%"` line as many times as the underlying transfer happened to
       report that same rounded percentage (chunk-driven, not
       percentage-change-driven) — confirmed directly in a real run's
       output (`"KernelCache: 0%"` five times in a row) — fixed by tracking
       the last percentage actually printed. `Blackb0x/Libraries/
       xpwntool.c`'s `decrypt()` (this project's own copy, see its
       provenance note elsewhere in this file) printed a raw `input_path`
       and a decrypt-flag debug line, with a **missing trailing newline**
       on the first one that visibly ran the two together in real output
       (`"input_path kernelcache.release.j33idecrypt 0"`) — both removed.
       `third_party/xpwn` itself has a real leveled-logging facility
       (`XLOG(level, ...)` -> `Log()`, `libxpwn.c`) that `init_libxpwn()`
       defaults to "suppress nothing" (`GlobalLogLevel = 0xFF`, and `Log()`
       suppresses only when `level >= GlobalLogLevel` — a value that high
       passes everything through, not the reverse) — of ~130 `XLOG()` call
       sites across the whole vendored tree, only 3 use level 4 or 5, and
       both of the specific lines cluttering every real run
       (`img3.c`'s raw payload-hash dump, `lzssfile.c`'s LZSS
       compressed/uncompressed-length "match" line) are among them — added
       `libxpwn_loglevel(4)` right after `init_libxpwn()` in `xpwntool.c`'s
       `decrypt()` to silence just those two, leaving the ~127 level-0-3
       calls (genuinely useful progress/status output) untouched. The
       `iBootPatcher`/`CBPatcher` per-instruction trace
       (`patch_boot_args: Entering...`, `find_*: Found ... at 0x...`, etc,
       ~186 plain `printf()` calls across `Blackb0x/Libraries/
       libiboot32patcher/*.c`, no leveled-logging mechanism to hook)
       deliberately left alone — unlike the other noise, each line there
       appears exactly once per patch, not repeated, and is a genuine audit
       trail of what got changed in the boot chain, not debug spam.
   - **The next `--dry-run` (device in DFU, all five bugs above fixed) completed
     fully clean end to end for the first time** — iBSS/iBEC/kernelcache all
     patched successfully, ramdisk built and SSH-granted, no crashes, no
     errors, quiet output. This was the first real confirmation the whole
     download -> decrypt -> patch -> re-encrypt pipeline genuinely works
     against real hardware, not just against isolated repro harnesses.
   - **A sixth bug — and the actual, final blocker — surfaced on the first
     genuine non-`--dry-run` attempt: `checkm8()` itself failed immediately**
     (`"Failed to stall pipe -9."`, right at its very first exploit-setup
     step). checkm8 *deliberately* induces a USB pipe stall as the first
     step of its exploit sequence — a stall here is the expected, correct
     signal, not a failure — so `DeviceManager.cpp`'s
     `usb_req_stall()`/`usb_req_leak()`/`usb_req_no_leak()` call sites check
     their result against `IRECV_E_PIPE`/`IRECV_E_TIMEOUT`
     (libirecovery's own `irecv_error_t` enum, -10/-11). But those three
     helpers — and the handful of raw `irecv_usb_control_transfer()` calls
     later in the same function — all funnel through
     `irecv_usb_control_transfer()`, which is platform-conditional
     (`third_party/libirecovery/src/libirecovery.c`, `#ifdef HAVE_IOKIT`):
     on macOS it calls `iokit_usb_control_transfer()`, which explicitly
     translates IOKit's own status codes into `IRECV_E_*`
     (`kIOUSBPipeStalled -> IRECV_E_PIPE`,
     `kIOReturnTimeout`/`kIOUSBTransactionTimeout -> IRECV_E_TIMEOUT`,
     confirmed by reading that translation table directly) — but on Linux
     it's a bare pass-through to `libusb_control_transfer()`, returning
     libusb's own raw `LIBUSB_ERROR_*` codes completely untranslated
     (`LIBUSB_ERROR_PIPE = -9`, `LIBUSB_ERROR_TIMEOUT = -7` — different
     numbers in a completely unrelated enum). The original `DeviceManager.m`
     checks were correct for the platform it was ever run on (macOS/IOKit);
     porting straight to Linux/libusb silently broke every one of these
     checks, since a real, successful stall now reports as -9 instead of
     -10. Confirmed directly against the real error text in the log above.
     Fixed by changing all six comparisons in `checkm8()`
     (two `usb_req_stall()` sites, three `usb_req_leak()`/`usb_req_no_leak()`
     sites, one raw payload-upload `irecv_usb_control_transfer()` call) from
     `IRECV_E_PIPE`/`IRECV_E_TIMEOUT` to `LIBUSB_ERROR_PIPE`/
     `LIBUSB_ERROR_TIMEOUT` (`<libusb.h>`, already available transitively via
     `deps::usb`) — this is the exact same platform-conditional-translation
     gap, applied consistently everywhere the exploit relies on it, not just
     the one call site that happened to fail first. **Not yet re-verified
     against real hardware** — this fix hasn't had a live retry yet as of
     this writing; the previous five bugs were each confirmed with a real
     device before moving on, and this one should be too before treating it
     as done.
   - **Before that retry: a full pass removing raw debug `printf()` noise
     from `DeviceManager.cpp`, and unifying every remaining user-facing
     message in `Cli.cpp` into one consistent style.** Watching real runs
     scroll by (the five-bug session above, then the real checkm8 attempt)
     surfaced something the isolated repro harnesses never would: the same
     "device connected"/"exploit status" information printed two or three
     times in three different formats (a raw `DeviceManager.cpp` printf, a
     `[bracket-tag]`-prefixed `Cli.cpp` rendering of the *same* event via
     the `DeviceEventSink` callback, sometimes both). `DeviceManager.cpp`
     turned out to carry ~50 leftover `printf()`/ANSI-escape-colored debug
     lines from the original app's own development (`"trying to send
     ibss"`, `"no upload client m8"`, `"sendiBSS_ATV32(%s, %llu)"`, raw
     ECID/CPID/nonce hex dumps, `"\x1b[36mUploading soft DFU\x1b[39m"`,
     `"0x8947 configuration"`, ...) — none of them gated behind any
     verbosity flag, all firing unconditionally alongside the *already
     existing*, clean `DeviceEventSink` (`sink_.onStatus`/`onProgress`/
     `onDeviceAdded`/`onDeviceUpdated`/`onDeviceRemoved`) channel that
     `Cli.cpp` was built to render. Fixed by making `DeviceManager.cpp`
     itself silent on success — every routine status message now flows
     through exactly one path, `sink_`, with real failures going to stderr
     instead of stdout — and having `Cli.cpp` alone own the rendering, in
     one unbracketed style throughout (`"Connected to AppleTV3,2 (2685...)
     in DFU"`, `"Trying checkm8..."`, `"(dry run) Would try checkm8"` — no
     more `[status]`/`[connected]`/`[disconnected]`/`[device]`/`[dry-run]`
     tags, no more restating the same device identity on consecutive
     lines). Also removed while touching this: two genuinely dead
     functions (`print_device_info()`/`print_hex()`, declared and defined
     but never called from anywhere), `AppleTVDevice::printDeviceInfo()`/
     `humanDeviceName()` (both existed only to feed the now-removed
     redundant `"Selected: "` line — `Cli.cpp` had already printed the
     exact same device identity one event earlier via `onDeviceAdded`), and
     a couple of `send*()` functions whose own `sink_.onStatus("Sending
     X...")` calls duplicated `Cli.cpp`'s `sendComponentsToDevice()`
     printing the same thing itself (which also already knows and reports
     the actual Sent/Error outcome, something the removed calls didn't).
     One real, if minor, correctness fix fell out of this along the way:
     `sendiBEC()` previously always returned `0` regardless of whether
     `irecv_send_file()` actually succeeded, so a real transfer failure
     would still print `"Sent"` — now it returns the real result.
   - **`selectDevice()` (`Cli.cpp`) now prints a one-time reminder if
     nothing shows up within 5 seconds of waiting**: `"Still searching...
     Please power-cycle your Apple TV after a failed exploit attempt."` —
     added after a real run needed exactly that (see below): several of
     `checkm8()`'s own early-return paths never call `irecv_reset()`/
     `irecv_close()` before bailing, which can leave a partially-exploited
     device's USB stack wedged until it's fully power-cycled, not just
     re-DFU'd. Fires once per wait (a `std::chrono::steady_clock`
     timestamp + one-shot flag, not a per-poll check), regardless of
     whether waiting on `--ecid`/`--udid`, the sole device, or an
     interactive multi-device pick.
   - **The next real (non-dry-run) attempt — after the power-cycle above —
     got further than ever before: all the way through checkm8's
     "Executing payload" stage** (the actual pwn) **before segfaulting.**
     Root cause, found by reading the exact code path: after running the
     payload, `checkm8()` reconnects to the device to verify the pwn took
     (`client = get_tv(ecid); info = irecv_get_device_info(client); ...
     info->serial_string...`) — but `get_tv(ecid)` can return `nullptr`
     (it does its own ~6-attempt/1s-apart retry internally, then gives up
     and logs `"ERROR: Unable to find device"` to stderr), and neither this
     port nor the original `DeviceManager.m` it was ported verbatim from
     ever checked for that before dereferencing the result — turning "the
     device took a little longer to re-enumerate after running injected
     SecureROM-level code" into a straight null-pointer segfault instead of
     a clean `"Checkm8 unsuccessful"`. Confirmed directly: a real run got
     exactly this far (`"Disconnected (...)"` printed right after
     `"Executing payload"`, then `"ERROR: Unable to find device"`, then
     `segmentation fault`) — this specific reconnect is also the single
     most likely one in the whole function to need extra patience, since
     it's the only one checking on a device that just finished executing
     injected code rather than reconnecting to one already sitting quietly
     in a known DFU/Recovery state. Fixed two ways: (1) every `get_tv()`
     result in `checkm8()` (the initial connect, both mid-sequence
     reconnects, and this final one) is now null-checked before use,
     failing cleanly instead of crashing; (2) this specific final reconnect
     also gets its own extra retry loop (5 more attempts, 1s apart) on top
     of `get_tv()`'s own internal one, since it's the one place in the
     function where "not found yet" genuinely might just mean "give it
     another few seconds," not "the device is gone."
   - **That retry did fix the segfault, but the device genuinely never came
     back this time — not even in `lsusb` after a physical unplug/replug**,
     a strictly worse symptom than the earlier runs (which at least got
     that far and just needed a power-cycle). Prompted a direct
     cross-reference against `gaster`
     (github.com/verygenericname/gaster) — the actual checkm8
     implementation `palera1n` (a real, current, widely-used jailbreak
     tool covering this same device class) delegates to today, not a
     from-scratch implementation of its own. First confirmed the low-level
     exploit itself isn't in question: `gaster`'s own per-chip config table
     (`checkm8_check_usb_device()`) has an entry for `"SRTG:[iBoot-1458.2]"`
     (this exact AppleTV3,2 firmware string) with `cpid=0x8947`,
     `config_large_leak=626`, `config_overwrite_pad=0x660` — byte-identical
     to this file's own `get_exploit_configuration()` for the same cpid.
     What *is* different, and well-evidenced enough to act on: `gaster`'s
     own USB-wait helper (`wait_usb_handle()`) never gives up at all — an
     unbounded loop that just keeps polling until the device reappears —
     and its top-level state machine (`gaster_checkm8()`: RESET -> SETUP ->
     SPRAY -> PATCH) resets and retries the *entire* exploit from scratch,
     also in an unbounded loop, on any stage failure, rather than a
     single-shot linear attempt giving up on the first hiccup. Adopted both
     patterns, in bounded form (the CLI still needs to eventually report a
     real failure rather than hang forever): a new `get_tv_patient()`
     helper (up to 30 one-second attempts, used for every reconnect inside
     the exploit, not just the post-payload one) and a new
     `checkm8Attempt()`/`checkm8()` split — `checkm8Attempt()` is the
     existing single-pass sequence, completely unchanged at the USB
     request/payload/timing level; `checkm8()` now retries it up to 3 times
     on failure instead of giving up on the first one. **Deliberately did
     not touch the actual low-level request sequence itself** — unlike the
     connection-management architecture, that's confirmed correct (see the
     config match above), and rewriting exploit-critical, hardware-timing
     code (this file's own header comment: "ported VERBATIM... not
     'improved' during conversion") on a guess, without being able to test
     it, is a real way to make things worse, not better.
     - **Still unresolved and likely a separate, hardware/environment-level
       problem, not something more software patience can fix**: a device
       that won't even show up in `lsusb` after a physical unplug/replug is
       gone at the kernel/host-controller level, not just slow to
       re-enumerate. This machine has no legacy EHCI controller at all —
       `lspci` shows only a Tiger Lake-LP Thunderbolt 4 controller and a
       500-series xHCI 3.2 controller — and `gaster`/`palera1n`'s own
       documented hardware caveats (AMD hosts having a "very low success
       rate" with checkm8; Apple-Silicon-Mac USB-C ports sometimes needing
       a plain USB hub in between to work around DFU-mode enumeration
       issues) are exactly this class of host-controller-specific quirk,
       just for a different vendor/OS combination — plausible that
       Thunderbolt/xHCI has a similar one here. If a full power-cycle of
       the Apple TV itself (not just a USB replug), a different cable, and
       a different physical port don't help, routing through a plain
       (non-Thunderbolt) USB hub is the next thing to try, per that same
       precedent.
   - **Neither the retry/patience fix above nor this cable/hub/power-cycle
     angle has been re-verified against real hardware yet.**
   - The retry/patience fix was tried against real hardware and **did not
     fix it** — the device still didn't reappear. Prompted a look at
     switching the whole exploit backend to shell out to a proven external
     binary instead, the way `palera1n` itself does, rather than continuing
     to harden this file's own USB state machine blind. Two real
     candidates, and they are *not* the same tool:
     - `palera1n`'s `legacy` branch (the shell-script one, targeting iOS
       15-16 on `A8`-`A11`) shells out to a separate `gaster` binary
       (`"$dir"/gaster pwn`) for the actual DFU pwn step — this is the one
       compared against above, and it's a small, MIT-licensed, fully open
       C source file.
     - `palera1n`'s current `main` branch (the C rewrite everything ships
       from today, including third-party wrappers like
       `roothide/Palera1n-roothide`) does **not** use `gaster` at all —
       `src/exec_checkra1n.c` downloads the real, official, **closed-source**
       `checkra1n` release binary from `assets.checkra.in` at build time
       (`src/Makefile`: `curl -Lfo $@
       https://assets.checkra.in/downloads/preview/$(CHECKRA1N_VERSION)/...`),
       embeds it as a resource blob, and at runtime extracts it to a temp
       file and `posix_spawn`s it directly. The "exploit" in this path is
       the actual checkra1n team's own binary, not something portable or
       auditable — you can shell out to it, but you can't read or adapt its
       exploit logic the way `gaster`'s source allows.
     - **checkra1n cannot be used here at all, for either branch:
       confirmed no A5/`AppleTV3,2` support exists anywhere in that
       tool.** checkra1n's officially supported floor has always been `A7`
       (iPhone 5s) — `A5`/`A6` support (e.g. iPhone 4S) was requested as
       far back as December 2019 (`checkra1n/BugTracker#628`) and was
       never added. `palera1n`'s own README (both branches) states its
       device requirement as `A8`-`A11` on iOS/tvOS 15+; its documented
       Apple TV support is explicitly Apple TV HD (`A8`) and Apple TV 4K
       (`A10X`) only. `Palera1n-roothide` specifically scopes itself even
       further, to `A9`-`A11`/iOS 15-18 only (it drives the roothide
       Dopamine2/TrollStore chain, which needs an iOS15+-era kernel).
       None of this is a missing flag or config option — even if
       checkra1n's raw bootrom-exploit stage could technically still pwn
       an `A5` chip (the SecureROM bug it's built on does span `A5`-`A11`),
       its binary almost certainly hard-rejects unrecognized `cpid`s before
       trying, and its entire downstream chain (PongoOS payload, KPF,
       ramdisk) is compiled against iOS15+/tvOS12+ kernel offsets that
       don't exist for tvOS 7.x/8.4.x. Whatever reliability was observed
       running a `Palera1n-roothide` build against real hardware on this
       same machine, it cannot have exercised this device's exploit path —
       checkra1n would have refused the `AppleTV3,2` outright — so it's not
       usable evidence about this project's specific failure.
     - `gaster`, by contrast, **is confirmed to support this exact chip and
       firmware** — not just "the general `A5` family" by name, but this
       device's literal SRTG build string. `gaster.c`'s
       `checkm8_check_usb_device()` detects the target purely by matching
       the USB serial number's SRTG string against a table, with no
       device-name/product-type allowlist involved at all; the
       `" SRTG:[iBoot-1458.2]"` branch (this exact AppleTV3,2 iBoot) sets
       `cpid = 0x8947` plus a full set of real, non-placeholder gadget
       addresses (`dfu_handle_request`, `usb_core_do_transfer`,
       `payload_dest_armv7`, etc. — the `_armv7` naming, and the fact that
       upstream `0x7ff/gaster` ships dedicated
       `payload_handle_checkm8_request_armv7.S`/`.bin` files alongside the
       `A9`+ ones, both confirm `gaster` has real, deliberate 32-bit/`A5`-
       `A6`-class chip support, not just accidental overlap from an
       ARM64-focused tool). This is the strongest form of confirmation
       available short of running it: source-level, chip-specific, and
       independent of any device-support marketing list.
     - Conclusion: `gaster` remains the only viable "switch to a proven
       external implementation" path for this project. Shelling out to
       official `checkra1n` (either by hand-building it like `palera1n`
       does, or bundling a release) is not an option — it would simply
       refuse the device.
   - **Actually switched over.** The retry/patience fix (`get_tv_patient()`/
     `checkm8Attempt()`/`checkm8()` above) was tried against real hardware
     and did not fix the "device did not reappear" failure — confirming the
     problem was never about this port's own patience/retry architecture.
     `DeviceManager.cpp`'s `checkm8Attempt()` no longer drives the low-level
     USB request sequence in-process at all: it now shells out to the
     vendored `gaster` binary (`third_party/gaster`, a git submodule
     pointing at `verygenericname/gaster` directly — the exact fork
     `palera1n`'s `legacy` branch downloads, unmodified, no fork of our own
     needed) via a new `runGaster()` helper (fork/exec, combined
     stdout+stderr captured and relayed to the user on failure, since
     gaster never writes to stderr itself — everything comes out on
     stdout). `gaster pwn` replaces everything from "Configuring checkm8
     exploit" through "Executing payload" in one call; `checkm8Attempt()`
     still independently reconnects and checks for `"PWND:["` in the serial
     string afterward rather than trusting gaster's exit status alone. On
     failure, `gaster reset` is run for cleanup (matching palera1n's own
     `_pwn()`). `runGaster()` enforces its own timeout (180s for `pwn`, 15s
     for `reset`; SIGTERM then SIGKILL) — necessary because gaster's own
     wait-for-device and pwn-retry loops are genuinely unbounded, and this
     project's CLI still needs to eventually report a real failure instead
     of hanging forever if the device is truly gone. `checkm8()`'s own
     3-attempt outer retry is kept as a secondary safety net, though it's
     expected to matter much less now that gaster's own internal state
     machine already retries unboundedly within a single `pwn` call.
     The old hand-ported exploit code (`usb_req_stall`/`usb_req_leak`/
     `usb_req_no_leak`, `get_exploit_configuration()`/
     `get_payload_configuration()`, the `checkm8_config_t` struct and
     `S518947X_OVERWRITE` macro in `DeviceManager.hpp`, and the
     `#include "checkm8.h"` payload-data header) is deleted outright, not
     kept-but-unused — its exploit parameters were already confirmed
     byte-identical to gaster's own equivalents before this switch, so
     there's nothing left to reference it for.
     - **Build**: `third_party/gaster` is compiled directly by this
       project's own `CMakeLists.txt` (a new `add_executable(gaster ...)`
       target, landing in the same output directory as `blackb0x` itself,
       built automatically as part of the default `all` target), not via
       gaster's own Makefile. The one snag: gaster's `libusb` Makefile
       target links real OpenSSL (`-lcrypto`) purely for the one `EVP_*`
       AES-256-CBC call `gaster_decrypt_file()`/`gaster_decrypt_kbag()` use
       (neither of which `checkm8Attempt()` ever invokes — it only runs
       `pwn`/`reset` — but the symbols still have to resolve at link time
       regardless, single translation unit). Rather than vendor real
       OpenSSL just for that, `gaster.c`'s bare `#include <openssl/evp.h>`
       is redirected through this project's existing wolfSSL
       OpenSSL-compatibility shim instead — the exact same
       `OPENSSL_INCLUDE_DIR`/`-include .../wolfssl/options.h` treatment
       `xpwn`/`dmg` already get, confirmed to work with zero source
       changes to `gaster.c` at all (wolfSSL was already built with
       `--enable-opensslall`). One real bug hit and fixed along the way:
       `xxd -i`'s emitted C variable name is derived from its *input path*,
       not the output filename — passing the full absolute
       `third_party/gaster/payload_A9.bin` path (as originally written)
       produced a mangled name like
       `_var_home_..._third_party_gaster_payload_A9_bin` instead of the
       bare `payload_A9_bin` `gaster.c`'s own `#include "payload_A9.h"`
       expects, which compiled fine but failed at link time with six
       `undefined reference to 'payload_*_bin'` errors. Fixed by running
       `xxd` with `WORKING_DIRECTORY` set to `third_party/gaster` and a
       bare relative filename, matching how gaster's own Makefile invokes
       it.
     - **Not yet verified against real hardware.** Confirmed cleanly:
       `gaster` itself builds, links statically against the same vendored
       libusb/wolfSSL as the rest of this project (`ldd build/gaster` shows
       only libc/libm), and runs (`--help`-equivalent usage banner prints
       correctly with no device attached). `blackb0x` itself still builds
       clean against the rewritten `DeviceManager.cpp` with no new
       warnings, `ldd build/blackb0x` is unchanged, and `--dry-run` still
       works. None of this exercises the actual exploit path, though — the
       real test is still an actual checkm8 attempt against the real
       AppleTV3,2.
   - **First real-hardware test: `gaster pwn` got stuck in an
     uninterruptible kernel USB wait (`D` state) after checkm8 ran and the
     device rebooted.** Investigated live, on the actual stuck process,
     rather than guessing: `ps` showed the `gaster pwn` child in state `Dl+`
     (`D` = uninterruptible sleep — the one process state SIGKILL cannot
     terminate; the kernel only wakes it when whatever blocking call it's
     in returns on its own). `lsusb`/`lsusb -v` run fresh, independently,
     against the exact same device at the exact same time worked fine and
     returned real descriptor data (`idVendor 0x05ac`, `iSerial "Apple
     Mobile Device (DFU Mode)"`) — so this was never "the bus/device is
     dead," specifically gaster's own already-open handle/session was
     stuck on something a brand new libusb session wasn't. This pointed at
     a real, separate bug in `runGaster()` itself, found by inspection
     right after: its post-SIGKILL cleanup called a *blocking*
     `waitpid(pid, &status, 0)`, meaning as soon as a child ever landed in
     `D` state, the intended timeout-and-move-on behavior didn't actually
     work — it just relocated the hang from `gaster` into `blackb0x`
     itself, silently, with no diagnostic at all. Fixed: `runGaster()` now
     gives `SIGTERM`/`SIGKILL` a short *bounded* grace period (polled via
     non-blocking `waitpid(..., WNOHANG)`, never a blocking wait), and if
     the child still hasn't been reaped, checks `/proc/<pid>/status` for
     `D (disk sleep)` and reports that specifically — "stuck in an
     uninterruptible kernel USB wait, can't be killed by software, try
     physically unplugging the Apple TV" — instead of a generic timeout
     message, then moves on regardless (the orphaned child is left to be
     reaped whenever/if its blocking syscall ever actually returns).
   - **`runGaster()` now streams gaster's stdout/stderr straight through to
     blackb0x's own stdout/stderr live, verbatim**, instead of buffering
     silently and only dumping on failure — genuinely useful for exactly
     the class of problem above: a stuck run is something worth watching
     happen in real time, not reconstructing after the fact. Implemented
     with two separate pipes (not one merged one, so gaster's stdout and
     stderr — currently identical, since gaster itself never writes to
     stderr, but not guaranteed to stay that way — map onto blackb0x's own
     correctly) and `poll()` instead of a fixed busy-sleep, since there are
     now two file descriptors to watch instead of one.
   - **`checkm8Attempt()` now checks whether the device is already in
     pwned DFU before ever invoking gaster at all** (a quick, one-shot
     `get_tv()`/`"PWND:["` check, same as the post-pwn verification, just
     run first). Directly motivated by the `D`-state incident above: if
     gaster's pwn actually succeeds and the device reboots into pwned DFU,
     but gaster's own reconnect-and-verify step is what gets stuck
     afterward, the exploit itself already worked — retrying it from
     scratch (this project's own 3-attempt outer retry, or a fresh CLI
     invocation after killing a stuck one) would otherwise re-run the whole
     spray/patch sequence pointlessly against an already-pwned device
     instead of just noticing it's done and moving on.
   - **Checked whether gaster's cpid 0x8947 configuration actually matches
     the original (`DeviceManager.m`) checkm8 implementation this whole
     project is ported from** — it does not, but not because of a mistake:
     they're two independently-written, fundamentally different
     *techniques* for exploiting the same checkm8 bootrom bug. The
     original's `case 0x8947:` (`DeviceManager.m` ~line 554) sets exactly
     two numbers — `large_leak`/`overwrite_offset` — for the classic
     "spray heap holes, overwrite a fixed byte pattern at a computed
     offset, execute an uploaded payload blob" approach (this is what
     `checkm8Attempt()` itself did before the switch to gaster, and those
     two numbers were already confirmed byte-identical to gaster's own
     `config_large_leak`/`config_overwrite_pad` for this same cpid).
     gaster's own `checkm8_check_usb_device()` entry for `0x8947` sets a
     completely different kind of data instead — hardcoded *gadget
     addresses within this specific iBoot build* (`dfu_handle_request`,
     `usb_core_do_transfer`, `aes_crypto_cmd`, `memcpy_addr`,
     `payload_dest_armv7`, etc.) — because gaster's technique for
     non-A9-class armv7 chips (confirmed via `gaster.c`'s own dispatch
     logic: cpid `0x8947` falls through to the generic
     `payload_notA9_armv7_bin` + `payload_handle_checkm8_request_armv7_bin`
     path, not a per-chip special case) is to directly patch live iBoot
     function pointers at those addresses, not spray-and-overwrite. Neither
     approach is "the config"; they're not directly comparable
     number-for-number beyond the two that already matched.
     **More relevant than the config comparison**: this project's own
     original port of the classic spray-and-overwrite approach — before
     gaster ever entered the picture — hit the *exact same class of
     symptom* (device not reappearing cleanly after "Executing payload")
     that prompted the whole gaster investigation in the first place. That
     was never resolved by patience/retry logic either. Combined with the
     live `D`-state evidence (a kernel-level USB syscall not returning,
     not a userspace logic bug in either implementation), the more likely
     explanation is a Linux kernel/host-controller-level issue with how
     this specific machine's USB stack handles a device that resets itself
     off the bus mid-transaction (exactly what SecureROM payload execution
     does) — independent of which checkm8 implementation triggers it. This
     doesn't rule out a gaster-specific bug, but it means switching
     exploit backends again would not obviously fix this class of failure
     if it's truly host-controller-level.
     **Still unresolved, still not confirmed working end-to-end.**
   - **The "live streaming" fix above was actually blind the whole time —
     found on the very next real attempt.** A fresh run with all the fixes
     above showed `"Exploiting with checkm8"` → `"Disconnected (...)"` →
     `"checkm8: gaster pwn timed out waiting for the device."` with *zero*
     gaster output in between, timeout to timeout, despite `runGaster()`
     streaming its pipes live. Root cause: glibc's stdio only line-buffers
     stdout/stderr when attached to a terminal (`isatty()`); attached to a
     pipe — exactly what `runGaster()` does — it silently switches to full
     block buffering instead, and `gaster.c` never calls
     `setvbuf()`/`fflush()` itself. A short-lived invocation (no args,
     prints usage, exits) looks fine either way since `exit()` flushes
     stdio regardless — confirmed directly, this is exactly why it wasn't
     caught in the earlier "does gaster run" sanity check — but a `gaster
     pwn` that runs a long time before ever exiting (precisely the "stuck"
     case this whole timeout/D-state mechanism exists for) never flushes
     its `"Stage: RESET"`/`"Stage: SETUP"`/etc progress lines to the pipe
     at all. Fixed by prefixing the exec with `stdbuf -oL -eL` (GNU
     coreutils; `LD_PRELOAD`s a constructor that calls `setvbuf()` before
     `gaster`'s own `main()` runs — confirmed directly: without it, a test
     run's output only appeared after the process exited; with it, output
     — including a `"[libusb] Waiting for the USB handle with VID: 0x5AC,
     PID: 0x1227"` line never seen before — appeared within 0.5s), with a
     fallback to exec'ing `gaster` directly if `stdbuf` isn't installed.
     This means every previous real-hardware attempt in this file's own
     history was diagnosed without ever actually seeing which of
     `gaster_checkm8()`'s four stages (RESET/SETUP/SPRAY/PATCH) it was
     stuck in — the D-state hang could have been happening before the
     exploit payload ever ran, not just after, and there was no way to
     tell. The next real attempt is the first one that will actually show
     this.
   - **Conclusion, with the buffering fix's real visibility: this is a
     host-machine/USB-controller-level limitation, not an exploit bug in
     either implementation, and not fixable by more software patience.**
     A real run's live stage output (`RESET`/`SETUP`/`SPRAY`/`PATCH`)
     showed the exploit payload itself genuinely executing successfully
     (`Stage: PATCH` / `ret: true` — the furthest ever confirmed, on either
     implementation) — but the *reconnect* immediately after read the
     device as not-yet-pwned, then the next stage (a mundane
     "re-prime the device into a known DFU state" step, not exploit code)
     failed outright, and reconnecting after *that* hung again.
     Confirmed directly on repeat attempts: which stage it hangs after is
     inconsistent — sometimes `SETUP` (before any exploit code has run at
     all), sometimes `PATCH` — meaning the failure isn't tied to anything
     the exploit payload does; it's this host's USB stack unreliably
     handling the reset-close-reopen cycle gaster performs after *every*
     stage transition, regardless of which stage triggered it. Combined
     with the earlier confirmed genuine kernel-level `D`-state hang (not a
     userspace logic bug) and this machine's all-Thunderbolt/xHCI USB
     topology (no legacy EHCI at all, confirmed via `lspci`), this is
     conclusive enough: two independent, differently-implemented checkm8
     exploits both hit the identical failure class at the identical
     conceptual boundary (device-initiated USB reset → reconnect), at
     genuinely random points in the sequence. No further software fix
     inside this project — more retry patience, a different exploit
     backend, deeper protocol changes — is expected to help; the retries
     already happen (gaster's own loop is unbounded and self-healing) and
     still don't reliably converge. **Tried the one cheap mitigation this
     pointed at — routing through a plain, non-Thunderbolt USB hub — twice,
     with two structurally different hubs (see the "Known machine" entry in
     "Environment notes" above for both hardware probes: a Microchip
     USB2807/USB5807-based Dell dock, and a Terminus/VIA Labs plain hub) —
     and it did not help either time.** With direct connection, Thunderbolt
     routing, and two different hub topologies all hitting the identical
     device-reset/reconnect failure, this is conclusively a real limitation
     of this specific machine's USB controller/driver stack for
     checkm8-class work, not a topology quirk a hub can route around. The
     practical fix is running `blackb0x` from different hardware entirely —
     ideally one with a legacy EHCI controller or a known-good xHCI
     implementation for this kind of workload — not further changes to this
     codebase.
   - **SUPERSEDED — the above was a single-machine conclusion; a real,
     fixable software cause was found once the same hang reproduced
     identically across multiple, unrelated Linux machines.** Running the
     vendored `gaster` binary directly (`sudo ./build/gaster pwn`, bypassing
     `blackb0x`/`runGaster()` entirely) hung again — this time captured live
     with `dmesg -w` running in a second terminal at the exact moment it
     froze, right after `Stage: SETUP` / `ret: true`, before the next
     stage's own `[libusb] Waiting for the USB handle...` line ever
     printed. The kernel log showed the real cause directly, not inferred:
     ```
     apple-mfi-fastcharge 3-3: usbfs: process 402495 (gaster) did not claim interface 0 before use
     apple-mfi-fastcharge 3-3: reset high-speed USB device number 29 using xhci_hcd
     ```
     followed by a disconnect/reconnect storm with garbled descriptors
     (`Product: Љ`) and `-71`/`-110`/`-75` USB errors. `apple_mfi_fastcharge`
     is a real, in-tree, commonly-autoloaded Linux driver (`drivers/usb/misc/
     apple-mfi-fastcharge.c`, upstream author Bastien Nocera) for Lightning
     fast-charge negotiation — confirmed present and loaded
     (`modinfo apple_mfi_fastcharge`) with alias
     `usb:v05ACp*d*dc*dsc*dp*ic*isc*ip*in*`. A real upstream fix,
     [`USB: apple-mfi-fastcharge: don't probe unhandled devices`](https://lkml.iu.edu/hypermail/linux/kernel/2011.0/03254.html),
     added a `mfi_fc_match()` check restricting probing to product IDs
     `0x1200`-`0x12ff` — but this device's real DFU-mode PID is `0x1227`
     and its normal-mode PID is `0x12a7`, both squarely inside that
     "fixed" range. So even a fully up-to-date, upstream-patched kernel
     still auto-binds this driver to the Apple TV whether it's in DFU mode
     or not. `gaster` never calls `libusb_claim_interface()` (or any
     kernel-driver-detach equivalent) — it talks straight to the default
     control pipe via raw `libusb_control_transfer()` calls — so this
     driver stays attached the whole time and independently calls its own
     `usb_reset_device()` whenever gaster's raw, exploit-timing-sensitive
     transfers confuse it. Two independent actors (gaster's own
     `reset_usb_handle()` calls after every stage, and this driver's own
     recovery-triggered resets) issuing USB resets against the same device
     at once is exactly the kind of race that corrupts enumeration and can
     wedge the xHCI command ring hard enough to produce the previously
     observed D-state hangs — this is very likely the same underlying
     mechanism as the "device-initiated reset" theory above, just with the
     actual second actor identified instead of blamed on the host
     controller itself. Explains the multi-machine reproducibility
     directly: this module ships in essentially every mainstream
     distro kernel, unrelated to any one machine's chipset/topology —
     the earlier single-machine hub/Thunderbolt-routing investigation
     never had a chance to rule this out, since nothing on that machine's
     side varied it.
     - **Not yet re-verified against real hardware.** The fastest
       available manual confirmation
       (`sudo modprobe -r apple_mfi_fastcharge && sudo ./build/gaster pwn`,
       safe since the module's refcnt was 0 — nothing else on the test
       machine was actively using it) had not been reported back as of this
       writing.
     - **Two code-based fixes were tried in `DeviceManager.cpp` and then
       both abandoned, per explicit user direction, in favor of a
       documented manual step instead — the same pattern already used for
       `usbmuxd --no-preflight`.** Worth keeping the trail since the
       reasoning that ruled each one out is real, even though none of this
       code exists in the tree anymore:
       - **First attempt (wrong): a bare `modprobe -r` up front, nothing
         else, in a small RAII guard scoped around `checkm8Attempt()`.**
         Tried against real hardware — the module came right back on its
         own before the exploit got anywhere. Root cause: every one of
         gaster's stage-transition reconnects fires the kernel's own
         module-autoload path (`request_module()`, driven by the
         re-enumerating device's USB modalias — the same mechanism whether
         the kernel invokes `modprobe` directly via a usermode helper or
         udev does it on the kernel's behalf), completely independently of
         anything either blackb0x or gaster does in userspace. Removing an
         already-loaded instance once does nothing to stop the very next
         reconnect from loading it straight back in — a single sysfs
         interface-unbind *before* invoking gaster was also considered and
         rejected for the identical reason (gaster's own internal
         RESET→SETUP→SPRAY→PATCH state machine reconnects several times
         *inside one `gaster pwn` call*, with no way for code outside that
         subprocess to repeat a per-stage unbind in between).
       - **Second attempt (also abandoned): the RAII guard writing a
         temporary `/etc/modprobe.d` `blacklist` entry itself** (removed
         on construction along with an already-loaded, currently-unused
         instance; both restored in the destructor on every exit path). A
         `blacklist` directive is the right *mechanism* — `modprobe`/
         `libkmod` honor it specifically for *automatic*, alias-triggered
         loads while still allowing an *explicit* `modprobe
         apple_mfi_fastcharge` to work — but having `blackb0x` itself
         silently write and delete a system-wide `/etc/modprobe.d` file at
         runtime was the wrong place to put that mechanism: not
         signal-safe (a hard `SIGKILL`, or an uncaught `SIGINT`, skips the
         destructor and can leave the blacklist file and/or the removed
         module behind), and inconsistent with how this exact class of
         problem is already handled elsewhere in this project.
       - **Final decision: document it as a one-time manual setup step,
         exactly like `usbmuxd --no-preflight`, and add no code at all.**
         `blackb0x` never checks for or manages the daemon flag that
         `usbmuxd --no-preflight` requires either — that's a documented
         README/AGENTS.md prerequisite the user sets up once, not
         something enforced at runtime — and this driver conflict is the
         same shape of problem: a system-level configuration issue, not
         something a single exploit run should be silently patching system
         state to work around. README.md gained a new "One-time system
         setup: blacklist `apple_mfi_fastcharge`" section (mirroring the
         `usbmuxd` one immediately below it) with the exact
         `/etc/modprobe.d/blacklist-apple-mfi-fastcharge.conf` +
         `modprobe -r` commands; AGENTS.md's runtime-requirements list and
         "Current status" section were updated to match.
     - **Tested against real hardware with the module actually blacklisted
       — confirmed real, confirmed insufficient on its own.** `lsmod`
       during the test showed `apple_mfi_fastcharge` genuinely not
       loaded (the blacklist worked as designed — no reload, unlike the
       bare-`modprobe -r` attempt above), and no `gaster` process was left
       hung afterward. **The identical corruption still happened anyway**:
       a fresh `sudo journalctl -k` capture during the same `sudo
       ./build/gaster pwn` run showed the exact same
       "usbfs: process ... (gaster) did not claim interface 0 before use"
       → "reset high-speed USB device ... using xhci_hcd" pair (twice,
       once per completed stage), followed by the same disconnect/garbled-
       re-enumeration storm (`error -90`, `config 1 has 0 interfaces`,
       `can't set config #1, error -110`) — except this time every one of
       those kernel log lines is attributed to the generic `usb` bus name
       instead of `apple-mfi-fastcharge`, direct confirmation that no
       competing driver is involved anymore. `gaster` hung again at the
       same point (`Stage: SETUP` / `ret: true`, then `wait_usb_handle()`
       for `SPRAY` never finding the device again), and the device was
       left sitting in the same corrupted state observed live afterward
       (`lsusb -v -d 05ac:1227`: `iManufacturer`/`iProduct` still read
       garbled two-byte strings, `Couldn't open device`) — it never
       actually recovered on its own, matching the "needs a physical
       unplug/replug" recovery already documented above.
       **Conclusion: the `apple_mfi_fastcharge` conflict was real, and the
       blacklist genuinely fixes that specific conflict, but it was never
       the (sole) root cause of this hang.** With it conclusively ruled
       out, the corruption is happening purely between `gaster`'s own
       control-transfer usage (interface-recipient requests without ever
       claiming the interface — triggering the "did not claim" warning on
       its own, independent of any driver) and its own unconditional
       post-stage `reset_usb_handle()`/`libusb_reset_device()` call
       colliding with however this host's USB core/xHCI driver
       reinitializes the device afterward. This is genuinely the same
       failure class the very first (pre-`apple_mfi_fastcharge`,
       single-machine) investigation described — that conclusion was
       correct as far as it went; the `apple_mfi_fastcharge` finding was a
       real, additive discovery (worth keeping — it's one less variable in
       the way) but not the fix for the underlying problem.
     - **`gaster` forked to test the host-side-reset theory directly.**
       Per this project's own convention (fork a vendored dependency rather
       than carry a local patch — see "Vendored dependencies" in
       `AGENTS.md`), forked to **regulad/gaster**, `linux-reset-race`
       branch, off the exact commit already pinned
       (`d4b98423ee92935ba04646ea41a1ed74a27202a1`). `.gitmodules`'s
       `third_party/gaster` entry now points at that fork/branch instead of
       `verygenericname/gaster` directly.
       - **The actual change**: `gaster_checkm8()`'s state machine
         (`RESET → SETUP → SPRAY → PATCH`) calls the same
         `reset_usb_handle()` (a host-triggered `libusb_reset_device()`)
         unconditionally after every single stage, win or lose. Reading
         each stage's own ending sequence shows this isn't uniformly
         necessary: `checkm8_stage_reset()` and `checkm8_stage_patch()`
         both explicitly put the device into `DFU_STATE_MANIFEST_WAIT_RESET`
         first (via `dfu_set_state_wait_reset()`/`dfu_check_status()`), a
         real DFU-protocol state that genuinely requires a bus reset to
         advance out of — for those two, the reset is doing necessary work.
         `checkm8_stage_setup()` and `checkm8_stage_spray()` are pure
         host-side control-transfer races (heap-hole timing tricks) whose
         own last request is a plain STALL probe / `DFU_CLR_STATUS` — no
         protocol-mandated reset-wait state at all. The corruption observed
         above happened at exactly this boundary: right after
         `Stage: SETUP` / `ret: true`, before `SPRAY`'s own
         `wait_usb_handle()` ever found the device again. The fork adds a
         `completed_stage` variable (capturing which stage just ran, before
         the existing code advances `stage` to the next one) and skips the
         `reset_usb_handle()` call specifically when the just-completed
         stage was `SETUP` or `SPRAY` *and* it succeeded — a failed stage
         still falls back to `STAGE_RESET` and still gets the real reset,
         unchanged from upstream, since that recovery path genuinely wants
         a clean device state before retrying from scratch. A 26-line diff
         against upstream, touching only the state machine's control flow —
         no change to any of the actual exploit request sequence, payload,
         or timing values themselves, per this file's own repeated caution
         about not touching verified-correct exploit-critical code.
       - **Compiles clean**: built standalone with the exact `xxd -iC`
         payload-header generation `CMakeLists.txt` uses (same working
         directory + bare relative filename convention, to get the same
         `payload_*_bin` variable names `gaster.c` expects) and `gcc -c`
         against the already-built `build/deps/include` tree, matching the
         real `add_executable(gaster ...)` target's own include/define
         flags (`-DHAVE_LIBUSB`, wolfSSL's `options.h` force-included, the
         same `libusb-1.0` include path).
     - **Second fix on the same fork/branch, same real root cause the
       `apple_mfi_fastcharge` investigation surfaced but only partly
       addressed: `gaster` never actually claims the USB interface.**
       Every DFU class request this file sends (`bmRequestType` `0x21`,
       recipient = interface) went out over usbfs with interface 0
       unclaimed — this, not any specific competing driver, is what the
       kernel's own "usbfs: process ... (gaster) did not claim interface 0
       before use" warning was reporting, on every single real-hardware
       capture in this whole investigation, `apple_mfi_fastcharge` bound
       or not. `wait_usb_handle()` now calls
       `libusb_set_auto_detach_kernel_driver(handle->device, 1)` right
       after opening the device (Linux-only, a documented no-op on other
       platforms) and then `libusb_claim_interface(handle->device, 0)`
       before running `usb_check_cb`/returning the handle; the interface
       is released again on both the "wrong device" retry path and in
       `close_usb_handle()`. Two effects: proper libusb usage (interface
       claimed for every request that needs it, matching normal libusb
       API contract instead of relying on usbfs's permissive-but-warned
       unclaimed-request fallback), and — via
       `libusb_set_auto_detach_kernel_driver()` — automatic detach/
       reattach of any kernel driver bound to that interface (exactly
       `apple_mfi_fastcharge`'s case) scoped to precisely the claim/
       release window, at the code level, rather than depending solely on
       the driver being blacklisted system-wide ahead of time. The
       existing README/AGENTS.md blacklist documentation is left in place
       rather than removed on the strength of this alone — auto-detach
       should make it redundant in theory, but that's exactly the kind of
       claim this file's own history says not to trust without a real
       test.
     - **Compiles and links clean** (rebuilt standalone the same way as
       the previous fix, same flags), still fully statically linked
       (`ldd`: only `libc`/`libm`/`ld-linux`).
     - **Tested against real hardware with both fixes together — the
       `SETUP`/`SPRAY` reset-skip is confirmed wrong, and reverted.** This
       run showed neither the earlier corruption nor any kernel log noise
       at all after `Stage: SETUP` / `ret: true` — just silence, and
       `gaster` hanging in the same place. Direct proof it was a genuine
       kernel-level hang, not a userspace retry loop: `ps` showed `Dl+`
       (confirmed `D`, uninterruptible), and — since I had shell access to
       the same physical test machine for this investigation —
       `sudo cat /proc/<pid>/stack` gave an exact kernel stack instead of
       another guess:
       ```
       usb_start_wait_urb
       usb_control_msg
       usb_reset_configuration
       usbdev_do_ioctl
       __x64_sys_ioctl
       ```
       `usb_reset_configuration()` is the kernel-side handler for
       `USBDEVFS_SETCONFIGURATION` — i.e. this is `libusb_set_configuration()`
       inside the *next* `wait_usb_handle()` call (for `SPRAY`), blocked
       forever on a `SET_CONFIGURATION` control URB that never completes.
       `lsusb -v` at the same moment showed clean, uncorrupted descriptors
       this time (unlike the earlier `apple_mfi_fastcharge`-era runs) —
       the device is sitting there looking fine at the descriptor-cache
       level, but its actual USB peripheral controller has stopped
       responding to this specific control request.
       **Conclusion: the post-`SETUP` reset was never merely
       precautionary DFU-protocol cleanup — it's load-bearing at the
       hardware level.** `checkm8_stage_setup()`'s technique (aborting
       async control transfers mid-flight via `libusb_cancel_transfer()`
       to manipulate heap timing) appears to leave the device's own USB
       peripheral hardware unresponsive to further standard requests
       until a real bus reset clears it — independent of any DFU protocol
       state, which is exactly the angle the original reasoning for this
       fix missed. Reverted the reset-skip entirely (`gaster_checkm8()`
       back to upstream's unconditional post-stage `reset_usb_handle()`
       call for every stage); the interface-claim/auto-detach fix from
       the previous commit stays, since it's independently justified.
     - **Tested against real hardware in this reverted (claim-fix-only)
       form — one real, permanent improvement confirmed, the core
       reconnect corruption still not fixed.** The improvement:
       `/proc/<pid>/stack` this time showed
       `hrtimer_nanosleep`/`common_nsleep`/`__x64_sys_clock_nanosleep` —
       the ordinary `sleep_ms()` between `wait_usb_handle()`'s poll
       attempts, not a kernel-level wedge. The interface-claim fix
       genuinely eliminated the unkillable `D`-state hang; this run
       livelocked in an ordinary retry loop instead, which at least means
       the process itself stays responsive. The core problem persisted
       anyway: `dmesg` showed the identical corruption class as the very
       first (pre-any-fix) capture — two resets against the same device
       number right after `RESET` and `SETUP` complete, then 10 seconds
       later a real `"device firmware changed"` + disconnect, then a
       cascade of `error -110`/`error -75`/"config 1 has 0 interfaces"
       re-enumeration failures. **Confirmed it never recovers on its
       own** — left to run, it stayed in this state rather than
       eventually settling into a clean reconnect.
     - **Conclusion: three independent, real, confirmed software causes
       have now each been ruled out one at a time
       (`apple_mfi_fastcharge`, the SETUP/SPRAY reset-skip theory, and
       unclaimed interfaces), and the identical corrupted-reconnect
       symptom survived the elimination of every one of them.** Two of
       the three fixes are real, permanent improvements worth keeping
       regardless (no more competing kernel driver, no more unkillable
       `D`-state hang, no more "did not claim interface" warnings) — but
       none of them, together or separately, fixed the actual exploit
       hang. This converges strongly on the same conclusion the very
       first single-machine investigation reached, now corroborated by
       controlled elimination of every specific software mechanism this
       session could identify and test, on top of the original report
       that this exact symptom reproduces across multiple different
       Linux machines: **this looks like a genuine host-side (kernel/
       xHCI) limitation reinitializing this specific device after a
       reset performed mid-exploit, not a userspace software bug in
       either `gaster` or this project.** Further blind changes to
       `checkm8_stage_setup()`/`checkm8_stage_spray()`'s own request
       timing are not recommended without a new, specific mechanism to
       test — this file's own repeated lesson about not guessing at
       verified-correct exploit-critical code applies in full now that
       three good-faith guesses have each been individually disproven
       rather than confirmed.
     - **A real bug in the interface-claim fix itself, found by directly
       validating the two things actually in question rather than
       accepting the "host limitation" conclusion at face value**: (1)
       whether libusb's Linux sysfs-based device matching (this project
       builds libusb with `--disable-udev`, falling back to the
       `linux_netlink.c` backend for hotplug — see `CMakeLists.txt`'s own
       comment) actually finds this exact VID/PID pair, and (2) whether
       the device genuinely still exists on the bus at the moment
       `gaster` is stuck. Verified both directly: wrote a standalone probe
       program (built, run, and deleted — not part of the permanent
       tree), linked against the exact same static `libusb-1.0.a` this
       project already builds, calling the identical
       `libusb_open_device_with_vid_pid()` sequence `wait_usb_handle()`
       uses. Run live, with shell access to the same physical test
       machine, **while `gaster` was actively stuck waiting**: the probe
       found and opened the device on the very first call, and
       `libusb_get_configuration()`/`libusb_set_configuration(1)` (the
       exact call previously seen wedged in `D` state) both succeeded
       immediately. This directly confirms sysfs-based matching works
       correctly and the device is genuinely present and at least
       partially responsive — ruling out "libusb can't find/open the
       device" as an explanation.
       - `libusb_claim_interface(handle->device, 0)`, however, failed
         with `LIBUSB_ERROR_INVALID_PARAM` — libusb only returns that
         when the interface number doesn't exist in the device's cached
         config descriptor. `lsusb -v -d 05ac:1227` at the same moment
         confirmed why: a genuinely truncated configuration descriptor
         (`bLength 9`, `wTotalLength 0x0019`, `bNumInterfaces 0` —
         missing its interface descriptor entirely), stable and
         unchanged across several minutes of no exploit activity at all,
         plus empty/unreadable string descriptors. Real, persistent
         device-side descriptor corruption following `SETUP`'s heap-race
         technique, not a momentary glitch.
       - **The actual bug**: the previous commit's `wait_usb_handle()`
         *gated* success on `libusb_claim_interface()` succeeding — but
         upstream `gaster` never claims the interface at all, so it never
         had this dependency. With the config descriptor genuinely
         showing zero interfaces, the claim fails every single time,
         which meant `wait_usb_handle()` closed the handle and retried
         *without ever reaching `usb_check_cb()`* (i.e.
         `checkm8_check_usb_device()`'s own serial-string/cpid check) at
         all — turning a possibly-transient, exploit-related descriptor
         state into an unconditional, permanent "device not found," on
         top of whatever the string-descriptor corruption would have
         caused on its own.
       - **Fixed**: `wait_usb_handle()` now calls
         `libusb_get_active_config_descriptor()` and only attempts
         `libusb_claim_interface()` when `bNumInterfaces > 0`; either way
         it proceeds to `usb_check_cb()` regardless of whether the claim
         happened, matching upstream's original permissiveness for
         exactly the case where claiming isn't possible. Every DFU class
         request this file sends was, is, and remains interface-recipient
         (`bmRequestType` `0x21`) regardless of whether the claim
         succeeds — claiming when possible only fixes the kernel's own
         "did not claim interface 0" warning and enables auto-detach for
         a conflicting kernel driver; it was never required for those
         requests to actually go out. `usb_handle_t` gained an
         `interface_claimed` flag so `close_usb_handle()` only releases
         what was actually claimed (calling
         `libusb_release_interface()` on an unclaimed interface is a
         guaranteed error, not a harmless no-op).
       - **Compiles and links clean**, still fully statically linked.
         **Not yet re-verified against real hardware** — the actual test
         is whether `checkm8_check_usb_device()` now gets a chance to run
         against the still-corrupted-descriptor device, and if so,
         whether its own string-descriptor read succeeds or fails for
         the same underlying reason. If it fails too, that would show
         this exact corruption (not just the interface-claim gate) is
         what's actually blocking progress in every variant tried so
         far, upstream included — a materially different, narrower
         conclusion than "host-controller limitation."
5. **Phase 7 — packaging.** The end goal is deliberately minimal: clone the repo
   (with binary assets), build the static executable, run it. Resource-path
   resolution for `Blackb0x/Files/*` and a README rewrite (the CLI's
   self-identification banner is `"blackb0x (regulad's linux port)"` — update the
   README to match, don't reintroduce the old `"Blackb0x (Linux port)"` phrasing)
   are the only real remaining work. **No udev rules, no install rules** — the tool
   runs entirely as root by design (see "Decisions already made" above), so there's
   no non-root USB permission story to build, and no reason to install it anywhere
   other than wherever it was built. **The README must also document the
   `usbmuxd --no-preflight` systemd drop-in** (see the "libimobiledevice was
   forked" bullet above for why it's required) as an explicit setup step — without
   it, this hardware's normal-mode discovery silently never fires, with no error
   surfaced anywhere in `blackb0x` itself to point a user at the actual cause.

**Hardware-gated checkpoints** (cannot be verified without a physical Apple TV 2/3 in
DFU/Normal mode over real USB): device enumeration via `libirecovery`, the actual
`checkm8`/`SHAtter`/`send*` exploit functions, and the full end-to-end jailbreak/
tether-boot flow. Everything else — dependency builds, networking, plist/DMG parsing,
CLI plumbing — can and should be verified on the Linux dev box with no device attached.
**The only physical unit available for any of this is an AppleTV3,2** — every
"verified against real hardware" claim elsewhere in this file is against that one
model (`checkm8`, the wolfSSL/SSLv3 lockdownd handshake, the real-usbmuxd/
`--no-preflight` path, AFC). `AppleTV2,1` (SHAtter) and `AppleTV3,1` (checkm8 via
external Arduino hardware, see `checkExploit()` in `Cli.cpp`) are implemented from
source/protocol analysis and the original app's own logic, not from a real device —
treat those two paths as unverified until someone tests on the actual hardware.

## Entrypoint injection point: backstepped from `/etc/rc.boot` to `/sbin/launchd`

The original (pre-rewrite) tool always spliced its PID-1 replacement into
`/sbin/launchd` (see `.claude/LEGACY_FLOW.md`'s Stage 1). Mid-rewrite, real
disassembly of an AppleTV2,1 10B809 `RestoreRamdisk` showed `/etc/rc.boot`
itself is LC_MAIN-entered directly by the kernel on that firmware — not
merely a preamble before `launchd` — so `bakeRamdisk()`'s
`spliceFileContentInPlace()` call was switched to target `/etc/rc.boot`
instead, on the theory that injecting at the true first entry point is
strictly better than injecting at `launchd`.

That assumption didn't generalize. A real bake against AppleTV3,1/AppleTV3,2
12H606 failed with `/etc/rc.boot does not exist on the mounted volume`.
Mounting that firmware's actual ramdisk and inspecting it directly (not
guessing) showed `/etc/` on that build is nearly empty (just empty `group`
and `master.passwd` files) — no `rc.boot` at all — while `/sbin/launchd`
does exist. So the 10B809 finding was real but firmware-specific, not a
property of these restore ramdisks in general: at least one later firmware
generation dropped `rc.boot` as a kernel-invoked stage entirely.

Reverted to always targeting `/sbin/launchd` universally — the same choice
the original tool made, and the one thing guaranteed to exist and be real
PID-1 across every firmware generation, rather than trying to detect or
special-case which injection point a given firmware uses. The ad-hoc
signing identity changed to match: `com.apple.launchd` (was `com.apple.rc`,
matching `/etc/rc.boot`'s own CodeDirectory identifier — no longer
applicable now that the splice target is `launchd` again).

## Reopening macOS support: root cause on `--allow-multiple-definition`, and gaster's own IOKit path

A long real-hardware session against AppleTV3,2 (ECID 2685369898254) kept
surfacing timing-dependent libusb/DFU-state races in `DeviceManager.cpp`'s
`boot_client()` (the soft-DFU iBSS uploader) — most recently, an
intermittent "boots into the real OS instead of continuing the exploit
chain" failure where neither always resetting the USB bus after the final
control transfer nor never resetting it were consistently correct (see
`boot_client()`'s own inline comment for the full account; the
state-conditional fix landed — reset only when the device's own GETSTATUS
reports DFU state 8/`dfuMANIFEST-WAIT-RESET` — is a real, spec-grounded fix
but wasn't confirmed to fully close out the intermittent failure by the time
this section was written). Rather than keep chasing what may be inherent
libusb-on-Linux timing behavior, `.claude/TODO.md`'s item 4 (macOS support,
previously scoped-but-parked) was reopened as the more promising direction:
run the real chain against gaster's and libimobiledevice's native,
first-party macOS frameworks instead of a Linux libusb path that's already
shown itself to be fragile against this exact device.

Two pieces of real, verified work came out of picking this back up (both
build clean on Linux — this session had no Mac to actually compile/run the
Apple-only branches on):

1. **The `-Wl,--allow-multiple-definition` link flag was a workaround for a
   real, understood conflict, not an unavoidable one.** `libusbmuxd`'s
   `common/collection.c` and `libimobiledevice-glue`'s `src/collection.c`
   both vendor an externally-linked copy of the same generic collection
   ADT. Diffed directly (not assumed): identical `struct collection`
   layout, byte-identical function bodies for every symbol libusbmuxd
   defines; glue's copy is a strict superset, adding one extra function
   (`collection_copy()`) that libusbmuxd's own code never calls. Since
   nothing depends on libusbmuxd's specific copy surviving, `libusbmuxd_ext`
   (`CMakeLists.txt`) now runs `ar d <installed libusbmuxd.a> collection.o`
   as an extra `INSTALL_COMMAND` step right after `make install`, so only
   glue's copy of the object ever reaches final link. This removes the
   duplicate-definition conflict at its source instead of suppressing the
   linker's complaint about it — confirmed the Linux build still links
   clean with the GNU-ld-only flag removed entirely. That flag had no
   Apple-linker equivalent as of Xcode 15+ (the old `-multiply_defined
   suppress` synonym is gone from the new linker), so this was a real
   blocker for macOS, not just untidy — now it's simply gone, on every
   platform.
2. **gaster already ships a real, upstream-maintained IOKit implementation
   — the build just never selected it.** `third_party/gaster/gaster.c`'s
   top-of-file includes are `#ifdef HAVE_LIBUSB` / `#else`: the `#else`
   branch pulls in `<CommonCrypto/CommonCrypto.h>` and
   `<IOKit/usb/IOUSBLib.h>` directly, no vendored dependency needed at all
   (both are always-present system frameworks on any Mac). This project's
   `CMakeLists.txt` was defining `HAVE_LIBUSB` unconditionally for the
   `gaster` target regardless of host OS, which — per the still-open TODO
   item this reopening started from — meant even a macOS build would have
   inherited whatever libusb's Darwin backend does, not gaster upstream's
   own native path. Now gated `if(APPLE) ... else() ... endif()`: Apple
   builds skip `HAVE_LIBUSB`/libusb/wolfSSL entirely and link `-framework
   CoreFoundation -framework IOKit`; Linux keeps the existing libusb+wolfSSL
   build byte-for-byte. Scoped deliberately narrow — this only changes
   which implementation gaster's own standalone `pwn`/`reset` exploit step
   uses; it does not touch libirecovery's own USB transport, which the rest
   of `DeviceManager.cpp` (`sendiBSS`/`sendiBEC`/etc.) still goes through on
   every platform, and whose Darwin behavior remains genuinely unverified
   (see `.claude/TODO.md` item 4's "needs real macOS hardware" list).

`ResourcePath.cpp`'s `resolveGasterPath()` (now `_NSGetExecutablePath()` +
`realpath()` behind `#ifdef __APPLE__`) and `DeviceManager.cpp`'s
`isUninterruptible()` (now a `ps -o state=`-based fork/exec+pipe check on
Apple, matching the file's existing no-`popen()`/no-`system()` convention,
instead of reading `/proc/<pid>/status`) and `runGaster()`'s `stdbuf`
requirement (now probes for `gstdbuf` — Homebrew coreutils' default
prefixed name — as a fallback when plain `stdbuf` isn't on `PATH`) were the
other three blockers already named in `.claude/TODO.md` item 4; all three
are mechanical platform bridging with no design decision of their own
worth recording here beyond what that TODO entry already says.

## Real-hardware results: gaster doesn't work on macOS at all, blackb0x-pwn does, Linux is unreliable on every machine tested

Everything in the two entries above (the gaster IOKit branch selection, the
`libirecovery_iokit_ext` build, `blackb0x-pwn`) was built and reasoned
about without a real Mac available — genuinely unverified, by this
project's own repeated admission in both entries. Real hardware testing has
since settled several of those open questions, and the answers reorganize
this project's actual platform recommendation:

- **gaster does not work on macOS, full stop — not something the IOKit
  branch selection fixed, not something anything else tried fixed either.**
  Every real attempt to run gaster's own `pwn` step on macOS against a real
  AppleTV3,2 has failed. This is a real, repeated result, not a single bad
  run — treat "gaster on macOS" as a known-broken path going forward, not
  an open question.
- **`blackb0x-pwn` — this project's own original checkm8, built standalone
  against libirecovery's native IOKit backend (see the entry above) — does
  work**, confirmed against real AppleTV3,2 hardware. Since this is the
  opposite of gaster's result on the exact same platform and exact same
  device, `blackb0x.cpp`'s own checkm8 dispatch (`DeviceManager.cpp`'s
  `checkm8Attempt()`) now takes a `pwnTool` parameter ("gaster" or
  "blackb0x-pwn") threaded from a new `--pwntool` CLI flag
  (`Cli.hpp`/`Cli.cpp`) — Apple-only (it's rejected-with-a-warning, not
  accepted, on Linux, where `blackb0x-pwn` isn't even built), defaulting to
  `blackb0x-pwn` there specifically because that's the one that's actually
  been confirmed to work. `--pwntool gaster` is still available on macOS
  for anyone who wants to keep debugging gaster itself.
- **A real, reproducible Apple Silicon quirk**: a direct USB-C connection
  from an Apple Silicon Mac to the Apple TV was unreliable for
  `blackb0x-pwn`; putting a plain (non-Thunderbolt) USB hub in between made
  it reliable. Not understood *why* — recorded as a practical tip
  (README.md, `.claude/TODO.md`), not a fixed root cause.
- **Linux, meanwhile, remains unreliable — on every real machine tried.**
  This whole "reopen macOS support" effort (see the two entries above) was
  originally prompted by exactly this: real-hardware DFU-state races and
  non-deterministic USB behavior chased at length earlier in this file
  (`boot_client()`'s reset-timing saga, the `apple_mfi_fastcharge`
  conflict, etc.), still present on a *second*, independently-confirmed
  machine beyond whatever was used for that earlier chase — one real Intel
  11th-gen PC and one real AMD Zen 2 PC have now both shown the same class
  of problem. That rules out "just one bad host controller" as the
  explanation; whether it's fixable in this project's own code at all, or
  an inherent property of Linux's USB stack under this kind of
  timing-sensitive exploit traffic, is still genuinely open. Practical
  upshot: macOS + `blackb0x-pwn` is, as of this writing, the only
  confirmed-reliable way to actually run this tool's checkm8 step, despite
  Linux being the platform this project was originally, and still is,
  primarily developed on.

## Real-hardware fact: the Apple TV never attempts a normal boot while USB is connected

Confirmed directly on real hardware: an Apple TV 2/3 with a USB cable
plugged in never boots normally on its own, even sitting idle outside any
blackb0x run — it stays parked wherever DFU/Recovery/checkm8 activity last
left it until the cable is unplugged, at which point it boots tvOS
normally and runs fine (ruling out NAND/hardware health as an explanation
for anything observed with USB attached). Two practical consequences:

- Every boot the device performs *while connected to run blackb0x at all*
  is therefore an abnormal one from the device's own perspective — it
  never gets a clean software shutdown first, since blackb0x's own
  `checkDeviceLeftRecoveryModeAfterBoot()` failure path (DeviceManager.cpp)
  always ends with the device sitting in Recovery mode, not powered off
  cleanly. The `[NAND] s_cxt_boot:88 sftl: error, unclean shutdown;
  adopted N spans` line every real `sendKernelCache()`/
  `sendStockRestoreTail()` console capture has shown so far is consistent
  with this alone — a normal boot with the cable unplugged doesn't go
  through this same path at all, so its presence here isn't itself
  evidence of NAND corruption or failing hardware; don't chase it as a
  root cause without checking whether it also happens on a genuinely
  cable-unplugged boot (real console access would be needed to know, and
  isn't available here anyway once the cable's out).
- When isolating a boot failure, "unplug it, does it still boot fine
  normally?" is a fast, real way to confirm the device itself is healthy
  independent of whatever blackb0x is doing over USB — already used once
  to rule out a hardware-failure theory for a persistent post-`bootx`
  "Boot Failure Count" increment (the device booted and ran fine unplugged
  in between blackb0x attempts, ruling out failing NAND as the cause of
  that failure).

### A full power-cycle means BOTH cables out, not just one

A related hardware fact, recorded because half of it is easy to get wrong: the
Apple TV draws power from USB as well as from AC, and the USB rail is enough to
keep some registers intact across an AC disconnect. **Pulling only one cable
does not reset the device.** An AC-only or a USB-only unplug leaves state
behind, and the box comes back up still carrying whatever wedged it — to
power-cycle it completely, unplug AC *and* USB. Everywhere this log says
"power-cycle the device" (the wedged-USB-stack recoveries during the checkm8
work, the "unplug it, does it still boot normally?" health check above) that is
what is meant, and any earlier attempt that appeared not to be helped by a
power-cycle may simply not have had one.

## Unresolved: the stock (`--stock-recovery`/`--stock-securerom`) restore-tail path has never been gotten to boot successfully

Extensive real-hardware iteration against a real AppleTV3,2 (12H606, and
briefly 12H1006/"latest") has not produced a single successful boot via
`sendStockRestoreTail()`, on either sub-path:

- **`--stock-recovery` without `--stock-securerom`** (checkm8 + a genuinely
  stock, unpatched iBEC): every attempt reaches `bootx`, gets it
  acknowledged over USB, and then the device resets back to iBoot's own
  command prompt (`Boot Failure Count` incrementing on a real, freshly
  power-cycled device — confirmed NOT a stuck/stale counter, and
  confirmed NOT a NAND-health issue, since the same device boots and runs
  tvOS completely normally once unplugged — see the entry above).
- **`--stock-firmware --stock-recovery --stock-securerom`** (real
  SecureROM, no checkm8 at all): has not yet even reliably reached
  `bootx` — every real run so far died earlier, at RestoreLogo/Ramdisk,
  with `irecv_send_file()` failing outright ("Unable to upload data to
  device") on the reused post-ticket connection, despite a real ApTicket
  round-trip with Apple succeeding immediately before it.

Real idevicerestore's actual `recovery.c`/`dfu.c` source (fetched fresh
from GitHub, not assumed/half-remembered) was read start to finish and
compared line-by-line against this project's own `sendStockRestoreTail()`/
`sendKernelCache()`/`sendiBEC()`/`sendRamdisk()` for this exact device
class (build_major=12>8, non-image4, non-macos_variant, non-custom).
Every concrete, confirmable discrepancy found this way was fixed:
`bootx`/`go` need `bRequest=1` not the default 0 (`irecv_send_command_breq()`);
a missing zero-length DFU_DNLOAD-class control transfer
(`0x21`/`1`/zero-length) right after the kernelcache upload, before
boot-args/`bootx`; a missing `setenv boot-args rd=md0 ...` for the stock
(uncompiled-in-args) iBEC path specifically **[superseded — that command is a
no-op on this bootloader, and was already one when it was added. iBoot never
reads the `boot-args` variable on its kernel-boot path, and the value sent here
is byte-identical to the string it hardcodes anyway. It is kept for
`idevicerestore` fidelity, not for effect; see "`setenv boot-args` is INERT on
this bootloader" at the end of this file]**; a missing
`irecv_usb_set_configuration(client, 1)` after the post-iBSS reconnect;
a missing `setenv auto-boot false`/`saveenv` before RestoreLogo; a
missing `getenv ramdisk-delay`; and a reused, potentially-stale USB
connection across the whole ticket→RestoreLogo→Ramdisk→DeviceTree→
KernelCache sequence (now retried once against a fresh reconnect per
step). None of it — individually or all together — has resolved the
underlying failure on real hardware as of this writing.

**Conclusion, stated plainly rather than left implicit**: something is
still genuinely different between this project's implementation and real
idevicerestore's for this exact restore-tail sequence, and it has evaded
detection through a real, full source-level comparison. This is not from
lack of trying — it's an honest, currently-unsolved gap. Worth
considering for whoever picks this up next:

- The comparison so far has been idevicerestore's `recovery.c`/`dfu.c`
  read directly and matched function-by-function; it has NOT included a
  live, side-by-side USB packet capture (e.g. `usbmon`/Wireshark) of a
  real idevicerestore run against comparable old hardware, which would
  catch anything at the wire level that reading source can't (timing,
  transfer chunking, an endpoint/interface selection difference, a
  standard (non-DFU-class) USB control request neither of us has been
  looking for).
- Worth an explicit caveat: the stock diagnostic route specifically
  (`--stock-recovery`/`--stock-securerom`) never touches `BakeRamdisk.cpp`
  at all — it sends Apple's own downloaded `RestoreRamDisk` untouched (or
  decrypt()-only), never the `dist/`-baked one. So a ramdisk-baker bug
  can't be the direct cause of the stock-path failures documented above.
  It remains a live suspect for the *separate*, ultimate goal (a real
  jailbroken boot via the patched `dist/*.dmg` ramdisk), since
  `bake-all-ramdisks` is Linux-only today and has never actually been run
  on the macOS host all of this real-hardware testing has been done from
  — see item 4a in `.claude/TODO.md`, "macOS support for the ramdisk
  baker", the next thing being investigated.

## Porting the ramdisk baker to macOS: `hdiutil` instead of loop-mount, and a portable dependency resolver instead of `apt`

Following directly from the item above: with the stock restore-tail path's
failure fully documented as unresolved via source comparison alone, and
`bake-all-ramdisks` never once having been run on the actual macOS machine
all real-hardware testing happens on, this was the next concrete thing to
rule in or out. Full design/implementation plan:
`/var/home/regulad/.claude/plans/parsed-painting-cocke.md` (not itself
checked into the repo, referenced here for anyone who has it).

**Two hard constraints shaped the design, both confirmed by direct
checking rather than assumed:**

- **No containerization was available in the environment this was ported
  from at all** — not just podman specifically; no viable container
  runtime existed there, so `podman machine`'s usual "run a Linux VM
  under the hood" escape hatch for macOS was never on the table either.
- **Real Debian `apt-get` has no usable native Apple Silicon path.**
  MacPorts carries a real `dpkg` port, but not `apt-get`. Fink packages
  the genuine upstream apt/dpkg codebase and would otherwise have been
  the obvious answer, but is unmaintained (last release Feb 2022) and
  explicitly, upstream-confirmed unsupported on Apple Silicon
  (fink/fink#232) — checked and ruled out on this specific hardware
  before being proposed as a real option, not assumed.

**HFS+ volume build**: rather than port Linux's three-mount loop-mount
dance (`mount -t hfsplus -o loop`, `mkfs.hfsplus`, grow-in-place — see
`BakeRamdisk.cpp`'s own "three designs tried" comment for why grow-in-
place was rejected there, real B-tree growth bugs in xpwn's in-memory
writer and in `libhfsp`) as-is, the macOS path generalizes a technique the
*original* Objective-C app (`Patcher.mm`'s `patchRamdisk:ssh:`, before this
project's C++ port) already used for exactly one legacy case
(`AppleTV2,1_4.*` firmware): synthesizing a whole HFS+ volume directly from
a plain folder tree via `hdiutil create -srcfolder ... -format UDRW
-layout NONE`, rather than growing an existing volume. The Linux code's
own real improvement — computing the target size dynamically instead of a
hardcoded legacy constant — carries over; the macOS path now mounts the
original once (`hdiutil attach -nobrowse -readonly`), reads its volume
label via `diskutil info -plist` (parsed with the already-vendored
libplist, the same way `IPSW.cpp` already parses plists elsewhere in this
project — there's no `blkid` equivalent on macOS), copies the content out
to a plain staging directory, splices in `/sbin/launchd` + `/blackb0x`
there exactly as the Linux path already does against its own scratch
mount, measures the real content size with a portable `std::filesystem`
walk (replacing GNU-only `du -s --block-size=1` — fixed on *both*
platforms, not just macOS, since it's strictly more precise either way),
and builds the final correctly-sized volume in one `hdiutil create` call.
One real mount/unmount cycle instead of Linux's three, and no
`CAP_SYS_ADMIN`/loop-mount mechanism needed at all —
`copyVolumeHeaderMetadata()`, pure byte-level work on the raw image bytes,
needed no changes and is shared unmodified by both platforms.

Two portability fixes rode along in the same area: the vendored
`third_party/xpwn/includes/hfs/hfsplus.h` uses the `register` storage-
class specifier (removed from the language in C++17; GCC only warns,
Clang on macOS hard-errors) — bracketed with `#define register`/`#undef
register` around the one include that pulls it in transitively
(`<dmg/dmglib.h>`), rather than editing the vendored header itself or
downgrading `BakeRamdisk.cpp`'s language standard (rejected — the file
uses `std::filesystem` throughout, a real C++17 *library* feature, not
just a language one). And `struct stat`'s `st_atim`/`st_mtim` fields
(Linux/glibc spelling) become `st_atimespec`/`st_mtimespec` on Darwin,
following the same `#if defined(__APPLE__)` + inline-rationale-comment
style `ResourcePath.cpp` already established for this kind of platform
split.

**Dependency resolution and preinstall, without `apt`/`dpkg`/containers at
all**: `scripts/build_deb_cache.py`'s real container-based, real-`apt-get`
picklist generation, and `BakeRamdisk.cpp`'s own podman-based
`computePreinstalledPackages()` (which runs real `dpkg --unpack
--force-depends` + `--configure -a` + `--audit` inside a container)
both needed a macOS-side replacement given the two constraints above. The
key fact making a portable replacement viable at all: `computePreinstallEligibleFilenames()`
already guarantees every bake-time-eligible package has no real
preinst/postinst script (transitive closure against
`prebake_package_blacklist.txt`) — the *only* reason real `dpkg` was ever
needed for the preinstall step in the first place was to correctly
sequence script execution around a genuine dpkg/tar/gzip/sed dependency
cycle. With no scripts to run, portable extraction is equivalent: pull
each package's `data.tar.*` directly (`ar`/`tar`, no container), merge
it onto the preinstall root, and hand-assemble a `Status: install ok
installed` stanza — reusing/generalizing `extractDebAndBuildStanza()`/
`mergeRealFilesystemTree()`/`buildStatusStanzaFromControl()`, which
already existed in `BakeRamdisk.cpp` for `stageEtasonatv()`/
`stageP0sixspwn()`.

For picklist generation itself, a new, deliberately-scoped-down,
explicitly-marked-experimental script was written rather than trying to
make the real podman/`apt-get` path work through some indirection:
`scripts/build_deb_cache_experimental_no_container.py`. It operates only
against `.deb`s already vendored in `Blackb0x/Debs/` (fails loudly,
pointing at growing `Blackb0x/Debs/` on a real Linux+podman machine
first, if a dependency closure needs something not already vendored),
walks dependency groups by bare package name only (comma-separated
groups, `|` alternatives, `Provides:` for virtual packages — no real
version-constraint comparison at all, matching the same simplification
`BakeRamdisk.cpp`'s own `parseDependencyGroups()` already makes for a
different purpose), and declares the same synthetic `firmware` package
both the real podman script and `kPreinstallInnerScript` already declare,
for the same reason (firmware-version-gated `Depends:` lines with no
real package backing them). Every known gap versus `build_deb_cache.py`
is documented directly in its own module docstring rather than left
implicit. `computeGlobalDebcacheOnce()` now picks between the two scripts
via `#if defined(__APPLE__)`, since both share the same `--output-dir`/
`picklist.txt`/`resolved_packages.txt` contract by construction — nothing
else about how the output is consumed needed to change.

Validated with 18 fully-portable unit/end-to-end tests
(`scripts/test_build_deb_cache_experimental_no_container.py`, real
`ar`/`tar` only, no podman/network/root — run on Linux specifically
*because* that's exactly where a script whose entire point is "run
somewhere podman doesn't" ought to be validated: if the algorithm itself
is wrong, catching it on Linux is just as valid as catching it on macOS
would be), and against this project's real data (`Blackb0x/Misc/packages.txt`,
107 real vendored `.deb`s) as a one-off check: 60 of 63 top-level
packages resolved cleanly, the other 3 being exactly the expected,
already-documented exclusions (`com.ih8sn0w-squiffy-winocm.p0sixspwn`,
`essential`, `net.tihmstar.etasonuntether`). Getting to that clean result
surfaced two genuine, previously-unknown bugs, one of them in
already-shipping C++ code, not just the new script:

- 14 real, already-vendored `org.tihmstar.*.deb`s store their control
  member as plain `control` inside `control.tar.*` instead of the
  universally-assumed `./control`. Fixed in the new Python script by
  listing the archive's contents first and matching whichever spelling is
  actually present — and the identical fix was ported to
  `BakeRamdisk.cpp`'s own `readDebControlInfo()`, which had carried the
  same hardcoded `./control` assumption since before this port, applied
  uniformly to `control`/`preinst`/`postinst` there since all three
  member names show the same inconsistency.
- `rtadvd` (a real, non-optional, already-vendored dependency of
  `network-cmds`) has a bare `Depends: firmware` with no alternative —
  without a synthetic `firmware` package to satisfy it, everything
  depending on `rtadvd` falsely failed to resolve even though nothing was
  actually missing. The fix mirrors what `build_deb_cache.py`'s own
  container script and `kPreinstallInnerScript` already do for the exact
  same reason.

**Also landed in the same effort, smaller and independent:** `bake-all-ramdisks`
gained a `--build <buildID>` flag alongside the existing `--device
<model>`, combinable the same way `--signed-only` already combines with
`--device` — useful for iterating against one specific `(device,
buildID)` tuple while bringing this port up, instead of paying for every
known build on every test run. And `blackb0x` itself (the main CLI
binary, not `bake-all-ramdisks`) now self-bakes a missing or stale
`dist/*.dmg` ramdisk on demand when running with sufficient privilege
(root on Linux, or on macOS) instead of hard-requiring a separate manual
`bake-all-ramdisks` run first: `Cli.cpp`'s `downloadAndPatchComponents()`
computes bake-need right after the firmware manifest is parsed, spawns
`bake-all-ramdisks --device <model> --build <buildID>` in the background
as early as possible (skipping the Apple `RestoreRamdisk` download
entirely on this path, which `Patcher::patchRamdisk()` never actually
used anyway), and only blocks on it (`waitpid()`) immediately before the
patched ramdisk is actually needed.

**Not yet real-hardware/real-macOS verified.** Everything above was built
and tested on Linux only — every `#if defined(__APPLE__)` branch compiled
correctly (confirmed via careful reading and, where possible, exercising
the shared non-gated helpers directly against real fixture `.deb`s) but
has never actually executed, since no Mac was available in the
environment this was written in. Two specific spots are flagged directly
in `BakeRamdisk.cpp`'s own comments as unverified pending a real Mac: the
exact `-fs "Case-sensitive HFS+"` argument string for `hdiutil create`,
and whether `hdiutil create -format UDRW -layout NONE`'s real output is
genuinely flat raw bytes or carries extra UDIF structure beyond a
trailing "koly" block (defensively mitigated by a new
`unwrapUDIFIfPresent()` safety net, itself untested against real
`hdiutil` output). The actual motivating question — whether finally
baking a ramdisk natively on the real Mac test machine, rather than
copying one over from elsewhere, resolves the persistent post-`bootx`
boot failure documented in the entry above — is also still open, only
answerable by actually running this on that machine.

## `blackb0x-pwn` on Linux: an error-code bug, then a wire-level teardown of why checkm8 still fails

`blackb0x-pwn` (this project's own hand-ported checkm8, confirmed working on real
AppleTV3,2 hardware on macOS) had never been run on Linux at all — it was gated
behind `if(APPLE)` in `CMakeLists.txt`. Nothing in `Blackb0x/Source/Pwn/` is
actually Apple-specific, though: it uses only libirecovery's backend-independent
`irecv_*` API, and every call it makes has a real libusb implementation. The gate
was a packaging decision from the macOS effort. Building it on Linux turns it into
a **control against gaster**, which matters because the entry above concludes that
this machine's USB stack "genuinely cannot handle checkm8's device-initiated reset
reliably" — a conclusion resting entirely on gaster, the only implementation ever
run here.

**That conclusion is wrong, and this entry supersedes it.** Across every run below,
the device reset and re-enumerated cleanly three times, promptly, every time. The
resets are not the problem.

### Two real bugs found before any measurement was possible

- **The `IRECV_E_PIPE`/`LIBUSB_ERROR_PIPE` gap, again.** First Linux run died
  instantly with `Failed to stall pipe -9.` — and `-9` is `LIBUSB_ERROR_PIPE`, a
  *successful* stall, which is checkm8's first exploit step. `irecv_usb_control_transfer()`
  does not normalise its return across backends: IOKit translates into
  libirecovery's enum (`IRECV_E_PIPE == -10`, `IRECV_E_TIMEOUT == -11`), libusb is
  a bare pass-through of libusb's own unrelated codes (`-9`, `-7`). All six checks
  compared against the IOKit spelling only. **This is the identical gap documented
  in the libimobiledevice-fork entry above**, fixed once in the `DeviceManager.cpp`
  `checkm8()` that predates this file, across the same six call sites; it returned
  because `Checkm8Pwn.c` was recovered from the original Objective-C, which only
  ever ran on IOKit. Now both spellings are accepted, which is safe because
  `iokit_usb_control_transfer()`'s translation table cannot produce `-9`/`-7` at all.
- **A use-after-free, double free and hang in libirecovery's libusb async-cancel
  path.** `irecv_async_usb_control_transfer_with_cancel()` freed its buffer while
  the transfer was still queued *and* left `LIBUSB_TRANSFER_FREE_BUFFER` set, never
  called `libusb_free_transfer()`, and waited on `status != LIBUSB_TRANSFER_CANCELLED`
  — which never terminates for a transfer that completes instead of being cancelled,
  since the field is bzero'd and `LIBUSB_TRANSFER_COMPLETED` is itself 0. Unreachable
  until now (its only consumer ran on IOKit). Fixed on **regulad/libirecovery@`libusb-async-cancel-fix`**.

### Method: usbmon, because the tools' own logging cannot see the answer

Kernel lockdown (forced on by Secure Boot) blocks the debugfs usbmon interface
outright — `sudo cat /sys/kernel/debug/usb/usbmon/3u` returns `EPERM` no matter
what, and `modprobe usbmon` "succeeds" while changing nothing because usbmon is
built in. The binary interface at `/dev/usbmonN` is not debugfs and is not
restricted, and libpcap talks to it directly: `sudo tcpdump -i usbmon3 -w out.pcap`.
`scripts/analyze_usbmon_checkm8.py` summarises the result per request type.

This matters because every heap-grooming request in checkm8 is *expected* to fail,
so the tools' own "did it fail the right way?" checks pass identically whether the
device serviced a request or the host cancelled it before it left. Only the wire
distinguishes those.

### What the wire shows

The whole sequence executes, in order, on Linux: 2 stalls, 626+1 leaks, 1 no-leak,
the 2048-byte bug-setup download, the abort, the 1660-byte overwrite, the 678-byte
payload. Nothing is missing or misrouted.

**The bug-setup partial transfer is precisely controllable.** `DEBUG_CANCEL_DELAY_US`
sweeps how much of it the device consumes, and the relationship is cleanly linear:

    bytes ~= 0.321 * delay_us - 5.5      (~321 bytes/ms, near-zero intercept)

against a theoretical ceiling of 512 B/ms for 64-byte packets at one transaction per
125us microframe. Predictions from this fit matched later runs to within one packet.
**Linux's stack is behaving predictably and controllably here, not erratically.**
Swept across 0 -> 1600 bytes — the entire window the tool considers valid — every
value fails.

**The overwrite is what never lands.** That transfer (`bmRequestType=0x00,
bRequest=0`, 1660 bytes) carries the 1632-byte pad and the 28-byte
`callback = 0x34000000`, and it is the step that performs the actual corruption:

| bug-setup `sent` | overwrite outcome |
|---|---|
| 0 | stalled, **0 bytes** |
| 128 | stalled, **0 bytes** |
| 320 | stalled, **0 bytes** |
| 1472 | accepted, **576 bytes** = `0x800 - 1472`, then stops |

At `sent = 1472` the device accepts it as a *continuation of the same 2048-byte DFU
buffer* and stops exactly when that buffer is full — reproducible at both 100ms and
500ms timeouts, so it is a structural ceiling, not a rate limit. That leaves two
incompatible requirements: acceptance needs `sent` above 320, and fitting all 1660
bytes needs `sent <= 388` (`2048 - 1660`). **No setting satisfies both**, which is
why no cancel delay helps.

Two dead ends worth recording so they are not re-tried: holding the connection open
through the overwrite (`DEBUG_KEEP_CONNECTION`, skipping the close/500ms/reopen)
makes it *worse* — a groom leak returns `EPROTO` and every transfer after it also
`EPROTO`s in ~141us, so the close/reopen is **recovering** from an endpoint error
state rather than causing the problem. And the reopen path was audited and is
innocent: `irecv_usb_set_configuration()` is guarded by `libusb_get_configuration()`,
`irecv_close()` skips `libusb_release_interface()` for DFU-mode clients, and
`irecv_usb_set_interface(0,0)` is a usbfs ioctl with no wire traffic.

### gaster fails differently, and the contrast is the open question

gaster does **not** fail the same way, contrary to what the entry above assumes. Its
own equivalent request (`bmRequestType=0x00, bRequest=0`, 1632 bytes) **does deliver
— up to all 1632 bytes, on 300 of 384 attempts.** So large malformed control writes
are not blocked on this host. gaster's problem is the opposite: `checkm8_stage_setup()`
requires that request to come back `USB_TRANSFER_STALL`, and on Linux it *succeeds*.
It stalled once in 384 tries, so gaster spun in its unbounded retry loop and never
left SETUP.

**So the same request delivers for gaster and is refused for blackb0x-pwn**, differing
only in what precedes it: gaster issues it immediately after the aborted download on
one connection, while blackb0x-pwn issues it after `CLRSTATUS`, a close/reopen, a
stall and a leak. That asymmetry is the unresolved question, and it is not a host-stack
limitation.

### Where this leaves the platform conclusion

Not "Linux cannot do checkm8". The resets work, the partial-transfer mechanism is
precise and predictable, and large malformed control writes are delivered. What has
not been established is why the device refuses the overwrite for one implementation
and accepts it for the other. The decisive measurement is what that same transfer does
on a **working macOS run** — whether it is supposed to deliver 1660 bytes. To make that
a single run,`analyze_usbmon_checkm8.py` now also reads `DLT_USB_DARWIN` captures
(`sudo ifconfig XHC20 up && sudo tcpdump -i XHC20 -w out.pcap`). That Darwin
pseudo-header layout is **unverified** — Wireshark's dissector gave the field order but
the source fetch truncated before the offset arithmetic, and libpcap defines only the
DLT number — so the script tries two candidate layouts, validates each against the
capture's own `header_len` and enum fields, and **refuses to print numbers if neither
matches** rather than emitting a confident guess. Sanity-check its first real output.

All investigation knobs in both binaries are `DEBUG_`-prefixed and every default is the
macOS-confirmed behaviour, so an unset environment is the original path byte for byte:
`DEBUG_CANCEL_DELAY_US` (100), `DEBUG_OVERWRITE_TIMEOUT_MS` (100), `DEBUG_RECONNECT_ATTEMPTS`
(30), `DEBUG_KEEP_CONNECTION` (unset), `DEBUG_IGNORE_GROOM_ERRORS` (unset); gaster adds
`DEBUG_CANCEL_DELAY_US` and `DEBUG_SETUP_FULL_PAD`.

## iBoot32Patcher: vendored as a submodule, run as a separate binary, and stripped back to the patches actually wanted

Three changes, one investigation. Prompted by a real-hardware result: with
blackb0x's own patched iBSS/iBEC, the device would not boot even a
**fully stock** firmware suite (`--stock-firmware` alone, keeping the
patched bootchain), and iBoot's own banner showed **Boot Failure Count
incrementing with Panic Fail Count unchanged** — i.e. iBoot rejected the
suite before ever executing the kernel. Per `Cli.cpp`'s own description of
that flag, that implicates the iBSS/iBEC patches themselves rather than
the kernel patch or the baked ramdisk.

### The patcher was a copy, and it had inherited bugs

`Blackb0x/Libraries/libiboot32patcher*` was a near-verbatim copy of
`zzanehip/iBoot32Patcher` (itself a fork of `iH8sn0w/iBoot32Patcher`,
GPL-3.0-or-later, 2013-2016). Measured rather than assumed: `finders.c` and
`functions.c` were **byte-identical** after the include-path rewrite,
`patchers.c` differed by 19 lines (all one local fix), and the
`libiboot32patcher.c` wrapper differed by a dead commented-out include block
and trailing whitespace. Two genuine bugs, one of them inherited verbatim:

- **`patch_kaslr()` fell off the end of a non-void function on every branch
  that actually applied its patch.** Undefined behavior whose garbage return
  read back non-zero on x86_64 (masking it) but 0 on arm64, so a real macOS
  run treated a *successful* KASLR patch as a hard failure. Already fixed
  locally before this work; now a commit on the fork.
- **`iBootPatcher()` tested its `RSA` argument twice**, so its `debug`
  argument was dead and `patch_debug_enabled()` ran whenever the RSA patch
  was requested. Confirmed present in zzanehip's source verbatim, so
  inherited, not a porting mistake. It survived a decade because zzanehip's
  own `main()` parses `--debug` correctly — the bug exists only in the
  library entry point, which essentially only blackb0x ever called.
  `patch_debug_enabled()` rewrites `BL get_value_for_dtre_var("debug-enabled")`
  into `MOVS R0,#1; MOVS R0,#1`, forcing that DeviceTree variable to answer 1
  forever. It lands on iBEC but not iBSS (it sits behind `has_kernel_load()`,
  and iBSS has no kernel-load routine). `Patcher.mm:248` passed
  `debug="FALSE"` too, so the original app never wanted it either — though
  the original linked prebuilt `.a` files, so whether *its* binary carried
  the bug is unknown and this is not proof the original was affected.

**No mainstream fork was a viable base.** Checked `iH8sn0w`, `dora2ios`,
`LukeZGD` and `NyanSatan`: none has `patch_kaslr` at all, and none exposes a
library entry point. `dora2ios`/`NyanSatan` do handle `--debug` correctly,
but adopting either would mean porting `patch_kaslr` back in. zzanehip is the
only fork with both pieces, so the fork is
**regulad/iBoot32Patcher@`blackb0x`**. Note this buys *provenance*, not
maintenance: upstream's last commit was 2016 and no fixes are coming. The
win is that the two fixes are visible commits instead of silent edits to a
file that looked like a pristine vendored copy.

### It is now a separate program, for licensing reasons

iBoot32Patcher is GPL-3.0-or-later; blackb0x has **no LICENSE file at all**.
Statically linking it made the whole binary a GPLv3 derivative. It is now
built as its own executable (`add_executable(iBoot32Patcher ...)`) and
fork/exec'd via `Patcher.cpp`'s `runIBoot32Patcher()` + `ResourcePath`'s
`resolveIBoot32PatcherPath()`, matching how `gaster`/`blackb0x-pwn` are
already invoked. Verified: `nm build/blackb0x` contains **zero** patcher
symbols. The submodule also brings upstream's own `LICENSE` file along,
which the in-tree copy never had. **Do not collapse this back into
`add_library()`** — it is a licensing constraint, not a packaging taste.

`runIBoot32Patcher()` deliberately does not reuse
`DeviceManager.cpp`'s `runLineBufferedSubprocess()`: that machinery exists
for long-running exploit tools that must stream progress out of a pipe, and
this is a short-lived file-in/file-out program whose stdio its own `exit()`
flushes.

### Boot-args moved out of the binary, and one of them was inverted

> **SUPERSEDED, and this was the central bug.** This subsection's decision —
> drop `-b`, set boot-args at runtime with `setenv boot-args`, collapse two
> iBECs into one — has been reversed. `setenv boot-args` is inert on
> AppleTV3,2's iBoot-1537.9.55: the variable is settable and nothing ever reads
> it back, so what this change actually did was remove
> `amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1` from every boot.
> `-b` and the two-iBEC design are both back. The disassembly, the fix and the
> confidence statement are in "`setenv boot-args` is INERT on this bootloader"
> at the end of this file. Everything below is kept as the record of what was
> believed and why — and one paragraph of it (the `patch_boot_args()` hazard
> writeup) is still correct and still load-bearing.

The patcher is now asked for `-r` (+ `-k`, + `-t` when a ticket is in play)
and **nothing else** — no `-b`, no `-d`. Boot-args are set at runtime with
`setenv boot-args`, which is how `sendStockRestoreTail()` and real
idevicerestore have always done it, delivered through
`sendFileThenCommand()`'s `extraCommandBeforeMain` so the order is
upload → zero-length `DFU_DNLOAD` → `setenv boot-args` → `bootx`, matching
idevicerestore exactly. Pointedly **not** followed by `saveenv`: persisting
`rd=md0` into NVRAM would leave the device root-mounting a ramdisk that is
no longer there on every subsequent normal boot. (The stock path saves
`auto-boot false` because that *is* meant to persist, and likewise does not
save boot-args.)

Dropping `patch_boot_args()` removes the most invasive patch in the tool and
the only one that can mis-patch silently. Both of blackb0x's boot-args
strings were longer than iBoot's own `rd=md0 nand-enable-reformat=1
-progress` (53 and ~95 chars vs 39), which **always** triggered its
relocation path: repoint the xref into the "Reliance on this certificate"
string, `strcpy()` there unbounded, scan byte-by-byte for an `IT` instruction
with no end-of-buffer guard (the author's own comment calls it "kinda
hacky"), then write an 8-bit PC-relative immediate with **no range check** —
`ldr_rd_null_str->imm8 = (diff / 0x4)` silently truncates past 255 and
repoints a load somewhere arbitrary. None of that reports failure.

Moving the args also surfaced a **real inversion** that the compiled-in
version had hidden. `patchiBEC()` built two iBECs differing *only* in
boot-args: `args1` **with** `rd=md0` became `iBECDowngrade`, `args2`
**without** it became `iBECBoot`. But `Cli.cpp` sends `iBECDowngrade` for
`--tether-boot` (which sends **no** Ramdisk) and `iBECBoot` for the
jailbreak path (which **does** send Ramdisk + DeviceTree). Exactly backwards:
the jailbreak path uploaded a ramdisk and then told the kernel to root off
NAND, so `entrypoint.c` could never have run as PID 1. `sendKernelCache()`
now takes an explicit `ramdiskBoot` flag (`!tetherBoot` at the call site)
and picks the args accordingly.

With boot-args out of the binary the two iBEC builds became byte-identical,
so `patchiBEC()` now produces **one** output and points both
`PatchedComponents` fields at it, the way `useStockIBEC()` already did.

The runtime args are matched to what the ramdisk actually does —
`entrypoint.c` execs as PID 1, writes its files, and reboots, nothing more:
`rd=md0 -v amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1
pio-error=0`. `rd=md0` is what makes the ramdisk the root device at all;
the AMFI/code-signing args are what let an unsigned PID 1 exec; `-v` is
free console output on a path whose only other diagnostic is
`checkDeviceLeftRecoveryModeAfterBoot()`. `nand-enable-reformat=1` is
deliberately absent — this boot plants files on the existing filesystem and
must not reformat it.

**Not yet tested on hardware.** The build is clean and `blackb0x_tests`
passes, but whether removing the unintended `debug-enabled` patch (or fixing
the `rd=md0` inversion) changes the stock-suite boot failure is an open
question that needs the Mac. The `debug`/`RSA` fix is the first thing to
bisect, since it is the one patch that was being applied against the
project's own stated intent.

## Tether-boot removed: one send flow, one iBEC, no `onlyBootComponents`

> **Both halves of this are superseded.** `--tether-boot` came back as a
> kernel-integrity probe ("`--tether-boot` revived (and corrected)", below), and
> the "one iBEC" half was reversed with it once `setenv boot-args` was proven
> inert: two baked iBECs exist again, `dist/iBEC-<tuple>` and
> `dist/iBECTether-<tuple>`, because a compiled-in boot-args string is a
> property of the binary and one image cannot serve both `rd=md0` and
> `rd=disk0s1s1`. The premise stated in the first sentence below — "with
> boot-args moved to runtime... the tether-boot path had no remaining reason to
> exist" — rested on a runtime channel that never worked. See "`setenv
> boot-args` is INERT on this bootloader" at the end of this file. Kept as the
> record of what was removed and why, which is still the accurate inventory.

With boot-args moved to runtime (previous entry) the tether-boot path had no
remaining reason to exist, and keeping it was actively costing clarity: it
was the reason there were two iBEC builds, two boot-args sets, and an
`onlyBootComponents` flag threaded through four files. All of it is gone.
There is now exactly one flow: iBSS → iBEC → RestoreLogo → Ramdisk →
DeviceTree → KernelCache(`bootx`).

Removed, and what each was for:

- **`--tether-boot` / `CliOptions::tetherBoot`** — re-booted an
  already-installed *tethered* jailbreak (one with no untether) by sending
  iBEC and a kernelcache only, no Ramdisk, so the kernel rooted off NAND.
- **`AppleTVDevice::didTetheredBoot`** — set by that path, read by nothing.
- **`kTetherBootArgs`** and `sendKernelCache()`'s `ramdiskBoot` parameter —
  the non-`rd=md0` boot-args existed only for that path. `sendKernelCache()`
  now sets `kRamdiskBootArgs` unconditionally, which is correct because a
  Ramdisk is always sent before it now.
- **`onlyBootComponents`** — existed *solely* to express "tether-boot, so
  skip RestoreLogo/Ramdisk/DeviceTree". It was always false once tether-boot
  was gone, so it is dropped from `parseManifest()`, `Patcher`,
  `sendStockRestoreTail()`, and `downloadAndPatchComponents()`; the two
  `if (!onlyBootComponents)` blocks are now unconditional.
- **`PatchedComponents::iBECDowngrade`/`iBECBoot`** → one `iBEC` field.
  These had already become byte-identical files once boot-args left the
  binary; tether-boot was the only consumer of the second variant.
- **`prebootPathFor()`/`downgradePathFor()`** — no longer any second output
  to name.
- **`buildToRequest`** is now unconditionally `kJailbreakTargetBuild`.
  Tether-boot was the only caller that requested `device.buildID` instead,
  which is what let it target a firmware other than the one pinned build.

**This is a real capability reduction, stated plainly rather than buried:**
tether-boot was the only way to use the firmwares that have no untether —
tvOS 7.x on Apple TV 3 and tvOS 7.1.2 on Apple TV 2,1 (`.claude/LEGACY_FLOW.md`
logs that install as `"Installing iOS 7 tether"`, "genuinely tethered, no
untether exists"). Installing on those builds was already unreachable from
this port, since `buildToRequest` only ever resolved to the single pinned
`kJailbreakTargetBuild`; removing tether-boot drops the ability to *re-boot*
such an install afterwards. `README.md`'s device table no longer claims
tethered support.

Also dropped with it: the tether-boot preflight in `runCli()`, which
asserted `device.jailbroken = 1`, defaulted an unknown AppleTV2,1 to
7.1.2/11D258, required every other model to have connected in Normal mode
first so its real version/build were known, and refused AppleTV2,1 on 6.1.4
outright. None of those constraints apply to the install flow.

Build is clean and `blackb0x_tests` passes. Nothing here has been run against
hardware.

## `bake-all-bootloaders`: pre-patching the non-ramdisk suite, and the two crashes that found

`.claude/TODO.md` item 5 ("Pre-patch firmware components ahead of time")
for everything that isn't the ramdisk. `bake-all-bootloaders`
(`Blackb0x/Source/BakeAllBootloaders.cpp`) downloads and patches iBSS, iBEC,
KernelCache and DeviceTree for every `(device, buildID)` under
`Blackb0x/ImageKeys/`, writing `dist/bootchain/<device>_<buildID>/`.

It is a separate binary from `bake-all-ramdisks` for one specific reason:
**nothing it does needs root.** Every step is download, decrypt and
byte-level file patching — no loop mount, no `hdiutil`. So unlike the
ramdisk baker it can be run and re-run freely, which makes it the only way
to exercise `patchiBSS()`/`patchiBEC()`/`patchKernel()` — and therefore the
`iBoot32Patcher` binary they now fork/exec — across every known firmware
with no hardware attached. It shares `Patcher.cpp` with `blackb0x` itself
rather than reimplementing the patch calls, so the two cannot drift.

Design notes worth keeping:

- **Per-component pass/fail, not one verdict per target.** Which component
  failed is the diagnostic: "every iBEC fails" and "one build fails
  everything" mean entirely different things.
- **Download failure and patch failure are reported separately, and only
  the latter sets the exit status.** Apple no longer hosts many of these
  builds; that is not this project's bug.
- **Published filenames carry no extension.** The container format really
  does differ per component and per model — verified on the wire:
  `devicetree`, `iBEC` and `kernelcache` all start `33676d49` (`Img3`),
  but `iBSS` starts `0e0000ea`, a raw ARM reset vector, because
  `patchiBSS()` deliberately keeps the raw patched iBoot for Apple TV 3
  and deletes the re-wrapped img3 (its own `j33i` branch). Naming them
  `*.img3` would have been a lie.
- **`--out <dir>`, and an up-front writability check.** `dist/` is
  typically root-owned from a previous `sudo ./bake-all-ramdisks`, which
  makes the default output location unwritable for a deliberately-rootless
  tool. It says exactly that instead of emitting four identical per-file
  `ENOENT`s after paying for every download and patch.

### Two real crashes, found on the first sweep

**1. `xpwntool.c`'s `decrypt()` dereferenced NULLs it had just diagnosed.**
All three of its error paths — `cannot open infile`, `cannot open outfile`,
`cannot duplicate file from provided template` — printed the message and
then fell through into the NULL pointer, so each was a SIGSEGV rather than
a failure. This is the actual root cause behind the "`decrypt()` a
nonexistent file corrupts the heap" hazard `Patcher.cpp` documents in
several places: callers could not detect it (`decrypt()` returns `void`) and
the process did not survive long enough for them to inspect its output
either. All three now bail out, leaving the zero-byte output file that
`Patcher.cpp`'s own emptiness checks look for. `decrypt()` is still `void`
— this makes the failure survivable and detectable, not reportable; giving
it a return value is a wider change across every call site.

Caught because `AppleTV3,2` `10B144b`'s kernelcache downloads perfectly
(6006020 bytes) and has a real `Kernelcache` entry in its `.keys` file, but
`openAbstractFile*()` refuses it — so the sweep died with exit 139 on target
1 of 34 instead of skipping one firmware. `Blackb0x/Libraries/xpwntool.c` is
no longer "unchanged from `zzanehip/xpwntool-swift`"; AGENTS.md records the
divergence.

**2. `patchKernel()` never checked its first `decrypt()`.** It guarded
`patch_kernel()`'s failure (that fix is documented in its own comment) but
handed `patch_kernel()` whatever the preceding `decrypt()` left behind,
which on failure is a zero-byte file that `patch_kernel()` crashes on. Now
checked explicitly. With fix 1 in place this guard is reachable and turns
the crash into one clean, attributed line.

### What the sweep actually measured

Full `--device AppleTV3,2` run, 34 known builds. Of those that are still
downloadable from Apple:

- **iBSS: patched successfully on every single one.**
- **iBEC: patched successfully on every single one** — so RSA + ticket
  patching, with `-b`/`-d`/`-k` all now dropped, works across the whole
  known build range, not just the pinned target.
- **DeviceTree: fine everywhere** (it is a pass-through, no patch step).
- **KernelCache: fails on 20 of the 26 downloadable builds.** The six that
  work are `10B329a`, `11B511d`, `12B466`, `12H903`, `12H911` and `12H914`.
  8 of the 34 could not be downloaded at all (Apple no longer hosts them),
  which the tool reports separately and does not count as a patch failure.

Final tally: 6/34 complete, 8 undownloadable, and per-component patch
failures of iBSS 0, iBEC 0, kernel 20, devicetree 0.

The kernel result is measured, not a regression: `kJailbreakTargetBuild` is
`10B329a`, which is one of the six that work, so the real flow is
unaffected — the kernelcache always comes from that pinned build, never from
whatever the device happens to be running. But "the kernel patch works on
roughly a quarter of the fetchable AppleTV3,2 builds" was previously an
unexamined assumption, and it is exactly the coverage gap
`.claude/TODO.md` item 6 exists for.

Two things in that table are worth a second look by whoever picks this up.
The three newest builds (`12H903`/`12H911`/`12H914`) all patch cleanly while
most of the middle of the range does not, which is not the shape you would
expect from simple signature drift over time. And `12H606` — the build most
of this project's real-hardware testing has been done against — is among the
kernel failures. That does not affect the jailbreak flow (which never
patches the device's own kernel), but it does mean any future diagnostic
that wants to patch `12H606`'s kernelcache specifically will not work today.

Still to do for item 5: `blackb0x` does not yet *consume*
`dist/bootchain/`. The live path still downloads and patches inside the
window the device is sitting in pwned DFU.

## Correction: `-k` restored, `-t` made unconditional

Two corrections to the entry above, both on direction from the project
owner, and the first of them straightforwardly my error.

**`-k` (KASLR) is back, unconditionally.** It had been dropped on the
reasoning that nothing downstream reads the kernel slide — `patch_kernel()`
patches the kernelcache file before upload and `entrypoint.c` is an ordinary
userland PID 1. That argument was not good enough to justify the change:
`Patcher.mm:248-249` passes `kaslr="TRUE"` on both of the original app's
iBEC patches with no condition on it, and the original is the only
configuration this project has ever seen boot. Dropping a patch the
known-working reference applied unconditionally, on a first-principles
argument and with no hardware to check it against, was the wrong call.

**`-t` (ticket check) is now unconditional too**, and `patchiBEC()` has lost
both of its parameters. `flags` was already dead (an explicit `(void)flags;`).
`ticket` existed for one call site — a version heuristic carried over from
the original `setIBECPath:` that passed `false` for paths containing "4.".
There is no configuration in which a patched iBEC wants the ticket check
left in: the only real question is whether a ticket gets *sent*, and that is
decided at send time, not bake time. `sendStockTail()` (Cli.cpp:820) is
already gated on `--stock-recovery`, so `--stock-firmware` on its own sends
no ticket regardless. A patched iBEC that still enforced the check could
never load blackb0x's own patched kernel or ramdisk anyway, since no real
ticket can authorize those.

iBEC is therefore `-r -k -t`, all three unconditional. iBSS stays `-r` alone
(it has no kernel-load routine, so the others are no-ops there).

### What unconditional `-k` costs on pre-iOS-6 builds

Measured with `bake-all-bootloaders --device AppleTV2,1`, which is the only
model with pre-6 firmware in `ImageKeys/`: iBEC patching now **fails** on
those builds, e.g. `9A335a`:

```
main: Error doing patch_kaslr()!
patchiBEC: iBoot32Patcher failed for .../iBEC.k66ap.RELEASE.dfu (exit 255)
```

This is not a mis-patch and not a regression in the patch itself —
`patch_kaslr()` only implements `os_vers` 6, 7, 8 and 9, and returns failure
for anything else. The underlying reason is that **KASLR did not exist
before iOS 6**, so there is genuinely nothing to disable on those builds and
asking for `-k` is asking for a patch that has no meaning there.

The full sweep puts the boundary exactly where that explanation predicts.
iBEC fails on precisely the iOS 5-era builds -- `8M89`, `9A334v`, `9A335a`,
`9A336a`, `9A405l`, `9A406a`, `9B179b`, `9B206f`, `9B830` -- and succeeds on
every iOS 6-or-later build in the set (`10A406e`, `10A831`, `10B329a`,
`10B809`, `11A502`, `11B511d`, `11B554a`, `11D169b`, `11D201c`, `11D257c`,
`11D258`). Nine failures, all on one side of the iOS 6 line, none on the
other. iBSS patches cleanly on every build that downloaded at all; the six
iBSS/DeviceTree "failures" are the iOS 4.x-era `8C*`/`8F*` builds Apple no
longer hosts, which the tool distinguishes in its note column.

One real gotcha this run exposed in the tool itself: three targets reported
"already built, skipping", carrying output from the earlier run made
*before* `-k` was restored. Staleness here is `--force`-only by design (the
inputs are Apple's immutable per-build components, so only this project's
own patch code changes the output) -- but that means **any change to the
patch flags or patch code requires `--force`**, or the sweep silently
reports stale successes. Worth remembering before reading any table.

It does not affect the real flow: `kJailbreakTargetBuild` is `10B329a`
(6.1.3-era), where `patch_kaslr()` applies cleanly, and every component the
jailbreak sends comes from that pinned build rather than from whatever the
device is running. So this is a sweep-coverage consequence, not a
functional one.

Left unconditional deliberately rather than re-introducing a version gate,
since a gate is exactly the kind of conditional that hid the `-t` problem.
If pre-6 AppleTV2,1 builds ever need to patch cleanly, the honest fix is a
`patch_kaslr()` that reports "nothing to do" for pre-6 iBoot instead of
failure — a change to the fork, not a caller-side heuristic.

## What `nand-enable-reformat=1` actually does, and a correction

An earlier note in this file justified omitting `nand-enable-reformat=1`
from the ramdisk boot-args on the grounds that it "must never appear here"
because the boot "plants files on the existing filesystem, it does not
reformat it". **That reasoning was wrong** and is corrected here rather than
edited away, since it was stated as fact without being checked.

What the arg actually does, researched properly:

- **It only AUTHORIZES a reformat. It does not perform one.** The format and
  filesystem imaging are done by `asr` running under `restored` during a
  real restore; the boot-arg merely permits that step. idevicerestore's
  `recovery_enter_restore()` sets it for every iOS restore
  (`rd=md0 nand-enable-reformat=1 -progress`, plus `-restore` on iOS 10+ and
  for macOS variants), and Apple's own PurpleRestore exposes it as a
  default in its "Restore Boot-Args" field.
- This project's ramdisk runs `entrypoint.c` as PID 1 and never starts
  `restored` or `asr`, so nothing would invoke a format and the arg would
  be **inert** here, not destructive.
- **It is genuinely load-bearing on some chips**, which is the opposite of
  the original claim: without it the restore ramdisk can fail to bring the
  flash stack up at all. SSHRD_Script appends
  `nand-enable-reformat=1 -restore` for exactly three CPIDs -- `0x8960`
  (A7), `0x7000` and `0x7001` (A8). This project's chip is `0x8947`, which
  is not among them, and no A5-specific requirement is documented anywhere.

So why leave it out? Because the closest real-world reference for this exact
job leaves it out. **Legacy-iOS-Kit uses two different boot-arg sets on
32-bit devices**, and the split maps precisely onto the distinction that
matters here:

- SSH-ramdisk flow (boot a ramdisk, work on the existing filesystem -- the
  same shape as what blackb0x does):
  `rd=md0 -v amfi=0xff amfi_get_out_of_my_way=1 cs_enforcement_disable=1 pio-error=0`
- Restore/downgrade flow: adds `nand-enable-reformat=1`.

`kRamdiskBootArgs` is that SSH-ramdisk string modulo argument ordering,
which is a useful independent check on a value that had been arrived at from
first principles.

**If `entrypoint.c` ever boots but cannot see or mount the data partition,
adding `nand-enable-reformat=1` is a cheap first thing to try** -- it cannot
format anything without `asr`, and the only evidence against it is that the
32-bit reference tooling does not use it for this case. Also worth knowing
from the same source: 32-bit devices on iOS 9+ are documented as sometimes
having trouble mounting `/dev/disk0s1s2` at all, with `fsck_hfs -f` as the
workaround.

### The real defect this turned up

`sendFileThenCommand()` **ignored `irecv_send_command()`'s return value** for
its `extraCommandBeforeMain`. That was fine when the only user was
`getenv ramdisk-delay` (fire-and-forget, as in real idevicerestore), but
`setenv boot-args ...` now goes through the same hook, and it is the only
thing putting `rd=md0` and the AMFI/code-signing args in front of the
kernel now that `patch_boot_args()` is gone.

**Correction.** The sentence above previously ended by asserting that the
`setenv` was "the only thing putting `rd=md0` and the AMFI/code-signing args in
front of the kernel." It was putting **nothing** in front of the kernel.
AppleTV3,2's iBoot never reads the `boot-args` environment variable on its
kernel-boot path, so that command always succeeded and always did nothing, and
the fallback described in the next paragraph was not a fallback — it was the
only string the kernel ever saw. Boot-args are baked into iBEC again
(`iBoot32Patcher -b`); see "`setenv boot-args` is INERT on this bootloader" at
the end of this file. `sendKernelCache()` sends no `setenv` at all now, so the
`extraCommandMustSucceed` flag below has no user on the jailbreak path — it is
kept because the hazard it guards (a fire-and-forget hook silently swallowing a
real failure) is exactly the class of bug this whole episode turned out to be.

A silent failure there is not destructive -- iBoot would fall back to its own
compiled-in `rd=md0 nand-enable-reformat=1 -progress`, which formats nothing
without `asr` -- but that default has **no** `amfi=0xff` or
`cs_enforcement_disable=1`, so `entrypoint.c` could not exec as PID 1 and
the boot would fail with nothing in the log to explain why. (Read that last
sentence again in light of the correction above: it is an accurate description
of what was happening on **every** run, not of a hypothetical silent failure.
The reasoning was right and only the trigger was wrong.) There is now an
`extraCommandMustSucceed` flag, set only by `sendKernelCache()`; the
`getenv ramdisk-delay` callers keep their fire-and-forget semantics.

## Answering "did you gate `-k` for everything <6?": no, and the better fix

The answer was **no** -- `-k` was passed unconditionally, and pre-iOS-6 iBEC
patching therefore hard-failed. That is fixed now, but in the fork rather
than with a caller-side version gate.

`patch_kaslr()` (regulad/iBoot32Patcher@`blackb0x`, commit `22a114a`) now
distinguishes two cases it previously collapsed into one failure:

- `os_vers < 6`: **success, nothing to do.** KASLR did not exist before iOS
  6, so there is no slide-applying branch to NOP out. Reporting failure made
  any caller that asks for the patch unconditionally -- the only sane way to
  ask for it -- unable to patch an iOS 4/5-era iBEC at all, over a patch
  that was never meaningful there.
- unrecognized / newer: still a failure, kept deliberately distinct so
  "this tool has no KASLR patch for this iBoot" cannot become a silent
  no-op.

Fixing it here rather than in `Patcher.cpp` keeps the caller honest: it
still asks for KASLR unconditionally and the patcher answers truthfully for
the build in front of it, instead of the caller second-guessing iBoot
versions it cannot see.

### Full AppleTV2,1 sweep after the fix

29/29 targets, no crashes. iBEC now patches on all eight iOS 5-era builds
that previously failed (`9A334v`, `9A335a`, `9A336a`, `9A405l`, `9A406a`,
`9B179b`, `9B206f`, `9B830`).

**Exactly one genuine iBEC patch failure remains: `8M89`** (iBoot-931, the
only 4.x-era build in the set), and it is `patch_ticket_check`, not KASLR:

```
patch_ticket_check: Unable to find 3 iboot_str_3_xref!
main: Error doing patch_ticket_check()!
```

That is precisely what the old `path.find("4.")` heuristic -- the one
removed when `-t` became unconditional -- was working around. The comment
written at the time predicted this outcome and argued it was the better one:
failing loudly beats silently shipping an iBEC that still enforces a ticket
check. It now has a measured scope: one build, on a device/firmware this
project does not target.

The other six `FAIL` rows on that sweep are all iOS 4.x-era `8C*`/`8F*`
builds Apple no longer hosts at all, which the note column distinguishes as
download failures.

### The sweep had to be made crash-proof first

Two consecutive full AppleTV2,1 runs died partway through -- both at target
23 of 29 -- producing no summary at all, while every target they died on
patches fine when run by itself in a fresh process. A sweep drives the
vendored `decrypt()`/`patch_kernel()` code four times per target across
dozens of targets in one process, and that code does not tolerate it: it has
already produced one confirmed NULL-deref crash and carries a documented
history of heap corruption.

Rather than chase that through `third_party`, each target now runs in a
**forked child** (`bakeOneForked()`), reporting its result back over a pipe
as one line. A crash costs exactly one row instead of the run and everything
after it, and -- more usefully -- the parent sees the signal and prints
`CRASHED (signal N)` instead of the run just silently stopping. The child
exits via `_exit()` specifically so it does not flush inherited stdio
buffers and duplicate the parent's output.

This is what made a complete 29/29 table possible at all, so it is
load-bearing rather than defensive.

## gaster is removed: it never pwned an AppleTV3,2, on Linux or macOS

`gaster` is gone from the tree — submodule, CMake target, `--pwntool` flag, the
`regulad/gaster` fork, all of it. `blackb0x-pwn` is now the only pwntool, built
unconditionally on every platform.

**The reason is simply that it never worked.** Across the whole investigation
recorded in the sections above, gaster was never once able to put the one
available AppleTV3,2 into pwned DFU — not on Linux 7.1.x, and not on macOS 26.
No configuration, no instrumentation, no fork commit ever produced a successful
pwn on this hardware. Everything that follows is secondary to that single fact.

### Why the project adopted it, and why that reasoning failed

The original case for gaster was `AGENTS.md`'s standing convention: prefer a
real, maintained implementation over a hand-port. gaster is the tool palera1n's
`legacy` branch shells out to, it is open source, and it carries a config entry
for this exact device — `gaster.c`'s `checkm8_check_usb_device()` matches
`SRTG:[iBoot-1458.2]` and sets `cpid = 0x8947` with a full address table
(`config_large_leak`, `config_overwrite_pad`, `memcpy_addr`,
`usb_core_do_transfer`, …), plus sibling entries for the other A5s at `0x8950`
and `0x8955` and a dedicated armv7 payload path. So this was never a case of
gaster not covering the chip on paper. Note the contrast with **checkra1n**,
which genuinely has never supported A5-family chips at all (A7 and up only) and
was ruled out on those grounds much earlier — the two rejections have different
reasons and should not be conflated.

What the convention did not anticipate is that "someone else maintains it" and
"it works on this hardware" are different claims. gaster's A5/armv7 path is
present in source but, on the evidence here, unexercised: nothing in this
project's experience suggests anyone has landed checkm8 on an A5 with it.

### What was spent finding that out

The fork (`regulad/gaster@linux-reset-race`, twelve commits) carried: claiming
interface 0 with `libusb_set_auto_detach_kernel_driver()` instead of sending DFU
class requests unclaimed; a reverted experiment in skipping the post-`SETUP`/
`SPRAY` reset (disproved on real hardware — the reset is load-bearing, the device
wedged in `D` state inside `usb_reset_configuration` without it); per-stage
logging and a 60s per-reconnect timeout; `DEBUG_CANCEL_DELAY_US` and
`DEBUG_SETUP_FULL_PAD`; and the missing IOKit
`send_usb_control_request_async_{,no_data_}precise_cancel` implementations.

Three real, independent software causes were found and fixed along the way
(`apple_mfi_fastcharge` auto-binding in DFU mode; unclaimed interfaces; and a bug
in the interface-claim fix itself that gated success on a claim which a
zero-interface descriptor makes impossible). All three were genuine. **None of
them was the cause**, and the "Linux host-stack limitation" conclusion drawn after
them was subsequently retracted by the `usbmon` work in the section above.

The final measured position on gaster specifically: its SETUP stage requires the
1632-byte malformed control write to come back `USB_TRANSFER_STALL`, and on Linux
it *succeeds* instead — 300 of 384 attempts delivered the full payload, and it
stalled exactly once. gaster therefore spun in its own unbounded retry loop and
never left SETUP. That is a coherent description of a failure, not a fix, and no
further blind changes to its exploit-timing code were justifiable.

### What removing it costs, and what it doesn't

The one genuinely useful thing gaster provided at the end was a **control**: two
independent implementations failing differently on the same host is what proved
the failure is not a blanket "Linux cannot do checkm8" (gaster's equivalent
transfer *delivers*; `blackb0x-pwn`'s is refused). That contrast is already
measured and written down in the section above, so deleting the code does not
delete the finding.

The fork was **archived, not deleted** (`regulad/gaster`, read-only as of
2026-09-18). That matters more than it sounds: an archived GitHub repository is
still public and still cloneable, so `git submodule update --init` keeps working
for every commit before this one, and the twelve instrumentation commits stay
readable. Deleting it would have broken both, since `.gitmodules` on those commits
still points at that URL and no local copy of the fork survives on any dev machine.

The open question is unchanged and does not involve gaster: **what the overwrite
transfer does on a working macOS run.** See the tcpdump instructions in the
section above.

## The macOS wire capture is a dead end: XHC20 needs SIP off, and is broken anyway

The section above names "what that same transfer does on a working macOS run" as the
decisive outstanding measurement, and prescribes
`sudo ifconfig XHC20 up && sudo tcpdump -i XHC20 -w out.pcap` to get it. **That
procedure is not available on this project's macOS test machine, and probably not on
any current macOS.** Researched rather than assumed, because it was about to cost a
recovery-mode reboot:

- **SIP must be fully disabled.** Apple DTS (Quinn "The Eskimo!") on the Catalina USB
  capture thread: *"It seems that this support is now disabled by default. To get it
  back, you have to disable SIP."* The USB capture interfaces do not merely fail to
  open without it — they do not appear in `ifconfig` at all, which is why the usual
  symptom is "interface XHC20 does not exist" rather than a permission error. `sudo`
  is not a substitute; SIP is disabled only from Recovery via `csrutil disable`, and
  on Apple Silicon that additionally requires setting the boot policy to Reduced
  Security first.
- **And it appears broken even with SIP off.** A report on that same thread from
  August 2025: *"The 'sudo ifconfig XHC20 up' method doesn't seem to work anymore in
  macOS 15.6.1"*, posted with `System Integrity Protection status: disabled`. A
  January 2026 follow-up asking whether USB capture is possible at all on 15.x/26.x
  has no answer. Three Feedback requests for a narrower `csrutil` flag (FB7429319,
  FB8326129, FB14365299) are open, the oldest unanswered since 2020.
- **Apple Silicon has a separate, older defect** even when the interface does come up:
  all-zero payloads, reproduced under plain `tcpdump` as well as Wireshark.

Sources: Apple Developer Forums threads 124875 and 95380; Wireshark wiki
`CaptureSetup/USB`; `ask.wireshark.org` question 30854.

### Consequences for this project

`scripts/analyze_usbmon_checkm8.py`'s `DLT_USB_DARWIN` support is now **unverified and
unreachable**. It was written to read a capture nobody on this project can produce, and
its own header comment already flags the Darwin pseudo-header layout as a guess scored
against two candidates. Do not trust it, and do not spend time refining it, until
someone actually has a Darwin capture in hand. The Linux `DLT_USB_LINUX_MMAPPED` path
is unaffected and remains correct — that is the half that has produced every real
measurement so far.

The decisive question is unchanged: **does the overwrite transfer deliver its 1660
bytes on a working macOS run?** What changes is how it gets answered. Since the wire is
unavailable on the working side, the measurement has to come from inside the process,
which means instrumenting `blackb0x-pwn` itself and diffing its output across the two
platforms.

One thing makes that genuinely viable rather than a consolation prize, though it is
**not** the thing it first looked like. Checked against
`third_party/libirecovery/src/libirecovery.c` rather than assumed:

`iokit_usb_control_transfer()` (`:1420`) fills an `IOUSBDevRequestTO` whose `wLenDone`
field the kernel populates with the bytes actually moved — but the switch on the result
returns `req.wLenDone` **only** in the `kIOReturnSuccess` case, and collapses
`kIOReturnTimeout`/`kIOUSBTransactionTimeout` to a bare `IRECV_E_TIMEOUT`. So the count
is discarded on timeout on macOS too, exactly as it is on Linux, where the libusb branch
(`:1448`) is a straight `libusb_control_transfer()` returning `LIBUSB_ERROR_TIMEOUT`.
`Checkm8Pwn.c`'s comment that "how far this got is only visible on the wire" is
therefore accurate **as the code stands on both platforms** — it is not a Linux-only
limitation, and there is no free number waiting to be printed.

What makes it recoverable is that the partial count exists just below each wrapper and
**this project already forks libirecovery**. IOKit's `req.wLenDone` is a live struct
field after a timed-out `DeviceRequestTO`, and libusb's own sync control wrapper
likewise has `transfer->actual_length` before it throws the value away. Surfacing both
is a small, additive change to a fork already carrying async-cancel fixes, and it yields
the same measurement on both platforms rather than one privileged side — which is
strictly better for the diff this is all for.

The caveat to respect: on timeout `wLenDone` reflects what the host controller believes
it sent, which is not automatically what the device accepted. That is the precise gap
the usbmon capture closes on Linux, and it is why the Linux-side numbers stay the
reference for anything the two disagree about.

## The overwrite was never the problem: a working macOS trace refutes the whole analysis

`DEBUG_TRACE_TRANSFERS` (added because macOS will not give up the wire) produced the
first side-by-side of a **successful** macOS run against a failing Linux one. The result
retracts most of the two sections above.

| | macOS (**succeeds**, `PWND:[checkm8]`) | Linux (fails) |
|---|---|---|
| bug setup consumed | **0** of 2048 | 64 of 2048 |
| `OVERWRITE` ret | −10 (`IRECV_E_PIPE`) | −9 (`LIBUSB_ERROR_PIPE`) |
| `OVERWRITE` moved | **0** | **0** |
| `payload-upload` moved | **0** | **678** |

### Retraction 1: a stalled overwrite moving 0 bytes IS success

The section "Where this leaves the platform conclusion" treats the overwrite being
refused as *the* failure, and builds an arithmetic argument on top of it: acceptance
needs the groom above 320, fitting 1660 bytes needs it at or below 388, the device
treats 1472+ as a continuation of the same 2048-byte buffer, therefore "no setting
satisfies both" and no cancel delay can help.

**Every step of that reasoning rests on the premise that the overwrite is supposed to
deliver its bytes, and it is not.** The macOS run that pwns the device stalls the
overwrite having moved **zero bytes**, identically to Linux. The acceptance window, the
320/388 conflict, and the "structurally impossible" conclusion describe a non-symptom.
Disregard them.

### Retraction 2: the in-code assertion is backwards

`Checkm8Pwn.c` prints `want 0 < n <= 1632` for the bug-setup count and guards on
`sent > config.overwrite_offset`. macOS succeeds with **n = 0**, outside that range.
Linux fails with n = 64, inside it. Whatever that range meant, it does not describe the
working configuration.

### The one real divergence, and a cheap oracle

`payload-upload` is the only row where `moved` differs: **0 on the successful run, 678
on the failing one.** The reading that fits: after a successful overwrite the device is
running the injected handler and never ACKs the data stage, so nothing moves; on Linux
it is still running stock DFU and absorbs all 678 bytes.

That makes `payload-upload moved != 0` a **success/failure oracle available before the
final reset**, much earlier than the closing PWND check, and it is the fastest way to
tell "the overwrite did not take" from "the overwrite took and something later broke."

### Ruled out: the cancel delay, conclusively this time

The obvious hypothesis from the table was that Linux's 64 consumed bytes were the
problem -- Linux is measurably *faster* at every stage (`bug-setup/abort` 378us vs
738us, `groom1/stall` 327us vs 422us), so the data stage plausibly beats the 100us
cancel window on Linux and not on macOS. The previous sweep had only ever gone upward
from 100us (its default range is `100,250,...,3000`), so downward was untested.

**Swept 0,10,20,30,40,50,60,75,90: every single one consumed 0 bytes -- exactly matching
macOS -- and every single one still failed.** Nine for nine, device wedged each time.

So the bug-setup count is reproducible on demand and is *not* the differentiator. The
cancel delay is now ruled out in both directions, on the correct success criterion
(PWND). The sweep's closing hint, "the abort is still landing before the host controller
starts the data stage -- try larger delays", is actively misleading: landing before the
data stage is the macOS behaviour and is what we want.

### Where the failure actually is, and the next suspect

With consumed == 0 and the overwrite stalling at 0 bytes -- both now matching macOS --
Linux still dies at `checkm8: device did not reappear after payload execution`, and the
device is left **wedged**: off the USB bus entirely, needing a power cycle, not merely a
failed reconnect.

That points at the post-payload reset, where `irecv_reset()` (`libirecovery.c:2548`)
does genuinely different things per backend:

- **IOKit** calls `ResetDevice()` **and then `USBDeviceReEnumerate(handle, 0)`**, and
  explicitly tolerates `kIOReturnNotResponding` from both -- precisely what a device
  executing injected SecureROM code would return.
- **libusb** calls bare `libusb_reset_device()` and discards the return value.

**Correction, after checking libusb and the kernel rather than assuming:** an earlier
version of this entry claimed the libusb path does "no re-enumerate". That is wrong.
`libusb_reset_device()` is `USBDEVFS_RESET`, which reaches the kernel's
`usb_reset_and_verify_device()`; that re-reads the device, config and serial
descriptors and, if `descriptors_changed()`, takes `goto re_enumerate` ->
`hub_port_logical_disconnect()`, logically disconnecting and re-adding the device.
libusb documents the userspace half: if descriptors change "the device will appear to
be disconnected and reconnected... the device handle is no longer valid", reported as
`LIBUSB_ERROR_NOT_FOUND`. Since checkm8's payload appends `PWND:[checkm8]` to the
serial string, that is exactly the branch taken. So Linux does re-enumerate, and
libirecovery discarding the return value is harmless here because the caller closes
and re-discovers immediately afterwards regardless.

What survives is narrower, and is a weaker suspect than first written:

- IOKit's `USBDeviceReEnumerate()` is **unconditional** -- it terminates the IOUSBDevice
  nub and re-enumerates as if the device were physically replugged, every time. Linux
  re-enumerates **only if** `descriptors_changed()`; otherwise it restores the previous
  configuration and keeps the same device instance.
- IOKit's explicit tolerance of `kIOReturnNotResponding` says Apple's path *expects* an
  unresponsive device at this point. Nothing equivalent is expressed on the libusb side.

Whether either difference actually matters here is **not established**, and the
mechanism originally asserted for it was false. Treat this as a lead, not a diagnosis.

(Tangentially confirmed while checking: `libusb_reset_device()` is reportedly broken on
macOS itself since OS X 10.11, with the suggested fix being Apple's own
`usbDeviceReEnumerate`. Irrelevant to this project -- the macOS build uses libirecovery's
IOKit backend and never goes through libusb -- but worth knowing before anyone proposes
"just use libusb everywhere" as a way to remove the asymmetry.)

Sources: libusb API docs (`libusb_reset_device`); Linux `usb_reset_and_verify_device()`
/ `descriptors_changed()`; libusb issue #455.

**The decisive next measurement is one run**: Linux at any delay <= 90us with
`DEBUG_TRACE_TRANSFERS=1`, reading `payload-upload moved`.

- `moved == 0` -> the overwrite took and the payload ran; the failure is purely reset /
  re-enumeration, and the fix is in `irecv_reset()`'s libusb branch.
- `moved == 678` -> the overwrite still is not taking, the reset is a red herring, and
  the divergence is somewhere in the grooming that the per-stage timings do not capture.

Do not skip this. Both branches above are plausible and they lead to completely
different places.

## The overwrite is not landing at all, and the two "cancel" mechanisms are not equivalent

The oracle from the previous entry was run: Linux, `DEBUG_CANCEL_DELAY_US=50`,
`DEBUG_TRACE_TRANSFERS=1`. Result: **`payload-upload moved = 678`.**

That is the "the overwrite did not take" branch. The device is still running stock DFU
when the payload arrives and absorbs all 678 bytes into its buffer; on macOS it absorbs
0 because the injected handler is already running by then. So:

- **The post-payload reset is a red herring.** It is downstream of an overwrite that
  never landed. The `irecv_reset()` asymmetry recorded in the previous entry stands as a
  code difference but is not the cause of this failure. Stop looking there.
- The reconnect pattern also shifted, worth noting: 23 x "Unable to find device" then 7
  x "Unable to connect to device". The device *does* come back on the bus and then
  cannot be opened -- the `bNumInterfaces 0` descriptor corruption documented earlier,
  not a device that is simply absent.

### Everything measurable now matches macOS, and it still fails

At `DEBUG_CANCEL_DELAY_US=50` the Linux trace matches the working macOS trace on every
value the instrumentation can see: bug setup consumed 0, `OVERWRITE` stalls with
`moved 0`, both grooms stall/time out as expected, per-stage timings within noise
(Linux slightly faster throughout). The only differing row, `payload-upload moved`, is a
*consequence* of the failure, not a cause.

So the divergence is in something none of these numbers capture.

### The prime suspect: "consumed 0" means two different things

The bug setup does not go through the traced control path. It is
`irecv_async_usb_control_transfer_with_cancel()`, and the two backends implement
"submit, wait, cancel" with genuinely different primitives:

| | macOS (IOKit) | Linux (libusb) |
|---|---|---|
| submit | `DeviceRequestAsync()` | `libusb_submit_transfer()` |
| cancel | `USBDeviceAbortPipeZero()` | `libusb_cancel_transfer()` -> `USBDEVFS_DISCARDURB` -> `usb_unlink_urb` |

`USBDeviceAbortPipeZero()` aborts **a pipe**, on a request the controller has already
put on the wire: the device saw the `DFU_DNLOAD wLength=0x800` SETUP, prepared a buffer,
and then had its data stage abandoned. That dangling buffer *is* checkm8's precondition.

`USBDEVFS_DISCARDURB` on a URB that has not yet reached the controller simply dequeues
it, and **the SETUP never transmits**. The device sees nothing and allocates nothing.

Both report `consumed = 0`. The byte counter cannot distinguish "SETUP delivered, data
stage aborted" from "request never left the host". This would explain the whole shape of
the evidence: why reproducing macOS's `consumed = 0` on Linux changed nothing, and why
Linux jumps from 0 bytes at 90us straight to 64 bytes at 100us with no window in
between -- there may be no delay at which Linux transmits the SETUP and then stops
before the first data packet.

**This is a hypothesis, not a finding.** It is consistent with every measurement so far
and it names a real, documented difference in primitives, but nothing yet observes the
device's side of it.

### What was added to test it

`DEBUG_DFU_STATUS=1` queries `DFU_GETSTATUS` (0xA1, 3) at four points -- before/after the
bug setup, before/after the overwrite -- and prints the device's own decoded
`bStatus`/`bState`. The discriminator is the state immediately after the bug setup:

- `dfuDNLOAD_IDLE` / `dfuDNBUSY` -> the device is holding a buffer; the SETUP arrived and
  the precondition exists. Hypothesis refuted, look elsewhere.
- `dfuIDLE` -> the device never saw the request. Hypothesis confirmed, and the fix is to
  make the libusb path guarantee SETUP transmission before cancelling.

**`DFU_GETSTATUS` is not passive**: it advances the DFU state machine (`dfuDNLOAD_SYNC`
becomes `dfuDNBUSY`/`dfuDNLOAD_IDLE` by the act of asking). A run with the flag set is
not evidence about a run without it and may break an exploit that would otherwise
succeed. Set it on **both** platforms, compare, and turn it back off -- unlike every
other `DEBUG_` knob here, this one deliberately departs from the macOS-confirmed path.

The bug-setup call is also now timed (`call took N us`, printed alongside the consumed
count). It is not in the trace table because it does not go through the traced path. A
call returning in barely more than `delayUs` never round-tripped anything, which is weak
corroboration for the same hypothesis.

Finally, the `want 0 < n <= 1632` wording on that line is corrected in place: it now says
the range is **not** the success condition, because macOS succeeds outside it.

## Confirmed: on Linux the bug-setup SETUP never reaches the device

The `DEBUG_DFU_STATUS` probe answered it in one run, and the answer does not need the
macOS side for comparison:

```
dfu-status @ before bug setup  -> bStatus 0 (OK), bState 2 (dfuIDLE), bwPollTimeout 50 ms
bug setup: cancel delay 50 us -> device consumed 0 of 2048 bytes, call took 415 us
dfu-status @ after bug setup   -> bStatus 0 (OK), bState 2 (dfuIDLE), bwPollTimeout 50 ms
```

**`dfuIDLE` after the bug setup is conclusive.** Per the DFU spec, `DFU_DNLOAD` with
`wLength > 0` from `dfuIDLE` moves the device to `dfuDNLOAD_SYNC`, and `GETSTATUS` from
there returns `dfuDNBUSY` or `dfuDNLOAD_IDLE`. A device that had seen and accepted the
request cannot report `dfuIDLE`; a rejection would be `dfuERROR`. The probe runs
*before* the `DFU_ABORT` (0x21, 4) that follows the bug setup -- checked, not assumed --
so this is not the abort resetting the state either.

The device never processed the download. The SETUP never reached it.

So **checkm8's precondition has never existed on Linux.** The dangling partial buffer is
the entire point of the bug setup, and there is no buffer. Both grooming passes, the
overwrite and the payload upload have all been operating on an ungroomed heap. This
retroactively explains every earlier "all the numbers match macOS and it still fails"
result: the numbers that matched were all downstream of a precondition that was absent.

The other two probes returning `-7` is expected, not a defect: by then the grooming has
deliberately stalled and leaked EP0, so `GETSTATUS` times out. No information either way.

### Where the working window must be

Two measurements bracket the transition:

| cancel delay | consumed | device state after |
|---|---|---|
| <= 90us | 0 | `dfuIDLE` (confirmed at 50us) -- SETUP never sent |
| 100us | 64 | SETUP sent **and** first data packet landed |

The state actually wanted -- SETUP delivered, **zero** data bytes consumed, device in
`dfuDNLOAD_SYNC` holding a buffer -- lies between them. That is also what macOS achieves:
it reports `consumed 0` *and* pwns the device, which is only coherent if its device saw
the SETUP. `USBDeviceAbortPipeZero()` aborts a pipe whose SETUP the controller has
already put on the wire, so IOKit gets that state by construction; libusb's
`USBDEVFS_DISCARDURB` has to race for it.

### This is probably a race, not a threshold

The SETUP transmits on a frame/microframe boundary whose phase relative to
`libusb_submit_transfer()` is arbitrary. If so, a fixed delay near the boundary lands in
the window only some of the time, and the same delay will succeed intermittently rather
than never or always.

That reframes a standing oddity: every serious checkm8 implementation retries
unboundedly (gaster's own RESET/SETUP/SPRAY/PATCH state machine restarts on any stage
failure), while standalone `blackb0x-pwn` makes exactly one attempt. A racy precondition
plus a single attempt looks indistinguishable from deterministic breakage -- which is
what this investigation has been treating it as.

**Next, in order of cost:**

1. Retry the same delay near the boundary many times (~60 runs at 95us) and look for any
   run that reports `consumed 0` and then succeeds, or a nonzero consumed below 64.
2. With `DEBUG_DFU_STATUS=1`, sweep 91-99us looking for `bState` after the bug setup that
   is anything other than `dfuIDLE`. Remember the probe perturbs the state machine, so
   use it to locate the window, then re-run without it.
3. If the window proves unreachable by timing alone, the fix belongs in
   `irecv_async_usb_control_transfer_with_cancel()`'s libusb branch: it must guarantee
   the SETUP is on the wire before cancelling, which `libusb_submit_transfer()` +
   `usleep()` + `libusb_cancel_transfer()` structurally cannot.

### Making it loopable: retry the bug setup, not the whole exploit

The retry experiment above was blocked by a practical problem: the device wedges on
**every** failed run and needs a physical power cycle plus manual DFU re-entry, so
"just run it 60 times" is 60 manual interventions rather than a loop.

The confirmation above dissolves that. The run fails at the *first* step, and everything
destructive happens after it -- the second grooming pass, the malformed 1660-byte
overwrite, the payload upload, and the payload-executing reset. Those are what corrupt
the device's descriptors and wedge it. Running them when the precondition is known
absent is pure cost.

And a *failed* bug setup leaves the device pristine: it never saw the SETUP, so it is
still in `dfuIDLE` with nothing allocated. Retrying therefore needs no reset, no power
cycle, and no manual DFU re-entry.

`DEBUG_BUGSETUP_RETRIES=N` (default 1, the original single-shot behaviour) retries the
bug setup in-process until `readDfuState()` reports anything other than `dfuIDLE`,
sending `DFU_ABORT` between attempts to keep the device in a known-clean idle. If all N
attempts lose the race it prints that plainly and **exits without running any of the
destructive stages**, leaving the device still enumerable for the next attempt.

This is the same shape as gaster's own per-stage retry -- its RESET/SETUP/SPRAY/PATCH
machine restarts a stage on failure rather than pressing on -- which standalone
`blackb0x-pwn` never had.

Cost, stated honestly: one `DFU_GETSTATUS` per attempt, and GETSTATUS advances the DFU
state machine. On the attempt that finally wins, it moves `dfuDNLOAD_SYNC` to
`dfuDNBUSY`/`dfuDNLOAD_IDLE`. Polling GETSTATUS after a DNLOAD is exactly what a
spec-compliant DFU host does, so it is unlikely to destroy the dangling buffer -- but
that is **unverified**, and it is why this is opt-in rather than the default. If a run
reports the buffer established and then still fails at the overwrite, this probe is the
first thing to suspect.

### The race is confirmed non-deterministic, and the first retry loop had two bugs

First run of `DEBUG_BUGSETUP_RETRIES=500`:

```
bug setup: attempt 3 established device state -1 (status request failed) -- proceeding
bug setup: cancel delay 100 us -> device consumed 0 of 2048 bytes, call took 332 us, attempts 3
Failed to send abort.
```

**The headline is the consumed count.** At `DEBUG_CANCEL_DELAY_US=100` this run consumed
**0** bytes; the earlier single-shot run at exactly the same 100us consumed **64**. Same
delay, same host, same device, different outcome. The bug setup is genuinely
non-deterministic, which is direct evidence for the frame-boundary race rather than a
timing threshold, and it retires the implicit assumption behind every sweep run so far:
that a given delay produces a repeatable result. It does not. Any future sweep needs
repeats per delay, not one sample.

Two real bugs in the loop, both now fixed:

- **`-1` was treated as success.** `readDfuState()` returns -1 when the `GETSTATUS`
  request itself fails, which means EP0 has degraded and the device state is unknown --
  emphatically not "a buffer exists". The loop proceeded on it, which is the single
  condition where proceeding is least defensible. It now stops and reports, leaving the
  destructive stages unrun.
- **The inter-attempt `DFU_ABORT` was harmful.** It was sent to "keep the device in a
  known-clean idle", but the loop only retries when the device is *already* `dfuIDLE` --
  that being the condition it retries on -- so the abort was redundant by construction.
  SecureROM's DFU is a minimal implementation and need not handle an ABORT from
  `dfuIDLE` the way the spec describes; EP0 died within three iterations. Removed, plus
  a settle pause between attempts (`DEBUG_BUGSETUP_RETRY_DELAY_US`, default 2000us).

Note the run did **not** wedge the device in the destructive sense: `Failed to send
abort.` aborts before the second groom, the overwrite and the payload upload, so nothing
ran against the ungroomed heap. The degradation was EP0-level from the retry loop
itself, not the exploit's own corruption.

The loop now also reports how many attempts got the device to consume anything, which
turns each run into a sample of the race's hit rate rather than a single pass/fail.

## RETRACTED: "the SETUP never reaches the device". dfuIDLE is not diagnostic

The entry "Confirmed: on Linux the bug-setup SETUP never reaches the device" is **wrong**
and should not be relied on. Its conclusion was drawn from `bState == dfuIDLE` after the
bug setup, reasoning from the DFU spec that a device which had accepted a `DFU_DNLOAD`
could not report idle.

The refutation came from the retry loop built on top of it:

```
bug setup: attempt 35 -- DFU status request failed, ... Stopping before the destructive stages.
bug setup: 35 attempts, 32 of them got the device to consume bytes
```

**32 of 35 attempts moved bytes, and every one of them still reported `dfuIDLE`** --
otherwise the loop would have broken out and proceeded. Bytes moving proves the SETUP
reached the device and a data stage ran. So `dfuIDLE` plainly does not mean "the device
never saw the request".

### Why the spec reasoning was invalid here

checkm8 **is a bug in this state machine.** The aborted `DFU_DNLOAD` leaks a heap buffer
that the DFU state machine no longer tracks; the device returning to `dfuIDLE` while a
dangling pointer survives is not an anomaly to be explained away, it is the
vulnerability. Reasoning "a compliant device could not report idle here" against a
device whose non-compliance is the entire exploit was a category error.

`DFU_GETSTATUS` reports what the state machine believes. The exploit lives in what it
failed to record. **There is therefore no known oracle at the bug-setup stage**, and
`DEBUG_DFU_STATUS` cannot supply one -- it remains useful only for gross states like
`dfuERROR`, not for confirming the precondition.

### What actually survives from that work

- **The bug setup is non-deterministic.** 0 vs 64 bytes consumed at an identical 100us
  delay, and now 32/35 attempts consuming bytes within a single run at that same delay.
  Any sweep taking one sample per delay is measuring noise; repeats are mandatory.
- **Repeated bug setups degrade EP0 within roughly 35 attempts**, ending in
  `GETSTATUS` failing outright. Unbounded in-process retry of this stage is not viable
  without a reset between attempts, which is what the loop was built to avoid.
- **The consumed count is observable and controllable**, which is the one lever the race
  actually offers.

### What this does NOT restore

It does not revive the overwrite-acceptance analysis or the "host-stack limitation"
verdict; both remain retracted on independent evidence. It also does not reinstate the
`irecv_reset()` lead, which died on `payload-upload moved = 678`.

The honest state: the divergence between a working macOS run and a failing Linux one is
**not currently localised**. Everything observable matches. Three successive attempts to
name the cause (host stack, overwrite acceptance, SETUP delivery) have each been refuted
by later measurement, and the pattern in all three is the same -- a confident mechanism
proposed from partial instrumentation, then contradicted once better instrumentation
existed. The next candidate explanation should be treated with that history in mind.

### The retry loop, repurposed around the one observable that is real

`DEBUG_BUGSETUP_RETRIES` no longer retries on DFU state -- that premise is retracted
above. It now pairs with `DEBUG_BUGSETUP_TARGET_CONSUMED=N`: retry the bug setup until
its consumed-byte count is exactly N, then proceed. `DFU_GETSTATUS` is gone from the
loop entirely, both because it is not diagnostic here and because it was contributing to
the EP0 degradation that ends the run after ~35 attempts.

This is worth having only because the count is genuinely non-deterministic at a fixed
delay. It lets a run ask a question single-shot runs cannot pose reliably: does
"consumed exactly 0, the value a working macOS run reports" plus everything downstream
actually succeed? Earlier single-shot runs at <=90us reported consumed 0 and failed, but
each was one sample of a race, so that is weak evidence rather than a refutation.

If the target is never hit within the retry budget the run proceeds anyway and says so.
There is no oracle to gate on, and silently not running the exploit would be worse than
running it and reporting what happened.

## Consumed-count matching is not sufficient either; and the grooming has never been verified on the wire

`DEBUG_BUGSETUP_TARGET_CONSUMED=0` hit its target on the first attempt, proceeded, and
failed exactly as before -- overwrite stalled with `moved 0`, device did not reappear
after payload execution. So reproducing macOS's `consumed 0` is **not sufficient**, and
that avenue is closed.

Current state, stated plainly: **every observable matches between the working macOS run
and the failing Linux run.** Consumed count, overwrite return and moved count, both
grooming passes, per-stage timings. The only differing row, `payload-upload moved`, is
downstream of the failure. Four proposed causes have now been refuted by later
measurement (host-stack limitation, overwrite acceptance, SETUP delivery, consumed
count). No fifth mechanism is proposed here.

### The unexamined observable

Everything measured so far is the *host API's* view: what `irecv_*` returned. Nobody has
checked what the **626 grooming leaks actually do on the wire**, and there is specific
reason to suspect them now.

`usb_req_leak()` is `irecv_usb_control_transfer(..., 0x40 bytes, timeout 1ms)`, and the
leak works precisely because the device allocates a buffer for that request and the host
never completes it. But libusb implements a control-transfer timeout by **cancelling the
URB** -- the same `USBDEVFS_DISCARDURB` mechanism whose behaviour at the bug setup turned
out to be racy and non-deterministic. If some fraction of those 626 requests are
discarded before their SETUP reaches the wire, the device allocates nothing for them and
the heap groom is quietly incomplete. Every one of them would still report
`LIBUSB_ERROR_TIMEOUT`, which is exactly what the trace shows, and exactly what the code
checks for. The trace cannot tell a leak that happened from a leak that never left the
host.

IOKit's timeout path is a completion timeout on a request already submitted to the
controller, which does not have this failure mode.

This is **not** a proposed cause -- it is an observable nobody has looked at, which the
existing instrumentation structurally cannot see, and which plausibly differs between
the platforms for a now-demonstrated reason.

### How to look

`usbmon` works on Linux and `scripts/analyze_usbmon_checkm8.py`'s
`DLT_USB_LINUX_MMAPPED` path is the half of that script that has produced every real
measurement so far (the Darwin half remains unreachable, see above):

```
sudo tcpdump -i usbmon<BUS> -w /tmp/pwn.pcap        # bus from the kernel log: "usb 3-3" is bus 3
DEBUG_TRACE_TRANSFERS=1 sudo -E ./build/blackb0x-pwn checkm8 --ecid <ecid>
python3 scripts/analyze_usbmon_checkm8.py /tmp/pwn.pcap
```

The number to read is how many `0x80 0x06 wValue=0x0304 wIndex=0x040A` SETUPs actually
appear on the bus. If it is 626, the groom is real and this is another dead end. If it is
materially fewer, the heap was never groomed as intended and every downstream stage --
including the overwrite that "stalls" identically on both platforms -- has been operating
on a heap that does not match what the exploit assumes.

Either answer is worth having, and unlike the macOS-side questions, this one is
answerable on the hardware actually available.

## Linux support removed

Called by the user after the investigation above ran out of leads. Everything non-Apple
is gone: `CMakeLists.txt` now hard-fails off Apple rather than dying deeper inside a
vendored ExternalProject, and the Linux branches are deleted from `BakeRamdisk.cpp`
(the whole loop-mount path), `DeviceManager.cpp` (`/proc/<pid>/status` D-state probe,
the unprefixed-only `stdbuf` search), `ResourcePath.cpp` (`/proc/self/exe`), and
`Cli.cpp` (the `geteuid()` self-bake gate). `libirecovery` is configured
`--with-iokit` unconditionally.

**Why:** checkm8 never worked on Linux. The exploit sequence runs to completion and the
task-struct overwrite never lands. Four explanations were proposed and each refuted by
later measurement, and by the end every observable matched a working macOS run while the
outcome still differed. The sections above carry each dead end and why it died; the
detail exists so nobody repeats those experiments.

### What was deliberately kept

- **The 64 MiB ramdisk hard-fail** (`DEBUG_RAMDISK_LIMIT_MIB`). Not platform-specific:
  the ceiling was confirmed against the live AppleTV3,2, and it is *unenforceable at
  runtime* because `ramdisk-size` does not exist on 32-bit iBoot at all (zero
  occurrences in the decrypted iBECs of AppleTV2,1/iBoot-1537.9.55 and
  AppleTV3,2/iBoot-1458.2, neither of which carries the older
  `kRamdiskMaxSize`/"Ramdisk too large" mechanism). The bake-time check is the only
  guard that exists.
- **`DEBUG_TRACE_TRANSFERS` and `DEBUG_DFU_STATUS`.** Platform-neutral, off by default,
  and the only reason any of this was measurable. Deleting them would make resuming
  expensive.
- **`libusb`**, still vendored. It is cross-platform, builds fine on macOS, and
  unpicking it from the link graph is an unverifiable change (see below).

### What was removed beyond the platform branches

`scripts/analyze_usbmon_checkm8.py` and `scripts/sweep_pwn_cancel_delay.py` (both
Linux-only investigation tools for a closed investigation), and
`DEBUG_BUGSETUP_RETRIES`/`DEBUG_BUGSETUP_TARGET_CONSUMED` with their `readDfuState()`
helper — built to chase the consumed-count avenue, which closed.

### Two consequences to be honest about

1. **Ramdisk baking is now unverified end to end.** The Linux loop-mount path was the
   one that worked — it produced every `dist/` ramdisk and the 29/29 patch sweep. The
   surviving `hdiutil` path has never been run on real macOS; it was written with no Mac
   available. This was raised before the removal and the decision was reaffirmed, so it
   is accepted cost, not oversight. `BakeRamdisk.cpp`'s own caveat and `.claude/TODO.md`
   item 4a still flag it.
2. **None of this was build-verified.** The removal was performed on a Linux host, which
   the change itself makes unable to configure the project. Verification was structural
   only: preprocessor nesting balance re-checked to depth 0, no orphaned `static`
   functions, no remaining invocations of `blkid`/`mkfs.hfsplus`/`mount`/`losetup`, and
   `-fsyntax-only` on the two platform-independent `Pwn/` translation units. **The first
   real macOS build is the actual test.** The most likely failure sites are
   `BakeRamdisk.cpp`, where 309 lines were removed mechanically, and the `hdiutil`
   codepath that has never run anywhere.

### Two vendored libraries dropped with it

`libusb` and `p0sixspwn` are gone from `third_party/`, taking the submodule count from
19 to 17 (`gaster` was the other, earlier).

**`libusb` was the one genuine casualty of dropping Linux.** Four independent checks, so
nobody has to re-derive this: nothing under `Blackb0x/` or `entrypoint/` includes
`<libusb.h>` (every hit is a comment); libirecovery's `configure.ac:114-126` only reaches
`PKG_CHECK_MODULES(libusb, ...)` in the branch *not* taken when `--with-iokit` is passed
and IOKit is present, which is now unconditional; `libirecovery-1.0.pc.in` declares no
`Requires` at all, so nothing propagates transitively; and xpwn's own libusb-dependent
targets were already being skipped (`libusb is required for dfu-util!` /
`libusb is required for xpwn!` in its configure output) with `pwnmetheus2` disabled in
our fork. No other vendored dependency wants it either — checked
`libusbmuxd`/`libimobiledevice`/`libimobiledevice-glue`/`libplist`/`libtatsu`/
`libfragmentzip`, where every apparent hit is a `libusbmuxd` substring match.

Removed with it: `libusb_ext`, the `deps::usb` imported target and the
`INTERFACE_INCLUDE_DIRECTORIES` hack that existed because libusb installs its header to
`include/libusb-1.0/` while consumers `#include <libusb.h>` unprefixed, the
`file(MAKE_DIRECTORY ${DEPS_INCLUDE}/libusb-1.0)` that existed only so CMake could
validate that property at configure time, and the `--disable-udev` rationale block (a
Linux static-linking concern with no meaning on Darwin).

**`p0sixspwn` was never wired up at all** — not a Linux casualty, just dead weight that
predated this. Zero CMake references, and nothing anywhere read `third_party/p0sixspwn`:
`stageP0sixspwn()` runs `ar x` against the real
`com.ih8sn0w-squiffy-winocm.p0sixspwn_1.4-1_iphoneos-arm.deb` in `Blackb0x/Debs/`. It was
added in `c67038e`, the same commit that added that `.deb` and deleted
`Blackb0x/Files/p0sixspwn.tgz`, so it looks like a reference checkout that never became a
build input. Note `.claude/TODO.md` item 3 ("Reimplement p0sixspwn's postinst in
`entrypoint.c`") is still open and that source is exactly what it would want — re-add the
submodule if that work starts, rather than treating its absence as a decision about item 3.

**Verified still needed**, recorded so nobody prunes them on a guess: `libgeneral`
(`libfragmentzip.c:16` includes `<libgeneral/macros.h>`), `libpng` and `bzip2`
(`ibootim.c` uses `png.h`, `bspatch.c` uses `bzlib.h`, both compiled into
`add_library(xpwn ...)` which `blackb0x` links), `zlib` (libzip/libpng/curl), and both
`libplist` copies (deliberate — see above).

One cosmetic leftover: the libirecovery fork's branch is still named
`libusb-async-cancel-fix`, after libusb fixes this project no longer compiles. Renaming a
pushed branch that `.gitmodules` pins is not worth the churn.

## Stock diagnostic paths after the decryption strip: prep bugs, the stock-iBSS decrypt, RestoreLogo, and the --stock-firmware-old/-new split

Revisiting the boot failure after the library-vendoring work, three separate
things were wrong with the `--stock-*` diagnostic routes, all consequences of
the earlier decision to strip every host-side `decrypt()` call out of the
`blackb0x` jailbreak binary (crypto now lives only in `bake-firmware`; see the
Patcher/PatcherPatch split above). None of these existed while `blackb0x`
still decrypted stock components itself — they only surfaced once it stopped.

**1. `--stock-firmware --stock-recovery` failed at prep with "missing
DeviceTree."** The bake gate in `downloadAndPatchComponents()` was
`needsRealRamdisk = !stockRamdisk && !stockFirmware`, so a fully-stock run
skipped `ensureBakedFirmware()` entirely — correctly, since it takes every
component from Apple's IPSW. But DeviceTree was the one required component with
*no download path at all*: it was fetched unconditionally via `takeBaked()`,
which on a route that never baked hits a `dist/` entry that was never produced.
Fixed by giving DeviceTree the same bake-first/download-fallback shape as
RestoreLogo (it is sent byte-for-byte unmodified either way — Apple's IPSW copy
and bake-firmware's published copy are identical, see `setDeviceTreePath()`).

**2. The bake gate was wrong for `--stock-firmware` alone.** `--stock-firmware`
by itself keeps blackb0x's own *patched* iBSS/iBEC (they exist only in `dist/`)
and stocks just the kernel/ramdisk — so it genuinely needs a bake. But the old
gate skipped the bake for it too (`!stockFirmware` was false), so its
`takeBaked()` iBSS/iBEC/DeviceTree calls could hit an unbaked `dist/` and print
the spurious "bake it first" message the user reported. Replaced with
`fullyStock = stockFirmware && stockRecovery`: the bake is skipped *only* when
every component is coming from the IPSW verbatim, which is the sole
fully-stock combination.

**3. `--stock-recovery` without `--stock-securerom` needs the stock iBSS
decrypted host-side first.** This is the one that explains "not booting, and
the Boot Failure Count never even increments." On A5, a non-`--stock-securerom`
iBSS goes through checkm8's `boot_client()`, whose `check_img3_file_format()`
strips the img3 wrapper and uploads the DATA tag's bytes **verbatim, without
decrypting** (the exploited SecureROM then executes them as code). That is
exactly why `patchiBSS()` leaves the *baked* ATV3 iBSS decrypted
(`outputs_.iBSS = patchedPath`, not the re-encrypted `outPath`). A *stock* iBSS
is a still-encrypted img3, so uploading it unchanged is ciphertext-as-code: no
iBoot ever runs, no `bootx`, and the on-device boot-failure counter — which
only advances when iBoot itself attempts and fails a boot — never moves.

A first pass here *refused* this combination outright, on the reasoning that
blackb0x no longer decrypts anything. That was wrong, and the fix was
reverted: the coherent answer is to decrypt the stock iBSS host-side for this
one route, exactly as `useStockIBSS()`'s own long-standing comment already
described. That is the sole, deliberate exception to "blackb0x does no
decryption," and it is the same reason the baked iBSS is the one `dist/`
component published decrypted rather than re-encrypted. `src/StockIBSSCrypt.cpp`
is a small first-party AES-CBC img3 decrypt (its img3 walk mirrors
`check_img3_file_format()` exactly) built on **wolfSSL** — already on
blackb0x's link line via `deps::wolfssl` — so it pulls no xpwn/GPL code into
blackb0x. `useStockIBSS()` invokes it only for the boot_client route
(A5 && !`--stock-securerom`), using the local `keys/` iBSS entry blackb0x
already loads; every other route (`--stock-securerom`'s real SecureROM, or
AppleTV2,1/A4's pwned SecureROM over `irecv_send_file`) still gets the
encrypted original untouched, because those decrypt the img3 themselves and a
decrypt would invalidate the signature/KBAG.

**Missing RestoreLogo on the reconnect-per-step path.** `sendComponentsToDevice()`
documents its flow as iBSS → iBEC → **RestoreLogo** → Ramdisk → DeviceTree →
KernelCache, and the single-connection `sendStockRestoreTail()` (stockRecovery)
sent RestoreLogo — but the ordinary reconnect-per-step path (normal jailbreak
and `--stock-firmware-*` without `--stock-recovery`) skipped it entirely. Added
`DeviceManager::sendRestoreLogo()` (file + `setpicture 4`, same as the stock
tail) and a call between iBEC and Ramdisk, sent whenever the manifest actually
had a RestoreLogo (non-fatal on failure — it is a cosmetic boot image).

**The `--stock-firmware` split.** (Superseded — the split was reverted and
`--stock-firmware-new` deleted; see "Can an old iBoot boot a NEWER build's
kernelcache/DeviceTree/ramdisk?" below. What follows is the state as of this
session.) `--stock-firmware` is gone, replaced by two flags that differ only in
which build supplies the stock OS suite:
- **`--stock-firmware-old`** — the old build the patched iBSS/iBEC are for
  (`kJailbreakTargetBuild`); a self-consistent old suite end to end. This is
  what `--stock-firmware` alone used to be, and the build `--stock-recovery`/
  `--stock-securerom` now require.
- **`--stock-firmware-new`** — the newest currently-signed build's stock suite
  (kernel/ramdisk/DeviceTree/RestoreLogo), still loaded by the OLD patched
  iBSS/iBEC. No APTicket and no local keys are needed for it: the patched
  iBEC's ticket check is defeated and the stock images go out still-encrypted
  (the iBEC AES-decrypts them via the GID key). Tests whether the old patched
  bootloader can hand off to a *newer* OS at all — the natural next probe once
  the `--stock-securerom` fully-stock path was seen to panic right after
  RestoreLogo/`setpicture 4` on real hardware.

This made `downloadAndPatchComponents()` carry two build IDs: `bootloaderBuild`
(the baked iBSS/iBEC and the bake target — always `kJailbreakTargetBuild` for
any patched-bootloader mode) and `buildToRequest` (the downloaded OS suite +
manifest). They are equal in every mode but `--stock-firmware-new`. `keys/` are
loaded for `bootloaderBuild` (the iBSS-decrypt keys, and the only build that
has any), and the baked iBSS/iBEC use a `bootSuffix` while baked OS components
use an `osSuffix`.

Net effect on the diagnostic matrix, all verified to build (both target
groups), prep, and parse:

- **default** (no stock flags) — full bake; patched everything.
- **`--stock-firmware-old`** — bake old build; patched iBSS/iBEC + DeviceTree
  from the bake, stock old kernel/ramdisk downloaded, RestoreLogo sent if the
  manifest has one.
- **`--stock-firmware-new`** — bake old build for the patched iBSS/iBEC only;
  newest-signed kernel/ramdisk/DeviceTree/RestoreLogo downloaded and sent under
  that old bootloader.
- **`--stock-firmware-old --stock-recovery`** — no bake; stock iBSS decrypted
  host-side for the boot_client route, rest downloaded.
- **`--stock-firmware-old --stock-recovery --stock-securerom`** — no bake; the
  real-SecureROM fully-stock path (the one still panicking after RestoreLogo).

The separate, ultimate goal — a real jailbroken boot via the patched `dist/`
ramdisk — is untouched by this and remains open.

## Three-way send-flow comparison (original blackb0x vs idevicerestore vs our port): the missing `go` after iBEC

"No path boots." Rather than keep guessing, the post-exploit send sequence was
extracted faithfully from two references and compared line-by-line against our
port: (a) the ORIGINAL Objective-C blackb0x (`git show 907b64b^:...`,
`MainView.m -componentsReady:`, `DeviceManager.m`, `Patcher.mm`) — a
known-working AppleTV 2/3 jailbreak — and (b) the vendored idevicerestore
(`recovery.c`/`dfu.c`/`idevicerestore.c`).

What the comparison **ruled out** (so nobody re-chases them):

- **iBEC encrypted vs decrypted** — our `patchiBEC()` re-encrypts via the
  template trick (`decrypt(patched, out, key, iv, "FALSE", original)`), so the
  baked iBEC is a signed/encrypted img3, exactly like the original's, which
  iBSS's iBoot AES-decrypts via KBAG+GID. Correct on both.
- **ATV3 iBSS decrypted** — both send the decrypted+patched iBSS for the
  boot_client route. Correct.
- **`setenv auto-boot false` + `saveenv`** — the original never sent it and
  worked; its omission on our non-stock path is intentional (confirmed by the
  maintainer), not the cause.
- **Component order** — the original untethered order is
  iBSS→iBEC→DeviceTree→Ramdisk→Kernel; idevicerestore and we use
  Ramdisk→DeviceTree. Left as-is: both images are tagged before `bootx`, so
  iBoot doesn't care, and it's not worth churning.

The **one substantive divergence acted on**: after uploading iBEC, real
idevicerestore **executes it** — `dfu_enter_recovery()` does `sleep(1)`,
`irecv_send_command_breq(client, "go", 1)`, then a zero-length DFU_DNLOAD-class
control transfer `(0x21, 1, 0, 0, NULL, 0)` (guarded `build_major < 20`, always
true for A5), and `recovery_send_ibec()` does the same `go`+zero-length. The
original blackb0x sent **nothing** after the iBEC file — it relied on
`IRECV_SEND_OPT_DFU_NOTIFY_FINISH` alone to jump into the image — and our port
inherited that. Per the maintainer's call to favour idevicerestore (the
maintained reference) over the OG here, `sendiBEC()` now issues the same
`go` (bRequest=1) + zero-length execute transfer after the upload. `go` is
best-effort (logged, non-fatal): the device re-enumerates into Recovery as
iBEC starts, so the command can return before the follow-up transfer lands, and
the next step reconnects via `get_tv_patient()` anyway.

If iBEC was previously not fully executing (NOTIFY_FINISH finalizing the
download without a clean jump), that also plausibly explains the
`--stock-securerom` panic right at RestoreLogo/`setpicture 4` — the first
command issued to a not-properly-running iBEC. RestoreLogo/`setpicture` is kept
(per the maintainer) rather than dropped; the `go` fix is the first thing to
retest before investigating `setpicture` further.

## AppleTV3,2 iBSS never executed: the missing checkm8_bootkit trampoline (`dfu_boot`)

The `--send-only ibss|ibec` diagnostic (added for exactly this) settled where the
boot chain dies on a real AppleTV3,2 (CPID 0x8947). After `--send-only ibss`,
`irecovery -s` reported **iBoot-2261.30.37 on both the stock and non-stock paths** —
which is *not* any build we upload (our jailbreak build 10B329a is iBoot-1458.2). That
version is the device's own installed OS iBoot (Apple TV Software 7.x / iOS 8.x era):
our uploaded iBSS never ran, the device reset, and it fell back to NAND iBoot, which is
what printed "Memory image not valid" and what the recovery serial at the end reported.
So the whole downstream hunt (RestoreLogo/`setpicture`, kernelcache) was moot — the
failure is right at the top, the iBSS handoff.

Root cause, found by comparing against the original Objective-C app: the original
delivered iBSS on **AppleTV3,2 specifically** via `libbootkit`'s `dfu_boot()`, not the
`boot_client()` it used for AppleTV3,1. `libbootkit` is NyanSatan's
[checkm8_bootkit](https://github.com/NyanSatan/checkm8_bootkit) (flattened into the
app). Its `dfu_boot()` wraps the iBSS in an `"exec"` `usb_command_t` (ipwndfu's custom
protocol) followed by a device-specific ARM boot trampoline carrying the CPID-0x8947
SecureROM offsets (`platform_bootprep`, `arch_cpu_quiesce`, `memmove`,
`platform_get_boot_trampoline`, …). The checkm8 payload our exploit installs
(`checkm8_payload_8947`) only *executes* an uploaded image when it sees that `"exec"`
command; it ignores a raw image. Our port had dropped `libbootkit` and routed
AppleTV3,2 through `boot_client()` — a raw image upload with no `"exec"` wrapper and no
trampoline — so the payload uploaded the iBSS to RAM and never jumped to it.

Fix: vendored the needed piece of checkm8_bootkit into flat `src/` — `bootkit.c` /
`bootkit.h`, `dfu_boot()` plus its helpers (`validate_device`, `construct_command`,
`construct_payload`, `send_command`, `send_chunks`, `get_config`), the CPID-0x8947
config, and the trampoline payload blob (reproduced from `payload.S` in a comment, not
cross-compiled). `sendiBSS_ATV32()` now calls `dfu_boot()`; `boot_client()` stays for
AppleTV3,1. Only what's needed was copied — no `main`/tool, no logging/ops/protocol
split of current upstream, no `save_command`/debug dump, no configs for other SoCs.
checkm8_bootkit declares no license; kept under the same research-tool terms as
`checkm8.h`/`SHAtter.h`, attributed in the file header. Considered a proper git
submodule but rejected it: upstream has no license, its current form is refactored and
depends on its own `lilirecovery` fork + Objective-C, and it never contained the
flattened single-file `dfu_boot()` we actually use.

Retest with `--send-only ibss`: if `irecovery -s` now reports **iBoot-1458.2**, our
iBSS is finally executing and the investigation moves to the iBEC load; if it still
reports 2261.30.37, `dfu_boot()`'s trampoline/offsets need another look.

## Chasing the kernelcache "Size mismatch from lzss" all the way into iBoot's decompressor — and exonerating the whole compression pipeline

The `--stock-ramdisk` isolation (patched kernel + stock ramdisk) died at the
kernelcache with iBoot printing:

```
Attempting to validate kernelcache @ 0x80000000
Size mismatch from lzss 0x009fffda, should be 0x00a00000
error loading kernelcache
```

i.e. iBoot decompressed the kernelcache to `0x9fffda`, 38 bytes short of the
`0xa00000` its complzss header claims, and refused it. This section records how
far that was chased before the compression pipeline was cleared entirely.

**Tooling built for this (kept):** `bake-kernel` (BakeKernel.cpp), a standalone,
rootless kernelcache baker bake-firmware shells out to; `patchKernel()` size
instrumentation; and `BLACKB0X_KEEP_KERNEL_TEMPS` (keeps the decrypted,
decompressed, unpatched kernel). Rootless + one-tuple means the whole
decrypt→CBPatcher→re-encrypt path can be run and inspected in seconds without a
device.

**Instrumented sizes.** Original kernelcache decompresses to `0xa00000` (exactly
10 MiB — Apple pads it; the real Mach-O ends in a long zero run with a lone
`0x4c` at the final byte). CBPatcher patches in place, output size unchanged at
`0xa00000`. So nothing in the patch stage changes the length.

**CBPatcher is byte-identical to the original.** The original app shipped a
prebuilt x86_64 `libcbpatcher.a`; ours is rebuilt from the zzanehip/CBPatcher
fork. Ran the original `.a` under Rosetta on the identical decompressed kernel:
`cmp` of the two outputs is **0 differing bytes**. Same patch sites, same result.

**xpwn's LZSS compressor is byte-identical to the original AND lossless.** The
original shipped a prebuilt x86_64 `libxpwntool.a`; ours is built from
regulad/xpwn. Linked the original's `compress_lzss` (Rosetta) and ours against
the identical patched kernel: both emit `0x5b9e59` bytes, `cmp`-identical, and
both round-trip losslessly (`0xa00000 → 0x5b9e59 → 0xa00000`, exact `memcmp`).
Grepping the entire original source/comments/docs for `lzss`/`compress`/`size
mismatch` turns up nothing — the original had no awareness of, and no fix for,
this. The kernel pipeline is byte-for-byte the original's; the original's A5
jailbreak almost certainly never booted this way either.

**iBoot's decompressor, disassembled.** Decrypted the AppleTV3,2 10B329a iBEC
(`iBoot-1537.9.55`, the exact build the device runs), extracted the raw ARM
(img3 DATA at +0x40, base `0x9ff00000` from the vector-table literals), found the
error string at `0x9ff38a1d`, its caller at `0x9ff1add6`, and the decompressor
wrapper at `0x9ff22b24` → real decoder at `0x9ff22b58` (ARM, reached via `blx`).
Disassembled the decoder instruction by instruction (Capstone): it is a
**faithful, standard reference LZSS** — 4096-byte ring buffer filled with `' '`,
`THRESHOLD=2`, identical flag/literal/match handling to xpwn's
`decompress_lzss`, and it *ignores* its `dstlen` argument, stopping only when the
compressed input is exhausted. No quirk, no early-out, no divergence from the
reference.

**The header-offset red herring, resolved.** Apple's complzss header is **0x180
bytes** (compressed data at +0x180; a `version=1` field sits at +0x14). iBoot
reads `checksum@+8`, `length_uncompressed@+0xc`, `length_compressed@+0x10`, and
the payload at `+0x180`. xpwn's `CompHeader` has `padding[0x16C]` → sizeof
`0x180`, and `closeComp()` writes the payload at exactly `+0x180`, version field
preserved. The offsets match. An earlier extraction that pulled the compressed
stream from `+0x14` instead of `+0x180` is what produced a bogus decompressed
size (`0x9ff97c`) and the false "xpwn is lossy" scare — that was a measurement
bug, not a real one.

**The artifact is complete — verified on both the local bake and the CI
artifact.** Decrypted the dist `KernelCache-AppleTV3,2_10B329a`, extracted the
compressed stream at the correct `+0x180`, and decompressed it with the
reference decoder: exactly `0xa00000`, byte-identical to the patched kernel,
matching its own header. Then pulled the newest CI `firmware-AppleTV3,2`
artifact (`gh run download`): its kernelcache is **byte-identical** to the local
bake and likewise decompresses to a full, correct `0xa00000`.

**Conclusion — the compression pipeline is exonerated.** The compressor, the
published artifact (local and CI), and iBoot's decoder all independently agree
on `0xa00000`, and iBoot's decoder is a faithful reference that would accept the
artifact. Therefore the on-device `Size mismatch 0x9fffda` cannot originate in
the bake: for iBoot to decode 38 bytes short of a stream that decodes fully
everywhere else, the compressed data reaching its decoder must be **short at the
tail**. That leaves two possibilities, both downstream of the bake:

1. **A stale kernelcache on the device** — an older `dist/` artifact than the
   known-complete one (ruled out for CI: the current CI artifact is verified
   complete and identical to a good local bake).
2. **Delivery truncation** — `sendKernelCache()`'s USB upload dropping the tail
   (a short final DFU packet, or an img3-DATA/AES-block size mismatch), so iBoot
   reads `length_compressed=0x5b9e59` but the last compressed bytes are missing
   or zeroed and decompression stops ~38 bytes early.

Next step is to retest against the verified-complete CI kernelcache: if the
error persists, it is delivery (audit `sendKernelCache`); if it clears, the
device had been booting a stale artifact.

## Past `bootx`: the `-z` lzss bypass worked, and why serial console debugging is not available

With the `-z` lzss patch baked into iBEC (see `bake-iboot` and the
`iBoot32Patcher` fork's `patch_lzss_check()`), the on-device `Size mismatch
from lzss` rejection is gone: `bootx` is accepted and the kernelcache actually
executes. The observable proof is a **state change past iBEC**: the device
drops off the USB bus entirely (iBEC, which served the recovery/DFU USB
interface, is gone and nothing re-enumerates it) and the front **status-LED
blink cadence slows**. That is consistent with the kernel booting into *our*
ramdisk: entrypoint.c replaces `/sbin/launchd` and never brings the USB gadget
up (it just writes files and reboots), so a silent bus is expected once we
reach userspace.

But it does **not** reboot within a minute, which it must if entrypoint.c ran
to completion. So the failure has moved *past the kernelcache*: the kernel
boots, but either it never reaches PID 1 (panic / `md0` root-mount failure /
`launchd` exec refusal) or entrypoint.c runs and stalls before its final
`reboot(2)`. After sitting, Menu+Play/Pause forces DFU, so the device is in a
live post-`bootx` state, not hard-hung at the bootloader.

### Why serial console debugging is not available

The obvious next move is to make the `-v` boot log observable by routing the
kernel console to the UART: `debug=0x8` (`DB_KPRT` in `osfmk/kern/debug.h` —
enables kernel serial output and initializes the UART, with no `DB_HALT` bit so
panic behavior is unchanged) plus `serial=3` (serialmode bits: `0x1` output,
`0x2` input; `3` = full bidirectional console, matching Apple's own restore
environments' `debug-uarts=3` / `boot-args=serial=3`). A patched iBEC — which
we already ship — is required for the kernel to honor custom boot-args at all.
(**Sharper than it was written**: a patched iBEC is not merely required, it is
the *only* delivery mechanism. The args have to be baked into it with
`iBoot32Patcher -b`, because `setenv boot-args` over the recovery protocol is
never read back — so adding `serial=3` later means a `bake-iboot` re-bake, not
a tool flag. See "`setenv boot-args` is INERT on this bootloader" at the end of
this file.)

That was implemented and then reverted, because **the AppleTV3,2's UART is on
internal hardware test-points, not the micro-USB port.** The micro-USB is
USB/DFU only; true serial requires soldering to on-board footprints (community
teardowns identify a ~10-pin ARM-JTAG-style connector and a 30-pin FPC — see
the XDA "Apple TV3 JTAG points" thread). Without a soldered tap there is
nowhere to read the log, so `serial=3` would emit onto a wire we cannot see.
Booting via iBEC (not full iBoot) also never initializes the framebuffer, so
there is no on-screen console either. The `serial=3`/`debug=0x8` reasoning is
recorded in `kRamdiskBootArgs`'s own comment for anyone who later adds a
hardware tap; it is deliberately not in the live boot-args. (That constant
lived in `DeviceManager.cpp` when this was written; it is in `Patcher.hpp`'s
`bootargs` namespace now, shared between the baker and the jailbreak binary.)

### The channel that is left: USB re-enumeration as a proof-of-life beacon

With no serial and no framebuffer, the remaining zero-hardware, host-visible
signal is USB **re-enumeration**. We cannot bring a USB *interface* up from
entrypoint.c ourselves — advertising the restore/AFC gadget needs the IOKit
userspace USB stack (`restored`/`usbmuxd`) we deliberately do not link, which
is exactly why the bus goes silent after `bootx`. But `reboot(2)` *is* a USB
event: the device leaves the kernel and reappears in DFU/recovery on the bus
(and the LED cadence changes with it). So an early `reboot(2)` from entrypoint —
before any mount — is a clean binary test: if the ATV power-cycles right after
`bootx`, entrypoint definitely executed as PID 1 and the bug is downstream
(mounts / `merge_tree` / the final reboot); if it stays dark, the kernel never
reached our `launchd` (panic / `md0` mount / exec-time code-signing). That
bisection needs no hardware and is the next diagnostic step.

## entrypoint.c syscall ABI audit: the carry-flag error convention, `reboot`'s arity, and a real `fork`/`vfork` bug

Prompted by a question about `sys_reboot`'s flag, the whole freestanding
syscall layer (`entrypoint.c` has no libSystem, so every wrapper is hand-rolled
`svc #0x80`) was audited against xnu's `syscalls.master` and `libsyscall`. All
25 syscall *numbers* were already correct classic-BSD/xnu values, and all arg
*counts* matched except `reboot`. Three ABI defects were found and fixed; the
third is significant enough that it could by itself explain a boot that reaches
userspace and still fails to come back.

**1. `reboot` was called with one argument, but the syscall is two.** xnu
`syscalls.master` #55 is `reboot(int opt, char *msg)`; libc's userspace
`reboot(int)` is a 1-arg wrapper over it. Our wrapper passed only `opt`, leaving
`msg` as garbage in `r1`. Harmless for our flags (msg is only read for
`RB_PANIC`/command opts), but wrong — now passes an explicit `NULL`. Also
switched the flag itself from `1` to `0`: the original binary passed `1`
(`RB_ASKNAME`), a bootstrap-prompt flag with nothing to act on under iOS — an
inert wrong value. `RB_AUTOBOOT` (`0`) is the correct "reboot normally".

**2. No wrapper honored Darwin's carry-flag error convention.** Unlike Linux
(which returns `-errno` in the result register), Darwin signals syscall failure
by *setting the carry flag*, with `r0` holding a *positive* errno; libSystem's
stubs are what convert that to the C `-1`/`errno` convention. Our wrappers
returned `r0` raw, so on failure they returned a small positive errno — and a
failed `open()` returning e.g. `ENOENT` (2) would sail straight past `if (fd <
0)` as if it were a valid fd, likewise for `stat`/`mount`/`read`. Fixed by
appending `rsbcs r0, r0, #0` (conditional reverse-subtract-from-zero, predicated
on carry-set — a plain ARM conditional instruction, valid because the file is
built `-arch armv6`, i.e. ARM not Thumb) to every `svc`, negating the errno on
error so all the existing `< 0` / `!= 0` checks become meaningful. `"cc"` was
added to each asm's clobber list since `svc` writes the flags. Verified in the
built binary: `rsbhs r0, r0, #0` (otool's spelling of `rsbcs`) follows every
`svc`.

**3. `sys_vfork()` never told the child it was the child.** This is the real
bug. On armv7 Darwin, `fork`/`vfork` return the child pid in `r0` for *both*
processes and flag the child in `r1` (`0` = parent, `1` = child); libSystem's
`__fork.s` is what zeroes `r0` in the child. Our `sys_vfork()` was
`__syscall0(SYS_vfork)` — it read only `r0`, so **the child saw its own pid, not
0.** `set_auto_boot()`'s `if (pid == 0)` child branch therefore never ran, and
`/usr/sbin/nvram auto-boot=1` was never exec'd. Since SecureROM clears
`auto-boot` after any USB boot, that means even a fully-working entrypoint would
reboot into recovery instead of the installed OS — indistinguishable from the
outside from "the jailbreak didn't work". And under `vfork`'s shared-address-
space rule the mis-branched child then *returned from the calling frame*, which
is undefined behavior. Fixed with a dedicated `sys_fork()` that inspects `r1`
and returns `0` in the child (verified in the disassembly: `svc` → `bhs`
error → `cmp r1,#0` / `beq` parent / `mov r0,#0` child), and by switching
`set_auto_boot()` from `vfork` to `fork` so the child has its own address space
and can safely run C and `execve`/`_exit` — which is what the original binary's
own child-spawn helpers used anyway. The parent now also only `wait4`s when the
fork actually succeeded (`pid > 0`).

### Driving the LED as a signal: not reachable from here

The other candidate for an observable signal was the front status LED, whose
cadence is seen to change after `bootx`. But that cadence changes while *only*
our minimal entrypoint is running — no SpringBoard, no backboardd, none of the
userspace that would normally drive it — which means at this boot stage the LED
is driven automatically by the kernel/a kext, not by a process we could co-opt.
Driving it deliberately would need IOKit (a user client for the GPIO/LED
service, reached via mach messaging + `IOServiceOpen`/`IOConnectCallMethod`);
restore ramdisks do exactly this from C (e.g. iphone-dataprotection's
`ramdisk_tools/IOKit.c`, and `restored_external` talking to `AppleImage3NORAccess`),
but only because they link IOKit.framework over libSystem/mach. entrypoint.c is
freestanding with no mach layer at all, and the ATV3 LED's specific service
name/selector is undocumented, so this is impractical without abandoning the
freestanding design. The `reboot(2)` beacon remains the one viable, host-visible
(USB re-enumeration) proof-of-life signal.

## `--tether-boot` revived (and corrected) as a kernel-integrity probe

The ramdisk-install path stalls after `bootx` (kernel boots — USB drops, LED
cadence changes — but entrypoint.c never completes its reboot), and it is a bad
path to debug: no serial (hardware test-points only), no framebuffer (iBEC never
inits the display), and a blind PID-1 installer. So `--tether-boot` was brought
back as a diagnostic that sidesteps all of that.

It sends `iBSS -> iBEC -> DeviceTree -> KernelCache('bootx')` — **no RestoreLogo,
no Ramdisk** — with NAND-root boot-args (the ramdisk set **minus `rd=md0`**), so
the patched kernel boots the OS already on the device's NAND instead of an
install ramdisk. The payoff is observability: a NAND boot brings up the real OS,
which initializes the framebuffer and comes up **on the TV screen**. That makes
it a clean bisection — if the OS appears, checkm8 -> iBEC -> the patched/`-z`
KernelCache is proven intact end to end and the jailbreak failure is downstream
in the ramdisk/entrypoint path; if it hangs the same way, the kernel /
DeviceTree / `-z` patch is implicated. The existing
`checkDeviceLeftRecoveryModeAfterBoot()` already reads the right signal (leaving
Recovery == the kernel booted the OS).

Two corrections over the original app's tether-boot (the one this port had
removed in c8c4980):

- **It sends a DeviceTree.** The original tether path (`tetherbootClick`) sent
  iBEC + KernelCache and no DeviceTree at all — but the kernel needs one to
  boot, so the original was either broken or leaned on something undocumented.
- **The NAND-root args are the ones actually without `rd=md0`.** The original
  had `patchiBEC()`'s two arg sets wired to the wrong iBECs (rd=md0 into the
  tether path, non-rd=md0 into the install path). Boot-args are runtime now
  (`setenv boot-args`), so this is just a `bool ramdiskBoot` on
  `sendKernelCache()` picking `kRamdiskBootArgs` vs the new `kTetherBootArgs`.
  **[Superseded: boot-args are baked into iBEC again, so this is no longer a
  send-time choice. `bake-iboot` publishes two images —
  `dist/iBEC-<tuple>` with `rd=md0` and `dist/iBECTether-<tuple>` with
  `rd=disk0s1s1` — and `--tether-boot` picks the file rather than the string.
  The `bool ramdiskBoot` survives as the selector between those two files. The
  correction to the *original's* wiring stands; only the delivery changed. See
  "`setenv boot-args` is INERT on this bootloader" at the end of this file.]**

Dropped from the original's version and NOT revived: the "device must already be
jailbroken / must have connected in Normal mode first / assume AppleTV2,1 =
7.1.2" preflight, and requesting the device's own installed build instead of the
pinned `kJailbreakTargetBuild`. This tether-boot deliberately boots the
jailbreak-target build's patched kernel, so the device's NAND should be running
that same build for a clean boot (a large version skew may panic; even a partial
boot still proves the kernel decompressed and executed). It installs nothing —
`needsPostInstall` stays unset — and is mutually exclusive with the `--stock-*`
diagnostics, which drive their own send paths.

## SOLVED: the kernelcache "Size mismatch from lzss" was an IMG3 DATA alignment bug (xpwn PR #7), not compression

After a long chase this was root-caused, fixed, and verified. The whole "the
kernelcache is malformed / compression is broken" saga above resolves to a
one-line regression in the vendored xpwn, in the IMG3 re-encryption — nothing to
do with LZSS itself.

### How it was cornered

1. **`--tether-boot` isolated it to the kernel.** Booting the stock NAND OS off
   our patched kernel (no ramdisk) failed the same way, and iBoot's **Boot
   Failure Count incremented while Panic Fail Count did NOT** — i.e. iBoot never
   handed control to the kernel; it rejected the image at load. So the fault was
   in the kernelcache load, common to both the install and tether paths, and the
   `-z` lzss-check bypass had only ever *masked* the complaint (it was walked
   back).

2. **The earlier "compression exonerated" finding had tested the wrong oracle.**
   Every off-device round-trip used xpwn's own decoder (and a reimplementation),
   which agreed with xpwn's own encoder — of course it did. The real oracle is
   iBoot, and the hardware said no.

3. **A byte-structure audit of the IMG3 DATA element** (stock download vs our
   re-encrypted output, keys not even needed — the tag headers are cleartext)
   showed both declare `length_uncompressed = 0xa00000`, so the complzss header
   was fine. The difference was structural:

   | | DATA `dataLength` | `dataLength % 16` | `totalLength - 12` (AES body) | `% 16` |
   |---|---|---|---|---|
   | **Stock (boots)** | 0x5ba041 | 1 | 0x5ba050 | **0** |
   | **Ours (fails)** | 0x5b9fd9 | 9 | 0x5b9fdc | **12** |

   Apple pads the DATA element so its AES body is **16-aligned**; ours was only
   **4-aligned**, leaving a partial final AES-CBC block.

4. **A survey settled that it is a universal Apple invariant, not a
   coincidence.** Across **108 DATA elements — 27 IPSWs, 10 devices (iPhone3,1 …
   AppleTV3,2), 10 iOS versions (4.3–10.3), components kernelcache/iBSS/iBEC/LLB
   — `(totalLength - 12) % 16 == 0` held in every single case, zero exceptions**,
   while `dataLength` stayed the true (usually not-16-aligned) length and TYPE/
   KBAG were only 4-aligned. So Apple always 16-aligns the *encrypted DATA*
   element specifically; the true length lives in `dataLength`.

### Root cause

The DATA payload is AES-CBC encrypted. AES-CBC only processes whole 16-byte
blocks; both Apple's tooling and xpwn leave anything past the last full block as
plaintext. Apple avoids the problem entirely by padding the DATA element to a
16-byte boundary, so **there is never a partial final block** — the whole
compressed stream is inside full, encrypted blocks. Our re-encryption produced a
DATA element ending 9 bytes into a partial block, so the last 9 compressed bytes
were left un-encrypted, and iBoot's handling of that partial tail corrupted them
— the complzss decoder then hit garbage near the end and stopped ~38 output
bytes short of the declared `0xa00000` ("Size mismatch from lzss 0x9fffda,
should be 0xa00000"). The stock kernel survives the same class of layout because
its stream happens to end only **1** byte into the tail (tolerable); ours ended
**9** bytes in (several LZSS tokens).

The regression is **xpwn [PR #7](https://github.com/planetbeing/xpwn/pull/7)**
(Djayb6). Upstream originally computed `size = (((dataSize + 16) / 16) * 16) +
sizeof(AppleImg3Header)` — a 16-aligned DATA body. PR #7 replaced it with
`size = dataSize + sizeof(AppleImg3Header)` plus a 4-byte align, to stop xpwn
adding a stray 16 bytes to *already*-16-aligned bootloaders (which broke SHSH
partial-hash byte-identity). That fix was right for bootloaders but threw out the
alignment kernelcaches depend on. Our fork sat on the post-PR-#7 behavior, so
`bake-kernel`'s output was 4-aligned. This also finally explains why the original
NSSpiral/Blackb0x — which links the *same* xpwn `compress_lzss` and img3 path —
was fragile here: whether a given kernel booted came down to where its compressed
length happened to land relative to 16.

### The fix (in the `regulad/xpwn@legacy` fork, `ipsw-patch/img3.c`)

Three coordinated changes, scoped to the encrypted DATA element:

- **`writeImg3()` grow path:** 16-align the element body with a true ceiling,
  `alignedBody = ((dataSize + 15) / 16) * 16`. This adds nothing when `dataSize`
  is already 16-aligned (so iBSS/iBEC output is byte-for-byte unchanged and PR
  #7's "no stray 16 bytes" goal is still honored) and rounds up only when it is
  not — i.e. only the kernelcache moves. The buffer is allocated to
  `alignedBody + IMG3_AES_OVERREAD_PAD` and everything past the true `dataSize`
  (the 16-align pad and the wolfSSL over-read slack) is zeroed so the padding
  encrypts deterministically.
- **`closeImg3()` encrypt:** encrypt the whole aligned body,
  `((size - sizeof(AppleImg3Header)) / 16) * 16`, instead of
  `(dataSize / 16) * 16` — so the last real bytes are inside a full encrypted
  block, no plaintext tail. `setKeyImg3()`'s decrypt already uses this length, so
  the round-trip stays symmetric.
- **`writeImg3Default()`:** for the encrypted DATA element only, write the whole
  encrypted body (`size - sizeof(AppleImg3Header)`) straight from the buffer,
  rather than `dataSize` bytes plus fresh zeros — otherwise the encrypted pad
  (the tail of the final cipher block) would be replaced by zeros on disk and
  corrupt the last real block on decrypt. Other elements, and the already-aligned
  bootloaders (`paddingSize == 0`), keep the original zero-fill path.

### Verified

`bake-kernel` output for AppleTV3,2 10B329a now has DATA body `0x5b9fe0`
(`% 16 == 0`) with `dataLength` still the true `0x5b9fd9`; decrypting the full
body and decompressing yields exactly `0xa00000` with a valid `FEEDFACE` Mach-O
at the front — byte-structurally identical to how Apple lays out the DATA
element. iBSS/iBEC output is unchanged (their `dataLength` is already
16-aligned). A separate check confirmed the CI kernelcache itself always
contained a complete, real XNU kernel (`Darwin Kernel Version 13.0.0 …
xnu-2107.7.55.2.2 … RELEASE_ARM_S5L8947X`) — the artifact was never the problem,
only its IMG3 packaging. Hardware confirmation via `--tether-boot` is the next
step.

## `--tether-boot` needs `rd=disk0s1s1`: the NAND root device

With the img3 alignment fix, `--stock-ramdisk` boots cleanly on hardware — the
kernel is confirmed intact (the "Size mismatch from lzss" wall is gone). But
`--tether-boot` still did nothing. Cause: `kTetherBootArgs` had NO `rd=` at all.
`rd=` is the boot-arg that tells the kernel which partition holds PID 1
(launchd) / the root filesystem; a restore bootloader like iBEC boots a
kernelcache via `bootx` without fsboot's automatic NAND-root setup, so with no
`rd=` the kernel comes up with no root device and hangs — exactly the "nothing
happens" symptom. Fixed by setting `rd=disk0s1s1`, the system partition (the
same one `entrypoint.c` mounts as `/`, with `disk0s1s2` as `/var`, matching the
standard iOS fstab `/dev/disk0s1s1 / hfs ro`). This only affects `--tether-boot`
(the install path keeps `rd=md0`); it is a runtime `setenv boot-args`, so it
changes only the `blackb0x` tool, not the baked firmware.

**Correction to that last clause.** It was not a runtime `setenv`, in the sense
that matters: the `setenv` was sent and ignored, so this change delivered
`rd=disk0s1s1` to nothing and `--tether-boot` was still booting with iBoot's
hardcoded `rd=md0 nand-enable-reformat=1 -progress` — a ramdisk root device with
no ramdisk uploaded, which is its own sufficient explanation for "still did
nothing". The diagnosis in this section (a `bootx` from a restore bootloader
needs an explicit `rd=`, or the kernel comes up with no root device) is
correct and unaffected; only the delivery mechanism was wrong. `rd=disk0s1s1`
now lives baked into `dist/iBECTether-<tuple>`, which **does** mean this change
touches the baked firmware and a re-bake is required for it to take effect. See
"`setenv boot-args` is INERT on this bootloader" at the end of this file.

## `--tether-boot` visibility: send the RestoreLogo so the display comes up

`--tether-boot` still showed "nothing" even with `rd=disk0s1s1`. A survey of
reference tethered-boot implementations (redsn0w, kloader, nyansatan's dualboot,
synackuk's fast-tethered-boot, the SSH-ramdisk toolchains) confirmed our recipe
is otherwise canonical: `iBSS -> iBEC -> DeviceTree -> KernelCache('bootx')`,
`bootx` is the correct final command (not `fsboot`/`go`), `rd=disk0s1s1` is the
right root partition, and the restore DeviceTree is fine for a NAND boot. The
one behavioral difference from our working ramdisk boot was that tether-boot
dropped the RestoreLogo -- and on these devices iBoot only initializes the
display/framebuffer when it has a picture to draw (`setpicture`). The kernel's
verbose (`-v`) output renders into that same framebuffer, so with no logo a NAND
boot is invisible whether it succeeds, hangs, or panics. redsn0w injects its own
boot logo for exactly this reason. So the earlier "nothing happens" may well have
been a *working or panicking* boot we simply couldn't see.

Fix: send the RestoreLogo on the tether-boot path too (only the Ramdisk stays
tether-specific), before the DeviceTree (which loads over the logo's memory once
drawn -- the existing order already satisfies this). Tool-only change; the baked
firmware is unaffected.

Remaining possibilities if it's still dark with the logo, per the same survey:
(1) the true serial console needs `debug=0x14e serial=3` on a hardware UART tap
to distinguish invisible-but-booting from a real hang -- not available without
soldering (the ATV3 UART is on test-points); (2) the kernel reaches userspace
but hangs mounting the NAND root because the installed OS on disk0s1s1 is not a
clean, fully-restored 10B329a (tether-boot loads OUR 10B329a kernel against
whatever is on NAND -- a version mismatch will hang). `--stock-ramdisk` avoids
both by carrying its own self-contained root, which is why it boots regardless.

## Warning when the NAND OS won't match: read the installed build via lockdownd in Normal mode

Since `--tether-boot` loads OUR kernel (kJailbreakTargetBuild, 10B329a) and roots
off whatever is on NAND, it only works if the installed OS is that same build; a
version mismatch hangs at the NAND root-mount with no visible output -- the exact
"nothing happens" symptom. blackb0x already reads the authoritative installed
`ProductVersion`/`BuildVersion` via `lockdownd_get_value` (DeviceManager.cpp),
but ONLY when the device is seen in **Normal mode** -- that is the only channel
that exposes the on-NAND version. In DFU/Recovery there is no way to read it
(iBoot exposes SRTG/CPID, not the installed OS build). (The other way to learn it
without a booted OS would be to boot the stock ramdisk and read
`/System/Library/CoreServices/SystemVersion.plist` off `disk0s1s1` -- not
implemented; the Normal-mode read is simpler and already present.)

So `runCli()` now runs a `--tether-boot` preflight against `device.buildID`:
warn on a mismatch (installed build != kJailbreakTargetBuild -- tether boot MAY
not work), confirm on a match, and when it is simply unknown (the device was
never seen in Normal mode this session) say so and note tether boot may not
work. It is purely advisory and NEVER refuses: a nearby build whose kernel ABI
did not change may still boot, so it always proceeds and lets the hardware
decide. This is a softer subset of the original app's tether-boot preflight,
which required a prior Normal-mode connection outright. Tool-only change.

## CHECKPOINT: kernel fixed, --stock-ramdisk boots, but the real jailbreak ramdisk still does "nothing"

State of play at this checkpoint (post the IMG3 16-align fix):

- **Kernel is fixed and confirmed.** The IMG3 DATA 16-alignment fix landed
  (xpwn fork `4481d66`, main `fb808d3`); `--stock-ramdisk` now boots on
  hardware ("takes the kernel without issue"). So checkm8 -> iBSS -> iBEC ->
  DeviceTree -> KernelCache(`bootx`) is sound end to end, and iBoot's
  "Size mismatch from lzss" rejection is gone. `-z` was walked back (unused).
- **`--tether-boot`** recipe is canonical (`rd=disk0s1s1`, RestoreLogo sent so
  the framebuffer inits, `bootx`), with an advisory NAND-build preflight
  (main `b439e38`/`c1941ff`/`a21a57d`/`059d38e`). It is gated on the device's
  installed OS matching kJailbreakTargetBuild (10B329a); status on hardware
  still pending / version-dependent.
- **The full jailbreak (our baked ramdisk + entrypoint.c) still does
  "nothing."** This is the live problem.

### Isolation: it is NOT the ramdisk's IMG3 packaging

The baked RestoreRamDisk's IMG3 DATA element is naturally 16-aligned already:
`dataLength = 0x24d7000` (`% 16 == 0`), because a DMG is sector-sized (512-byte
multiples are always 16-multiples). So unlike the kernelcache, the ramdisk was
never affected by the 4-vs-16 alignment bug, and the img3 fix does not change
it. iBoot can decrypt it and the kernel can copy it to `md0`. The delivery path
is also shared with `--stock-ramdisk`, which works. **The ONLY thing that
differs between the working `--stock-ramdisk` and the failing jailbreak is the
ramdisk's CONTENT: our overlay** -- `entrypoint.c` spliced in as `/sbin/launchd`
(via BakeRamdisk.cpp's spliceFileContentInPlace), the staged `/blackb0x` tree +
dpkg/apt payload, and the DMG resize. So the fault is downstream of the kernel,
in the baked ramdisk content or in entrypoint.c's own execution.

**Correction to the inference, not to the observation.** It is true that the
only bytes differing between the two runs are the ramdisk's, and the isolation
above is sound as far as it goes. What does not follow is "therefore the fault
is *in* the ramdisk content". The fault can equally be an *interaction*: a
boot-arg that one ramdisk's PID 1 needs and the other's does not. That is what
it turned out to be — ours is ad-hoc-signed and needs the AMFI/code-signing
bypass, Apple's is properly signed and does not, and the bypass was being
delivered over a channel that never worked. The teardown that followed this
section spent its whole effort inside the ramdisk because of this sentence. See
"`setenv boot-args` is INERT on this bootloader" at the end of this file.

### What "nothing" means now, and the first thing to check next session

The install path DOES send RestoreLogo before the Ramdisk, so the display/
framebuffer IS initialized, and `-v` is in the boot-args -- therefore the kernel
should render verbose boot text to the HDMI output, and entrypoint.c's own
`console_print()` (it opens `/dev/console`, dup2 to fd 1/2) should appear too
("Searching for disk...", "blackb0x Jailbreak - by @NSSpiral", "Mounting
filesystem...", etc.). So the decisive observation is **what appears on the TV
during a full jailbreak run**:
  - Kernel `-v` text then a stop -> note WHERE it stops (md0 root-mount? the
    `exec /sbin/launchd`? a panic backtrace?).
  - entrypoint's own lines appear -> it reached PID 1; see which line is last
    (which mount / merge_tree / step it dies on).
  - Truly nothing (no logo, no text) -> the kernel is not booting our ramdisk at
    all (early md0 mount panic before console), which points at the DMG/HFS we
    rebuilt.

**`-v` was not in the boot-args either.** The premise of this whole subsection —
"`-v` is in the boot-args, therefore the kernel should render verbose boot
text" — is false for every run made before the baked-boot-args fix. `-v` was
being delivered by `setenv boot-args`, which this bootloader never reads, so the
kernel booted with iBoot's hardcoded `rd=md0 nand-enable-reformat=1 -progress`
and produced no verbose output at all. **A dark screen was therefore the
expected outcome of a perfectly healthy boot**, and the three-way observation
above could not have distinguished anything. `-v` is baked into both iBECs now,
so the experiment is worth running for the first time. See "`setenv boot-args`
is INERT on this bootloader" at the end of this file.
Also worth distinguishing from before: is it the old "USB drops + LED cadence
slows + no reboot" state (kernel booted, entrypoint didn't finish), or truly
dark (kernel not booting)? That single observation splits the remaining tree.

### Ranked hypotheses for the baked-ramdisk failure

1. **entrypoint.c reaches PID 1 but hangs/crashes before its final reboot.**
   The observable success signal is the device power-cycling (entrypoint's
   `reboot(0)` at the end); "nothing" = it never gets there. The syscall ABI
   bugs are now fixed (carry-flag errors, `reboot` arity, `fork` child
   detection -- `9d663c4`), so a prior silent failure in `set_auto_boot()`/a
   mount check may now surface. With the framebuffer up, its own prints should
   show the last step reached.
2. **Kernel cannot mount `md0`** because the rebuilt DMG/HFS (decrypt -> mount
   -> overlay -> resize -> rebuild -> re-encrypt) is malformed or oversized ->
   early panic before much console output. Compare our baked DMG against a
   freshly-decrypted stock one (does it mount cleanly on the Mac? is HFS
   intact? is the resize sane?).
3. **entrypoint exec/signing**: it is ldid-signed `com.apple.launchd` and the
   boot-args carry `amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1`,
   so this should be covered -- but if AMFI still refuses an ad-hoc-signed PID 1
   the kernel would fail to exec init. The `-v` log would show it.
   **[Correction, and this is now the leading candidate rather than a
   long shot: the boot-args did NOT carry those three args. They were being
   sent with `setenv boot-args`, which this bootloader accepts and never reads,
   so the kernel booted with iBoot's hardcoded default and code-signing
   enforcement fully on. "This should be covered" was false for every run ever
   made. See "`setenv boot-args` is INERT on this bootloader" at the end of this
   file.]**
4. **Framebuffer console renders nothing** even though it booted (least likely
   now that RestoreLogo is sent) -- would make a working boot look dark.

### Concrete next steps (ready to implement)

- **entrypoint reboot-beacon** (best no-hardware signal): behind a compile-time
  switch, have entrypoint call `reboot(0)` as its very first action. If the ATV
  power-cycles seconds after `bootx`, entrypoint definitely reached PID 1 and
  the bug is downstream (mounts / merge_tree / final reboot); if it stays dark,
  the kernel never reached our launchd (panic / md0 / exec). This bisects the
  tree with zero hardware and was designed earlier -- just not wired in.
- **Verify the CI baked ramdisk**: download run 35530176871's
  `firmware-AppleTV3,2`, decrypt the RestoreRamDisk, loop-mount the DMG, and
  confirm `/sbin/launchd` is our entrypoint Mach-O and `/blackb0x` is populated
  and the HFS is clean.
- **Watch the HDMI output** during a full run (framebuffer is up via
  RestoreLogo) and report the last line -- this likely settles it directly.
- Optional: have entrypoint write an early marker onto NAND (`/mnt1/.../var`,
  disk0s1s2) so that after forcing DFU and booting `--stock-ramdisk`, the marker
  can be read back to confirm how far entrypoint got.

Commits this session: xpwn `4481d66`; main `9d663c4` (entrypoint ABI),
`1bb9a3e` (--tether-boot), `38bf62a`/earlier (bake-iboot/-z, now walked back),
`fb808d3` (IMG3 16-align + -z walkback), `b439e38`/`c1941ff`/`a21a57d`/`059d38e`
(--tether-boot rd=disk0s1s1, RestoreLogo, advisory version preflight).

## Can an old iBoot boot a NEWER build's kernelcache/DeviceTree/ramdisk? (research, then the `--stock-firmware-new` removal)

`--stock-firmware-new` (added in "Stock diagnostic paths after the decryption
strip" above) existed to answer exactly one question: *can blackb0x's patched
10B329a iBSS/iBEC hand off to a stock OS suite from a newer, currently-signed
build?* It was never run to a conclusion on hardware. Before spending more
hardware time on it, the question was researched directly, because if the
answer is "no, structurally," then every run of that flag produces a failure
indistinguishable from the ones this project is already chasing — and a
diagnostic whose negative result means nothing is worse than no diagnostic.

### What was established

**1. iBoot does not version-check the images it loads.** IMG3 carries a `VERS`
tag, documented by The Apple Wiki's IMG3 File Format page as "iBoot version of
the image," alongside `TYPE`, `BORD`, `CHIP`, `SEPO` and `PROD`. Nothing found
treats `VERS` as enforced — not the wiki, not iBoot32Patcher, not the vendored
idevicerestore `img3.c` this repo already compiles. The checks iBoot really
makes, and the ones iBoot32Patcher defeats, are the RSA signature (`SHSH`/
`CERT`), the APTicket/personalization digests, and the img3 `TYPE` against the
tag being requested. So "the older iBEC rejects the newer kernelcache *because
it is newer*" is **not** the failure mode. That much is settled.

**2. But iBoot is not a passive loader — it is the DeviceTree's co-author, and
that is where mixing breaks.** The Apple Wiki's DeviceTree page states plainly
that the bootloader "populates the various entries of the tree and then passes
it to XNU"; the copy shipped in the IPSW is a template, not a boot-ready tree.
The decisive source is NyanSatan's *Running unsupported iOS on deprecated
devices* — notable here because this project already vendors his
`checkm8_bootkit`, and because it is the only published, detailed account of
precisely our scenario on 32-bit Apple hardware: booting an **iOS 6.0**
kernelcache under **iOS 5.1.1's iBoot** on an S5L892x (iPhone 3GS, then iPod
touch 3). His findings, in his words:

- "The most broken thing was DeviceTree — iOS 6 added a lot of new nodes and
  properties." He had to write a Python DeviceTree differ to compute the
  5.1.1→6.0 node/property delta and graft it into the older tree by hand
  (including stripping iPhone-specific entries before applying it to an iPod).
- One of the new properties is `nvram-proxy-data` in the `chosen` node, and it
  must hold a raw NVRAM dump: "leaving it empty will make kernel get stuck
  somewhere very early."
- Fixing that required patching **iBoot itself** — "replacing a call to
  `UpdateDeviceTree()` with my own little function" that calls the real one and
  then additionally populates `nvram-proxy-data` and `random-seed`. The old
  iBoot had simply never heard of the property, so nothing filled it.

That is the crux, and it generalizes: **the set of DeviceTree properties an
iBoot knows how to populate at runtime is frozen at that iBoot's own build.** A
newer kernel that requires a newer runtime-filled property gets an empty one
and hangs before it produces console output. There is no signature error, no
`bootx` rejection, no boot-failure-count increment — it looks exactly like
"nothing happens," which is the symptom this project has been bisecting for
several sessions already.

The mechanism is corroborated independently of NyanSatan. A QEMU-iOS-boot
writeup reports the kernel crashing immediately on an unmodified device tree
because it expected iBoot to have populated the timer frequency and an early
random seed, which had to be scripted in by hand. ChefKiss's Inferno discussion
#117 shows the same class of failure from the other direction — a newer kernel
against an older DeviceTree panicking on missing `amcc` / `carveout-memory-map`
nodes, with grafting the newer node in as the only fix.

Note what NyanSatan's success does *not* show: he did not send the newer
build's DeviceTree verbatim. He built a hybrid tree and patched iBoot's
tree-population routine. `--stock-firmware-new` did neither — it sent Apple's
newer DeviceTree unmodified through an iBEC patched only by iBoot32Patcher
(signature, boot-args, KASLR, ticket check; nothing touching `UpdateDeviceTree`).

**3. `boot_args` is a secondary, unquantified hazard.** XNU's 32-bit ARM
`boot_args` (`pexpert/pexpert/arm/boot.h`) is explicitly versioned:
`Revision`/`Version` header fields with `kBootArgsRevision` = 1,
`kBootArgsRevision2` = 2, `kBootArgsVersion1` = 1, `kBootArgsVersion2` = 2; the
revision-2 layout is the one carrying `bootFlags` and `memSizeActual` after
`CommandLine[]`. The kernel side does **not** gate on it — no revision check or
panic exists in `osfmk/arm/arm_init.c`, which only ever reads `memSize`,
`physBase` and `virtBase`. So an older iBoot filling a revision-1 struct for a
kernel compiled against revision 2 yields silently-garbage fields rather than a
clean refusal. **Honest caveat: which 32-bit build bumped that revision was not
established.** Apple never published `pexpert/pexpert/arm/` for the xnu-2050 /
2422 / 2782 drops (iOS 6 / 7 / 8); the header read here is xnu-4570 (2017). So
this is a plausible mechanism, not a demonstrated one, and specifically *not*
evidence of a hard 6→7 or 7→8 boundary. No such documented boundary was found.

**4. Nobody does this in practice, and the real tools structurally cannot.**
idevicerestore resolves every component through a single `build_identity` out
of one BuildManifest (`build_identity_get_component_path`); there is no code
path that mixes manifests, and the one published example of a genuinely mixed
restore had to hand-write a replacement `BuildManifest.plist` to do it. Every
tethered-boot / SSH-ramdisk toolchain checked — SSHRD_Script, LukeZGD's
Legacy-iOS-Kit (which covers exactly our 32-bit A5 hardware), mineek's tethered
downgrade guide, NyanSatan's own dualboot pages — takes iBSS, iBEC, DeviceTree,
trustcache (where applicable), kernelcache and ramdisk from one IPSW.
SSHRD_Script's "the iOS version doesn't have to be the version you're currently
on" is about the *device's installed OS*, not the chain: the booted suite is
always one self-consistent build. That is the community's whole answer to
cross-version work — vary the device, never the suite. This is "no one does
this" rather than "this cannot work," and it is reported as such; on its own it
proves nothing. It matters only as corroboration that nobody has found the
shortcut either.

### Verdict and confidence

- **High confidence (~85%)** that `--stock-firmware-new` *as implemented* —
  Apple's newer kernelcache/DeviceTree/ramdisk sent verbatim under an
  iBoot32Patcher-patched older iBEC — cannot produce a booting system.
- **High confidence** that its failure would be an early, silent hang with no
  console output and no boot-failure-count increment, i.e. **uninterpretable**:
  indistinguishable from the failures it was meant to help distinguish.
- **Low confidence that it is fundamentally impossible.** NyanSatan's work is a
  direct counterexample to the strong claim: an old iBoot *can* boot a newer
  kernel. It just needs a hand-merged DeviceTree and an iBoot patched at
  `UpdateDeviceTree`, which is far outside what this project's patcher does and
  well beyond what a diagnostic flag is worth.

Per the standing instruction — if research is inconclusive, assume it cannot
work — the flag is treated as unusable.

### Corrected afterwards: the owner had already run it, and it failed

The paragraph that stood here said this conclusion was reasoned from published
third-party evidence and that nobody had run `--stock-firmware-new` against an
AppleTV3,2 and watched it fail. That was wrong, and the correction matters
enough to record rather than quietly edit away. The owner had in fact tried the
flag on real hardware, and it did not work. So the removal rests on a hardware
observation after all, not only on the assume-it-cannot-work instruction.

Be precise about what that does and does not establish, because it is easy to
over-read. It confirms the ~85% claim — the flag *as implemented* does not boot
this device — and it is consistent with the DeviceTree mechanism above. It does
**not** upgrade the low-confidence claim to a proof of impossibility: a silent
early hang is exactly what a missing runtime-filled `/chosen` property would
produce, but it is also what half a dozen unrelated faults would produce, and
that indistinguishability was the whole reason the flag was judged useless as a
diagnostic. NyanSatan's counterexample still stands, and still costs a
hand-merged DeviceTree plus an `UpdateDeviceTree` patch.

If anyone ever wants the real measurement, the path back is in git history, and
the thing to add alongside it is a DeviceTree differ, an `UpdateDeviceTree`
patch, and the reboot beacon — never the flag on its own, which is precisely
the configuration already known to fail without saying why.

### What changed as a result

The old/new split is collapsed back to a single flag:

- `--stock-firmware-new` is **removed**, along with everything that existed only
  to support the distinction: `CliOptions::stockFirmwareOld`/`stockFirmwareNew`
  and the `stockFirmware()` helper collapse to one `bool stockFirmware`; the
  mutual-exclusion check between the two; the newest-signed-build selection
  branch in `runCli()`; and `downloadAndPatchComponents()`'s two-build
  `bootloaderBuild`/`buildToRequest` split with its paired `bootSuffix`/
  `osSuffix`, which existed for no other reason (in every surviving mode the two
  builds are equal, so one suffix derived from the manifest's real build ID is
  both simpler and strictly no less correct).
- `--stock-firmware-old` is renamed to plain **`--stock-firmware`**, with its
  behaviour unchanged: patched iBSS/iBEC from `kJailbreakTargetBuild` plus a
  stock kernelcache/ramdisk/DeviceTree from that *same* build — one
  self-consistent suite, which is the configuration every real tool uses.
  `--stock-recovery` and `--stock-securerom` now require `--stock-firmware`
  (they required `--stock-firmware-old` before; same flag, new name).

### Corollary for retargeting `kJailbreakTargetBuild`

Worth recording separately, because it is the opposite question and the answer
is the opposite: **moving the entire chain — patched iBSS/iBEC *and*
kernelcache/DeviceTree/ramdisk — to one matched newer build is not affected by
any of the above.** Everything that breaks here is version *skew* between iBoot
and the tree/kernel it hands off to. A matched suite has no skew: the iBoot
populating the DeviceTree is the one that shipped with it, so every
runtime-filled property the kernel wants is one that iBoot knows about, and the
`boot_args` revision matches by construction. That is exactly the configuration
idevicerestore, SSHRD_Script and Legacy-iOS-Kit all use, on this same 32-bit A5
hardware. From iBoot's perspective a newer target is fine. The real constraints
on retargeting live elsewhere entirely — what iBoot32Patcher and CBPatcher can
actually handle for that build, whether `keys/` has an entry, whether the
persistence payload branch in `stageVersionBranch()` has an answer for its
`ProductVersion`, and the 64 MiB ramdisk ceiling.

Sources, in the order they carried weight:

1. NyanSatan, *Running unsupported iOS on deprecated devices* —
   <https://nyansatan.github.io/run-unsupported-ios/> (and
   <https://github.com/NyanSatan/SundanceInH2A>). The only detailed published
   account of an old 32-bit iBoot booting a newer iOS kernel, and the source of
   the `UpdateDeviceTree`/`nvram-proxy-data` finding.
2. The Apple Wiki, *DeviceTree* (<https://theapplewiki.com/wiki/DeviceTree>) and
   *IMG3 File Format* (<https://theapplewiki.com/wiki/IMG3_File_Format>) — that
   the bootloader populates the tree, and that `VERS` is metadata.
3. XNU `pexpert/pexpert/arm/boot.h` and `osfmk/arm/arm_init.c` (xnu-4570.1.46,
   apple-oss-distributions) — the `boot_args` revision constants and the absence
   of any kernel-side revision gate.
4. Corroboration on mismatched-DeviceTree panics: xia0's *Boot Newer iOS with
   QEMU* (<https://xia0.sh/blog/boot-newer-ios-with-qemu-step-by-step>) and
   ChefKissInc/Inferno discussion #117. Practice-side: `verygenericname/
   SSHRD_Script`, LukeZGD's Legacy-iOS-Kit wiki, `mineek/iostethereddowngrade`,
   and idevicerestore's single-`build_identity` component resolution.

## The ramdisk is exonerated: a forensic teardown, an independent rebuild with the ORIGINAL tool, and a reboot beacon to bisect what is left

The previous CHECKPOINT left four ranked hypotheses for why the full jailbreak
"does nothing" while `--stock-ramdisk` boots. Two independent lines of evidence
were run against them this session — a byte-level teardown of the exact ramdisk
CI bakes, and a from-scratch rebuild of the ramdisk using **upstream
NSSpiral/Blackb0x itself** as the reference implementation. Between them,
**hypotheses 2 and 3 are dead and 4 is demoted**; what survives is hypothesis 1
plus one narrow, low-probability variant of 2. A compile-time reboot beacon has
been added to `entrypoint.c` to bisect exactly that remainder with no hardware
beyond the device itself.

The short version: **nothing about the container is wrong.** The image, the
volume, the pristine tree and the PID-1 binary all check out. What is left is
either the kernel never exec'ing our `launchd`, or `do_install()` dying after it
does.

### The artifact under test

Everything below is against the RestoreRamDisk published by **CI run
35533758121** (commit `23218bd`), for `AppleTV3,2` / 10B329a — i.e. the exact
bytes a user's `blackb0x` run downloads, not a local bake. It is decrypted with
the **stock** 10B329a RestoreRamdisk key/IV (so the keys are right), and the
comparison baselines are a freshly-decrypted **pristine** Apple ramdisk for the
same build and the original tool's own rebuilt output. Evidence files:
`ci/READY`, `ours-full-listing.txt`, `ours-files.txt`, `stock-files.txt`,
`ours-blackb0x-tree.txt`, `stock-tree.txt`, `nonroot-owned.txt`.

### 1. The container is sound, measured rather than assumed

- **IMG3 wrapper**: 38,631,812 B with Apple's exact tag sequence
  (`TYPE`/`DATA`/`SEPO`/`KBAG`×2/`SHSH`/`CERT`), identical in shape to both the
  pristine ramdisk and the original tool's output. The DATA element decrypts
  cleanly under the stock key/IV.
- **Volume**: bare HFS+ (`H+`, v4), 4096-byte allocation blocks, not journaled,
  volume name `ramdisk` — the pristine volume's own personality, preserved,
  because the bake grows Apple's volume instead of synthesizing a new one.
- **`fsck_hfs -n -f` is clean.** The clean-unmount bit is set, and
  `blockSize × totalBlocks` equals the file size exactly — no truncation, no
  trailing slack, no UDIF/koly confusion.
- **Size**: 9,431 blocks (36.8 MiB) with **266 free** (~1.09 MB). Worth stating
  plainly because it inverts an earlier worry: the known-good **stock** ramdisk
  that boots today has **zero** free blocks — Apple ships it exactly full. We
  have strictly more headroom than the configuration already proven on this
  hardware.
- **Pristine tree intact**: all **237** stock entries are present and
  byte-identical to the freshly-decrypted stock ramdisk, with exactly one
  exception — `/sbin/launchd`'s content. Nothing was deleted, nothing else was
  modified. (The original tool, by contrast, also clobbers
  `/private/etc/rc.boot` and adds to `/bin`, `/sbin`, `/usr/bin`, `/usr/lib`,
  `/private/etc/ssh` and `/private/var/root`.)

### 2. Our `/sbin/launchd` is what we think it is

Read directly out of the mounted CI image:

- 13,200 B, thin Mach-O `arm_v6`, `MH_EXECUTE`, `NOUNDEFS`.
- `LC_UNIXTHREAD` with `pc = 0x1a84` = `_entry`, **bit 0 clear** — ARM mode, not
  Thumb, which is what the hand-rolled `svc` wrappers require.
- `LC_CODE_SIGNATURE` present; **no `LC_LOAD_DYLIB`, no `LC_LOAD_DYLINKER`** —
  the freestanding invariant holds in the shipped artifact, not just in the
  build tree.
- Mode `0555`, owner `root:wheel` — Apple's own metadata, preserved because the
  splice rewrites content in place rather than `rm` + copy.
- **Apple's `UF_COMPRESSED` flag is correctly cleared.** This matters more than
  it looks: the pristine `/sbin/launchd` is decmpfs-compressed, and had the
  splice left the flag set over plain content, the kernel would have needed a
  decompressor to exec PID 1 and would have read garbage instead. It did not.
- Ad-hoc signature, `Identifier=com.apple.launchd`, **no entitlements** — by
  design; `entrypoint/Makefile` passes only `ldid -S -Icom.apple.launchd`.

And `/blackb0x` is fully populated, not a stub or a half-copy: **1,118 regular
files** (plus 214 directories and 129 symlinks), of which **1,047 carry decmpfs
ZLIB compression** from `afsctool` (types 3 and 4 only — no LZVN/LZFSE, which
would be unreadable on this 2013 kernel). The dpkg status file lists **57
packages** including `xyz.regulad.blackb0x`; the local-debs repo has a real
`Packages` index; `postinstall.sh` is present; `bash`, `dpkg` and `apt-get` are
all non-truncated.

### 3. An independent rebuild using the ORIGINAL upstream tool

The strongest evidence here is not a measurement of our artifact at all — it is
a **working reference built from the same pristine input**. `NSSpiral/Blackb0x`
is alive, public and unrenamed; it was cloned at HEAD `226a1e6`, the ramdisk
recipe was extracted from `Patcher.mm`'s `-patchRamdisk:ssh:` (lines 335-516)
plus `-moveFileFromBundle:fileType:`, and a standalone driver was shimmed around
it. **Four stubs, each logged in `build_original_ramdisk.sh` as `STUB n:`**: the
cached IPSW component instead of a re-download (byte-identical), the 10B329a
RestoreRamdisk key/IV hardcoded instead of parsed from the bundle's `.keys`
plist (verified the bundle's plist and our `keys/` copy agree), `decrypt()`
called through a 12-line `main()` over our own `src/Img3Crypt.cpp` (the same
reimplementation of xpwn's `xpwntool` `main()`), and
`-imagekey diskimage-class=CRawDiskImage` added to `hdiutil` because current
macOS will not autodetect a bare HFS+ payload. Nothing else. **It built a real
60 MB ramdisk**, now stashed at `blackb0x-scratch/original/out/`.

The original's recipe, faithfully — it is much simpler than ours, and every
difference from ours is therefore informative:

1. `decrypt()` the IMG3 to a raw payload. On 10B329a that payload is *already* a
   bare HFS+ volume (9,953,280 B = 2430 × 4096, **0 free blocks**); nothing is
   converted.
2. Best-effort detach/eject cleanup; `rm -rf` + `mkdir /tmp/ramdisk_create`.
3. *(AppleTV2,1 4.x only, not this device)* the one `hdiutil create -srcfolder
   -format UDRW -layout NONE` rebuild-from-scratch case.
4. **`hdiutil resize -size 60MB`** — a fixed, hardcoded 60 MB. No content
   measurement, no shrink pass.
5. `hdiutil attach -mountpoint /tmp/ramdisk_create` — **no `-owners on`**, no
   `-nobrowse`, no `-readonly`.
6. `tar -xvf ssh.tar` — `bin/bash`, `bin/sh`, `bin/ls`, `bin/mount.sh`,
   `sbin/sshd`, `usr/bin/{device_infos,scp}`, `usr/libexec/sftp-server`, five
   dylibs, a full `private/etc/ssh/` **with pregenerated host keys**, and it
   overwrites the stock `private/etc/rc.boot` with a 369-byte shell script.
7. `tar -xvf RamdiskBins.tar` — `usr/bin/{dirhelper,plutil,untar,uname,gzip,
   otool,ldid,sleepcmd,tar}` + `libiconv.2.dylib`.
8. `tar -xpzf ATV-Cydia.tgz` into `/files/cydia/` — 15.6 MB, a full prebuilt
   Cydia/dpkg/apt/openssh root tree.
9. `tar -xvf Debs.tar` into `/files/` — 9 compatibility `.deb`s + a repo list +
   a pubkey.
10. `tar -xpzf p0sixspwn.tgz` into `/files/p0sixspwn/`.
11. **The "Anthrax" launchd**: `rm /sbin/launchd`, then `copyItemAtPath:` the
    bundled prebuilt `Files/launchd` (37,728 B, armv6) over it. Note *remove +
    copy*, not splice — so it lands with the bundle file's mode `0755` and the
    **running user's `501:20`**, not Apple's `0555 root:wheel`.
12. `mkdir /mnt/` — the mountpoint its PID 1 uses (**`/mnt`, not `/mnt1`**).
13. Plain copies into `/files/`: `.blackb0x`, the two plists, `setup.sh`,
    `profile`, `fstab.atv`.
14. `tar -xpvf tihmstar-untether.tar` into `/files/etasonATV/`.
15. More copies into `/files/`: `rtbuddyd.bin`, icons, repo lists, pubkey.
16. `ssh.tar` again, **whole, to `/ssh.tar`** at the volume root (3.7 MB) — the
    on-device installer re-extracts it onto the device.
17. `hdiutil detach`.
18. `decrypt()` again to re-encrypt and re-wrap, using the **original IMG3 as
    the template** (TYPE/SEPO/KBAG×2/SHSH/CERT cloned verbatim; only DATA
    changes).

No `resize -size min` shrink, no `afsctool`, no chown/chmod pass, no `ldid` at
bake time (its launchd ships presigned), and **the app runs unprivileged
throughout — no `sudo` anywhere**. Result: `out/original-ramdisk-decrypted.dmg`,
60 MB (15360 × 4096, 652 free), 1214 files / 215 dirs, `fsck_hfs` clean; wrapped
as a 62,916,996 B IMG3.

### 4. Side by side, and the ranked differences the rebuild found

| | **original** | **ours (CI)** | **pristine stock** |
|---|---|---|---|
| IMG3 | 62,916,996 B, Apple's tag layout | 38,631,812 B, **same layout** | 9,955,716 B, same |
| volume | HFS+ `H+` v4, 4096 B, unjournaled, `ramdisk` | identical personality | identical |
| total / free | 15,360 blk (60.0 MiB) / 652 free | 9,431 blk (36.8 MiB) / 266 free | 2,430 blk (9.5 MiB) / **0 free** |
| files / dirs | 1,214 / 215 | 1,414 / 286 | 236 entries |
| `fsck_hfs -n` | clean | clean | clean |
| PID-1 mountpoint | `/mnt` | `/mnt1` | both exist pristine |
| PID 1 owner/mode | **501:20, 0755** | 0:0, 0555 | 0:0, 0555 |

Ranked by how plausibly each could explain the failure:

1. **One bake-time-guessed persistence payload, possibly the wrong one.** See
   "two real functional defects" below; this is the single largest behavioural
   divergence found and it is a real bug regardless of the boot question.
2. **No fallback tooling on the ramdisk at all.** The original carries a working
   userland — `bash`, `sh`, `ls`, `sshd` with pregenerated host keys, `tar`,
   `gzip`, `ldid`, `otool`, `plutil`, `scp`, `sftp-server`. Ours carries none of
   it: our `/bin` is the stock seven binaries and `/usr/bin` is `sed` +
   `TiSerialFlasher`. **This is not why the boot fails — it is why the failure
   is invisible.** The original's ramdisk could be booted in its SSH variant and
   inspected live over the network. Ours cannot be inspected at all.
3. **1,047 decmpfs-compressed files inside `/blackb0x`**, where the original's
   `/files` has zero. Types 3 and 4 are demonstrably supported by this era's
   kernel (Apple's own pristine ramdisk uses type 4 for 15 files) and no
   LZVN/LZFSE was found, so this is *probably* fine — but every one of those
   files is read by `merge_tree()` as PID 1 on a 2013 kernel, and the original
   never exercised that path. Cheap check if the overlay ever copies out as
   zero-length or garbage: re-bake with the `afsctool` step disabled.
4. **Volume slack**: 266 free blocks vs the original's 652. `md0` is mounted
   read-write, so near-zero slack on a r/w root is tighter than anything the
   original shipped. Low risk — nothing in `entrypoint.c` writes to the ramdisk
   — but stopping the `resize -size min` shrink is free.
5. **Ownership is inverted** (next section — and demonstrably not load-bearing).
6. Cosmetic/by-design, recorded so nobody re-investigates: `/files` vs
   `/blackb0x`; `/mnt` vs `/mnt1` (both pristine dirs); prebuilt Cydia tarball
   vs real bake-time dpkg; `/ssh.tar` at the volume root; the original's
   collateral `rc.boot` overwrite (irrelevant — its launchd never runs it);
   signature identifier `launchd` vs `com.apple.launchd` and CD v0x20200 vs
   v0x20400; the original's compiled-in boot-args vs our runtime superset.
   **[The last item is struck: it was misfiled here. "Compiled-in boot-args vs
   our runtime superset" was neither cosmetic nor by design — it is the one
   difference on this list that turned out to matter, and it is the leading
   candidate for the whole failure. The original baked its args into iBEC
   because that is the only channel this bootloader has; our "runtime superset"
   was a superset of nothing, since `setenv boot-args` is accepted and never
   read back. This teardown spotted the difference, recorded it accurately, and
   then dismissed it. See "`setenv boot-args` is INERT on this bootloader" at
   the end of this file.]**

### 5. armv6 as PID 1: settled, and it is NOT the cause (~97%)

This had been an open worry — that a 2013 armv7 kernel might refuse to exec a
thin armv6 binary as PID 1, which would look exactly like "nothing happens."
It is answered, and the decisive evidence is hardware, not source reading.

**The original upstream "Anthrax" `/sbin/launchd`, read out of the original's own
rebuilt ramdisk, is itself a thin Mach-O `cputype=12 cpusubtype=6` (arm_v6)**,
`LC_UNIXTHREAD` `pc=0x590c` (bit 0 clear, ARM mode), `LC_CODE_SIGNATURE`, no
dylibs — the same header shape as ours, down to `ncmds 9` / `sizeofcmds 660`.
And that ramdisk **worked on this exact device and this exact build.** Whatever
else is wrong, armv6-as-PID-1 is not it.

The mechanism, for the record, since "armv6 died in iOS 7" gets repeated as
folklore. Thin binaries **do** go through `grade_binary()` twice — once in
`bsd/kern/kern_exec.c`'s `exec_mach_imgact()` at the `grade:` label, and again
in `bsd/kern/mach_loader.c`'s `parse_machfile()`, alongside a
`cputype != cpu_type()` equality test. Both pass:

- The ARM `grade_binary()` in `bsd/dev/arm/kern_machdep.c` (published from
  xnu-4570.1.46 onward and **byte-identical through xnu-8792**) is a
  fall-through ladder, not a table lookup. A host `CPU_SUBTYPE_ARM_V7` with an
  armv6 exec subtype misses the inner switch, falls into the V6 case, and
  returns **4** — accepted, just at a lower grade than a native match.
- `cpu_type()` on ARM returns `CPU_TYPE_ARM` unconditionally, so the equality
  test in `parse_machfile()` is satisfied by any ARM binary regardless of
  subtype.
- `dyld-210.2.3` (the iOS 6 dyld) carries the matching compatibility row
  `{V7, V6, V5TEJ, V4T, ALL}` — irrelevant to us since we load no dyld, but it
  confirms the same policy on the userspace side.

Neither tree contains anything that would have dropped armv6 in iOS 7.

The one genuinely surprising survey result, worth writing down because it looks
like counter-evidence and is not: **the stock ramdisk is 78 Mach-Os, 100%
arm_v7, zero armv6.** But our own `/blackb0x` overlay ships **14 thin armv6
executables plus 8 armv6 dylibs** — `uicache`, `sbreload`, `ldid`, `cynject`,
`ldrestart`, `MSUnrestrictProcess` — which is simply the standard Cydia payload
that routinely execs on armv7 iOS 6 devices in the field. So "everything in
Apple's tree is armv7" is a build-target artifact of Apple's own toolchain
settings, not a kernel constraint.

**Decision: keep `-arch armv6`.** An armv7 `-marm` build was tried and proven to
work — it satisfies every CI invariant — but it buys nothing, and changing the
target for no reason would only muddy the comparison against the original, which
is armv6 too.

One safety note that is not obvious and is worth keeping: **you cannot silently
end up with Thumb here.** Omitting `-marm` on an armv7 build is a *hard build
error*, because the `rsbcs` in `__syscallN`'s inline asm (the carry-flag error
convention, added in the ABI audit above) cannot be assembled outside an IT
block. The build fails loudly rather than producing a subtly broken PID 1.

### 6. Ruled out decisively: "PID 1 must be root-owned / mode 0555"

The original's `/sbin/launchd` is **mode 0755, owned by uid 501 gid 20**, and
**1,188 of its 1,422 objects are 501:20** — because the app runs unprivileged
and attaches without `-owners on`, so everything it writes lands as the building
user. It worked. Ownership and mode on PID 1 are therefore not load-bearing for
this boot, full stop.

That makes our own ownership drift a **correctness and security wart, not the
boot failure**: 214 entries under `/blackb0x` are non-root, including 102 at
`501:20` (the CI runner's own uid) and, worse, `/blackb0x/untether` at
`1000:985` — a *Linux build-container* uid/gid that has no meaning on iOS at
all. Four more objects are `501:501`. These should be fixed; they are not why
the device does nothing.

### 7. The reboot beacon: a proof-of-life signal, implemented

The "USB re-enumeration as a proof-of-life beacon" idea sketched earlier in this
log is now wired in. It is a **compile-time, default-OFF** diagnostic in
`entrypoint/entrypoint.c`'s `entry()`, guarded by `BLACKB0X_REBOOT_BEACON` — an
integer giving the approximate pre-reboot delay in seconds, `0` meaning compiled
out entirely. It is the **very first thing `entry()` does**, before the console
open, before the disk wait, before the mounts, before `do_install()`:

    busy_wait(N); sys_reboot(0); for (;;) { }

**Why it earns its place.** This device has no usable console — its UART is on
internal hardware test-points, not the micro-USB — so "nothing happens" cannot
distinguish *the kernel never exec'd us as PID 1* from *it exec'd us and we died
somewhere in the install path*. Those two have completely different fixes. A
reproducible power-cycle a few seconds after `bootx` proves our code reached
PID 1 and convicts the install path; staying dark convicts the md0/HFS mount or
the exec/AMFI path. One bit, zero hardware, and it splits the remaining tree
exactly in half.

Details that are deliberate rather than incidental:

- **The trailing infinite loop is load-bearing.** If `reboot()` ever returns,
  PID 1 must not fall through into the real install path and muddy the signal.
- **It deliberately skips `set_auto_boot()`**, so the device is expected to come
  back to **RECOVERY**, not the NAND OS. That is a second free confirmation, not
  a bug — do not "fix" it.
- **It goes through the `sys_reboot()` wrapper, never a hand-rolled `svc`**,
  specifically because that wrapper carries the recent ABI fixes: the two-arg
  `reboot(int opt, char *msg)`, `RB_AUTOBOOT = 0`, and carry-flag error
  negation. A beacon built on the old broken wrapper would be its own
  experiment.
- **Makefile wiring is `BLACKB0X_REBOOT_BEACON ?= 0` plus a conditional `-D`,
  and it is meant to be set as an ENVIRONMENT VARIABLE, not a `make` variable.**
  That is not a style preference: `BakeRamdisk.cpp` re-runs `make clean all`
  itself via fork+execvp, and that child inherits `environ`, so

      BLACKB0X_REBOOT_BEACON=10 ./build/blackb0x ...

  plumbs the beacon through an entire jailbreak run with no C++ changes at all,
  whereas a hand-run `make BLACKB0X_REBOOT_BEACON=10` would simply be overwritten
  by the bake-time rebuild. The recipe also prints a loud warning when the beacon
  is on, since that binary installs nothing.

Verified: both configurations build; `file` reports `Mach-O executable arm_v6`;
`LC_UNIXTHREAD` present; zero `LC_LOAD_DYLIB`/`LC_LOAD_DYLINKER`; `ldid` signs
as `com.apple.launchd`; and **the default build's SHA-256 is byte-identical
before and after the change** — the shipped artifact is provably unaffected.

### 8. Which CHECKPOINT hypotheses are now dead

- **Hypothesis 2, "the kernel cannot mount `md0` because the rebuilt DMG/HFS is
  malformed" — DEAD as stated.** The image is not rebuilt (the bake grows
  Apple's own volume), `fsck_hfs -n -f` is clean, the clean-unmount bit is set,
  `blockSize × totalBlocks` equals the file size, the IMG3 tag layout is Apple's,
  and the pristine tree is byte-identical except the one file we meant to
  change. The narrow surviving variant is *size*, not malformation: see below.
- **Hypothesis 3, "entrypoint exec/signing — AMFI refuses an ad-hoc-signed
  PID 1" — DEAD.** The original's PID 1 is ad-hoc signed with identifier
  `launchd`, CD v0x20200, no entitlements, mode 0755, uid 501, armv6 — strictly
  *less* conformant than ours on every axis — and it booted on this hardware.
  With `cs_enforcement_disable=1 amfi=0xff amfi_get_out_of_my_way=1` on top,
  there is no signing theory left standing.
  **[REVIVED, and it is now the leading candidate. The verdict above turns on
  two premises. The first — the original's PID 1 being less conformant than
  ours and booting anyway — is still true and still correctly measured. The
  second, "with `cs_enforcement_disable=1 amfi=0xff amfi_get_out_of_my_way=1`
  on top", is false: those args were delivered with `setenv boot-args`, which
  this bootloader accepts and never reads, so they were on top of nothing and
  every boot ran with code-signing enforcement active. The original's PID 1
  booted *because its iBEC carried those args baked in*, which is precisely the
  comparison this bullet thought it was controlling for. So the hypothesis was
  right about the mechanism and wrong about which component was failing to
  supply the bypass. See "`setenv boot-args` is INERT on this bootloader" at the
  end of this file.]**
- **Hypothesis 4, "the framebuffer renders nothing" — DEMOTED, not disproven.**
  RestoreLogo is sent on the install path so the framebuffer is initialized, but
  nobody has yet watched the HDMI output during a full run and reported the last
  line. That observation remains the cheapest unspent measurement.
- **Hypothesis 1, "entrypoint reaches PID 1 but hangs or dies before its final
  reboot" — SURVIVES, and is now the leading candidate.**

### 9. Two real functional defects found along the way

Neither is proven to be the current failure. Both are genuine bugs that would
independently produce "nothing happened" on a device that boots perfectly.

**(1) We ship ONE persistence payload, chosen at BAKE time.**
`BakeFirmware.cpp` resolves `productVersion` from
`newestVersionForDevice("AppleTV3,2")` — an ipsw.me lookup for the newest OS the
*model* ever shipped — and hands it to `stageVersionBranch()`, which stages
exactly one of etasonATV / iOS 7 tether / p0sixspwn. In practice that is always
**etasonATV, i.e. iOS 8.4 only**. The original carries **all three** and selects
at **runtime**, reading the device's real
`/mnt/System/Library/CoreServices/SystemVersion.plist` and branching on
`8.4*` → etasonATV, `7*`/`8*` → iOS 7 tether, `6.1.4` → p0sixspwn, else
"unsupported version". So if the target device is not on 8.4.x we install a
payload that cannot work — *and it is worse than inert*:
`fixup_etasonuntether_rtbuddyd()` replaces `/usr/libexec/rtbuddyd` with a `jsc`
symlink, which on a non-8.4 system is an active break of a system daemon.
**Cheap check before the next hardware run**: read the device's real installed
`ProductVersion` over lockdownd in Normal mode and compare it to what the bake
logged.

**(2) The staged apt lists cache is EMPTY, by design, and `postinstall.sh` does
not survive it.** `scripts/build_deb_cache_apt.py` writes `apt-lists/` empty
deliberately — its own module docstring says so, and the reason is sound: the
lists this resolver could produce would describe the synthetic `file://` repo
built out of `debcache/`, not the real repos, and would be *actively misleading*
on-device. But `stageAptListsCache()` stages that empty directory at
`/private/var/lib/apt/lists/` anyway, and `postinstall.sh` runs under a bare
`set -ex` with `apt-get update || true` as its **one and only** permitted
failure. With no network, apt has no candidate for anything, the first
`apt-get install` fails, `set -e` aborts the script, `install-done` is never
written — and the device that just rebooted successfully still shows nothing
installed. This is a real, independent path to the exact symptom being chased.

### 10. What is actually left, and the recommendation

The container and the PID-1 binary are both exonerated. Two candidates remain:

- **(i) The kernel never execs our `launchd`.** Either the `md0` mount of a
  36.8 MiB ramdisk fails, or the exec is refused. The signing half is dead (§8).
  The size half is the honest remaining unknown: **our chain has only ever been
  PROVEN with the 9.5 MiB stock ramdisk.** A 36.8 MiB ramdisk load has never
  been confirmed on our iBEC. That said, the original demonstrably loads **60
  MB** on this same hardware, so this is low risk — it is simply not yet proven
  *for our chain*.
- **(ii) entrypoint runs but dies inside `do_install()`.**

The beacon bisects exactly these two, and that is the next hardware run.

**Recommendation beyond the beacon**: the reason the original's failures were
debuggable and ours are not is its fallback userland — `bash`, `sshd` with
pregenerated host keys, `tar`, `ldid`, delivered by `ssh.tar` + `RamdiskBins.tar`.
Porting that into the bake, or adding an explicit `--ssh-ramdisk` mode, buys a
real console on the device. Given that every remaining hypothesis is about
*what happens after `bootx` where nothing is observable*, that is likely worth
more than any number of further blind iterations.

Evidence and write-ups behind this section (outside the repo, scratch only):
`blackb0x-scratch/ci/` (the decrypted CI and stock ramdisks plus `READY`),
`blackb0x-scratch/ours-full-listing.txt`, `ours-files.txt`, `stock-files.txt`,
`ours-blackb0x-tree.txt`, `stock-tree.txt`, `nonroot-owned.txt`, and
`blackb0x-scratch/original/{RECIPE.md,COMPARISON.md,build_original_ramdisk.sh,out/}`.
Kernel-side sources for §5: xnu `bsd/kern/kern_exec.c` (`exec_mach_imgact()`),
`bsd/kern/mach_loader.c` (`parse_machfile()`), `bsd/dev/arm/kern_machdep.c`
(`grade_binary()`, xnu-4570.1.46 through xnu-8792), and `dyld-210.2.3`.

## The kernelcache decrypt failure was the *read* side of the same 16-align bug

The IMG3 alignment fix above cured the encrypt side and was verified against
`AppleTV3,2` 10B329a, which is exactly the build that had always worked. A
systematic sweep of all 101 known (device, build) tuples through `bake-iboot`
and `bake-kernel` — the first time every tuple had been run rather than the
handful that were already known-good — showed the other half of the bug was
still there. 66 of 86 fetchable tuples died in `decrypt()` with `error: cannot
open infile`, including `AppleTV2,1` and `AppleTV3,1` at 10B329a, the two the
CI bake matrix excludes.

`readImg3Element()` reads `header->dataSize` bytes of the DATA element.
`setKeyImg3()` then AES-CBC-decrypts `((header->size -
sizeof(AppleImg3Header)) / 16) * 16` bytes — the full 16-aligned body, which
Apple always pads out and which the encrypt-side fix now correctly writes. The
bytes between the two are never read, so they are the `calloc`'s zeros rather
than real ciphertext, and the whole final cipher block decrypts to garbage.
The LZSS stream then ends inside that garbage, `complzss` stops a few dozen
bytes short of its declared length, and `createAbstractFileFromComp()` refuses
the image. `writeImg3Default()` was taught to emit the whole encrypted body;
the reader was never taught to consume it.

The mechanism explains the distribution exactly, which is how it was cornered.
A build fails when the corrupted block contains real compressed bytes, so
`dataLength % 16 == 0` always decodes (7 of 7 in the sweep), `% 16 == 1` is a
coin flip (10B329a on `AppleTV3,2` is the survivor — one real byte in the bad
block, and the decoder had already emitted its last output), and everything
from `% 16 == 2` up fails. An off-device reimplementation settled causation
rather than correlation: the same DATA element, same keys, decrypted xpwn's
way yields 12,013,523 of 12,013,568 bytes; decrypted over the full aligned
body it yields exactly 12,013,568. Reading `header->size -
sizeof(AppleImg3Header)` instead of `dataSize` in that one `default:` case
flipped **66 tuples from failure to success with zero regressions**, and left
the re-encrypted 10B329a kernelcache the same length it already was.

So `KernelCache: patch failed` on `AppleTV2,1`/`AppleTV3,1` was never a
CBPatcher limitation, and the CI matrix comment's diagnosis was right all
along — it just outlived the fix that was thought to have cured it. Once the
read side matches the write side, CBPatcher handles **every** build this
project can fetch and decrypt except three: `AppleTV2,1` 8M89, 8C150 and
8C154 (4.1/4.2/4.2.1, genuinely no matching signatures), and `AppleTV3,2`
10B809, whose complzss header declares `0xa00006` against a stream that
produces exactly `0xa00000` — a one-build Apple oddity, not an alignment
problem.

The measured evidence lives in `misc/verified_patcher_compatible.txt`, whose
`kernel-readfix` column records the post-fix result for every tuple, and which
is regenerated by `scripts/gen_verified_patcher_compatible.py`. The fix is
`regulad/xpwn@78cc777` on the `legacy` branch, following this repo's rule that
a vendored change is committed and pushed to its fork rather than left as a
dangling local patch.

### What the reader was actually truncating

The fix takes the body size from the element's own `totalLength` minus its
12-byte header, floors it at `dataSize` (never follow a header into a negative
length — a malformed img3 whose `totalLength` undercuts its own `dataSize`
would otherwise read backwards), and over-allocates by `IMG3_AES_OVERREAD_PAD`
so wolfSSL's block over-read has somewhere harmless to land.

Stated as a property of the reader rather than as a kernelcache story: that
`default:` case silently truncated **any** element whose body was padded past
`dataSize`, and by the invariant established above that is **every encrypted
element** — Apple 16-aligns exactly those. The other tags that fall into the
same case (`TYPE`/`SEPO`/`SHSH`/`CERT`) escaped by accident of not being
encrypted: their padding is a 4-align of zeros, and `writeImg3Default()` emits
`dataSize` real bytes plus fresh zeros regardless, so dropping the tail on read
and regenerating it on write happens to round-trip. Nothing in the reader
distinguished the two cases. It truncated both; only one of them could notice.

### The build that "always worked" was corrupt too, by exactly one byte

Landing the fix produced a surprise worth recording, because it corrects
something this log previously asserted. The re-encrypted `AppleTV3,2` 10B329a
kernelcache keeps its exact length (6,007,124 bytes both before and after),
which is what the sweep reported — but it is NOT byte-identical. Decrypting
both outputs and decompressing them shows why: the plaintext differs in four
bytes, three in the complzss header's adler32 field and one at the very end of
the compressed stream, and the decompressed 10 MiB kernels differ in **exactly
one byte, the last one** (offset `0x9fffff`, `0x4c` before the fix where the
stock kernel has `0x00`).

That single byte is the whole story of why this hid for so long. `dataSize` is
`0x5b9fd9`, so nine real bytes fell inside the final corrupted cipher block —
but the LZSS decoder had already emitted all `0xa00000` output bytes by the
time it reached them, and the one byte of damage that did land in the output
sat in trailing padding rather than in code or data the kernel ever reads.
`patchKernel()` then recomputed the adler32 over the corrupted image, so the
container was perfectly self-consistent: `complzss` verified, iBoot accepted
it, and the device booted. Every check the pipeline could make passed, because
the corruption had been laundered into the checksum.

So the encrypt-side fix in 4481d66 was verified against an output that was
itself still subtly wrong, and `--stock-ramdisk` has been booting a
one-byte-corrupt kernel this whole time. The lesson to carry forward is that a
self-consistent artifact proves nothing about a pipeline that regenerates its
own checksums — the only trustworthy oracle is a byte comparison against
independently-derived plaintext, which is what finally settled it here.

## The newest Apple TV 3 builds are not key-blocked; they are unencrypted

`keys/` stops at 12H914 for both `AppleTV3,1` and `AppleTV3,2`, and 12H923,
12H937 and 12H1006 were assumed untargetable for want of published keys. They
are not. Apple shipped those builds — and 12H903 and 12H911 before them — with
**no KBAG element at all** in iBSS, iBEC or the kernelcache. The Apple Wiki
publishes only a RootFS key for them for precisely that reason, not because
anyone failed to extract the rest. The repository already half-encodes this:
`keys/AppleTV3,2/AppleTV3,2_12H914.keys` is a plist of empty strings for the
boot chain, which is the correct and complete key material for an unencrypted
component, rather than a placeholder nobody got around to filling in.

**But "this build is unencrypted" is not uniform across components, and
assuming it is would have broken the ramdisk bake.** An IMG3 tag-list probe
run directly against the remote IPSWs settles it per component. At 12H914 the
RestoreRamDisk is still genuinely **encrypted** — two `KBAG(56)` elements,
cryptStates 1 and 2, aesType 256 — which is exactly why that same `.keys` file
carries a real `RestoreRamdisk` key sitting beside all those empty strings, and
why 12H911's does too. Apple stopped encrypting the ramdisk somewhere between
12H914 (December 2020) and 12H923 (April 2021). From 12H923 onward, and at
12H1006, every component is clean: the ramdisk's entire tag list is
`TYPE DATA SEPO`.

An earlier revision of this section asserted the ramdisk was unencrypted at
12H914 as well. That was wrong, and it is the kind of wrong that costs a bake:
`bakeRamdisk()` must decrypt the ramdisk to mount it, so an empty key there
would have failed at the one step that cannot be skipped. The general lesson is
the one this log keeps relearning — check each component's own bytes, because
Apple's encryption policy changed per component and per build, not per build
alone.

Verified by construction rather than by inspection: running the real
`bake-iboot`/`bake-kernel` pipeline against synthetic all-empty `.keys` files
(via `BLACKB0X_IMAGEKEYS_DIR`, so `keys/` is never shadowed) patches and
republishes iBSS, iBEC and the kernelcache for all three builds on both
devices. `misc/verified_patcher_compatible.txt` records these as
`verified-unencrypted`, and its generator now makes this probe part of the
sweep — "no `.keys` file" is a question to answer, not an answer, and the
honest count of genuinely key-blocked Apple TV tuples is zero.

The practical consequence is that `kJailbreakTargetBuild` can move from
10B329a to **12H1006 (8.4.7)** for `AppleTV3,1`/`AppleTV3,2` — the newest
build either device ever received — with both patchers verified, no key hunt
required, and `stageVersionBranch()`'s existing `rfind("8.4", 0)` already
routing it to the etasonATV untether. `AppleTV2,1` tops out at 11D258 (7.1.2)
on the tethered `dirhelper` branch. What stands between that and a real
retarget is the xpwn read-side fix, three empty `.keys` files, the 64 MiB
ramdisk ceiling, and hardware — a patched bake is still not a boot.

This also retires, on its own terms, the "can an old iBoot boot a NEWER
build's kernelcache?" question documented above. That section's conclusion was
that version *skew* between iBoot and the tree it hands off is the hazard, and
that a matched suite has none by construction. Retargeting to 12H1006 is
exactly a matched suite: patched iBSS/iBEC, kernelcache, DeviceTree and
ramdisk all from the one build. It needs no cross-version handoff to work.

## `setenv boot-args` is INERT on this bootloader: the kernel has been booting with code-signing enforcement on

This is the strongest mechanism-level explanation this project has produced for
its central bug, and it is **not yet confirmed on hardware**. Read every
"explains" below as "explains, pending a hardware run" — the disassembly is
solid and reproducible, the fix is landed and verified against the artifacts it
produces, and nothing has yet been booted on a real AppleTV3,2 with it.

### The symptom, unchanged for weeks

`blackb0x --stock-ramdisk` — patched iBSS, patched iBEC, patched kernelcache,
patched DeviceTree, and **Apple's stock, untouched RestoreRamDisk** — boots on
a real AppleTV3,2. The actual jailbreak — the identical chain with **our**
baked ramdisk, whose `/sbin/launchd` is our ad-hoc-signed `entrypoint` binary —
does nothing at all.

Everything that differs between those two runs had been chased into the
ramdisk's *contents*: the forensic teardown above compared our baked image to a
pristine one file by file, rebuilt the original tool's ramdisk independently for
comparison, and exonerated the container, the HFS+ volume, the decmpfs
compression, the ownership, and armv6-as-PID-1. The ramdisk kept coming back
clean because the ramdisk was never the problem.

The difference is in the boot-args, and they were never reaching the kernel.

### Proving the channel inert: the disassembly

Two agents independently decrypted **this project's own published
`dist/iBEC-AppleTV3,2_10B329a`** — AES-256-CBC over the IMG3 DATA tag, keys out
of `keys/`, image base `0x9ff00000` per iBoot32Patcher's own
`get_iboot_base_address()` — and searched the plaintext, rather than reasoning
about what a bootloader ought to do.

- **`"boot-args"` has exactly ONE reference in the whole image.** The string is
  at file offset `0x31c7c` (VA `0x9ff31c7c`). The single 32-bit literal naming
  it is at `0x3dd1c`, sitting in iBoot's **settable-env-var name table**,
  immediately after `"auto-boot"` and before `"debug-uarts"`/`"filesize"`. That
  table is the list of variables you are allowed to *set*; membership in it is
  not a read. A separate scan of every Thumb-2 `MOVW`/`MOVT.W` pair that could
  materialize that address found **zero** hits, which closes the obvious escape
  ("the real xref is register-materialized, not in a literal pool").
- **The kernel-boot routine builds its command line out of compiled-in
  constants.** Its literal pool at `0x1b190`–`0x1b1a4` holds, in order: an
  empty string, `"rd=md0 nand-enable-reformat=1 -progress"`, a second empty
  string, `"is-tethered"`, `"%s force-usb-power=1 "` and `"%s "`. The code at
  `0x1af42` loads the hardcoded restore string and `0x1af46` loads the null
  string; `0x1af5c`/`0x1af66` query the `is-tethered` env var to select between
  those two arms; the winner is `snprintf`'d into `gBootArgs.commandLine`.
  **`env_get("boot-args")` does not appear anywhere on that path.**

So on AppleTV3,2's iBoot-1537.9.55, `setenv boot-args ...` over the recovery
protocol is **accepted, stores the value, and is never read back**. It does not
fail, it does not warn, and `irecv_send_command()` returns success — because the
command really did succeed. It set a variable. Nothing reads that variable.

### Why this explains the stock-vs-ours asymmetry exactly

With the `setenv` dead, the kernel had been receiving iBoot's own hardcoded
`rd=md0 nand-enable-reformat=1 -progress` on every single run, and **none** of
`amfi=0xff`, `cs_enforcement_disable=1` or `amfi_get_out_of_my_way=1`.

Code-signing enforcement was therefore active on every boot this project has
ever performed. Under enforcement an ad-hoc-signed binary cannot be exec'd as
PID 1 — and that is the one axis on which the two ramdisks differ:
`--stock-ramdisk` ships Apple's own properly-signed `launchd`, which needs no
bypass at all and boots exactly as it always did; ours ships `entrypoint`,
ad-hoc-signed with `ldid`, which needs the bypass and never got it. The
asymmetry is not a property of the ramdisk's contents, its size, its filesystem
or its ownership. It is a property of which of the two `launchd`s requires a
boot-arg that was never delivered.

Note what this does to hypothesis 3 of the CHECKPOINT section ("AMFI refuses an
ad-hoc-signed PID 1"), which was marked **DEAD** in the ramdisk teardown above.
The evidence that killed it was sound as far as it went — the original tool's
own PID 1 is strictly *less* conformant than ours on every axis and booted on
this hardware — but it was killed partly on the grounds that
`cs_enforcement_disable=1 amfi=0xff amfi_get_out_of_my_way=1` were "on top" of
it. They were not on top of anything. The original's PID 1 booted because the
original's iBEC carried those args **baked in**; ours did not carry them at all.
The hypothesis was right about the mechanism and wrong about which component
was failing to provide the bypass.

### Corroboration from the original tool

The original NSSpiral/Blackb0x is the only configuration ever observed to boot
on this hardware, so what it did is evidence and not merely precedent. Its
`Blackb0x/Source/Patcher.mm`'s `-patchiBEC:flags:ticket:` (clone at
`/Users/regulad/repositories/blackb0x-scratch/original/upstream/`) **baked** its
boot-args into the image, passing them as the `args` parameter of its vendored
`iBootPatcher(infile, outfile, args, RSA, debug, ticket, kaslr)` — that
library's spelling of iBoot32Patcher's `-b`. And it called that function
**twice** against one decrypted input, producing two iBECs differing only in the
baked string:

    args1 = "rd=md0 amfi=0xff cs_enforcement_disable=1 pio-error=0"
    args2 = "amfi=0xff cs_enforcement_disable=1 pio-error=0 amfi_get_out_of_my_way=1 cs_enforcement_disable=1"

(`args2` really does repeat `cs_enforcement_disable=1`, and really does omit any
`rd=`; both are reproduced here verbatim rather than tidied. `-v` is commented
out on both lines.)

This port had collapsed that two-iBEC design into one, on the explicit reasoning
that a runtime `setenv` made the second image redundant and was "strictly
better" — recorded above under "Boot-args moved out of the binary" and
"Tether-boot removed: one send flow, one iBEC". That reasoning was wrong at its
root. The two iBECs were not redundancy; they were **the mechanism**. A baked
string is a property of the binary, so a binary that boots off `rd=md0` and a
binary that boots off NAND physically cannot be the same file.

### The fix as landed

In the working tree, not yet committed at the time of writing:

- **`-b` is restored in `patchiBEC()`**, which now produces **two** patched
  iBECs from one decrypted input — identical in every flag but the `-b` string —
  published as `dist/iBEC-<tuple>` and `dist/iBECTether-<tuple>`. `blackb0x`
  picks one by mode (`--tether-boot`); it never needs both.
- **`bake-iboot` gained `--boot-args` / `--tether-boot-args` /
  `--extra-boot-args`.** This is the piece that keeps the baked design cheap to
  work with: arming a directive now costs a seconds-long, rootless `bake-iboot`
  re-bake instead of a ~30-minute rooted ramdisk bake.
- **`--extra-boot-args` on `blackb0x` now fails hard (exit 2)**, naming the
  `bake-iboot` command that does work. It is not warned about and not silently
  ignored, because the whole failure being corrected here was a flag that
  reached nothing while reporting success.
- **`setenv boot-args` is gone from `sendKernelCache()`.** `DeviceManager.cpp`
  carries no `kRamdiskBootArgs`/`kTetherBootArgs` of its own any more; it only
  reports which string the `dist/` component it is about to send was baked with.

**It is deliberately KEPT in `sendStockRestoreTail()`**, and that is worth
recording as a genuine curiosity rather than an inconsistency. That function's
entire value is being byte-for-byte what real `idevicerestore`'s
`recovery_enter_restore()` sends, so that a failure there can be attributed to
something other than a protocol difference; removing a command the reference
tool sends would trade a harmless no-op for a new variable in the one diagnostic
whose worth is having none. And the command is harmless for a reason nobody
could have spotted: its value is **byte-identical to iBoot's own hardcoded
default** (`rd=md0 nand-enable-reformat=1 -progress`, the string at `0x38847`
loaded at `0x1af42`). The stock restore path has always been getting correct
boot-args — from iBoot's fallback, never from its own command. The channel was
dead there too, and it was invisible precisely because the dead channel and the
live fallback agreed.

### The 179-byte ceiling, and the 127 it replaces

A baked string has a length limit, and it is not the one previously recorded.
Two independent limits were measured in the decrypted iBEC; the smaller binds.

1. **The relocation site.** `patch_boot_args()` only relocates when the injected
   string is longer than iBoot's own 39-byte default, and it relocates by
   `strcpy`ing — unbounded, with no length check of its own — over the "Reliance
   on this certificate..." string. In this image that C string is 193 bytes long
   (file offset `0x3ebf4`), so a write of up to 193 characters provably touches
   only bytes that were already inside it. But only the **first 179** are the
   pure-ASCII certificate boilerplate; the trailing 14 are DER bytes from the
   embedded Apple root cert that happen to precede the next NUL, and those vary
   between builds. 179 is therefore the figure that holds for *any* build rather
   than for this one.
2. **The kernel command line.** iBoot `snprintf`s the selected string into
   `gBootArgs.commandLine` with size `0x100` — confirmed as `MOV.W r1, #0x100`
   at file offsets `0x1af7a` and `0x1af9c`, the two `snprintf` calls in the
   kernel-boot routine — matching XNU's `BOOT_LINE_LENGTH` of 256 on 32-bit ARM.
   iBoot then appends its own `" force-usb-power=1 "` (19) and
   `" backlight-level=%d "` (~22), leaving roughly 214.

179 binds, and it is **enforced as an error, never a truncation**: both limits
truncate silently, which is the same failure class as the dead `setenv`, so
`bake-iboot` and `patchiBEC()` refuse an over-long string instead of shortening
one. The shipped strings are 81 (ramdisk) and 87 (tether) bytes, leaving ~90 for
directives.

**This supersedes an earlier 127-byte figure** that appears in this log and in
the code's history. That number was never wrong — it is
`DeviceManager::kMaxRecoveryCommandLength`, the budget of iBoot's recovery
*command* parser — it simply belongs to a channel a baked string never passes
through. Recorded rather than deleted because "127" is the kind of number that
gets remembered and re-applied.

### Verification: decrypt the output, don't trust a clean build

The patch was confirmed by decrypting the images actually produced, not by
observing that `bake-iboot` exited zero. In **both** published images the string
at `0x3ebf4` is the injected boot-args (the certificate boilerplate is gone,
overwritten as designed), the kernel-boot pool slot at `[0x1b194]` points at it,
and **both** arms of iBoot's select resolve to it — the boot-args arm (`LDR` at
`0x1af42`) and the former null-string fallback (`LDR` at `0x1af46`).

That last detail is why a baked string wins unconditionally and why no `setenv`
could ever have overridden one even on a bootloader that read the variable:
`patch_boot_args()` repoints **both** arms, so whichever way the `is-tethered`
test goes, the injected string is what gets `snprintf`'d.

This check is not optional hygiene. `patch_boot_args()` is the most invasive
patch in iBoot32Patcher and the only one that can mis-apply without saying so —
it `strcpy`s unbounded, scans byte-by-byte for an `IT` instruction with no
end-of-buffer guard (the author's own comment calls it "kinda hacky"), and
writes an 8-bit PC-relative immediate with no range check
(`ldr_rd_null_str->imm8 = (diff / 0x4)` truncates past 255 in silence). That
criticism was made correctly when `-b` was *removed*, and it survives the
decision to bring `-b` back; the answer to it is the length check plus this
verification, not avoidance of the patch.

### Confidence, stated plainly

What is proven: the disassembly, in this project's own shipped artifact. The
`"boot-args"` xref count, the contents of the kernel-boot literal pool, the
absence of `env_get("boot-args")`, the 193/179-byte certificate string, the
`0x100` `snprintf` size, and the post-patch state of both select arms are all
direct reads of real bytes, reproducible by anyone with the keys.

What is inferred: that this is *the* cause of the jailbreak's failure. The
inference is strong — it is a single mechanism that predicts the exact observed
asymmetry, it matches what the one known-booting configuration did, and it
identifies a specific missing precondition (code-signing bypass) for a specific
observed non-event (our PID 1 never running). But it is an inference. **No
device has been booted with the fixed chain.** The hypotheses the CHECKPOINT
section left standing — entrypoint reaching PID 1 and dying inside
`do_install()`, the empty apt lists cache, the single bake-time persistence
payload — are all still live, and at least two of them independently produce the
same "nothing happened". If the next hardware run still does nothing, this
section is a real bug fixed and not the root cause, and it should be recorded
that way.

### The meta-lesson, which this log keeps relearning

**A component that accepts a command and silently ignores it is worse than one
that errors.** `setenv boot-args` returned success for the entire life of this
project. Every layer above it was correct: the command was well-formed, the
transport delivered it, the return code was checked, and an
`extraCommandMustSucceed` flag was even added specifically to make sure a
failure there could not pass unnoticed. None of that could detect a command that
succeeded at doing nothing. The same shape has now appeared three times in this
log — xpwn's self-consistent-but-corrupt kernelcache, whose regenerated adler32
laundered the damage; `patch_boot_args()`'s unchecked 8-bit immediate; and this.

**The only trustworthy check is to inspect the artifact that was actually
produced.** A clean build, a zero exit status, a successful return code and a
self-consistent output are all statements about the pipeline, not about the
thing the pipeline made. This fix was verified by decrypting the two iBECs and
reading the bytes at `0x3ebf4`, `0x1b194`, `0x1af42` and `0x1af46` — the same
method that finally settled the IMG3 alignment bug, and for the same reason.

## "Kernelcache image not valid" was a stale `sigCheckArea`, and the gate that rejected it runs before any signature code

Retargeting to 12H1006 put a patched kernelcache in front of real hardware, and
iBoot refused it outright: **"Kernelcache image not valid"**. Every instinct,
and a good deal of this log, reads that string as "the signature bypasses did
not take". It did not mean that, and the reason it did not is worth more than
the one-line fix it produced.

### What the hardware had already ruled out

Two results from the owner's box bracket the failure before any disassembly.
`--stock-firmware` **boots** — a stock kernelcache loaded through *our* patched
iBSS/iBEC — so the RSA and ticket bypasses demonstrably work on this newer
iBoot. `--stock-ramdisk` **fails**, and the only material difference between
the two paths is that the second ships an image this project repacked. A byte
diff of our iBEC against stock confirmed the patcher side independently: six
changed regions, all accounted for, with `patch_rsa_check` at `0x9ff18a9e` and
`patch_ticket_check` at `0x9ff1c394` both present and correct. Compression had
already been exonerated twice (the "Size mismatch from lzss" entry, and then
both halves of the 16-align bug). Signatures worked, compression worked, and
the container was still rejected — which left the container.

### The gate at `0x9ff18288`, and why its *ordering* is the diagnosis

Disassembling the img3 validation routine in a real `AppleTV3,2` 12H1006 iBEC
settles it. Before the routine looks at a signature, a ticket or a KBAG, it
runs a pure bounds check on the root header, in this order:

```
len >= 20
magic == '3gmI'
sizeNoPack <= len - 20
sigCheckArea <= sizeNoPack        <-- ours failed here
sizeNoPack + 20 <= fullSize
```

Any one of these failing returns error `0x16` (malformed) and the routine never
reaches the crypto at all.

That ordering is the finding. **A bootloader error that reads like a signature
rejection can come from a structural header check that runs strictly before the
signature code**, and when it does, no amount of iBoot32Patcher work can touch
it — the patched instructions sit downstream of a branch that was never taken.
"Not valid" is not a statement about trust; it is the generic string for *this
routine failing anywhere inside itself*, and only the order of the checks makes
the symptom legible. Read in that order, the failure is not ambiguous for a
moment: the first four fields are pure arithmetic on our own output, so the
question "is the image signed correctly?" never arises until the arithmetic
agrees.

### The stale field

`sigCheckArea` in iBoot's reading of the root header is `shshOffset` in xpwn's.
`writeImg3Root()` recomputes `fullSize` and `sizeNoPack` from the tags it
actually emitted, but it only ever updated `shshOffset` inside the loop branch
that fires when an SHSH element is written. An unsigned image has no SHSH, the
branch never fires, and what survives in the header is the **template's**
value — read off the stock input, and meaningless the instant the payload
changes size.

```
10B329a  signed, TYPE DATA SEPO KBAG KBAG SHSH CERT
         sigCheckArea 6004932 <= sizeNoPack 6007100    booted
12H1006  unsigned, TYPE DATA SEPO
         sigCheckArea 7705528 >  sizeNoPack 7705416    rejected
```

7705528 is exactly the *stock* image's `sizeNoPack`. Our recompressed payload
is 112 bytes smaller, so the stale offset pointed 112 bytes past the end of our
own file. That is also why only the kernelcache broke: it is the only IMG3 this
project **shrinks**. iBEC, iBECTether, DeviceTree and RestoreLogo are patched
at identical size and land back on the same number by coincidence of arithmetic;
the RestoreRamDisk carried the same wrong field and passed only because it
**grows**, which kept the stale value inside the file by luck rather than by
construction. Every unsigned IMG3 repacked smaller than stock was being
rejected on hardware, and had been for as long as anything was repacked smaller.

### The fix, and what it was checked against

`regulad/xpwn@0520d77` on `legacy` tracks a `haveSHSH` flag through the element
loop and, when no SHSH was written, sets `shshOffset` to the freshly recomputed
`dataSize`. It has to run *after* that recompute, since it consumes the new
value, and it cannot regress the signed path because `haveSHSH` short-circuits
it. The value itself is not a guess: Apple ships unsigned IMG3s with
`sigCheckArea == sizeNoPack`, checked across all eight stock 12H1006 images
(iBSS, iBEC, iBoot, LLB, DeviceTree, applelogo, ramdisk, kernelcache), so
matching that is reproducing the shipped shape. After the fix 12H1006 rebakes
with `sigCheckArea == sizeNoPack == 7705416`, 10B329a still correctly excludes
its signature blobs (6004936 <= 6007104), and the RestoreRamDisk's
silently-wrong field is corrected as a side effect. Bumped into this repo by
`880d03e`, following the rule that a vendored change is committed and pushed to
its fork rather than left as a dangling local patch.

Personalization is independently excluded as an explanation: this is a
malformed-container error, and the stock 12H1006 kernelcache carries no
SHSH/CERT to personalize into in the first place.

This was the **third** defect of identical character in `ipsw-patch/img3.c` in
one week — after the DATA 16-alignment on the write side, then the same bug's
read side. All three produced artifacts that were internally self-consistent,
passed every check the pipeline knew how to run, and were found only by decoding
the bytes actually produced after a failed hardware run. See "The pattern, and
this is the fourth time" below, which counts this one as the third of four.

## `debug=0x14e` was wrong: four dead bits and a redundant fifth. The folklore was false

The boot-args work above ended with a directive budget and an obvious thing to
spend it on: turn the kernel's debug output all the way up. `debug=0x14e` was
chosen on the strength of the standard XNU bit names
(`DB_HALT|DB_PRT|DB_NMI|DB_KPRT|DB_LOG_PI_SCRN`) and one widely-repeated claim
about the top bit — that `DB_LOG_PI_SCRN` (0x100) makes the kernel render panic
information onto the framebuffer, a channel `-v` alone does not cover. On a
device with no serial tap and no console reader, a panic-time framebuffer dump
is worth a lot, so the bit was bought.

**Both target kernelcaches were decoded to check, and the claim is false on
both.** The value is now `debug=0x2`.

### What the kernels actually do

- `logPanicDataToScreen` (10B329a `0x8031E90C`, 12H1006 `0x803BD154`) has
  **exactly one reader** in each image, inside `_panic()` — 10B329a's `_panic`
  at `0x80017c10`, calling at `0x80017dbe`; 12H1006's at `0x8001e954`, calling
  at `0x8001ec2a`. What the routine does is write **the same global that
  `DB_PRT` (0x2) writes in `pe_init_debug`, to the same value**. `0x100` is
  therefore a *deferred* `0x2`: it turns console output on at panic time instead
  of at boot. It is not a second channel and it touches no framebuffer. There is
  no `draw_panic_dialog`, no `panic_ui` and no `vc_progress` symbol or literal
  anywhere in either kernel.
- `0x2` **strictly subsumes** it. Every other writer of that global on both
  builds writes the ENABLED value, and nothing ever re-closes it, so a boot that
  set `0x2` is already in the state `0x100` would have produced at panic.
- `0x8` (`DB_KPRT`) is tested **nowhere** on either kernel. `0x4` (`DB_NMI`) is
  tested nowhere on 12H1006, which is the actual target. `0x40` (`DB_ARP`) is
  real but only feeds `kdp_init`'s KDP-over-Ethernet setup, which is unreachable
  on a box with no debugger attached.

That last set of negatives is **exhaustive rather than sampled**, which is the
only reason it is worth stating: on 12H1006 `debug_boot_arg` is read in exactly
two places in the whole image, so there is nowhere else for any of those bits to
be consumed. Four of the five bits in `0x14e` were dead and the fifth was
redundant.

The shipped strings are now 91 bytes (ramdisk) and 97 (tether) against the
179-byte ceiling derived in the previous section, leaving 87 and 81 for an
`--extra-boot-args` directive. (`src/Patcher.hpp`'s comment still quotes 93/99
and 85/79 in its arithmetic — those are the `0x14e`-era lengths, measured before
the correction landed.)

### The half that is real: `-d` is load-bearing, and that IS confirmed

Nothing above weakens iBoot32Patcher's `-d`. Both `pe_init_debug`
implementations **zero `debug_boot_arg` outright** unless `debug_enabled` is
set, and `debug_enabled` is filled from the device-tree `debug-enabled`
property, which is 0 on a production-fused retail unit. Without
`patch_debug_enabled()` forcing it true, `debug=` of any value is discarded
before it is ever tested.

Verified the way this log now requires: by **byte-diffing the iBEC we actually
produce against stock, on both builds**. The patch lands in iBoot's `/chosen`
population loop at the `"debug-enabled"` lookup — payload offset `0x01a28a` on
10B329a, `0x0195c2` on 12H1006 — turning that lookup's result into an
unconditional 1. Seven differing regions in total, each one attributable to a
flag we asked for. Not the patcher's exit code; the bytes.

### What was not established, stated as such

Whether that global gates the **video** console leg, a **serial** leg, or both,
was not settled — `cnputc` was not decoded far enough to say. Its 10B329a
initial value of TRUE (i.e. output suppressed until something clears it) is
consistent with a real output gate, and that is as far as the evidence goes.
This changes no decision: `0x2` covers whatever it gates and `0x100` adds
nothing on top of it either way.

### Who owns the display, checked rather than assumed

The other assumption underneath `-v` was that some userland daemon has to be up
before the kernel can print anything visible. It does not. The kernel console
draws into `boot_args->Video`, and **iBoot** populates that structure —
`PE_init_iokit` reads `/chosen/memory-map`, and the stock iBEC carries
`setpicture`, `display`, `chosen/memory-map` and the `-s`/`-v`/`debug=` scan
table. `restored_external`'s IOMobileFramebuffer/IOSurface work is the
**userland progress UI**: downstream of the kernel console and irrelevant to it.
So `-v` depends on iBoot having brought the display up (the RestoreLogo
`setpicture` send documented under "`--tether-boot` visibility" above), not on
any daemon starting.

### Two methodology corrections, because they will otherwise mislead the next person

Both of these are about *how the kernels were read*, and both were wrong in
notes this repo already carries at `misc` level.

1. **"There are no MOVW/MOVT string materialisations in either image" is
   FALSE.** `MOVW/MOVT Rd,#imm32 ; ADD Rd,pc` is the **dominant** PIC form in
   both kernelcaches. It is easy to miss twice over: the `MOVT` half is often
   negative (`movt r0,#0xfffc`), so the reconstructed sum must be **masked to 32
   bits** or it lands nowhere plausible and gets discarded as noise. That alone
   hid one of the two `debug` xrefs on 10B329a. The kernel also reaches some
   globals through `__DATA,__nl_symbol_ptr`, so a scan for direct references to
   an address finds nothing at all even when the global is read constantly. A
   "zero xrefs" result from either of these tools is a statement about the tool.
2. **A provenance trap in the firmware cache.**
   `~/.local/share/blackb0x/AppleTV3,2/<build>/kernelcache.release` — the
   *unsuffixed* name — is **bake output, not stock**, on both builds. The stock
   member extracted from the IPSW is `kernelcache.release.j33i` (12H1006) /
   `kernelcache.release.j33` (10B329a). The two are the same uncompressed length
   and differ by their complzss adler, so nothing about the file announces which
   one you have; on disk right now the unsuffixed copies are exactly 112 bytes
   smaller than their stock siblings (6007124 vs 6007236, 7705436 vs 7705548),
   which is the same 112-byte shrink the `sigCheckArea` fix documents. Earlier
   figures in this repo that were read off the unsuffixed name should not be
   re-trusted by path.

### The pattern, and this is the fourth time

`debug=0x14e` is not an isolated slip. It is the same failure this project has
now hit four times:

- the IMG3 DATA 16-alignment bug, **write** side (xpwn PR #7);
- the same bug's **read** side, found only after the write side was fixed;
- the stale `sigCheckArea` on unsigned IMG3s (commit `880d03e`, which says in
  its own message: third defect of identical character in that one file in one
  week);
- and now the debug bits.

Every one of them looked right from the outside. The artifact was internally
self-consistent, the tool exited zero, the standard names matched, the community
documentation agreed, and every check the pipeline knew how to run passed. In
the `0x14e` case the folklore was not even implausible — XNU really does have a
`DB_LOG_PI_SCRN` bit, it really is documented as panic-info-to-screen, and it
really does nothing of the kind on these two kernels.

**The only thing that has ever caught one of these is decoding the bytes that
were actually produced or actually shipped.** Bit names, header constants,
upstream documentation and forum consensus are all statements about some other
build; on an eleven-year-old A5 firmware they are hypotheses, and this log has
now spent four bugs learning that they are cheap to test and expensive to
assume.

## On-screen text from userland: the global-IOSurface route, and why `/dev/console` cannot be certified

With `debug=0x2` the kernel will print, but nothing on this hardware reads that
stream back (no serial tap — see "Why serial console debugging is not
available"). The display is the only output device the box has, so the question
became whether a process on the restore ramdisk can put arbitrary text on the
HDMI output. It can, and by a better route than the obvious one.

### What `restored_external` actually does with the display

Read out of `/usr/local/bin/restored_external` on the real decrypted 12H1006
ramdisk. It creates **exactly three IOSurfaces** at display-init time, each with
`kIOSurfaceIsGlobal = kCFBooleanTrue`: BGRA, write-combined, stride
`(width * 4 + 63) & ~63`. It programs them as compositor layers and presents
with `SwapBegin` / `SwapSetLayer` / `SwapEnd`:

    layer 0  <-  surface[2]            opaque background, one solid fill
    layer 1  <-  surface[0]/surface[1] alternating, bzero'd, so alpha 0
    layer 2  <-  NULL

with src and dst rects both `{0, 0, width, height}`. Surfaces[0]/[1] are the
double-buffered pair it alternates for the progress bar and the logo;
surface[2] is the full-screen background.

### Why that is a channel, and why it is better than IOMobileFramebuffer

**A global IOSurface is addressable by ID from any process.** The ramdisk's own
`IOSurface` binary exports `IOSurfaceLookup`, `IOSurfaceGetID`, the geometry
getters, `Lock`/`Unlock` and `GetBaseAddress`, and its whole dylib closure is
CoreFoundation + IOKit + `libSystem.B.dylib`, all present. So the sequence is:
scan IDs, match on pixel format and geometry, lock, blit.

The consequence that matters: **the display pipe is already scanning those
surfaces**, so stores into their pixels change the screen with **no swap and no
cooperation from `restored_external`**. We never open IOMobileFramebuffer, which
means its exclusive-access question — which could not be settled, and which
would have been a real risk of taking the display away from the one process that
knows how to bring it up — simply never arises.

Target surface[2], the opaque background, identified at runtime by reading pixel
(0,0): the background reads alpha `0xFF`, the two progress buffers read
`0x00000000`.

### Contention: we win while idle

Checked, not hoped. The only callers of the swap routine are the two draw
routines — a progress-bar update and an image blit. There is no timer, no
animation loop and no polling. While `restored_external` sits in `accept()` with
no host attached — which is the field state for every run this project makes —
it performs **zero swaps and zero pixel writes**, so anything written into those
surfaces stays up. A display hot-plug callback does redraw everything, so a slow
re-blit loop is cheap insurance, but it is insurance and not a correctness
requirement.

### `/dev/console` is kept, and cannot be certified

`entrypoint`'s output still goes to `/dev/console` (via the unit's
`StandardOutPath`, with `ensure_console_fds()` as the fallback). That costs
nothing and stays. But **nobody established whether console bytes remain visible
after `restored_external` points the display pipe at its own surfaces**, and it
is entirely possible that the kernel console is painting into a framebuffer that
is no longer being scanned out. One piece of evidence cuts the other way:
`restored_external`'s own log primitive ends at `fputs(msg, __stdoutp)` and
drains syslogd's ASL store on the way, re-printing every record as
`SYSLOG: %s` — so it is writing to the console itself, which is at least
consistent with the console still being a live sink after it starts.

**The one-boot experiment that settles it** is worth recording because it is
cheap and nobody has run it: boot the **stock, unmodified** 12H1006 ramdisk with
`-v` and watch the TV. If `restored_external`'s own
`Display Info: width=... height=...` line appears on screen, the console
survives the takeover and no IOSurface work is needed for basic diagnostics.

### Status: nothing has run on hardware

A host-side prototype exists in `blackb0x-scratch/fbtext/` — a public-domain 8x8
font (`fbtext.h`) and a BGRA blitter, verified by rendering to an image and
reading it back, so bit order, stride arithmetic and scaling are proven before
anything runs on-device — plus `proto_device.c`, a research sketch of the
lookup-and-blit sequence above. That is the whole of it. **This is not a working
on-screen channel.** The disassembly of `restored_external` is real, the
IOSurface exports are real, the host-side rendering is real; the device-side
path has never executed on an Apple TV.

## Daemon co-existence on the restore ramdisk: one real collision, one corrected claim

`entrypoint` now runs as an ordinary LaunchDaemon alongside Apple's own units
rather than as PID 1 (commit `5c381a4`), which raises a question the old design
never had to ask: can anything else on that ramdisk interfere with a
half-written NAND? Each unit was checked.

- **`syslogd` cannot touch the NAND.** Every path it names is an absolute
  ramdisk path, and `/var/log` and `/etc/asl.conf` do not exist on this ramdisk
  at all. Zero references to `/mnt`, `disk0`, `rdisk` or any mount call. One
  corrected detail: `ASL_DISABLE=1` in its plist is read by `libsystem_asl` **in
  clients**, not by syslogd — it stops syslogd logging *to itself* and does not
  disable the ASL store.
- **`ReportCrash` is on-demand only** — `MachServices`, no `RunAtLoad` — and its
  `-r /private/var/logs/restored` keeps whatever it writes on the ramdisk, in
  RAM.
- **`restored_external` is the only unit on either ramdisk with mount code at
  all**, and the mountpoint it has wired into itself is **`/mnt1`**:
  `create_partition_mountpoints`, the string `libpartition, mounting '%s' at
  '%s'`, and hardcoded `/mnt1/private/var` and `/mnt1/usr/sbin/lsof`. It only
  mounts on a host `StartRestore` message — which is forbidden while we run — so
  the collision was conditional rather than certain, but `entrypoint` had been
  using `/mnt1` for this project's entire life and there is no reason to keep
  sharing that name.

### The mountpoint moved to `/mnt2`, not `/mnt4`

Worth recording because the obvious answer was wrong. The mountpoint set is
**not the same on both generations**: 12H1006 ships `/mnt1 /mnt2 /mnt3 /mnt4`,
while 10B329a ships **only `/mnt1` and `/mnt2`** (checked directly on both
mounted volumes; all empty). `/mnt4` does not exist on the legacy generation at
all, so `/mnt2` is the only name that both pre-exists and is empty everywhere
this project bakes — and pre-existing matters, because `bakeRamdisk()` adds as
little to Apple's tree as it can and a `mkdir()` would additionally depend on
the ramdisk root being writable at that instant, which is not established.
`restored_external` names `/mnt1` and `/mnt2` on 10B329a and `/mnt1`..`/mnt4` on
12H1006, so no mountpoint is entirely un-referenced by it on either generation;
distance from the system-partition one is the whole of what is available, and it
is enough given that a host restore is a usage-rule violation regardless.

### Corrected: the reboot-on-child-exit behaviour is NOT 10B329a-specific

An earlier note in this repo recorded `restored_external`'s
reboot-when-its-child-exits behaviour as a property of 10B329a. **It is present
on 12H1006 too.** Its no-argument `main()` forks and execs *itself* with
`-server`; the parent `waitpid()`s, and when that child exits for any reason the
parent logs `restored exited ... - rebooting` and reboots via `/sbin/reboot`. If
that fires mid-merge, the device reboots with our mount dirty.

With no host attached the child blocks in `accept()` forever, so this is a tail
risk rather than a likely one, and the mitigation is chosen accordingly: **close
the dirty window** — `main()` `sync()`s and unmounts the instant the merge
returns, with nothing in between — rather than add a lock or wait for anything.
There is nothing to lock against (launchd, launchctl, xpcproxy, syslogd and
ReportCrash contain zero references to `/mnt`, `disk0`, `rdisk` or any mount
call), and **waiting is the one thing that provably widens the window**.

`restored_external`'s own startup was also checked rather than assumed to be
harmless: it sets an `IOPMUBootStage` property, starts a gas-gauge thread,
creates a listen socket, disables the watchdog, calls
`enable_usb_connections()`, and blocks. Every destructive primitive it has
(`WipeStorageDevice`, `clean_NAND`, `FormatForLwVM`, `partition_nand_device`,
`asr`) sits behind a host `StartRestore` message. That is the mechanism behind
the usage rule this project already had: do not point a restore client at the
device while a jailbreak ramdisk is running.

**Verdict: safe to co-exist.** One real collision, resolved by moving to
`/mnt2`; one tail risk, mitigated by ordering rather than by locking; everything
else on the ramdisk is provably incapable of touching a block device.

## Getting a pixel onto the TV took three wrong answers, each of which looked right

The channel itself was settled earlier (see "On-screen text from userland"
above): `/dev/console` is wired up and does not reach the display, measured by
a diagnostic ramdisk that printed a ten-second heartbeat through launchd's
`StandardOutPath` and produced nothing on screen for as long as anyone cared to
watch. Once `restored_external` points the display pipe at its own IOSurfaces,
the boot framebuffer the kernel console draws into is off-screen. So
`entrypoint/screen.h` looks `restored_external`'s global surfaces up by ID with
`IOSurfaceLookup()` and stores text straight into their pixels — no compositor
call, no cooperation from the owning process.

That was the easy part. **Three successive versions of the drawing code were
written, each of which was the obvious answer to the failure of the one before,
and the first two produced no readable output on hardware.** They are worth
recording in order, because each was defensible on paper and each was refuted by
one run on a real AppleTV3,2 at 12H1006.

### Iteration 1: choose the right surface — and choose the one that cannot be seen

The first version picked exactly one target: the opaque background,
`surface[2]`, layer 0. It was identified at runtime rather than by index, by
reading pixel (0,0) — the background reads alpha `0xFF`, the two progress-layer
buffers read `0x00000000`. The reasoning had two legs, and both were true: the
layer above the background is transparent, so text drawn underneath should show
through it; and the background is the one buffer `restored_external` does *not*
rewrite on a progress update, so our stores should survive.

**Hardware: the text was written correctly and was never visible.** It appeared
only in the instant the boot graphics were torn down at the end of the run —
which is itself the proof that the stores had been landing in the right memory
all along. The background layer is composited *under* the logo/progress layer,
and that layer is not transparent where we were drawing. "The layer above is
transparent" was a statement about how the buffer is initialised (`bzero`'d,
alpha 0), not about what is in it once the Apple logo has been blitted into it.

The lesson is narrow but sharp: the most defensible-sounding choice of target
was the single choice that could not be seen.

### Iteration 2: stop choosing — and the TV goes fully black

The fix for "picked the wrong one" is to stop picking. The second version
collects *every* plausible BGRA surface and writes the same text to all of them,
so whichever one is composited on top, scanned out, or swapped in next is
carrying it. Locks are still taken one surface at a time, never two at once.
Three surfaces is three times the blitting of one, which is nothing at a few
dozen glyphs per line.

**Hardware: the TV went fully black and stayed up.** No reboot, no panic — a
black screen for the whole run. That was us, and the cause is embarrassing in
hindsight: every console cleared its row band to **opaque black** before drawing
glyphs, which is correct and invisible on an opaque background layer and
catastrophic on a transparent overlay. Roughly forty rows of see-through surface
became a solid black sheet covering nearly the entire frame. The text was
drawing correctly the whole time; the rectangle painted behind it was what the
owner was looking at.

Both failures have the same shape. In iteration 1 the text was right and
invisible; in iteration 2 the text was right and the *background* was the
visible artifact. Neither run was a failure of the blitter, which had been
verified on the host against a rendered image before any of this ran on a
device.

### Iteration 3: `FBTEXT_NOFILL` — a sentinel, not a colour

`fbtext.h` gained `FBTEXT_NOFILL` (`0x00000001u`, a BGRA value — alpha 0, blue 1
— that no caller could ever mean as a real colour, so it cannot collide). It is
not a background colour; it is a background *sentinel*. A row clear with it does
nothing at all, and a glyph writes only its lit pixels. Text therefore lands on
top of whatever the display is already showing and every other pixel in the
frame is left exactly as it was found. It is now the default background for
every console this binary creates.

**Hardware: works.** Text over the Apple logo.

State the general lesson explicitly, because it generalises past this file:
**there is no colour that is correct to paint on a surface owned by another
process.** Any value we choose is a guess about content we do not own and cannot
read back cheaply (the mapping is write-combined). Not writing is the only
answer that is safe for all possible contents.

The cost accepted is real and bounded: a reused row overdraws the one before it
rather than replacing it. That is tolerable because the scrolling region is ~89
rows and a diagnostic run prints well under that, and because the status row
rewrites a *constant* string, so its repeats land on identical pixels. A blanked
display is the worse failure by a very wide margin.

### The glyph size, twice too big, and the divisor that silently cancelled itself out

The same commit sequence got the glyph scale wrong twice, and the second wrong
answer contains an arithmetic accident worth recording on its own.

The scale started at `width/416` — scale 3 on 720p, about 50 columns — taken
from ten-foot legibility guidance, whose rule of thumb is a minimum comfortable
glyph height of roughly 1/30 of the frame. That guidance is written for UI
glanced at from a sofa. This is a log, read by someone who has deliberately
walked over to the television to look at it, and the two have nothing to do with
each other. Scale 3 was measured as far too big on the real display. It went to
`width/640` — scale 2, about 75 columns — and was **still** too big, and still
short of columns. It is now `width/960` floored at 1, which on the 1280x720 this
device drives is scale 1: the font at its native 8x8, 160 columns and 90 rows
(89 scrolling plus the non-scrolling status row). There is no gentler step
available, because the font is a bitmap and the scale is an integer — below 2
the only value is 1.

**The accident.** The old code also inset the text area by `width/32` on each
side, as overscan protection. With *both* the inset and the scale divisor
proportional to `width`, the column count cancels out entirely:

    cols = (w - 2*(w/32)) / (8*(w/640)) = (w*30/32) / (w/80) = 75

Exactly 75 columns at **every** resolution. Two independent width-proportional
constants, each individually sensible, silently produced a resolution-*in*dependent
result. That is not merely a curiosity: it meant the first real error this
console ever displayed was truncated at column 75, and the truncation point
therefore revealed **nothing whatsoever about the panel's actual resolution** —
the one free measurement a truncated line would normally hand you. The overscan
inset is now gone entirely (text starts at pixel (0,0); this display maps 1:1),
and the console **wraps** by column rather than truncating, because on paths and
errno strings the informative part is at the tail, not the front.

Separately and compounding it, `SCREEN_LINE_MAX` was 112 while the error message
in question is **121 characters**. The line was therefore being truncated
*twice*, in two different places, for two different reasons — so fixing only the
renderer would have moved the cut rather than removed it, and would have looked
like a partial fix of one bug instead of the two bugs it actually was.
`SCREEN_LINE_MAX` is now 256.

## The data partition refuses new files, and `mkdir` is what proves it

The first real error the working console displayed was the install record
failing to open. The chronology matters, because each step eliminated the
comfortable explanation for the step before it.

- **`/var/mobile/Media/blackb0x_install.log`** — this project's install-record
  path since the original Blackb0x — failed with `EPERM`. The obvious
  explanation was immediately available and sounded complete: `/var/mobile/Media`
  is a data-protected location.
- **Moved to `/var/.blackb0x/install.log`**, which is not an arbitrary
  relocation: it sits beside `install-done` and the `postinstall.out.log` /
  `postinstall.err.log` the first-boot daemon writes, i.e. exactly where this
  project already keeps its on-NAND state, and it is directly under `/var`, which
  carries no protection class of its own. **Same `EPERM`, as root.** The
  comfortable explanation is dead.
- **The decisive observation:** `mkdir("/mnt/var/.blackb0x", 0755)` **succeeded**
  on that same volume in that same run. The directory is on the device, at mode
  0755. The file create beside it is refused.

That pair is the entire diagnosis. **A directory needs no per-file content key;
a regular file does** — creating one requires a fresh per-file key, wrapped by a
class key that comes out of the keybag. `mkdir` succeeding where `open(O_CREAT)`
returns `EPERM` is the *fingerprint* of iOS content protection with no keybag
loaded; it is not, as it first looks, evidence against a protection problem. Note
what it also rules out: a plain permissions problem would have failed the `mkdir`
too, and a read-only mount would have failed it too *and* returned `EROFS`
rather than `EPERM`.

### Corroboration, read off the ramdisk image itself

Three facts, all read out of the real decrypted 12H1006 restore ramdisk rather
than inferred:

- **`/usr/libexec/keybagd` is referenced but absent.** `/sbin/launchd`'s
  *embedded* bootstrap plist carries a job keyed `keybag`, with `Program`
  `/usr/libexec/keybagd` and `ProgramArguments` `keybagd --init`. The binary is
  **not present on the ramdisk**. The job is declared and cannot run.
- **`MobileKeyBag.framework` *is* present**, so the absence above is not simply
  "no keybag machinery exists here".
- **`restored_external` links `MobileKeyBag` and imports exactly three symbols:**
  `_MKBKeyBagCreateSystem`, `_MKBDeviceObliterateClassDKey`, `_MKBSetLogFunction`.
  Those are the restore-time *destructive* operations — create a brand-new system
  keybag, obliterate the class D key — plus a logging hook. There is no
  "load the existing bag" anywhere in that import set.

That is a coherent picture rather than three coincidences: a restore ramdisk is
designed to **erase** the data partition, not to write into it, so nothing that
ships on one ever needs to load an existing keybag.

One device-specific angle is worth stating because it changes what this costs to
fix: this is an Apple TV with **no passcode**, so its class keys unlock from
device-only material and need no user input. The bag does not need a *secret*;
it needs **loading**. That makes this a setup step this project does not
currently perform, not a wall it has run into.

**Loading it anyway was considered and rejected.** `keybagd` is absent from the
ramdisk but it *is* present on the device's own system partition, which is
mounted at `/mnt` by the time any of this matters — so `/mnt/usr/libexec/keybagd
--init` is, on its face, a one-line experiment against a many-file relocation.
It was not attempted, and the reason is the one that sinks most "just run
Apple's binary" ideas on a restore ramdisk: `keybagd` is a full-OS daemon and
will expect the dyld shared cache and the frameworks that come with it, none of
which this image carries. A ramdisk is not a small copy of the OS; it is a
different environment that happens to share a kernel. The cost of finding that
out is a boot cycle and the cost of being wrong about it is an unbounded chase
after whatever it links against next, so the relocation — which depends on
nothing but our own code — is the path taken instead.

This is worth recording as a *rejected* option rather than an unexplored one.
A future reader who rediscovers that `keybagd` sits on the mounted volume should
know it was seen and passed over deliberately, not missed.

### The scope of the consequence is much larger than one log file

State this plainly, because the log file is the least important thing on the
list. `/blackb0x` stages a large part of its payload onto the data partition:

- `/var/lib/dpkg` — dpkg's database;
- `/var/lib/apt` and `/var/cache/apt` — apt's lists and archives;
- `/private/var/.blackb0x` — this project's own state, including `postinstall.sh`;
- plus `/var/mobile` and `/var/root` entries.

If every file create on that volume is refused, **that entire portion of the
install has been failing silently on every run this project has ever done.**

It went unnoticed for a specific, fixable reason: `merge_tree()` recorded a
failure as `ok = 0` and moved on. A run in which an entire volume was refused
was therefore indistinguishable, from the outside, from a run in which one file
happened to be busy — and the only surviving signal was a single summary line
pointing the reader at a console stream that, until the screen console existed,
nothing on this hardware could read. Merge failures are now **counted**, with
the first kept in full along with its `errno`, reported after the merge and
written into the install record. A count plus one worked example is precisely
what separates "a file was busy" from "the volume said no"; printing all of them
would push the rest of the log off a 90-row screen.

### Open question at the time of writing: is it the volume, or is it us?

Not yet answered, and marked as such. Two things remain open:

1. Whether the **system** partition (`disk0s1s1`) accepts new files at all.
2. Whether the restriction is on the volume at all, rather than on the process.
   `EPERM` is equally the canonical sandbox denial for an ad-hoc-signed binary
   the kernel does not treat as a platform binary, regardless of the AMFI
   boot-args. The counter-argument is already on the table and is not weak:
   `mkdir` and `chown` are just as sandbox-able as `open(O_CREAT)`, and the
   `mkdir` went through.

`probe_writability()` was added to settle all of it in a single run: a create on
the **ramdisk's own root** as the control for "the restriction is on us, not the
volume"; a create on the **system partition**; `mkdir`-versus-create on the
**data partition** to confirm the pair above; and a **directory listing** of the
data partition to prove the mount is real, since a create failure on a mount
that silently did nothing would be a consequence rather than a cause. Every
probe cleans up after itself and none of them can fail the run.

## DECISION RECORD (not implemented): the staged payload moves to `/usr/share/blackb0x`, not `/opt`

**This describes a change that has not been made.** Nothing in the tree
implements it at the time of writing; a future reader should not go looking for
the code. It is recorded here so the reasoning does not have to be rebuilt when
the probe results come back.

**The plan, conditional on the data partition being confirmed off-limits at
ramdisk time.** Everything in the repository that refers to `/var/.blackb0x`
moves to `/usr/share/blackb0x`. Everything the merge would write to `/var` or
`/private/var` (the former is a symlink to the latter) is instead written under
`/usr/share/blackb0x/var`, to be moved into place by the first-boot postinstall
daemon — which runs in the fully booted OS, where the keybag **is** loaded and
`/var` is writable in the ordinary way. The ramdisk stops trying to write to a
volume it cannot write to, and the one component that provably can do it does it.

### Why not `/opt`

`/opt` is the reflexive answer for "third-party payload that is not part of the
base system", and it was considered and rejected. What was actually checked:

- **`/opt` does not exist on the 12H1006 restore ramdisk.**
- **`/opt` does not exist in the `/blackb0x` overlay**, whose root is exactly:
  `Applications Library System bin boot dev etc extrainst_ lib mnt private sbin
  tmp untether usr var`. That overlay is built from real Debian packages, so this
  is direct evidence that none of the packages this project stages install
  anything under `/opt`.
- **Darwin's own `hier(7)` does not document `/opt` at all.**

FHS does define `/opt` for add-on application software, and on Debian proper it
would be perfectly defensible. But the pre-rootless iOS jailbreak ecosystem maps
into `/usr`, `/Library` and `/Applications` instead, and the overlay's own layout
is direct evidence of that choice rather than an appeal to convention. (The
`/var/jb` convention people may reach for is rootless-era, iOS 15+, and entirely
irrelevant to a 2014 tvOS.)

One gap, recorded honestly: **only the restore ramdisk was inspected, not the
AppleTV's own booted system partition.** "Stock tvOS 8.4.7 ships no `/opt`" is a
strong inference from three independent observations, not a direct observation.

**The deciding factor was not convention, though.** It was the *partition*:
`/opt` would sit on `disk0s1s1` alongside `/usr` and `/Library`, and would
therefore dodge content protection exactly as well as `/usr/share/blackb0x`
does. On the thing that actually matters the two are equivalent.
`/usr/share/blackb0x` wins on tidiness alone — it lives inside an existing
hierarchy, it is unmistakably ours, and it does not invent a root-level
directory that a half-completed migration would leave behind on a user's device
forever.

### The constraint any future sizing decision has to work against

`disk0s1s1` has about **115 MB free** on the owner's device. That is not an
estimate: it was measured by the entrypoint's own `report_volume()` on the run
where the install died. The staged `/var` payload — dpkg's database, apt's lists
and apt's archives — has to fit inside that, on the **system** partition,
instead of on the data partition where it was always meant to live. Whatever
shape the migration eventually takes, 115 MB is the number it has to fit in.

## REJECTED: drawing first-boot install status over the Apple TV UI

The screen console works in the ramdisk, so the obvious next question was
whether the same trick could report `postinstall.sh`'s progress on the booted
device — an apt run over a slow or absent network is minutes of blank screen,
and it is the phase a user is most likely to misread as a brick. It was
investigated and dropped. Three separate reasons, and the third is the one
that actually settled it.

### The ramdisk technique does not transfer, for the reason it worked

`screen.h` works because of a property of `restored_external` specifically:
it publishes three IOSurfaces with `kIOSurfaceIsGlobal`, and while it sits in
`accept()` with no host attached it performs **zero swaps and zero pixel
writes**. Stores into those surfaces therefore stay on screen with no
compositor cooperation at all. That is not a general fact about iOS; it is a
fact about an idle restore daemon.

On a booted system the display is composited continuously. Anything written
into a layer is gone on the next frame, the UI's layers are not published for
arbitrary `IOSurfaceLookup`, and taking `IOMobileFramebuffer` directly would
mean contending for a display a person is actively watching. None of the code
that produced the ramdisk console is reusable here.

### The process is not the one the tvOS-era name suggests

Worth recording because the wrong name leads to the wrong research: this
device's UI is `/Applications/AppleTV.app/AppleTV`, the Frontrow-derived
appliance host. **Pineboard is the 4th-generation tvOS UI and does not exist
on an AppleTV3,2.** Both nitoTV and Kodi shipping
`com.apple.frontrow.appliance.*` icons are the same fact from the other side,
and `entrypoint.c` already stats that binary as its "is this an Apple TV"
check.

### The only workable design cannot cover the failure it exists for

Drawing over a live UI on a jailbroken device means a MobileSubstrate tweak
injected into `AppleTV.app` — a `UIWindow` at a high window level, rendering a
status string read from a file `postinstall.sh` writes. That cooperates with
the compositor instead of racing it, and the pinned ARM32 Xcode toolchain
already builds exactly that shape of artifact.

**But `mobilesubstrate` is installed BY the script the tweak would report
on.** It is line 38 of `packages.txt`, pulled in by the bulk
`apt-get install`. So the tweak is absent for the 60-second network wait, for
`apt-get update`, and for the Cydia install that precede it — and, decisively,
**if the network is down the tweak never installs at all**. A blank screen
caused by no network is the single most likely thing a user would want
explained, and it is the one case this design structurally cannot explain.
A reporting mechanism whose availability is conditional on the thing it is
reporting about succeeding is not a reporting mechanism.

That is fixable in principle — hoist `mobilesubstrate` to an offline install
from the staged cache immediately after Cydia, then load the tweak, then do
the rest — but it buys a partial answer at the cost of a new build target, a
new CI leg, and a reordering of the install for a device that already has two
working channels for this: SSH once the daemon is up, and
`/usr/share/blackb0x/postinstall.{out,err}.log` afterwards. The ramdisk phase,
which is where this project has actually been blind, keeps its console.

If this is ever revisited, the prerequisite is the offline `mobilesubstrate`
install, not the tweak; without that, the tweak is worth nothing on the only
boots where it matters.
