//
//  Patcher.hpp
//  Blackb0x
//
//  CLI port of Patcher.h/.mm. The class is split across two translation
//  units by whether the method decrypts firmware:
//    - Patcher.cpp holds the crypto-free half (baked-component loaders,
//      useStock* which now send Apple's original img3 untouched, the
//      dist/ existence check, and the component bookkeeping). It links no
//      xpwntool decrypt() and is compiled into BOTH binaries.
//    - PatcherPatch.cpp holds patchiBSS/patchiBEC/patchKernel: they
//      decrypt() (xpwntool.c) and fork/exec the two GPL patch tools
//      (iBoot32Patcher, CBPatcher) as separate binaries, and re-encrypt.
//      It is compiled into bake-firmware ONLY, so the blackb0x jailbreak
//      binary does no decryption at all -- it consumes bake-firmware's
//      already-encrypted dist/ output verbatim.
//
//  The ramdisk baking (the substantial decrypt/inject/re-encrypt step) lives
//  in BakeRamdisk.cpp now, also authoring-only. For the record it:
//    1. decrypt()s the ramdisk (Apple's own AES-CBC encryption, via
//       xpwntool.c).
//    2. Detects whether the decrypted image is UDIF-wrapped ("koly" trailer)
//       or already a raw HFS+ partition — confirmed empirically that older
//       (A4-era Apple TV 2/3) restore ramdisks decrypt straight to raw HFS+
//       with no UDIF wrapper at all, unlike the UDIF-wrapped root-filesystem
//       images third_party/xpwn's own ipsw-patch/main.c reference code
//       assumes. Only genuinely UDIF-wrapped images go through xpwn's
//       extractDmg() to unwrap to a raw HFS+ partition image first.
//    3. Builds the volume and injects every payload via `hdiutil`.
//    4. Writes the modified image back out, overwriting the decrypted file
//       in place — rewrapped via buildDmg() only if step 2 found it
//       UDIF-wrapped to begin with; otherwise written as raw bytes, matching
//       what decrypt() actually produced.
//    5. decrypt()s (re-encrypts) exactly as before.
//  Needs root, because step 3 preserves arbitrary file ownership (setuid
//  binaries, etc.).
//
//  HISTORY, because it explains a constraint that still applies: this step
//  used to have a second, Linux implementation that built a fresh volume with
//  `mkfs.hfsplus` and loop-mounted it via the kernel `hfsplus` driver. That
//  was the third design tried there — an in-memory xpwn Volume
//  (add_hfs()/grow_hfs()) and a from-scratch userspace HFS+ writer (libhfsp)
//  were both tried first and both hit real, reproducible data-corrupting bugs
//  under real firmware payloads, and growing the ORIGINAL volume in place was
//  tried and abandoned too. It is gone with Linux support, but the finding
//  that survives is that xpwn's and libhfsp's HFS+ WRITE paths are not
//  trustworthy for this — don't reach for them here. See patchRamdisk()'s own
//  comment in Patcher.cpp for the full investigation.
//
//  Note the loop-mount path was the VERIFIED one: it produced every dist/
//  ramdisk and the 29/29 patch sweep. The hdiutil path above has never run
//  against real hardware. See .claude/TODO.md item 4a.
//

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "IPSW.hpp"

// Whether dist/RestoreRamDisk-<deviceModel>_<buildID>.dmg needs a fresh bake --
// i.e. whether it exists at all. Factored out here so Cli.cpp's
// downloadAndPatchComponents() can ask the same question up front (before a
// device's own Patcher instance has even loaded keys for it) to decide
// whether to kick off a background bake-firmware run.
bool ramdiskBakeNeeded(const std::string& deviceModel, const std::string& buildID);

