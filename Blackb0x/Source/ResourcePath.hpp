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

// Resolves the path to the vendored `gaster` binary (see CMakeLists.txt —
// built as its own executable, landing in the same output directory as
// blackb0x itself): $BLACKB0X_GASTER if set, otherwise the "gaster"
// alongside blackb0x's own executable (via /proc/self/exe), which is
// reliable regardless of the current working directory the CLI happens to
// be invoked from — unlike resolveRamdiskPath() above, a CWD-relative
// fallback would break as soon as someone runs it from outside the build
// tree.
std::string resolveGasterPath();

// Resolves the path to the `blackb0x-pwn` binary (Apple-only — see
// CMakeLists.txt's `if(APPLE)` block, and docs/HISTORY.md for why:
// gaster does not work on macOS, blackb0x-pwn does): $BLACKB0X_PWN if
// set, otherwise "blackb0x-pwn" alongside blackb0x's own executable, same
// resolution strategy as resolveGasterPath() above.
std::string resolvePwnPath();

// Resolves the path to the `bake-all-ramdisks` binary (see CMakeLists.txt --
// built as its own executable, landing alongside blackb0x itself; Linux-only
// for now, see that target's own comment there): $BLACKB0X_BAKE_ALL_RAMDISKS
// if set, otherwise "bake-all-ramdisks" alongside blackb0x's own executable,
// same resolution strategy as resolveGasterPath()/resolvePwnPath() above.
// Used by Cli.cpp to self-bake a missing/stale dist/ ramdisk on demand
// instead of requiring a separate manual `sudo ./bake-all-ramdisks` step
// first -- see canSelfBakeRamdisk() there for when that's actually attempted.
std::string resolveBakeAllRamdisksPath();

// Resolves the path to the `iBoot32Patcher` binary (see CMakeLists.txt --
// built from the third_party/iBoot32Patcher submodule as its own executable,
// landing alongside blackb0x itself): $BLACKB0X_IBOOT32PATCHER if set,
// otherwise "iBoot32Patcher" alongside blackb0x's own executable, same
// resolution strategy as resolveGasterPath()/resolvePwnPath() above.
//
// It is a separate binary rather than a linked library ON PURPOSE: it is
// GPL-3.0-or-later and blackb0x declares no license, so linking it would
// make blackb0x a GPLv3 derivative. Patcher.cpp fork/execs it.
std::string resolveIBoot32PatcherPath();

// Resolves the loose .deb root: $BLACKB0X_DEBS_DIR if set, otherwise
// "Blackb0x/Debs" relative to the current working directory. bakeRamdisk()'s
// stageDebcache() (BakeRamdisk.cpp) copies exactly the subset
// scripts/build_deb_cache.py resolved from here into
// /blackb0x/var/mobile/.blackb0x/debs/ at bake time — not the whole
// (append-only, never-pruned) directory. The eventual goal is to source
// these from real Cydia repos directly rather than checking them in at all.
std::string resolveDebsPath();

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

// Resolves `relativePath` against the loose-legacy-asset root:
// $BLACKB0X_MISC_DIR if set, otherwise "Blackb0x/Misc" relative to the
// current working directory. Holds files that need in-place content
// splicing into the mounted volume at bake time rather than a plain
// `cp -a` (rc.boot) — see Blackb0x/Misc/README.md.
std::string resolveMiscPath(const std::string& relativePath);

// Resolves the entrypoint/ source directory (the freestanding ARMv6
// replacement for /sbin/launchd — see entrypoint/README.md):
// $BLACKB0X_ENTRYPOINT_DIR if set, otherwise "entrypoint" relative to the
// current working directory. bakeRamdisk() builds this fresh via podman on
// every bake rather than shipping a precompiled binary — see that
// directory's own README for why (freestanding, no libSystem, needs
// cctools-port's real Apple ld64 port; nothing to check in that a normal
// toolchain could reproduce).
std::string resolveEntrypointPath();

// Names bakeRamdisk()'s temporary decrypted intermediate file for a given
// input path (BakeRamdisk.cpp's own internal working file — the final
// output path is a caller-supplied parameter, not derived from this).
std::string decryptedDMGFor(const std::string& path);

// A stable content fingerprint of everything bakeRamdisk() merges in — the
// ramdisk/ overlay tree and the Debs/ .deb files — covering every regular
// file's relative path + content and every symlink's relative path +
// target, folded together (order-independent of directory traversal order:
// entries are sorted first). Not a cryptographic hash — this is purely a
// change-detection signal (see sumFileFor() below), not a security
// control, so a simple non-cryptographic hash is enough and avoids pulling
// in a crypto library just for this.
std::string ramdiskOverlayContentHash();

// The sidecar path bake-all-ramdisks writes ramdiskOverlayContentHash()'s
// value to after a successful bake, and Patcher::patchRamdisk() reads back
// to confirm a dist/ entry still matches the current ramdisk/ overlay
// before trusting it — catches "someone edited ramdisk/ and forgot to
// re-run bake-all-ramdisks" instead of silently shipping stale content.
std::string sumFileFor(const std::string& outputPath);
