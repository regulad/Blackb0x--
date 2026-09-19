//
//  ResourcePath.hpp
//  Blackb0x
//
//  Resolves paths under Blackb0x/ramdisk/ (the ramdisk overlay trees — see
//  Patcher.cpp's patchRamdisk()) and Blackb0x/ImageKeys/ (per-firmware .keys)
//  — replaces every [[NSBundle mainBundle] pathForResource:...] call site in
//  the original. Proper install-prefix resolution (CMAKE_INSTALL_DATADIR,
//  /usr/share/blackb0x) is Phase 7 work; for now this checks an override env
//  var, then falls back to a path relative to the current working
//  directory, which is sufficient for running the CLI from a build/dev tree.
//

#pragma once

#include <string>

// Resolves the ramdisk overlay root: $BLACKB0X_RAMDISK_DIR if set,
// otherwise "Blackb0x/ramdisk" relative to the current working directory.
// The whole tree mirrors the destination ramdisk's own filesystem layout,
// merged in with a single `cp -a` (one unconditional path — no more
// SSH-only vs full-Cydia split, see docs/HISTORY.md).
std::string resolveRamdiskPath();

// Resolves `relativePath` (e.g. "AppleTV2,1/AppleTV2,1_10A406e.keys") against
// the per-firmware decryption key root: $BLACKB0X_IMAGEKEYS_DIR if set,
// otherwise "Blackb0x/ImageKeys" relative to the current working directory.
// Kept separate from resolveRamdiskPath() above — these aren't shipped to
// the device like everything under ramdisk/, they're only ever read locally.
std::string resolveImageKeyPath(const std::string& relativePath);

// Resolves the path to the `blackb0x-pwn` binary — this project's one and
// only pwntool (see CMakeLists.txt, and docs/HISTORY.md for why the vendored
// `gaster` that used to sit beside it is gone): $BLACKB0X_PWN if set,
// otherwise the "blackb0x-pwn" alongside
// blackb0x's own executable, which is reliable
// regardless of the current working directory the CLI happens to be invoked
// from — unlike resolveRamdiskPath() above, a CWD-relative fallback would
// break as soon as someone runs it from outside the build tree.
std::string resolvePwnPath();

// Resolves the path to the `bake-firmware` binary (see CMakeLists.txt --
// built as its own executable, landing alongside blackb0x itself):
// $BLACKB0X_BAKE_FIRMWARE if set, otherwise "bake-firmware" alongside
// blackb0x's own executable, same resolution strategy as resolvePwnPath()
// above. Used by Cli.cpp to self-bake a missing dist/ ramdisk on demand
// (`bake-firmware --only ramdisk`) instead of requiring a separate manual
// baking step first -- unconditionally, since baking needs neither root nor
// a mount.
std::string resolveBakeFirmwarePath();

// Resolves the path to the `iBoot32Patcher` binary (see CMakeLists.txt --
// built from the third_party/iBoot32Patcher submodule as its own executable,
// landing alongside blackb0x itself): $BLACKB0X_IBOOT32PATCHER if set,
// otherwise "iBoot32Patcher" alongside blackb0x's own executable, same
// resolution strategy as resolvePwnPath() above.
//
// It is a separate binary rather than a linked library ON PURPOSE: it is
// GPL-3.0-or-later and blackb0x declares no license, so linking it would
// make blackb0x a GPLv3 derivative. Patcher.cpp fork/execs it.
std::string resolveIBoot32PatcherPath();

// Resolves the path to the `CBPatcher` binary (the kernel patcher; see
// CMakeLists.txt -- built from the third_party/CBPatcher submodule as its own
// executable, landing alongside blackb0x itself): $BLACKB0X_CBPATCHER if set,
// otherwise "CBPatcher" alongside blackb0x's own executable.
//
// Separate binary rather than a linked library for the same reason as
// iBoot32Patcher above: GPL-3.0. Patcher.cpp fork/execs it.
std::string resolveCBPatcherPath();

// Resolves the checked-in .deb cache root: $BLACKB0X_DEBCACHE_DIR if set,
// otherwise "debcache" relative to the current working directory.
// bakeRamdisk()'s stageDebcache() (BakeRamdisk.cpp) copies exactly the
// resolved subset from here into /blackb0x/var/.blackb0x/debs/ at bake time
// — not the whole (append-only, never-pruned) directory.
//
// Read-only from this process's point of view. The directory is GENERATED on
// Linux by scripts/build_deb_cache.py (real apt-get in podman; it refuses to
// run anywhere else) and committed; a bake only ever consumes it.
std::string resolveDebcachePath();

// Resolves `relativePath` against package/layout/ -- the xyz.regulad.blackb0x
// package's own source tree, laid out at the FINAL on-device paths
// (etc/apt/..., var/root/.profile, System/Library/LaunchDaemons/...), matching
// what real Cydia .debs ship (checked: the vendored packages use ./etc/, not
// ./private/etc/). $BLACKB0X_PACKAGE_DIR if set, otherwise "package/layout"
// relative to the current working directory, same convention as
// resolveMiscPath() below.
//
// These files used to live loose under Blackb0x/Misc/ and be staged one
// stageFile() call at a time. They are package payload now -- see package/
// and .claude/TODO.md item 11.
std::string resolvePackagePath(const std::string& relativePath);

// The package/ directory itself (not its layout/ subtree) -- where build.sh
// and packages.txt live. $BLACKB0X_PACKAGE_ROOT if set, otherwise "package".
std::string resolvePackageRoot();

// Resolves `relativePath` against the loose-legacy-asset root:
// $BLACKB0X_MISC_DIR if set, otherwise "Blackb0x/Misc" relative to the
// current working directory. Holds files that need in-place content
// splicing into the mounted volume at bake time rather than a plain
// `cp -a` (rc.boot) — see Blackb0x/Misc/README.md.
std::string resolveMiscPath(const std::string& relativePath);

// Resolves the entrypoint/ source directory (the freestanding ARMv6
// replacement for /sbin/launchd — see entrypoint/README.md):
// $BLACKB0X_ENTRYPOINT_DIR if set, otherwise "entrypoint" relative to the
// current working directory. bakeRamdisk() builds this fresh on
// every bake rather than shipping a precompiled binary — see that
// directory's own README for why (freestanding, no libSystem, needs
// cctools-port's real Apple ld64 port; nothing to check in that a normal
// toolchain could reproduce).
std::string resolveEntrypointPath();

// Names bakeRamdisk()'s temporary decrypted intermediate file for a given
// input path (BakeRamdisk.cpp's own internal working file — the final
// output path is a caller-supplied parameter, not derived from this).
std::string decryptedDMGFor(const std::string& path);
