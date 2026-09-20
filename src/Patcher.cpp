//
//  Patcher.cpp
//  Blackb0x
//

#include "Patcher.hpp"
#include "ResourcePath.hpp"

// NOTE: this TU links no xpwn/decrypt() code. Everything that decrypts or
// re-encrypts firmware -- patchiBSS()/patchiBEC()/patchKernel() and the
// GPLv3 patch-tool exec helpers -- lives in PatcherPatch.cpp, which is
// compiled ONLY into bake-firmware. The blackb0x jailbreak binary consumes
// bake-firmware's already-encrypted dist/ output verbatim and does no
// decryption at all. See PatcherPatch.cpp's header and docs/HISTORY.md.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

// See Patcher.hpp's own comment on why this is a free function, not a
// Patcher method -- patchRamdisk() below is just its first caller.
bool ramdiskBakeNeeded(const std::string& deviceModel, const std::string& buildID) {
    // Matches bake-firmware's own naming convention exactly (see
    // BakeFirmware.cpp) -- both sides need to agree on this without a
    // round trip, hence the plain, duplicated (not shared-header) format
    // string on each side.
    //
    // Existence is the whole check now. There used to be a .sum sidecar
    // carrying a content fingerprint of the ramdisk/ overlay, compared here
    // to catch "someone edited ramdisk/ and forgot to re-bake" -- that whole
    // system is gone (fragile, and it only ever guessed at staleness). Use
    // bake-firmware --force to rebuild an output that already exists.
    const std::string patchedDMG = "dist/RestoreRamDisk-" + deviceModel + "_" + buildID + ".dmg";
    return !fs::exists(patchedDMG);
}

// ---------------------------------------------------------------------------
// Patcher
// ---------------------------------------------------------------------------

Patcher::Patcher() {
    clearComponents();
    // No TestByteOrder() here any more: it only sets up xpwn's byte-order
    // globals for decrypt(), which this binary never calls. bake-firmware's
    // decrypt() path self-initializes -- xpwntool.c's decrypt() calls
    // init_libxpwn() (which calls TestByteOrder()) on every invocation, and
    // BakeFirmware.cpp also calls TestByteOrder() itself up front.
}


const FirmwareKeyPair* Patcher::keyFor(const std::string& imageName) const {
    auto it = keys_.find(imageName);
    return (it == keys_.end()) ? nullptr : &it->second;
}

void Patcher::loadKeysForDevice(const std::string& deviceID, const std::string& buildID) {
    keys_ = fetcher_.keysForDevice(deviceID, buildID);
    deviceModel_ = deviceID;
    buildID_ = buildID;
}


// Sends the stock iBSS exactly as Apple shipped it: still encrypted, still
// img3-wrapped, with NO host-side decryption. The jailbreak binary does no
// decryption at all now (all crypto lives in bake-firmware), and it does not
// need to here: whatever loads this iBSS decrypts the img3 itself. On a
// --stock-securerom (un-pwned) run the standard-DFU SecureROM decrypts and
// verifies it; and iBoot32Patcher, on the pwned path, only ever defeats the
// signature check -- never the img3 parse or the AES-decrypt (verified
// against the patcher source, see docs/HISTORY.md).
//
// stockSecurerom no longer changes what gets sent -- either way it is the
// untouched original. The one thing this cannot do is hand the checkm8
// boot_client() upload path a *raw, decrypted* stock iBSS: that path uploads
// the img3 DATA payload directly as code, so it needs plaintext, and
// producing a raw iBSS requires decryption, which only bake-firmware does
// now. A raw iBSS is exactly what the normal jailbreak path already gets from
// dist/ -- iBSS being the one component sent decrypted is why it is the sole
// exception to "everything reaches the loader still encrypted".
bool Patcher::useStockIBSS(const std::string& path, bool /*stockSecurerom*/) {
    fprintf(stderr,
            "--stock-recovery: sending the original downloaded iBSS untouched (still encrypted, still "
            "img3-wrapped) -- no host-side decryption; whatever loads it decrypts the img3 itself.\n");
    outputs_.iBSS = path;
    checkPatching();
    return true;
}