struct PatchedComponents {
    std::optional<std::string> iBSS;
    // One iBEC, not a downgrade/boot pair: the two only ever differed by
    // the boot-args compiled into them, and those are set at runtime now
    // (DeviceManager.cpp's sendKernelCache()). The tether-boot path that
    // was the only consumer of the second variant is gone -- see
    // docs/HISTORY.md.
    std::optional<std::string> iBEC;
    std::optional<std::string> kernel;
    std::optional<std::string> ramdisk;
    std::optional<std::string> deviceTree;
    // See setRestoreLogoPath()'s own comment -- unset whenever this
    // build's manifest doesn't list a RestoreLogo component at all.
    std::optional<std::string> restoreLogo;
    // See addLoadedByIBootComponent()'s own comment -- name -> local
    // downloaded path, one entry per IPSW.hpp's own
    // ManifestInfo::loadedByIBootComponents (usually empty).
    std::vector<std::pair<std::string, std::string>> loadedByIBoot;

    // --stock-securerom (Cli.hpp's CliOptions): the matching BuildManifest.plist
    // identity (IPSW.hpp's ManifestInfo::buildIdentity, passed through
    // unchanged) -- sendComponentsToDevice()/DeviceManager::sendiBSS() need
    // this to personalize iBSS with a real TSS-issued SHSH ticket. Unset
    // (nullptr) whenever stockSecurerom isn't in play.
    std::shared_ptr<void> buildIdentity;
    // IPSW.hpp's ManifestInfo::realBuildID, passed through unchanged --
    // personalizeIMG3Component() (Personalize.cpp) needs this alongside
    // buildIdentity above, to check against signedBuildsForDevice()
    // before ever sending a real TSS request.
    std::string buildID;
};

class Patcher {
public:
    Patcher();

    // Called once firmware keys are needed for a given device/build; loads
    // them and (re-)runs any patch step whose input path was already set.
    void loadKeysForDevice(const std::string& deviceID, const std::string& buildID);

    bool patchiBSS(const std::string& path);
    bool patchiBEC(const std::string& path);
    // uncompressedSizeOverride: DIAGNOSTIC (bake-kernel --uncompressed-size). If
    // nonzero and smaller than the decompressed patched kernel, trim the kernel
    // to that many bytes before re-compressing, so the resulting complzss header
    // reports that (smaller) length_uncompressed self-consistently. Used to test
    // whether iBoot accepts a kernelcache whose declared uncompressed size
    // matches what it actually decodes (dropping trailing padding). 0 = normal.
    bool patchKernel(const std::string& path, const std::string& productVersion,
                     size_t uncompressedSizeOverride = 0);
    // No path parameter (unlike patchiBSS()/patchiBEC()/patchKernel() above)
    // -- this never had one that actually did anything: it only ever looks
    // at dist/RestoreRamDisk-<deviceModel_>_<buildID_>.dmg, built from
    // loadKeysForDevice()/setBuildID()'s own member variables, not from any
    // argument. Cli.cpp's downloadAndPatchComponents() used to download
    // RestoreRamdisk from Apple first and pass its local path in here
    // unused -- that download has always been wasted work on this path and
    // is no longer done at all; see its own comment for what replaced it.
    bool patchRamdisk();

    // --stock-ramdisk (Cli.hpp's CliOptions): sends the RestoreRamdisk
    // exactly as Apple shipped it -- still encrypted, still img3-wrapped, no
    // host-side decryption, no /blackb0x merge, no entrypoint.c -- instead of
    // patchRamdisk()'s usual dist/ lookup. A diagnostic: if the device boots
    // this fine, the failure is in blackb0x's own ramdisk baking; if it fails
    // the same way, the failure is earlier in the chain (iBSS/iBEC patches,
    // kernelcache, devicetree, or the boot trigger itself).
    //
    // Sent untouched regardless of stockRecovery. This is the correction of a
    // wrong belief the earlier code held: that "with blackb0x's own patched
    // iBEC, RSA/ticket checks are bypassed and a decrypted, unwrapped ramdisk
    // is fine." It is not. iBoot32Patcher defeats ONLY iBoot's signature/
    // ticket/KASLR checks -- never its img3 parser or its AES-decrypt path
    // (verified against the patcher source; see docs/HISTORY.md). iBoot always
    // parses the img3 and decrypts the DATA tag itself via the KBAG + hardware
    // GID key, so a host-decrypted, unwrapped image is a headerless payload it
    // cannot load -- which is exactly why the old decrypt-on-stock path never
    // booted. The stockRecovery parameter is now vestigial.
    bool useStockRamdisk(const std::string& path, bool stockRecovery = false);

