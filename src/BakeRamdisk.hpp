//
//  BakeRamdisk.hpp
//  Blackb0x
//
//  The core "merge Blackb0x/ramdisk/ into one downloaded RestoreRamdisk"
//  operation — the only piece of this tool that needs CAP_SYS_ADMIN/
//  CAP_CHOWN (loop-mounting a real HFS+ image; see BakeRamdisk.cpp's header
//  comment for why an in-process, no-mount approach isn't viable). A
//  library, not a binary of its own — bake-firmware (BakeFirmware.cpp)
//  is the only thing that calls this, once per known firmware.
//

#pragma once

#include <string>

// `path` is the downloaded, still-encrypted RestoreRamdisk; `key`/`iv` are
// its "RestoreRamdisk" entry from a Blackb0x/ImageKeys/*.keys file.
// `productVersion` is this firmware's own BuildManifest.plist ProductVersion
// (e.g. "6.1.3", "8.4.2" — see IPSW.hpp's ManifestInfo) — used to pick which
// per-firmware persistence payload gets staged under /blackb0x (see
// stageBlackb0xTree() in BakeRamdisk.cpp); this decision used to be made at
// runtime, on-device, by entrypoint.c itself, but the firmware a given
// ramdisk targets is already fully known at bake time, so there's nothing
// left to actually branch on once the device boots. `entrypointBinaryPath`
// is the already-built entrypoint binary (see buildEntrypointBinary()
// below) — identical for every firmware target, so the caller builds it
// exactly once and passes the same path into every bakeRamdisk() call
// rather than this function rebuilding it itself each time. Writes the
// finished, re-encrypted result to `outputPath`. Returns false on any
// failure (see stderr for which step) — a real failure here means every
// other firmware target sharing the same cached debcache result (see
// computeGlobalDebcacheOnce()) will fail identically, so callers should
// treat it as fatal to the whole batch, not just this one target.
// A finished ramdisk over the size limit (64 MiB by default) is a FAILURE:
// `false` is returned and the oversized output is removed, because a ramdisk
// that big cannot be uploaded and leaving it in dist/ would get it silently
// reused. `outSizeWarning` is only set when the limit was explicitly disabled
// with DEBUG_RAMDISK_LIMIT_MIB=-1 — that is a real success (`true` is
// returned) with something worth surfacing in a batch summary. See the limit's
// own comment in BakeRamdisk.cpp for the knob and why 64 MiB is not arbitrary.
bool bakeRamdisk(const std::string& path, const std::string& key, const std::string& iv,
                  const std::string& productVersion, const std::string& outputPath,
                  const std::string& entrypointBinaryPath, bool& outSizeWarning);

// Builds entrypoint/'s freestanding ARMv6 replacement for /sbin/launchd by
// running `make` in entrypoint/ — see BakeRamdisk.cpp's own comment for full
// detail, and entrypoint/README.md for the one-time toolchain setup it
// assumes. Returns the built binary's path, or "" on failure.
std::string buildEntrypointBinary();
