# AGENTS.md — Blackb0x portable port

Short and operational: what an agent needs to move around this repo and build it
correctly, right now. For the *why* — every bug hunt, dead end, and decision's
evidence — see **[`docs/HISTORY.md`](docs/HISTORY.md)**. If something here seems to
need more justification than it gives, it's almost certainly explained there.

## What Blackb0x is

A jailbreak tool for 2nd/3rd-gen Apple TV (A1378/A1427/A1469) via the checkm8/SHAtter
DFU-mode boot exploit, which then side-loads Cydia + Kodi. A portable CLI port of an
original macOS Cocoa/Objective-C app (fully ported and deleted — see
`docs/HISTORY.md` or git history if you need to see what it looked like).

## Conventions (don't re-litigate without asking)

- **CLI-only, macOS-only.** No GUI — dropped, not dual-maintained. Linux support was
  removed outright (checkm8 never worked there; see "Current status" below and
  `docs/HISTORY.md`), and `CMakeLists.txt` hard-fails off Apple. Apple Silicon is the
  tested host.
- **Every third-party dependency is a git submodule under `third_party/`, built from
  source, statically linked.** Not FetchContent, not system packages, no exceptions —
  see the "vendored dependencies" table below and `CMakeLists.txt`'s
  `ExternalProject_Add` blocks. `zlib` used to be the one system-provided exception;
  it no longer is.
- **The only dynamic dependencies in the final binary are `libSystem`, `libc++`
  and Apple's own system frameworks.** Verify with `otool -L build/blackb0x` after
  any dependency change (`ldd` is the Linux spelling and does not exist here). The
  real, verified output is six lines: CoreFoundation, IOKit, Security,
  SystemConfiguration, `libSystem.B.dylib`, `libc++.1.dylib`. `bake-firmware` is
  the same list minus IOKit, which it has no reason to link — it talks to no USB
  device. `blackb0x-pwn` is CoreFoundation, IOKit and `libSystem` only, since it
  links neither wolfSSL nor curl. Nothing should ever appear from `/opt/homebrew`
  or `/usr/local`; that would mean a vendored dependency resolved against a system
  copy instead of being built from source.
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

- `src/` — first-party code, flat, and nothing else: `main.cpp`, `Cli.hpp`/`.cpp`,
  `Console.hpp`/`.cpp`, `DeviceManager.hpp`/`.cpp`, `IPSW.hpp`/`.cpp`,
  `IPSWDownloader.hpp`/`.cpp`, `Patcher.hpp`/`.cpp`, `PatcherPatch.cpp`,
  `Img3Crypt.hpp`/`.cpp`, `StockIBSSCrypt.hpp`/`.cpp`, `Personalize.hpp`/`.cpp`,
  `ResourcePath.hpp`/`.cpp`, `BakeRamdisk.hpp`/`.cpp`, `BakeFirmware.cpp`.
  **`Patcher` is split across two TUs by whether the method patches.**
  `Patcher.cpp` is the no-patch half (baked-component loaders, the `dist/`
  existence check, `useStock*`, and bookkeeping) and links no xpwn `decrypt()`
  and no GPL patch tools; it is compiled into both binaries.
  `PatcherPatch.cpp` holds `patchiBSS()`/`patchiBEC()`/`patchKernel()`, which
  call `decrypt()` (first-party glue in `src/Img3Crypt.cpp`, built over xpwn's
  public API) and fork/exec the GPL patch tools, and is compiled into
  `bake-firmware` ONLY. The upshot, and the invariant to keep:
  **the `blackb0x` jailbreak binary links no xpwn and no GPL patch/decrypt
  code** — not `blackb0x_xpwntool`, not `xpwn`, not the patch tools — and
  consumes bake-firmware's already-encrypted `dist/` output verbatim.
  Everything except the raw iBSS reaches its loader still encrypted and
  img3-wrapped, because iBoot32Patcher defeats only iBoot's
  signature/ticket/KASLR checks, never its img3 parse or AES-decrypt. The
  **one deliberate, narrow exception**: `src/StockIBSSCrypt.cpp` is a small
  first-party AES-CBC img3 decrypt (wolfSSL, already linked via `deps::wolfssl`
  — NOT xpwn, so no GPL), and `Patcher::useStockIBSS()` calls it to decrypt a
  *stock* iBSS for the checkm8 `boot_client()` route only (A5 without
  `--stock-securerom`), because that route uploads the img3 DATA payload
  directly as code and so needs plaintext — the same reason the baked iBSS is
  the one `dist/` component published decrypted.
  The original Objective-C (`AppDelegate`, `MainView`,
  `Blackb0x.h`/`.m`, `TaskManager`, and the old `.h`/`.m`/`.mm` counterparts of the
  files above) has been fully ported and deleted — check `docs/HISTORY.md`/git
  history if you need to see what it looked like. `checkm8.h`/`SHAtter.h` are exploit
  payload byte arrays, `#include`d directly by `DeviceManager.cpp` — not leftover
  Cocoa, keep these. `bootkit.c`/`bootkit.h` are vendored C too: `dfu_boot()`, copied
  and trimmed from NyanSatan's [checkm8_bootkit](https://github.com/NyanSatan/checkm8_bootkit)
  (flattened form the original app carried), it boots the iBSS on **AppleTV3,2** via
  ipwndfu's `"exec"` USB protocol + a CPID-0x8947 trampoline — the checkm8 payload
  won't run a raw upload. `sendiBSS_ATV32()` calls it instead of `boot_client()` (which
  stays for AppleTV3,1). checkm8_bootkit declares no license; kept under the same
  research-tool terms as `checkm8.h`/`SHAtter.h`, with provenance in the file header.
  Only what's needed is copied — one CPID config, no `main`/tool/debug paths.
- `src/Pwn/` — the standalone `blackb0x-pwn` exploit binary's own C sources
  (`main.c`, `Checkm8Pwn.c`), separate from the main CLI.
