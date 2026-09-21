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

// ---------------------------------------------------------------------------
// WHICH RAMDISK THIS BAKE PRODUCES -- the three-way boot bisect
// ---------------------------------------------------------------------------
//
// There are three ways to assemble an image out of one pristine Apple
// RestoreRamdisk, and they exist to take apart ONE failure observed on real
// AppleTV3,2 hardware:
//
//   * `--stock-ramdisk` (Apple's image, untouched, never near this function)
//     -> THE APPLE LOGO APPEARS.
//   * `--tether-boot` (our patched kernel, NAND root, no ramdisk at all)
//     -> THE INSTALLED OS BOOTS NORMALLY.
//   * Our baked jailbreak ramdisk (Full, below) -> NOTHING AT ALL. No logo.
//
// That last result is itself the evidence. This bake leaves Apple's
// /sbin/launchd byte-identical and leaves com.apple.restored_external.plist
// (RunAtLoad) untouched, and restored_external is what draws the logo. So if
// the kernel had mounted our image and run launchd AT ALL, the logo would
// appear whether or not our own binary was ever accepted. No logo means the
// failure is BEFORE launchd: the kernel is not successfully rooting off our
// image. Two candidate causes, and today nothing distinguishes them:
//
//   * SIZE. Stock 12H1006 is ~16.6 MB raw; ours is ~44.3 MB. The 64 MiB
//     ceiling enforced at the end of bakeRamdisk() came from a real device
//     short-writing at exactly 0x4000000 during UPLOAD -- which is where the
//     upload breaks, not necessarily what the kernel can mount and root off.
//     There is no runtime check to ask: `ramdisk-size` does not exist on
//     32-bit iBoot at all (measured; see that ceiling's own comment).
//   * OUR REBUILD MACHINERY. decrypt -> resize up -> attach -> write ->
//     detach -> resize to minimum -> re-seal is a lot of steps, any one of
//     which could produce an image that no longer mounts.
//
// So two extra images are baked alongside the real one, and the four points
// together identify which. Each is a normal dist/ entry, sent by its own
// blackb0x flag (see CliOptions in Cli.hpp):
//
//   * DiagRepack -- THE PRISTINE RAMDISK, OPENED AND RE-SEALED WITH NOTHING
//     ADDED. Same decrypt, same attach, same resize-up/resize-to-minimum
//     cycle, same detach, same IMG3 re-seal as a real bake; the ONLY
//     difference is that no staging happens, so not one byte of content
//     changes. If this does not boot, size and content are both exonerated
//     and the rebuild machinery itself is the bug.
//   * DiagBinary -- the entrypoint binary, its LaunchDaemon plist and our
//     /mnt mountpoint, and NO /blackb0x OVERLAY AT ALL. A few hundred KB over
//     stock instead of ~28 MB over. This separates the overlay's SIZE from
//     the install mechanism: if this boots and Full does not, size is the
//     answer; if neither boots but DiagRepack does, the install mechanism is.
//
// Both diagnostic images go through the same 64 MiB ceiling and the same IMG3
// validation guard (img3ValidateFile(), Patcher.cpp) as everything else --
// they are ordinary bake output that happens to be missing content, not a
// side path with its own rules.
//
// Producing them is gated behind bake-firmware's --diagnostic-ramdisks
// (BakeFirmware.cpp), because two extra bakes cost real time on every run.
enum class RamdiskVariant {
    // The real thing: entrypoint + plist + /mnt + the whole /blackb0x overlay.
    Full,
    // Open and re-seal, add nothing.
    DiagRepack,
    // entrypoint + plist + /mnt, no overlay.
    DiagBinary,
};

// `path` is the downloaded, still-encrypted RestoreRamdisk; `key`/`iv` are
// its "RestoreRamdisk" entry from a keys/*.keys file.
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
// `variant` selects which of the three images this call produces (see
// RamdiskVariant above); it defaults to the real one, so the diagnostics are
// always an explicit request and never something a caller gets by accident.
// `entrypointBinaryPath` is unused for RamdiskVariant::DiagRepack, which
// installs nothing -- it is still required, so one caller can loop over all
// three variants without special-casing the arguments.
bool bakeRamdisk(const std::string& path, const std::string& key, const std::string& iv,
                  const std::string& productVersion, const std::string& outputPath,
                  const std::string& entrypointBinaryPath, bool& outSizeWarning,
                  RamdiskVariant variant = RamdiskVariant::Full);

// Builds entrypoint/'s armv7 binary by
// running `make` in entrypoint/ — see BakeRamdisk.cpp's own comment for full
// detail, and entrypoint/README.md for the one-time toolchain setup it
// assumes. Returns the built binary's path, or "" on failure.
std::string buildEntrypointBinary();