// See Patcher.hpp's own comment. Same shape as patchiBEC() above: there is
// no downgrade-vs-boot distinction in the binary at all any more, so both
// PatchedComponents fields point at one file. Here that file is the plain
// decrypted iBEC, since no patching happens on this path.
bool Patcher::useStockIBEC(const std::string& path) {
    // See this method's own comment in Patcher.hpp -- always sent
    // untouched, unconditionally: whichever iBSS is now running
    // (useStockIBSS()'s own unpatched output) still has its RSA check
    // intact and verifies iBEC's img3 signature over the original
    // encrypted bytes, same as a real SecureROM does for iBSS.
    // Decrypting first invalidates that signature before it's ever
    // checked, regardless of --stock-securerom.
    fprintf(stderr,
            "--stock-recovery: sending the original downloaded iBEC untouched (still encrypted, still "
            "img3-wrapped) -- the stock iBSS that's now running still verifies its signature.\n");
    outputs_.iBEC = path;
    checkPatching();
    return true;
}


// Sends the stock kernelcache exactly as Apple shipped it: still encrypted,
// still img3-wrapped, with NO host-side decryption. iBoot always AES-decrypts
// the img3 itself via its KBAG + hardware GID key; iBoot32Patcher defeats only
// the signature check, so a host-decrypted, unwrapped kernelcache would fail
// iBoot's img3 parse before the patched check is ever reached. stockRecovery
// no longer changes anything here -- the untouched original is correct whether
// blackb0x's own patched iBEC or a stock iBEC is what loads it.
bool Patcher::useStockKernel(const std::string& path, bool /*stockRecovery*/) {
    fprintf(stderr,
            "--stock-firmware: sending the original downloaded kernelcache untouched (still encrypted, still "
            "img3-wrapped) -- no host-side decryption; iBoot decrypts the img3 itself.\n");
    outputs_.kernel = path;
    checkPatching();
    return true;
}

// The actual mount/merge/decrypt logic (and the design-history comment
// explaining why an in-process, no-mount approach isn't viable) lives in
// BakeRamdisk.cpp now — see bakeRamdisk()'s header comment there for the
// full investigation. blackb0x never invokes it itself: baking is a
// separate one-time-per-firmware step done in bulk by bake-firmware
// (BakeFirmware.cpp), not something this tool does on every run. That's
// possible because the overlay content is fully static (no per-device
// secrets get baked in — see BakeRamdisk.cpp), so the same patched output
// is valid for every device on a given firmware build. This just checks
// whether bake-firmware has already produced the dist/ entry this firmware
// needs, and tells the user to run it if not.
bool Patcher::patchRamdisk() {
    const FirmwareKeyPair* k = keyFor("RestoreRamdisk");
    if (!k) {
        fprintf(stderr, "patchRamdisk: no RestoreRamdisk keys loaded\n");
        return false;
    }

    const std::string patchedDMG = "dist/RestoreRamDisk-" + deviceModel_ + "_" + buildID_ + ".dmg";
    if (!fs::exists(patchedDMG)) {
        // dist/ may have *something* in it (or, since Cli.cpp's runCli() can
        // now self-bake on demand, may still be entirely empty at this
        // point) — either way, not this specific device+firmware, and
        // Cli.cpp's downloadAndPatchComponents() already tried a background
        // bake-firmware run for exactly this combination.
        // That's not a recoverable "try the next component" failure the way
        // a flaky download is: there is no ramdisk to send this device no
        // matter what else this run does, so stop hard here instead of
        // limping on to "Not all required components patched successfully",
        // which would leave the real cause one level removed from what's
        // actually printed.
        fprintf(stderr,
                "blackb0x: PANIC: no baked ramdisk for %s %s (%s doesn't exist).\n"
                "Bake it by re-running:\n"
                "  ./bake-firmware --only ramdisk --device %s --build %s\n"
                "Or drop the filters to bake every known combination at once.\n",
                deviceModel_.c_str(), buildID_.c_str(), patchedDMG.c_str(),
                deviceModel_.c_str(), buildID_.c_str());
        std::exit(1);
    }

    // There is no staleness check here any more. The .sum sidecar that used
    // to carry a fingerprint of the overlay content -- compared here to
    // catch "someone edited it and forgot to re-bake" -- is gone along with
    // the rest of that system. Existence is the gate; if this dist/ entry is
    // out of date, re-run `./bake-firmware --only ramdisk --force`.

    outputs_.ramdisk = patchedDMG;
    checkPatching();
    return true;
}