- `src/libraries/` is **gone**. It held copied/adapted upstream C
  (`xpwntool.{c,h}`, `idevicerestore_img3.{c,h}`, `libplist_compat.c`); all three
  were eliminated in favour of the real submodules:
  - `xpwntool.{c,h}` → first-party `src/Img3Crypt.cpp`, our own `decrypt()` glue
    over `third_party/xpwn`'s public `AbstractFile` API, compiled into
    bake-firmware only. GPL-3.0 (derives from / links GPL-3.0 xpwn). Do NOT add
    it, `xpwn`, or any decrypt path to `blackb0x`'s link line — the jailbreak
    binary does no decryption. It needs a per-source `${DEPS_INCLUDE}/wolfssl`
    include + `wolfssl/options.h` force-include (set on the bake-firmware
    target), because `<xpwn/nor_files.h>` reaches a bare `<openssl/aes.h>` that
    only resolves to wolfSSL's compat shim.
  - `idevicerestore_img3.{c,h}` → compiled straight from the
    `third_party/idevicerestore` submodule (`src/img3.c` + `src/log.c`) into the
    `idevicerestore_img3` static lib (blackb0x, for `--stock-securerom`). See its
    CMake block: idevicerestore ships no library or headers of its own, so only
    those two files are compiled, against its `src/` headers + `${DEPS_INCLUDE}`,
    with `HAVE_CONFIG_H` left undefined. LGPL-2.1-or-later.
  - `libplist_compat.c` → deleted with the libplist consolidation (below); every
    function it shimmed is real in the single vendored libplist 2.7.0.
  Both GPL patchers (`CBPatcher`, `iBoot32Patcher`) are their own submodules,
  built as separate executables and fork/exec'd, never linked (see the table
  below).

  **Whole-project licensing:** the project is **AGPL-3.0** (`LICENSE.md`, the
  FSF's text). AGPL-3.0 is compatible with the GPL-3.0 code it links and execs
  (wolfSSL — its vendored `COPYING` is GPLv3 — plus xpwn and the two patchers),
  so `blackb0x` and `bake-firmware` are AGPL-3.0 combined works. Dropping the
  xpwn static link from `blackb0x` (the `Patcher`/`PatcherPatch` split) keeps the
  decrypt path out of the shipped binary but does not by itself make it non-GPL —
  wolfSSL alone already does. The statically-linked LGPL-2.1 pieces
  (`idevicerestore_img3`, `deps::plist`/`imobiledevice`/`usbmuxd`/`irecovery`,
  etc.) are allowed under the LGPL §6 relink provision; whether the distribution
  must spell that out (a written offer / relink objects) is the remaining open
  licensing question for the owner.
- `src/tests/` — `RamdiskOverlayTests.cpp`, the one test target.
- `package/` — the `xyz.regulad.blackb0x` Debian package: `build.sh` (builds the
  `.deb` with Theos' `dm.pl` inside a container), `layout/` (its literal on-device
  file tree plus `DEBIAN/control` and `DEBIAN/postinst`), `packages.txt` (the apt
  package list templated into `postinstall.sh` at package-build time), and
  `local_only_debs.txt` (the packages bundled as a local apt repo at
  `/var/.blackb0x/local-debs`, for anything no live repo serves). The package
  needs no state beyond this tree — `bakeRamdisk()` builds it and installs the
  resulting `.deb` rather than hand-staging its contents.
- `debcache/` — the checked-in `.deb` cache (107 packages, ~58MB), at the repo
  root because it is a build input in its own right, not Blackb0x app data.
  **Stored through Git LFS** (see `.gitattributes`), so a clone made without
  git-lfs installed has 107 pointer files here instead of real archives. That
  failure is silent at build time and only shows up when a bake tries to read
  one; `git lfs pull` fixes it. `file debcache/*.deb` should say "Debian binary
  package", never "ASCII text".
  The bake consumes it for both the bundled local repo and the apt cache staged
  onto the ramdisk; real package archives, not loose files mirroring a
  destination path. **Generated on Linux, consumed everywhere.**
  `scripts/build_deb_cache.py` grows it with a real apt-get dependency solve
  inside podman and hard-refuses to run off Linux; a GitHub Action is the
  intended way to run it. No bake ever calls it — every bake reads the
  committed bytes, resolving the closure over them with
  `scripts/build_deb_cache_experimental_no_container.py`, which only reads.
  It used to be `Blackb0x/Debs/`.
- `dist/` — bake-firmware's output (gitignored, not checked in): one
  flat `<Component>-<device>_<buildID>` entry per component per known
  firmware, using Apple's own BuildManifest component keys: `iBSS-`, `iBEC-`,
  `iBECTether-`, `KernelCache-`, `DeviceTree-` and `RestoreRamDisk-...dmg`.
  `iBECTether-` is the one name that is *not* an Apple manifest key: **there
  are two iBECs per tuple**, identical but for the boot-args compiled into each
  (`iBoot32Patcher -b`) — `rd=md0` for the install path, `rd=disk0s1s1` for
  `--tether-boot`. They have to be separate files because this bootloader never
  reads the `boot-args` environment variable on its kernel-boot path, so a
  baked string wins unconditionally and `setenv boot-args` reaches nothing;
  `blackb0x` picks one by mode and never needs both. Do not collapse them back
  into one — see `src/Patcher.hpp`'s `bootargs` namespace for the disassembly
  and `docs/HISTORY.md`'s "`setenv boot-args` is INERT on this bootloader".
  There is no
  `bootchain/` subdirectory any more, and the ramdisk is no longer named
  differently from everything else. An entry that already
  exists is skipped; pass `--force` to rebuild. There is no staleness
  detection: a `.sum` sidecar holding a content hash of the bake inputs used
  to force re-bakes automatically, and it was removed as fragile, so **after
  changing anything that affects baked output, pass `--force`**.
- `keys/` — per-firmware IPSW decryption `.keys` files, read locally by
  `IPSW.cpp` only; never shipped to the device.
- `misc/` — bake-time assets that are neither code nor a package, four
  consumers total: `untether.bin` and `dirhelper` (staged onto the device),
  `apt/net.tihmstar.gpg` (staged as an on-device apt keyring),
  `prebake_package_blacklist.txt` (read by the preinstall-eligibility pass —
  "do not force-install this at bake time, its maintainer script must run on
  the device"), `never_stage_debs.txt` (read by `shouldSkipStagingDeb()` —
  "do not ship this `.deb` at all", purely a size decision; the two lists are
  not interchangeable and each file's header says why),
  and `firmware_versions.txt` (the `(device, buildID)` -> ProductVersion
  binding, regenerated by `scripts/generate_firmware_versions.py`).
  `apt/*.gpg.key` are the armored copies `scripts/build_deb_cache.py` feeds
  to gpgv. The `.list` files that used to live here are in `package/layout/`
  now. See `misc/README.md` — at 51KB it is the reverse-engineering writeup,
  not a directory index.
- `third_party/` — every vendored dependency (see table below).
- `docs/HISTORY.md` — the full debugging/decision log.

## Vendored dependencies

All built from source via `ExternalProject_Add`/`add_subdirectory` in `CMakeLists.txt`,
statically linked. **Forked** means: patched on our own branch, pushed, pointed to from
`.gitmodules` — not a local working-tree diff.

| Submodule | Source | Forked? |
|---|---|---|
| `libplist` | libimobiledevice/libplist | No — current HEAD (2.7.0). Installs as `libplist-2.0`; `libplist_ext`'s install step drops a `libplist.pc` alias beside it so consumers still asking for the old `libplist` pkg-config name resolve. This single copy replaced a former two-build setup (a 2.1.0 pin + a separate `libplist-modern`) and retired `libplist_compat.c` — every current-API function it shimmed is real here now |
| `libusbmuxd`, `libimobiledevice-glue` | libimobiledevice/* | No — current HEAD, plain upstream (both build against the single `libplist` above) |
| `libimobiledevice` | **regulad/libimobiledevice**@`legacy` | Yes — additive `--with-ssl-implementation=wolfssl`-selectable SSL backend for `idevice.c` (real OpenSSL/GnuTLS untouched, still selectable); plus `common/utils.{h,c}` now use libplist's `plist_format_t` instead of a local `enum plist_format_t` that collided with libplist ≥ 2.3.0's own (redefinition once built against current libplist) |
| `idevicerestore` | libimobiledevice/idevicerestore | No — current HEAD. Not built as an app; only `src/img3.c` + `src/log.c` are compiled into the `idevicerestore_img3` static lib for `img3_stitch_component()` (`--stock-securerom`) |
| `libirecovery` | **regulad/libirecovery**@`libusb-async-cancel-fix`, off synackuk/libirecovery | Yes — real stderr diagnostics on `irecv_send_buffer()`'s upload-failure paths, which previously returned `IRECV_E_USB_UPLOAD` silently unless built with `debug()` on; plus `irecv_usb_control_transfer_ex()`, which keeps the partial byte count on a stall/timeout that both ordinary wrappers discard. Built `--with-iokit` only. The branch also carries libusb-path fixes (a use-after-free, a double free and a non-terminating completion wait in `irecv_async_usb_control_transfer_with_cancel()`) that this project no longer compiles — the branch name is a leftover from when it did |
| `libfragmentzip` | **regulad/libfragmentzip**@`fix-cxx-stdbool-header` | Yes — one header fix (C++/`<stdbool.h>` collision) |
| `libgeneral` | tihmstar/libgeneral | No |
| `xpwn` | **regulad/xpwn**@`legacy` | Yes — a wolfSSL AES-CBC buffer over-read fix in `img3.c`, plus disabling the legacy-libusb-0.1-only `pwnmetheus2` subdirectory |
| `wolfssl`, `curl`, `libzip`, `libpng`, `bzip2`, `zlib` | upstream | No — current HEAD or latest stable tag; none of these existed in the original app |
| `apt` | **regulad/apt**@`blackb0x`, off Debian's `apt` 2.9.4 tag | Yes — Procursus' own Darwin portability patch set (nine diffs plus `apt-key.diff`, and the three source moves its `makefiles/apt.mk` performs: `private-output.cc`/`algorithms.cc` to `.mm` because both reach into Foundation, and `memrchr.cc` copied into `ftparchive/`), plus one fix of our own: `cacheset.h`'s `Container_iterator` arithmetic operators are `const` now. That last one is not in Procursus' set — `Container_iterator` claims `random_access_iterator_tag`, libc++ on macOS 26 takes it at its word and evaluates `__first - difference_type(1)` on a *const* iterator inside `std::sort`, and six translation units failed to compile without it. **A host build tool, never linked into anything** — it exists so bake-time dependency resolution can use a real solver instead of `scripts/build_deb_cache_experimental_no_container.py`. Needs Homebrew's `berkeley-db@5`, `openssl@3`, `xxhash`, `lz4`, `xz`, `gettext`, `dpkg` (for `Dpkg.pm` on `PERL5LIB`) and julian-klode's `triehash` on `PATH` |
| `CBPatcher` | zzanehip/CBPatcher (upstream, pinned) | No — plain upstream. **Built as a separate EXECUTABLE and fork/exec'd, never linked**, same GPL-3.0 reason as `iBoot32Patcher` below: it was a static library on blackb0x's own link line until that was noticed, which the in-tree copy's missing LICENSE file helped hide. No fork needed — upstream already ships a `main()` whose CLI (`<infile> <outfile> <version> [--nosb]`, nukesb defaulting to 1) is a drop-in for the `patch_kernel()` call this used to link. The old local delta (an `#ifdef __APPLE__` around CBPatch.c's Apple-only Mach-O includes, plus `portable_macho.h` standing in for them) existed only to build on Linux and died with Linux support |
| `iBoot32Patcher` | **regulad/iBoot32Patcher**@`blackb0x`, off zzanehip/iBoot32Patcher | Yes — two real bug fixes: `patch_kaslr()` fell off the end of a non-void function on every *successful* branch (garbage return read non-zero on x86_64, 0 on arm64, so a real macOS run treated a successful KASLR patch as a hard failure), and `iBootPatcher()` tested its `RSA` argument twice so the `debug` argument was dead and `patch_debug_enabled()` ran whenever the RSA patch was asked for. **Built as a separate EXECUTABLE and fork/exec'd, never linked** — it is GPL-3.0-or-later and blackb0x declares no license, so linking would make blackb0x a GPLv3 derivative. Do not "simplify" it back into a static library |

`src/Img3Crypt.cpp` (first-party, in `src/`, not `src/libraries/`) is our own
`decrypt()` glue over `third_party/xpwn`'s public `AbstractFile` API. It replaced
the old `src/libraries/xpwntool.c`, a copy of `zzanehip/xpwntool-swift`'s
function-wrapped version of xpwn's `xpwntool` CLI. The logic is xpwn's (open via
`openAbstractFile{,2,3}`, clone an IMG3 template with `duplicateAbstractFile2`,
copy the bytes across), so `Img3Crypt.cpp` is GPL-3.0. It keeps one local fix
carried over from the old copy: `decrypt()`'s three error paths (`cannot open
infile` / `cannot open outfile` / `cannot duplicate file from provided template`)
each printed the diagnostic and then fell through to dereference the NULL they had
just reported, so any one of them was a SIGSEGV rather than a failure. They bail
out now, leaving a zero-byte output file. That is the root cause behind the
"`decrypt()` a nonexistent file corrupts the heap" hazard `Patcher.cpp` documents;
`decrypt()` is still `void`, so callers detect failure by checking for that
zero-byte output. (The variable xpwn named `template` was renamed `templateFile`
here — it is a reserved keyword in C++, which this file is, unlike the old `.c`.)

## Build & run

```
cmake -S . -B build && cmake --build build -j$(sysctl -n hw.ncpu)

# Two build groups, by who runs the result rather than by how it builds:
cmake --build build --target jailbreak   # blackb0x, blackb0x-pwn (no decryption, no patchers)
cmake --build build --target authoring   # bake-firmware, vendored apt, tests, xpwntool + the patchers it execs

# apt is EXCLUDE_FROM_ALL and is pulled in by `authoring`, never by `all`.

# Once, in bulk, for every known firmware — NOT run by blackb0x itself.
# Writes flat dist/<Component>-<device>_<buildID> entries (Apple's manifest keys):
./build/bake-firmware [--device <model>] [--build <buildID>]
                      [--only bootchain|ramdisk] [--force] [--stop-early]

./build/blackb0x [--ecid <id> | --udid <id>] [--dry-run]
```

Five `add_executable` targets, in two groups. The **jailbreak** half is
`blackb0x` plus `blackb0x-pwn`, which `blackb0x` spawns itself. The
**authoring** half is `bake-firmware` plus the two standalone bake tools split
out of it, `bake-iboot` and `bake-kernel`; both of those run rootless and in
seconds, because they mount nothing — only `bake-firmware`'s ramdisk half needs
privilege. `bake-iboot` is the one to reach for when arming a boot-args
directive, since boot-args are compiled into the iBEC rather than set at
runtime (see the `dist/` component list above and `docs/HISTORY.md`).

**Root: `blackb0x` needs none; `bake-firmware` requires it unconditionally.** Linux's two
reasons for root are gone (the loop mount and raw-USB device nodes), and `blackb0x`
reaches the device through IOKit with no special permissions. `bake-firmware` checks
`geteuid()` up front for every run, including `--only bootchain`, which technically
needs no privilege — a bake takes minutes, the two halves run together by default, and
failing late after paying for every download and patch is worse than one uniform rule.
The requirement is real and measured, not assumed: `hdiutil attach`/`resize` need no
privilege at all, but `chown()` to root:wheel returns EPERM, the pristine
`/sbin/launchd` is root-owned and mode 0555 on some firmwares so even opening it
`O_WRONLY` needs root's mode bypass, and files created by a normal user land owned by
that user. Staging real ownership goes through the VFS and is root-only on Darwin.

**Its output is handed back, though.** Everything `bake-firmware` writes into
`dist/` is `chown`ed to `$SUDO_UID`/`$SUDO_GID` (`chownToSudoCaller()`,
`ResourcePath.hpp`), so a `sudo` bake does not leave build artifacts you need
`sudo` to read, move or delete — and a later non-root `blackb0x` run can still
write there. A no-op when those variables are absent, since a genuine root
login has nobody to hand ownership to. The IPSW download cache
(`ipswDataRoot()`) is deliberately left alone; it has the same problem and the
same fix would apply, but that is a cache rather than build output.

- **`bake-firmware`** is the single ahead-of-time baker. It was two binaries,
  `bake-all-ramdisks` and `bake-all-bootloaders`, split because the Linux ramdisk
  bake needed `CAP_SYS_ADMIN` for its loop mount while the bootchain half needed no
  root at all; with the loop mount gone that reason went with it, and the duplicated
  target-enumeration and filter code collapsed into one file. For every `.keys` file
  under `keys/` (i.e. every known device/firmware combination) it does
  two things:
  - **bootchain** — downloads and patches iBSS, both iBECs, KernelCache and
    DeviceTree into `dist/` as flat
    `iBSS-`/`iBEC-`/`iBECTether-`/`KernelCache-`/`DeviceTree-<device>_<buildID>`
    entries. This is the only way to exercise
    `Patcher::patchiBSS()`/`patchiBEC()`/`patchKernel()` across every known firmware
    with no hardware attached. Each target runs in a forked child, deliberately: the
    vendored patch code does not survive being driven dozens of times in one process
    (see `BakeFirmware.cpp`'s own comment). `patchiBEC()` takes both boot-args
    strings and emits both images from one decrypted input; the defaults are
    `bootargs::kRamdiskBootArgs`/`kTetherBootArgs` in `src/Patcher.hpp`, and a
    baked string may not exceed `bootargs::kMaxBakedBootArgsLength` (179 bytes,
    derived there from two measured limits in the decrypted iBEC). Over-long is
    an error, never a truncation.
    The bootchain half also exists as two standalone, **rootless** binaries,
    `bake-iboot` and `bake-kernel`, which bake-firmware shells out to per tuple.
    `bake-iboot --device <m> --build <b> --force --extra-boot-args "<s>"` is the
    supported way to change what the kernel boots with: it rewrites both iBECs
    in seconds. `blackb0x --extra-boot-args` does **not** work and exits 2
    pointing here — there is no runtime boot-args channel on this hardware.
  - **ramdisk** — **grows the original volume and injects into it; it does not
    rebuild it.** `hdiutil resize` the decrypted image to fit the staged payload,
    `hdiutil attach -owners on`, splice `/sbin/launchd` and copy `/blackb0x` straight
    onto the mounted original, detach. Apple's content is never copied out, which is
    what preserves its ownership, decmpfs compression, hard links and volume identity.
    Both `hdiutil` calls need `-imagekey diskimage-class=CRawDiskImage`, since the
    image is a bare HFS+ volume with no partition map. Do not reintroduce
    `hdiutil create -srcfolder` here: that was a porting error, it cost ~19 MiB by
    decompressing Apple's files, and it silently baked the wrong ownership.
    The overlay it injects is the `xyz.regulad.blackb0x` package plus the debcache,
    and the output is `dist/RestoreRamDisk-<device>_<buildID>.dmg`. **This path has still
    never been run end to end on real macOS** — the mechanism above was verified
    piece by piece against real decrypted ramdisks, but no complete bake has run;
    see `BakeRamdisk.cpp`'s caveat and `.claude/TODO.md` items 4a and 15. Stays
    in-process, unlike the bootchain half, because the debcache and the `entrypoint/`
    build are resolved once and shared across every target.

  `--device`/`--build` narrow a run to one model or one build. `--only bootchain`
  skips the entrypoint build and the debcache entirely, which is the fast loop for
  iterating on patch logic.

  **There is no `--signed-only`, and don't add one back.** It existed, and asked
  ipsw.me which builds Apple still signs so it could bake only those. That filter
  answers a question the baker has no stake in: what this project can jailbreak is
  decided by which tuples have a `.keys` file under `keys/`, and the live path pins
  a per-device target build from `kJailbreakTargets` (`Cli.cpp`) anyway — one row
  per supported model, since the three Apple TVs topped out at different firmwares
  and no single build is even available for all of them. Apple's signing window neither creates
  nor removes a bakeable target, so the flag only ever hid valid ones and cost a
  network round trip per device. `signedBuildsForDevice()` (`IPSW.hpp`) survives for
  the `--stock-*` diagnostic routes, which genuinely need it because a real SHSH
  ticket can only be issued inside that window.

  The overlay is fully static (no per-device secrets get baked in), so each patched
  output is valid for every device on that firmware; there's no reason to re-derive
  it on every `blackb0x` run. `blackb0x` never invokes the bootchain half at all (it
  still patches those four components live — `.claude/TODO.md` item 5), and for the
  ramdisk `Patcher::patchRamdisk()` just checks whether the `dist/` entry it needs
  already exists. If it doesn't, `Cli.cpp` spawns `bake-firmware --only ramdisk` for
  that one tuple in the background. Re-running is cheap: any output that already
  exists is skipped unless you pass `--force`.
**Where the firmware comes from.** `blackb0x` patches nothing and downloads no
IPSW on the normal path; it sends a suite out of `dist/`. When one is missing,
`ensureBakedFirmware()` (`Cli.cpp`) resolves it in a fixed order and stops at the
first that works: already present, then `gh run download` of the artifact
`.github/workflows/ci.yml` publishes, then — only if running as root — shelling
out to `bake-firmware`. Neither `gh` nor root is the one case that cannot work,
and it errors with both fixes spelled out rather than a bare failure.
`$BLACKB0X_ARTIFACT_REPO` overrides which repository's artifacts are pulled.

Nothing is ever re-fetched or re-baked over an existing `dist/` entry, so a
hand-built suite always wins. This replaced a background `bake-firmware` spawn
that ran concurrently with the download/patch pipeline; that pipeline no longer
exists, so there was nothing left to overlap it with.

- **`blackb0x`** drives everything else (DFU discovery, the exploit, uploads, checking
  jailbreak status over AFC2). No `geteuid() != 0` gate in `Cli.cpp`, and none needed —
  a real permission failure surfaces clearly from `libirecovery`'s own
  `irecv_open_with_ecid()`. No install step; the binaries run from wherever they're
  built.
- **`scripts/push_authorized_keys.sh`** is separate from both: a standalone,
  no-root-needed script you run by hand, after `blackb0x` reports the jailbreak is
  running, to grant yourself SSH access (see "Runtime requirements" below for what
  it actually does).

**Build-time system dependencies, verified against a real fresh clone + build (see
README for the full list)**: a C/C++ toolchain, GNU make, CMake ≥3.16,
autoconf/automake/libtool/pkg-config (most of the tree is autotools-based) — Homebrew
bundles none of the four, so install them explicitly. (`xxd` used to be required too,
solely to embed `gaster`'s payload binaries as C arrays; with gaster gone, nothing in
the build shells out to it any more.)

**Runtime requirements beyond the build** (not just build-time deps):
- `hdiutil`/`diskutil`/`cp`/`tar` (invoked directly as subprocesses, no shell) —
  `bake-firmware`'s ramdisk half only, all built in to macOS.
- `python3` — `bake-firmware`'s ramdisk half shells out to
  `scripts/build_deb_cache_apt.py` to resolve the debcache fresh on every bake.
  That script drives **this project's own vendored apt** (`third_party/apt`,
  staged to `build/apt-tools/` by the `apt` target), so a bake needs that built
  once: `cmake --build build --target apt`, or just `--target authoring`. It is
  a real Debian solve — version constraints, `Provides:`, `Conflicts:` — and it
  is passed the tuple's real ProductVersion, so firmware-gated `Depends:`
  resolve correctly and the right persistence payload gets picked per target.
  It replaced `scripts/build_deb_cache_experimental_no_container.py`, which is
  still present and still passes its tests but is no longer wired into
  anything; see `.claude/TODO.md` items 17 and 18.
- `afsctool` on `$PATH` (`brew install afsctool`) — `bake-firmware`'s ramdisk half
  HFS+-compresses the staged `/blackb0x` payload with it, and hard-fails without it.
  Not optional: the first real bake of `AppleTV3,2` 10B329a finished at 70.9 MiB
  against the 64 MiB ceiling, and this is what closes that gap (~55% on ordinary ARM
  binaries). **It is invoked as `afsctool -c -T ZLIB`, and the `-T ZLIB` must stay.**
  decmpfs codecs are per-kernel: the 10B329a kernelcache ships
  `AppleFSCompressionTypeZlib.kext` declaring `providesType3`/`providesType4` and
  carries no LZVN or LZFSE compressor at all, while `ditto --hfsCompression` on a
  current macOS produces LZVN (type 8). Compressing with the host default would bake
  something that boots on 8.4.x and fails on 6.1.x, silently, and only on hardware.
- `ldid` on `$PATH` (`brew install ldid`) — `bake-firmware`'s ramdisk half
  builds `entrypoint/` on every bake (once per run, not per firmware) and
  ad-hoc-signs it.
- **A pinned Xcode, for `entrypoint/`. Not the stock toolchain, and its SDK
  matters as much as its linker.** Apple's
  current `ld` has no native 32-bit ARM support and silently delegates
  armv6/armv7/armv7s to `ld-classic`, a frozen `ld64-957.1` fork deprecated
  since Xcode 15; an era-appropriate Xcode's own `ld64` carries those archs
  natively. Full justification in **`entrypoint/README.md`'s "Pinned
  toolchain"** — don't duplicate it here.
  **`entrypoint` is a dynamically linked armv7 binary** (`LC_MAIN`,
  `LC_LOAD_DYLINKER` → `/usr/lib/dyld`, exactly one `LC_LOAD_DYLIB` →
  `/usr/lib/libSystem.B.dylib`), not the freestanding armv6 one it was for
  most of this project's life — the same shape Apple's own `/sbin/launchd`
  has on both target ramdisks, which is the configuration known to boot on
  this hardware. So it links an SDK now, and **the SDK must be the one
  matched to the device**: matched is 0 phantom symbols on both branches,
  crossed is 787, which links clean and dies at dyld load with no console.
  `DEVICE=<model>` is what selects it. Operationally:
  - `Xcode_6.4.dmg` (AppleTV3,1 / AppleTV3,2, iPhoneOS8.4 SDK, `ld64-242.2`)
    and `Xcode_5.1.1.dmg` (AppleTV2,1, iPhoneOS7.1 SDK, `ld64-236.4`) in
    **`~/Downloads`** is the default and needs no configuration. Whole DMGs
    are fine; the build attaches them `-nobrowse -readonly` and detaches
    after, and never detaches a volume it did not attach.
  - `XCODE_TOOLCHAIN=<dir>` names one outright and **bypasses `~/Downloads`
    discovery entirely** (for CI, which has no such directory). It takes
    either an `Xcode.app` bundle or a bare extracted toolchain root — any
    directory with `usr/bin/clang`, which for the dynamic build must also
    carry the matched SDK at `SDKs/iPhoneOS<ver>.sdk`.
    `XCODE_SEARCH_DIR=<dir>` moves the
    default search instead. `DEVICE=<model>` picks the Xcode by device, and
    through it the deployment target and SDK (`IOS_MIN_FOR_6.4` = 8.4,
    `IOS_MIN_FOR_5.1.1` = 7.1). `IPHONEOS_SDK=<dir>` names an SDK outright.
  - Everything is read from the **environment** by `entrypoint/Makefile`
    (`?=` throughout; `runCommand()` is `execvp()`, which inherits
    `environ`). There is deliberately no `bake-firmware` flag for it, and
    adding one would just re-spell a working interface.
  - **It never falls back to the system compiler.** A bad path or an empty
    search directory is a hard failure. `XCODE_TOOLCHAIN=system` is the
    explicit opt-out and the only way to reach the stock compiler (it is also
    the only path that consults `CC`, so `make XCODE_TOOLCHAIN=system
    CC=arm-apple-darwin11-clang` is how a cctools-port toolchain is used now).
    That path now also needs `IPHONEOS_SDK=<dir>`: a current Xcode's
    `iPhoneOS.sdk` is arm64e-only, so there is nothing in it to link an armv7
    binary against, and the build says so rather than guessing.
  - The build knows nothing about how the toolchain arrived — no LFS, no split
    parts, no reassembly, no fetching. It takes an Xcode already in one piece.
  - This is a **supply-chain hedge, not a fix**: nothing here has been booted
    on a device, and the pinned and stock toolchains produce functionally
    equivalent but not byte-identical binaries.
  - Every bake also runs `verifyEntrypointRuntimeClosure()`
    (`BakeRamdisk.cpp`) against the mounted ramdisk: every `LC_LOAD_DYLIB`
    and the `LC_LOAD_DYLINKER` target must exist there and every undefined
    symbol must be exported by something under `/usr/lib` or
    `/usr/lib/system`, or the bake fails. It was a no-op while `entrypoint`
    was freestanding and landed early on purpose so the three-device bakes
    would exercise it before it could fail; **it does real work now** (31
    undefined symbols, one `LC_LOAD_DYLIB`). Verified by hand against both
    extracted firmware roots: zero unresolved on each, matched SDK to matched
    device. Do not special-case any path away.
  - **OPEN DEFECT, in `src/` and not owned by `entrypoint/`:**
    `BakeFirmware.cpp`'s `main()` still calls `buildEntrypointBinary()` ONCE
    before its per-firmware loop and does not set `DEVICE`, so a multi-device
    `bake-firmware` run splices one binary, built against one SDK, into every
    device's ramdisk. That was correct while the binary linked no SDK. The
    call has to move inside the per-firmware loop with `DEVICE=<model>` in
    the child's environment; both that file and `BakeRamdisk.cpp` carry
    comments marking the exact lines. CI is unaffected (one device per bake
    leg, `DEVICE` exported into the job environment), and
    `verifyEntrypointRuntimeClosure()` is the loud backstop until it lands.
  - This bullet used to say the Xcode Command Line Tools were enough, and
    before that it demanded a `cctools-port` build of
    `arm-apple-darwin11-clang` against an iPhoneOS 6.1 SDK from a 1.7GB
    archive.org Xcode 4.6 image. That chain existed to give a *Linux* host
    Apple's `ld64`/`as` and is still gone; the pin is a different requirement
    with a different reason.
- Theos (`$THEOS`, default `~/theos`) and `dpkg-scanpackages` (`brew install dpkg`)
  — `package/build.sh` builds the `xyz.regulad.blackb0x` `.deb` with Theos's
  `dm.pl` on every bake.

**No container runtime is required anywhere.** Podman used to be a hard runtime
dependency of three separate steps, and in every case it existed to give a *Linux*
host something macOS already has natively: Apple's `ld64`/`as` (via `cctools-port`)
for `entrypoint/`, a box with Theos on it for the `.deb`, and `p7zip` for the
by-hand extraction helpers. `scripts/build_deb_cache.py` is the one thing that still
wants podman — it is a Linux-only maintenance tool, never invoked by a build or a
bake, kept only because growing `debcache/` needs a real apt-get solve. See its
own header.
- `usbmuxd` — macOS's own built-in daemon; Normal-mode discovery has nothing to talk
  to without it. (Linux additionally needed it run with `--no-preflight` via a systemd
  drop-in or discovery silently never fired; that requirement is gone with Linux.)
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
- No kernel-module blacklisting, no udev rule, no systemd drop-in. All three were
  Linux-only requirements and are gone; see the README's "No one-time system setup".

## Install-time design (what ships on the ramdisk, and how)

Folded in from `.claude/NEO_FLOW.md`, which is deleted. That file was written as a
design doc for work not yet implemented; the work is done and has moved past the
plan in several places, so what follows is the as-built design. Where this and the
code disagree, the code is right.

This supersedes the pre-rewrite Objective-C tool's install flow entirely. Two
structural problems drove the rewrite: `postinstall.sh` hit the live network on
every single real-OS boot forever, with no offline fallback; and the base Cydia
filesystem was a flat, pre-extracted overlay with no corresponding `dpkg` database
entries, so `dpkg` never knew those files existed. The fix that shipped is a real
`dpkg`/`apt`-driven install, resolved once at bake time and run once per device at
first real boot — but **not** a fully network-disabled one. That specific piece of
the plan turned out to be impossible (see `postinstall.sh` below).

### How `/blackb0x` gets assembled

Nothing is copied into `/blackb0x` by hand at runtime. The whole tree is assembled
once at bake time by `BakeRamdisk.cpp`'s `stageBlackb0xTree()`, then blindly
replicated onto the real device by `entrypoint.c`'s `merge_tree()` at boot.

1. **Resolve the closure.** `scripts/build_deb_cache_experimental_no_container.py`
   walks `package/packages.txt` (real package *names*, not `.deb` filenames) into a
   transitive closure over the `.deb` bytes already committed in `debcache/`. It only
   reads; it cannot fetch. Growing `debcache/` is a separate, Linux-only, CI job —
   `scripts/build_deb_cache.py`, which does a real apt-get solve against the five
   configured repos inside podman and refuses to run off Linux. See that script's
   header and the `debcache/` entry in "Repo layout".
2. **Local-only fallback.** A few packages will never resolve through any live repo
   but do have a real recovered `.deb` (`essential`, currently the only one).
   `package/local_only_debs.txt` names them by filename. `package/build.sh` bundles
   exactly those into the package as a file-backed apt repo at
   `/var/.blackb0x/local-debs`, with a real `dpkg-scanpackages` index, reachable
   on-device through `sources.list.d/local.list`
   (`deb [trusted=yes] file:///var/.blackb0x/local-debs ./`). A `.deb` sitting in
   apt's cache with no matching `Packages` entry is invisible to apt's resolver —
   verified with a real minimal repro — which is why these need an index even though
   the main debcache doesn't.
3. **Decide bake-time force-install vs. real apt.**
   `computePreinstallEligibleFilenames()` walks the resolved closure against
   `misc/prebake_package_blacklist.txt`. Packages with a real, stateful,
   uninspectable or device-only postinst/preinst (`cydia`, `firmware-sbin`, `rtadvd`,
   `pam`, `pam-modules`, `essential`, the exploit-specific ones) are excluded, and so
   is anything that transitively depends on an excluded package — propagated to a
   fixpoint, not one level. An eligible package that still has a postinst gets it
   stripped between unpack and configure (safe: postinst only runs at `--configure`);
   one with a preinst gets its `.deb` rebuilt without that member before being
   unpacked at all (preinst runs *during* `--unpack`, so stripping after the fact is
   too late). Both decided per real `.deb` content, never hardcoded.
4. **Install the eligible set for real.** On Linux this ran inside a `debian:stretch`
   container for an era-appropriate `dpkg` 1.18.26: force-unpack everything
   (`--force-architecture --force-depends --unpack`, because this ecosystem has a
   genuine unbreakable circular Pre-Depends chain, `dpkg → tar → gzip/lzma → sed →
   dpkg`), then one `dpkg --force-depends --configure -a` to let dpkg's own solver
   break the cycle — the real debootstrap two-phase pattern. **macOS has no container
   runtime, so that path is gone**; `computePreinstalledPackages()` now extracts each
   eligible `.deb` with plain `ar`/`tar`, merges its payload, and hand-writes the
   matching dpkg status stanza and `info/` state. That is sound precisely because
   step 3 already proved no maintainer script ever executes on this path, which
   removes the ordering problem real dpkg was there to solve. `kPreinstallInnerScript`
   in `BakeRamdisk.cpp` is the old container script, kept as the record of what it
   did and executed by nothing.
5. **Cache it once per process.** `computeGlobalDebcacheOnce()` runs steps 1–4 exactly
   once per `bake-firmware` invocation (a static-local cache). The pipeline is
   firmware-independent; re-running it per target meant identical repeated work.
6. **Build and install the package.** `stageBlackb0xPackage()` runs
   `package/build.sh`, which produces a real `xyz.regulad.blackb0x` `.deb` with
   Theos's `dm.pl`, then installs it into the staged tree the same way
   `stageEtasonatv()` does — extract, merge payload, append a real dpkg status stanza.
   This replaced a dozen hand-rolled `stageFile()` calls (`.claude/TODO.md` item 11).
   `postinstall.sh`'s `__BLACKB0X_PACKAGES__` placeholder is templated by `build.sh`
   from `package/packages.txt` itself — deliberately *not* from this bake's resolved
   closure, so the package needs no bake state and stays independently buildable. It
   is also more correct on-device: bake-time resolution runs against a synthetic
   `firmware` package and a few entries legitimately fail there while resolving fine
   against real repos. Top-level `etc/`/`var/` are remapped to `private/etc/`,
   `private/var/` — the `.deb` ships them unprefixed (right for on-device dpkg, where
   they are symlinks), but `/blackb0x` is a flat mirror replicated literally.
7. **Stage the rest, per firmware.** The apt lists cache directory, the
   non-preinstalled `.deb` bytes into apt's real cache directory, and exactly one
   of three persistence payloads picked by this firmware's real `ProductVersion`.
   **The lists cache is staged EMPTY**, and deliberately so —
   `scripts/build_deb_cache_apt.py` writes `apt-lists/` empty on purpose (its own
   module docstring says why: the only lists it could generate would describe the
   synthetic `file://` repo built out of `debcache/`, not the real repos, and would
   be actively misleading on-device). So `stageAptListsCache()` stages a real path
   with no package index behind it, and **an offline `apt-get install` has no
   candidate for anything** — see `postinstall.sh` below for the consequence.
8. **Build the entrypoint binary once.** `BakeFirmware.cpp`'s `main()` calls
   `buildEntrypointBinary()` before its per-firmware loop, same pattern as the
   debcache cache, and passes the one built path into every `bakeRamdisk()` call.

### Persistence payloads

Picked by `stageVersionBranch()` off the firmware's real `ProductVersion`:

- **8.4.x** → `stageEtasonatv()`: the four loose payload files extracted directly
  from the real `net.tihmstar.etasonuntether` `.deb` in `debcache/` (not the old
  hand-assembled `tihmstar-untether.tar`, which is gone), with this project's own
  `misc/untether.bin` deliberately overriding the package's copy, held in dpkg so
  apt can never replace it. Plus the `/untether/expl.js` → `/--early-boot` symlink.
  The `rtbuddyd`→`jsc` swap is **not** done at bake time — see `entrypoint.c`'s
  `fixup_etasonuntether_rtbuddyd()`, which needs the real target volume. The only
  branch with a real untether.
- **7.x / 8.x (non-8.4)** → `stageIos7Tether()`: swaps a custom binary into
  `/usr/libexec/dirhelper` (`misc/dirhelper`). Genuinely tethered; no untether exists
  for this range. The technique traces cleanly to evasi0n6's published
  `dirhelper`-hijack mechanism (matching `remount()` call, confirmed against
  evasi0n6's archived source), but the compiled binary itself was never
  open-sourced by anyone — see `misc/README.md` for the full dead-end provenance.
- **6.1.4** → `stageP0sixspwn()`: p0sixspwn's own untether payload, extracted from
  the real vendored `.deb`.
- Anything else warns and stages common content only. No hard failure — a firmware
  with no persistence answer yet still gets everything else correctly.

### `entrypoint.c` replaces `/sbin/launchd`, not `/etc/rc.boot`

The one place this project's exploration went somewhere and came back. Real
disassembly of an AppleTV2,1 10B809 `RestoreRamdisk` found `/etc/rc.boot` to be an
`LC_MAIN` Mach-O, which was read as the kernel entering it directly, so the splice
target moved there for a while on the theory that injecting at the true first entry
point is strictly better. It didn't generalize: a real bake against AppleTV3,1/3,2
12H606 failed because that firmware's ramdisk has no `/etc/rc.boot` at all (`/etc/`
is nearly empty there, confirmed by mounting it). Reverted to always targeting
`/sbin/launchd`. The ad-hoc signing identity matches: `com.apple.launchd`.

**The theory behind the detour was also just wrong, established later.** The kernel
never execs `/etc/rc.boot` on any firmware — neither the 10B809 nor the 12H606
kernelcache contains that string at all, and `/sbin/launchd` is the sole compile-time
entry in XNU's `init_programs[]`. `rc.boot` really is `LC_MAIN`, which the original
disassembly got right, but `/bin/launchctl` is what runs it, several steps downstream
of PID 1. So `/sbin/launchd` is not merely the safer target, it is the only one the
kernel has ever known. See "What the firmware itself says about PID 1" below and
`entrypoint/README.md`.

`do_install()` is unconditional apart from two guards — bail if
`/mnt1/Applications/AppleTV.app/AppleTV` is missing (not an Apple TV), and `panic()`
if `/mnt1/var/.blackb0x/install-done` already exists rather than clobber live dpkg
state. No version branching happens on-device at all; that decision was made at bake
time, so `entrypoint.c` never needs to know what firmware it is on.

- `merge_tree()` is a generic recursive merge (real `stat()` owner/mode, symlinks
  recreated verbatim, existing destination directories recursed into rather than
  replaced). It replaced the old
  `create_cydia_directories()`/`MYSTERY_DIR_MODE`/`clone_directory()` machinery
  entirely — every directory `/blackb0x` needs is already a plain staged entry in it.
- `panic()` deliberately never returns and never reboots. An automatic reboot on a
  genuine failure would re-run the same ramdisk into the same panic every cycle, with
  nothing to show a human debugging over console/serial.
- The mount target is `/mnt1`, not `/mnt` — `/mnt1` and `/mnt2` are real pre-existing
  empty mountpoints on a pristine ramdisk; `/mnt` never was.
- `set_auto_boot()` runs the pristine ramdisk's own `/usr/sbin/nvram auto-boot=1`
  before every `reboot(2)`. SecureROM/iBoot clears that variable once a real DFU
  payload has run; without it a plain reboot risks leaving the device at the iBoot/DFU
  prompt instead of continuing into the installed OS. This is the one thing salvaged
  from the ssh-rd-derived `rc.boot` this project once looked at (now deleted).

### `postinstall.sh` — one-shot, network-opportunistic

Runs once the device boots into its real OS
(`xyz.regulad.blackb0x.postinstall.plist`, `RunAtLoad`, root). `StandardOutPath` and
`StandardErrorPath` point at two *separate* log files — pointing both at one path is
a real, long-documented launchd bug.

**The original plan called for hard-disabling the network for the whole install
window. That is impossible in practice**: Kodi and a few other real packages are far
too big for this A4-era ramdisk's budget, so they are never staged locally at all
(`kNeverStageDebs`). What shipped uses the network opportunistically instead:

- All five real sources.list.d entries are always present; `apt-get update` may reach
  them if it can. That is the **one and only** command in the script allowed to fail
  (`|| true`). Everything else runs under a bare `set -ex`, so a real failure stops
  the script, `install-done` never gets written, and the next boot retries the whole
  install from scratch.
- **With no network, apt knows nothing.** `/private/var/lib/apt/lists/` is staged,
  but **empty** (see step 7 above) — there is no bake-time package index on the
  device at all. So an offline `apt-get install` finds no candidate, it fails under
  the bare `set -ex`, `install-done` is never written, and the device shows
  "nothing happened" even on a boot that otherwise worked perfectly. This is a
  real, known defect, not a design choice; `docs/HISTORY.md`'s ramdisk-teardown
  section records it as one of two functional defects found independent of the
  current boot failure.
- Whatever `.deb` bytes did fit sit in apt's own cache
  (`/private/var/cache/apt/archives/`); apt finds them via its normal
  cache-before-download check, no `file://` source needed for the main debcache.

The sequence: exit if `install-done` exists (a real UTC timestamp, not an empty
touch); wait ≥60s since boot before touching apt, because this `RunAtLoad` job fires
before networking has necessarily associated and this old launchd has no portable
one-time-delay knob (`sleep` is not guaranteed present on a stock retail OS — dpkg's
own control file lists `bash` as a `Depends:`, so this ecosystem treats even a shell
as something it must supply; `coreutils-bin`'s cached `.deb` gets `dpkg -i`'d if
missing, though it is normally already bake-time-preinstalled); `apt-get update ||
true`; `apt-get install cydia` **on its own, first**, because its real postinst does
its own `/var/stash` relocation and nothing else may assume that environment exists;
then the templated package set; then `install -f`, plain `upgrade`, `autoremove`;
then write `install-done` last.

Plain `upgrade`, never `dist-upgrade`/`full-upgrade`: this on-device apt
(`apt7 0.7.25.3`) has no unified `apt` command and no `full-upgrade` at all, and
plain `upgrade` only touches already-installed packages, never demanding more from
the network than what is staged.

Worth knowing, because the pre-rewrite tool got this wrong: **the real `cydia`
package ships its own `/usr/libexec/cydia/startup`, `firmware.sh` and a real
`RunAtLoad` LaunchDaemon** (1.1.30, downloaded and inspected directly). The
synthetic-`firmware`-package declaration, GSC capability stanzas,
`Media/Cydia/AutoInstall` handling and `uicache` refresh all happen automatically
once `apt-get install cydia` runs through real dpkg. The old tool reimplemented all
of it by hand only because its flat-file-copy install never ran real dpkg, so
Cydia's own LaunchDaemon never got registered.

### Ramdisk sizing

`/blackb0x` does not fit in the pristine ramdisk's free space — Apple ships that
volume exactly full (0 free blocks). **The bake grows Apple's own volume and injects
into it; it does not rebuild it.** `afsctool`-compress the staged payload first (so
the volume is grown to fit the *compressed* tree), measure it, `hdiutil resize` the
decrypted image up to that plus a margin, `hdiutil attach -owners on`, splice
`/sbin/launchd` and copy `/blackb0x` straight onto the mounted original, detach, then
`hdiutil resize -size min` to shrink back. Both `hdiutil` calls need
`-imagekey diskimage-class=CRawDiskImage`.

The Linux-era objection to growing in place (confirmed bugs in xpwn's `grow_hfs()`,
`libhfsp` and the Linux kernel driver's own resize path) does not apply to Apple's
`hdiutil`, and the build-a-new-volume-and-`cp -a` flow that replaced it was a porting
mistake that cost ownership, decmpfs compression, hard links and volume identity —
see `BakeRamdisk.cpp`'s own header comment for the measured cost of each.

**The shipped volume is therefore plain, case-INSENSITIVE HFS+ (`H+`, v4)** —
Apple's original personality, preserved — not the case-sensitive HFSX this section
used to describe. Nothing creates a volume any more, so there is no `-fs` argument to
get right. **Known, accepted consequence: case-varying sibling names collapse.**
ncurses' terminfo tree is the visible case — on the shipped image
`usr/share/terminfo/E` holds both `Eterm-*` and `eterm`, and `usr/share/terminfo/a`
holds both `Apple_Terminal` and `ansi*`, where a case-sensitive volume would have
kept `e`/`E` and `a`/`A` apart. This is cosmetic: the entries are all still present
and findable, and nothing on the ramdisk runs ncurses. Do not "fix" it by
reintroducing a volume rebuild.

**A finished ramdisk over 64 MiB is a hard failure**, not a warning: the oversized
output is deleted and the bake returns false, because a ramdisk that big cannot be
uploaded and leaving it in `dist/` would get it silently reused.
`DEBUG_RAMDISK_LIMIT_MIB` overrides the ceiling; `-1` disables the check entirely and
turns it into a surfaced warning instead. (The ceiling was a 70MB rule-of-thumb
warning under the Linux design; the A4/A5 iBEC has no `ramdisk-size` variable to ask,
and 64 MiB was confirmed against a live AppleTV3,2 as at-or-very-near the real limit.)

## CI

`.github/workflows/ci.yml`. `build` runs on every push: both build groups, both
test suites, plus assertions on the invariants that broke during the macOS port
(no Homebrew paths in any shipped binary, vendored apt actually runs) and on
`entrypoint`'s Mach-O shape. Those last ones **inverted** with the dynamic
conversion: they used to demand freestanding armv6 / `LC_UNIXTHREAD` / zero
`LC_LOAD_DYLIB`, and now demand `arm_v7`, `LC_MAIN` with no `LC_UNIXTHREAD`,
exactly one `LC_LOAD_DYLIB` and it is `/usr/lib/libSystem.B.dylib`,
`LC_LOAD_DYLINKER` → `/usr/lib/dyld`, `LC_VERSION_MIN_IPHONEOS`, none of
`LC_DYLD_CHAINED_FIXUPS`/`LC_DYLD_EXPORTS_TRIE`/`LC_BUILD_VERSION`,
`Identifier=com.apple.launchd`, and **ARM mode rather than Thumb** — asserted
explicitly now (even `LC_MAIN` `entryoff`, plus a 4-byte encoding at `_main`)
because the hand-rolled `rsbcs` that used to make ARM mode self-enforcing is
gone. `bake` runs on every push (plus the schedule
and manual dispatch, but never on a pull request, since it runs the branch's
code under sudo), one runner per device, and publishes `dist/` as artifacts so
end users never need root or the authoring toolchain.

Artifacts refresh on the 23rd of every month.

**How the pinned `entrypoint/` toolchain reaches a runner.** No runner has a
`~/Downloads` full of DMGs, and the build hard-fails rather than substituting
the system compiler, so every job that builds `entrypoint/` (the `build`
job's artifact check, and every `bake` leg) provisions one first: read
`XCODE_FOR_<device>` out of `entrypoint/Makefile` so the mapping lives in one
place, install Rosetta 2 if the runner is arm64 (these toolchains are x86_64
with no arm64 slice), clone the private `regulad/xcode` mirror's Git LFS
*pointers only*, key a cache on those pointer hashes, reassemble the DMG from
1 GiB parts, mine out the minimum subset — `clang`, `ld`, clang's resource
dir, **and the iPhoneOS SDK's `usr/include` + `usr/lib` + `SDKSettings.plist`**
— then export `XCODE_TOOLCHAIN=<dir>` and `DEVICE=<model>`. Those two
variables are the whole interface; the baker knows nothing about how the
toolchain got there. The SDK half of that extraction used to be speculative
("not needed today, copied for the conversion"); it is load-bearing now.

## External references

- [ATV3 jailbreak writeup PDF](https://elhacker.info/Books/BOOKS%20PART%206/atv3_jb-.pdf)
  — potentially explains the `jsc` (JavaScriptCore) framework reference and the
  `/mnt/--early-boot` symlink found in the `etasonATV` branch of `entrypoint/`'s
  reverse-engineered install logic (see `misc/README.md`) — both were
  flagged there as "we replicate the action, not the underlying mechanism."

## Current status: macOS only

**Linux support is removed.** The build refuses to configure off Apple (`CMakeLists.txt`
fails fast rather than dying deeper in a vendored ExternalProject). Apple Silicon is the
tested host.

`checkm8` never worked on Linux, and that is why. The exploit sequence runs to
completion and the task-struct overwrite simply never lands. Four explanations were
proposed and each refuted by later measurement — a host-stack limitation, the overwrite
being refused, the bug-setup SETUP never arriving, and the consumed-byte count — and by
the end **every observable matched a working macOS run while the outcome still
differed**. `docs/HISTORY.md` records each dead end and why it died; the point of that
detail is that nobody re-runs those experiments.

Two findings from it are worth carrying forward regardless of platform:

- **The bug setup is non-deterministic.** The same cancel delay produced 0 and 64
  consumed bytes on identical hardware, and 32 of 35 attempts consumed bytes within a
  single run. Any measurement taking one sample per configuration is measuring noise.
- **`DFU_GETSTATUS` is not an oracle for the exploit precondition.** checkm8 *is* a bug
  in that state machine: the aborted `DFU_DNLOAD` leaks a buffer the state machine stops
  tracking, so a device reporting `dfuIDLE` while holding a dangling pointer is the
  vulnerability, not evidence against it.

What still works and is verified: normal-mode discovery (real `usbmuxd`, wolfSSL/SSLv3
lockdownd handshake, AFC), the full firmware download/decrypt/patch/re-encrypt pipeline,
`--dry-run`, and `blackb0x-pwn` against real AppleTV3,2 hardware on macOS.
**Only one physical unit has ever been available: an AppleTV3,2** — the `AppleTV2,1`
(SHAtter) and `AppleTV3,1` (external-hardware checkm8) paths are implemented from
protocol analysis only.

### First real macOS build (Apple Silicon, macOS 26.6.1, Xcode CLT + Homebrew)

The whole tree now configures, compiles and links on a real Mac, and `ctest` passes.
Three macOS-only defects had to be fixed to get there, none of which a Linux build
could ever have surfaced:

- **`bake-firmware` linked no Apple frameworks.** wolfSSL's own
  `DoAppleNativeCertValidation`/`LoadSystemCaCertsMac` and curl's `Curl_macos_init`
  call straight into CoreFoundation, Security and SystemConfiguration on Darwin. The
  target now links exactly those three, derived from the real undefined-symbol list.
  Deliberately **not** IOKit — unlike `blackb0x`, this binary talks to no USB device.
- **`ar rc` does not build a `.deb` on macOS.** Apple's cctools `ar` runs ranlib on
  every archive it creates and prepends a `__.SYMDEF SORTED` member, so the first
  member is no longer `debian-binary` and `dpkg-deb` rejects the result outright.
  `stripPreinstFromDeb()` (`BakeRamdisk.cpp`) and the resolver's own test fixtures
  pass `rcS` now. GNU ar omitted the symbol table without being asked, which is why
  this was invisible on Linux. Note `ar x` is fine — only the write side differs.
- **`tar --auto-compress` is create-only on macOS.** The system tar is bsdtar, where
  `-a` is a hard error under `-t` ("Option -a is not permitted in mode -t") and a
  warn-and-ignore under `-x`. `readDebControlInfo()` used it under `-t`, so every
  bake died on the first package it read. Every read-mode call dropped the flag; the
  one create keeps it, since that is what preserves the original `control.tar.*`
  compression.

Also worth knowing, both verified here rather than assumed:

- **macOS 26 ships its own `/usr/bin/stdbuf`** — a BSD reimplementation, not GNU
  coreutils. The `-oL -eL` spelling `runLineBufferedSubprocess()` passes is valid in
  both, and it was confirmed to genuinely line-buffer through a pipe, so
  `resolveStdbufBinary()`'s plain-`stdbuf`-first probe lands on it and needs no
  change. Homebrew's `gstdbuf` is now only the fallback for older releases.
- **A stale `build/` tree will fail confusingly.** A configure from before a
  submodule bump keeps the old installed headers, which showed up as
  `irecv_usb_control_transfer_ex` being undeclared. Delete that
  ExternalProject's stamps rather than reading the error literally.

**The ramdisk baker's macOS path is still unverified end to end**, but less of it is
unverified than before. Three of its four pieces now run here:

- **Bootchain**: a real sweep over the five then-currently-signed builds downloaded,
  decrypted and patched iBSS, iBEC and DeviceTree with zero failures. That was five of
  95 known tuples; the full sweep is still `.claude/TODO.md` item 6.
- **Debcache**: the portable resolver walks all 107 real archives and resolves 60
  top-level packages to 68 files, after the `ar`/`tar` fixes above.
- **`entrypoint/`**: builds and produces a correct artifact. (At the time of
  that first macOS build that meant armv6/`LC_UNIXTHREAD`/no `LC_LOAD_DYLIB`
  off the stock toolchain; it is a pinned-toolchain armv7
  `LC_MAIN`/libSystem binary now — see the pinned-Xcode bullet above.) The
  cross-toolchain it used to need is no longer needed at all.
- **`hdiutil` volume creation**: still never executed. This is the remaining gap, and
  its two flagged unknowns stand — the exact `-fs "Case-sensitive HFS+"` argument,
  and whether `hdiutil create -format UDRW -layout NONE` really emits flat raw bytes.

One incidental `hdiutil` finding from mounting real ramdisks by hand: a bare HFS+
volume with no partition map needs `-imagekey diskimage-class=CRawDiskImage`, or
`attach` fails with "image not recognized". `bakeRamdisk()` attaches exactly this
kind of image.

### What the firmware itself says about PID 1

Established by disassembling two decrypted kernelcaches and mounting two real
RestoreRamdisks, because `entrypoint/README.md` had this wrong. Recorded here because
it constrains any future redesign of the install entry point:

- **The kernel execs `/sbin/launchd` and nothing else.** Sole entry in XNU's
  `init_programs[]`, a compile-time constant, on both 10B809 and 12H606. No boot-arg
  redirects it — no `launchdsuffix`, no `launchd.debug`/`.development` (those are
  DEVELOPMENT/DEBUG-only kernels; these are RELEASE).
- **`/etc/rc.boot` is never kernel-entered.** Neither kernelcache references it. It is
  a real `LC_MAIN` Mach-O on 10B809, but `/bin/launchctl` runs it, and all it does is
  exec one of `restored_external`/`restored_update`/`restored`/`ramrod`. 12H606 drops
  it for a LaunchDaemon plist.
- **The kernel does parse shebangs**, so a script entry point is not blocked there:
  the 12H606 kernelcache carries XNU's `execsw[]` strings `Mach-o Binary`,
  `Fat Binary`, `Interpreter Script`. What blocks it is that neither ramdisk has a
  shell, and a staged one cannot resolve its dylibs. Full account in
  `entrypoint/README.md`'s "Why this is a binary and not a shell script".
- **dyld is live at PID 1.** Apple's own launchd on both ramdisks is `LC_MAIN` with
  `LC_LOAD_DYLINKER`. `entrypoint.c` is dynamically linked too now, for exactly
  that reason: matching what Apple's own PID 1 does here is the conservative
  choice, and the dylib-closure objection that once justified freestanding was
  measured away (the whole closure is `/usr/lib/libSystem.B.dylib`, whose
  re-export targets are 22/22 and 32/32 present on the two ramdisks).

**Three of five signed builds fail at kernelcache decrypt, and it is not a macOS
regression.** `AppleTV2,1` 10B809 and 11D258 and `AppleTV3,2` 12H606 all die in
xpwn's `createAbstractFileFromComp()`, while `AppleTV3,1` 10B809 and 12H606 succeed.
The correlation is exact: every failing build's IMG3 `DATA` tag has a `dataSize` that
is not a multiple of 16, and the only passing one is block-aligned. `Patcher.cpp`
already fails these cleanly rather than crashing, so the sweep survives them. Two
things are worth recording for whoever picks this up:

- `img3.c`'s decrypt and encrypt lengths are asymmetric — `setKeyImg3()` uses
  `((header->size - sizeof(AppleImg3Header)) / 16) * 16` while `closeImg3()` uses
  `(header->dataSize / 16) * 16`. Changing the decrypt side to match the encrypt side
  was tried and **does not fix it**; it changes the decompressed length without
  reaching the claimed one. The padded-tag-size form appears to be right, since Apple
  pads the encrypted payload up to the block boundary and the tag has exactly that
  much room.
- The LZSS stream decompresses to within ~50-70 bytes of its claimed length, so the
  bulk decrypts correctly and only the tail is wrong. That rules out a simply-wrong
  key.

The `DEBUG_`-prefixed knobs in `blackb0x-pwn` (`DEBUG_CANCEL_DELAY_US`,
`DEBUG_OVERWRITE_TIMEOUT_MS`, `DEBUG_RECONNECT_ATTEMPTS`, `DEBUG_KEEP_CONNECTION`,
`DEBUG_IGNORE_GROOM_ERRORS`, `DEBUG_TRACE_TRANSFERS`, `DEBUG_DFU_STATUS`) are kept.
Every default is the behaviour confirmed working on real hardware, so an unset
environment is the original path byte for byte. `DEBUG_DFU_STATUS` is the exception that
deliberately perturbs: `GETSTATUS` advances the DFU state machine, so a run with it set
is not evidence about a run without it.