    // --stock-recovery (Cli.hpp's CliOptions): sends iBSS/iBEC exactly as
    // Apple shipped them -- still encrypted, still img3-wrapped, no
    // iBootPatcher() call, so no KASLR/ticket/RSA-check patches. checkm8/
    // pwnTool still runs beforehand for the device to accept any file at all;
    // this only changes which iBSS/iBEC content is uploaded once pwned.
    // REQUIRES --stock-firmware (Cli.cpp's runCli() refuses otherwise): the
    // resulting stock iBEC enforces real APTicket verification on whatever it
    // loads next, which blackb0x's own patched kernel/ramdisk can never
    // satisfy. If the fully-stock suite boots, the failure is in blackb0x's
    // own iBSS/iBEC patches; if it fails the same way, it is elsewhere.
    //
    // stockSecurerom is now immaterial to what is sent -- either way it is the
    // untouched original img3. There is no host decryption here any more (see
    // useStockRamdisk() above for why decrypting would break the boot). The
    // one thing this cannot produce is a raw, decrypted stock iBSS for the
    // checkm8 boot_client() upload path, which uploads the img3 DATA payload
    // directly as code and so needs plaintext -- producing that requires
    // decryption, which only bake-firmware does now, and a raw iBSS is exactly
    // what the normal jailbreak path already gets from dist/. iBSS being the
    // one component that reaches its loader decrypted is the sole exception to
    // "everything is sent still encrypted"; see Cli.hpp's stockRecovery notes.
    bool useStockIBSS(const std::string& path, bool stockSecurerom = false);
    // Sends the original downloaded iBEC untouched, unconditionally -- same
    // reasoning as useStockRamdisk()/useStockKernel(): whichever iBEC-loader
    // (a stock iBSS's intact RSA check, or a real SecureROM) runs next
    // decrypts the img3 itself, and host-decrypting first would strip the
    // img3 and hand it a payload it cannot load.
    bool useStockIBEC(const std::string& path);

    // --stock-firmware (Cli.hpp's CliOptions): the kernelcache half of the
    // same idea -- sends the kernelcache exactly as Apple shipped it (still
    // encrypted, still img3-wrapped, no host decryption), no patch_kernel()/
    // CBPatcher call (no tfp0, no AMFI/memcmp bypass, no sandbox patch).
    // Combined with useStockIBSS()/useStockIBEC()/useStockRamdisk() this sends
    // a completely unmodified firmware suite end to end -- devicetree is
    // always sent unmodified anyway (see setDeviceTreePath()). If a fully-stock
    // suite boots, checkm8 and the bootx trigger are confirmed and the failure
    // is in one of blackb0x's own patches; if it fails the same way, the
    // failure is somewhere checkm8/the boot trigger doesn't control (or this
    // device/firmware genuinely cannot complete this boot path regardless).
    //
    // Sent untouched regardless of stockRecovery -- iBoot decrypts the img3
    // itself via its KBAG + GID key in every case (see useStockRamdisk()'s own
    // comment). The stockRecovery parameter is now vestigial.
    bool useStockKernel(const std::string& path, bool stockRecovery = false);