// Sends the stock RestoreRamdisk exactly as Apple shipped it: still
// encrypted, still img3-wrapped, with NO host-side decryption. Same reasoning
// as useStockKernel()/useStockIBEC(): iBoot decrypts the img3 itself via its
// KBAG + hardware GID key, and iBoot32Patcher defeats only the signature
// check, never the img3 parse/decrypt -- so a host-decrypted, unwrapped HFS+
// image is a headerless payload iBoot cannot load, which is exactly why the
// old decrypt-on-stock path never booted. stockRecovery no longer changes
// what gets sent. This is a diagnostic that skips the blackb0x-patched dist/
// ramdisk and entrypoint.c entirely, to isolate whether a boot failure is in
// blackb0x's own ramdisk baking or earlier in the chain.
bool Patcher::useStockRamdisk(const std::string& path, bool /*stockRecovery*/) {
    fprintf(stderr,
            "--stock-ramdisk/--stock-firmware: sending the original downloaded RestoreRamdisk untouched "
            "(still encrypted, still img3-wrapped) -- no host-side decryption; iBoot decrypts the img3 "
            "itself.\n");
    outputs_.ramdisk = path;
    checkPatching();
    return true;
}

void Patcher::setBakedIBSSPath(const std::string& path) {
    outputs_.iBSS = path;
    checkPatching();
}

void Patcher::setBakedIBECPath(const std::string& path) {
    outputs_.iBEC = path;
    checkPatching();
}

void Patcher::setBakedKernelPath(const std::string& path) {
    outputs_.kernel = path;
    checkPatching();
}

void Patcher::setBakedRamdiskPath(const std::string& path) {
    outputs_.ramdisk = path;
    checkPatching();
}

void Patcher::setDeviceTreePath(const std::string& path) {
    outputs_.deviceTree = path;
    checkPatching();
}

void Patcher::setRestoreLogoPath(const std::string& path) {
    outputs_.restoreLogo = path;
    checkPatching();
}

void Patcher::addLoadedByIBootComponent(const std::string& name, const std::string& path) {
    outputs_.loadedByIBoot.emplace_back(name, path);
    checkPatching();
}

void Patcher::checkPatching() {
    if (!outputs_.iBSS) return;

    if (!outputs_.iBEC) return;
    if (!outputs_.ramdisk) return;

    if (!outputs_.kernel) return;
    if (!outputs_.deviceTree) return;

    if (onComponentsReady) onComponentsReady(outputs_);
    clearComponents();
}

void Patcher::clearComponents() {
    outputs_ = PatchedComponents{};
}

std::vector<std::string> Patcher::missingRequiredComponents() const {
    std::vector<std::string> missing;
    if (!outputs_.iBSS) missing.push_back("iBSS");
    if (!outputs_.iBEC) missing.push_back("iBEC");
    if (!outputs_.ramdisk) missing.push_back("RestoreRamdisk");
    if (!outputs_.kernel) missing.push_back("KernelCache");
    if (!outputs_.deviceTree) missing.push_back("DeviceTree");
    return missing;
}