    // Feed an already-baked component straight in, with no patching and no
    // decryption. blackb0x uses these for the normal jailbreak path: it
    // consumes bake-firmware's dist/ output rather than producing its own, so
    // it never calls patchiBSS()/patchiBEC()/patchKernel() and therefore never
    // fork/execs iBoot32Patcher or CBPatcher at all. Same shape as
    // setDeviceTreePath() below, which has always worked this way because
    // DeviceTree is sent unmodified.
    //
    // The patch* methods above stay for bake-firmware, which is the only thing
    // that patches now.
    void setBakedIBSSPath(const std::string& path);
    void setBakedIBECPath(const std::string& path);
    void setBakedKernelPath(const std::string& path);
    void setBakedRamdiskPath(const std::string& path);

    void setDeviceTreePath(const std::string& path);

    // Like setDeviceTreePath() above -- sent unmodified, no decrypt/patch
    // step, since it's just an image displayed during Recovery-mode
    // restore, not something iBoot-patchable. Only ever called when
    // IPSW.hpp's ManifestInfo::restoreLogoPath is non-empty (see
    // downloadAndPatchComponents() in Cli.cpp) -- not every build's
    // manifest lists a RestoreLogo component at all.
    void setRestoreLogoPath(const std::string& path);

    // Like setRestoreLogoPath() above -- sent unmodified, no decrypt/
    // patch step. Called once per entry in IPSW.hpp's own
    // ManifestInfo::loadedByIBootComponents (see that field's own
    // comment) -- name must match the manifest's own component key
    // exactly, since DeviceManager::sendStockRestoreTail() sends it
    // alongside the "firmware" command the same way real idevicerestore
    // does, and nothing here validates it further.
    void addLoadedByIBootComponent(const std::string& name, const std::string& path);

    // See PatchedComponents::buildIdentity's own comment.
    void setBuildIdentity(std::shared_ptr<void> identity) { outputs_.buildIdentity = std::move(identity); }
    // See PatchedComponents::buildID's own comment.
    void setBuildID(const std::string& buildID) { outputs_.buildID = buildID; }

    // Read-only view of whatever has been patched so far.
    //
    // Only meaningful for callers that deliberately produce an INCOMPLETE
    // suite: checkPatching() hands a complete set to onComponentsReady and
    // then clears outputs_, so a caller producing everything should use that
    // callback instead. bake-firmware (BakeFirmware.cpp) is the
    // one that needs this -- it patches the bootchain + kernel + devicetree
    // and never touches the ramdisk, so onComponentsReady can never fire for
    // it, and it still needs to know which individual outputs landed.
    const PatchedComponents& components() const { return outputs_; }


    // Called once every component checkPatching() requires is available
    // (replaces MainView's componentsReady:). Fired synchronously from
    // whichever patch*() call completes the last required component.
    std::function<void(const PatchedComponents&)> onComponentsReady;

    // Human-readable names of whichever components checkPatching() is
    // still waiting on -- empty once everything required is present.
    // Mirrors checkPatching()'s own exact requirements, so
    // downloadAndPatchComponents()
    // (Cli.cpp) can report specifically what's missing on failure instead
    // of a blanket "not all components patched successfully" with no
    // detail at all -- a silently-failed download or patch step
    // (wrong/missing keys, a 404 for a build blackb0x has no local
    // support for, etc.) used to surface this way with no indication of
    // which step actually failed or why.
    std::vector<std::string> missingRequiredComponents() const;

private:
    IpswFetch fetcher_;
    std::map<std::string, FirmwareKeyPair> keys_;
    PatchedComponents outputs_;
    // Set by loadKeysForDevice() — patchRamdisk() needs these to compute
    // which dist/RestoreRamDisk-<device>_<buildID>.dmg bake-firmware should
    // already have produced.
    std::string deviceModel_;
    std::string buildID_;

    std::string getRealVersion(const std::string& version) const;
    void checkPatching();
    void clearComponents();

    const FirmwareKeyPair* keyFor(const std::string& imageName) const;
};
