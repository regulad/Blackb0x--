//
//  Patcher.cpp
//  Blackb0x
//

#include "Patcher.hpp"
#include "ResourcePath.hpp"

extern "C" {
#include <xpwntool.h>
#include <xpwn/libxpwn.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// The GPL patchers: invoked as separate programs, never linked
// ---------------------------------------------------------------------------

// Both patch tools this file drives are GPL-3.0 and blackb0x declares no
// license of its own, so linking either would make the whole binary a GPLv3
// derivative. Each is built as its own executable from its own submodule
// (see CMakeLists.txt) and run here as an independent program:
//
//   iBoot32Patcher  the bootloader patcher (iH8sn0w, 2013-2016)
//   CBPatcher       the kernel patcher (JonathanSeals; zzanehip's fork)
//
// That is a licensing requirement, not a style choice -- do not replace
// either with a direct iBootPatcher()/patch_kernel() call. CBPatcher in
// particular WAS linked in until this was noticed: it sat in the in-tree
// libraries directory with no LICENSE file alongside it, which is how a GPLv3
// static library ended up on blackb0x's link line unexamined.
//
// Deliberately NOT reusing DeviceManager.cpp's runLineBufferedSubprocess():
// that exists for long-running exploit tools that must stream progress out
// of a pipe while they work, and its whole stdbuf/timeout/D-state apparatus
// is dead weight here. This is a short-lived, file-in/file-out program whose
// output goes straight to blackb0x's own stdout/stderr (inherited, so its
// stdio is flushed by its own exit()), and whose only result that matters is
// the exit status.
//
// Returns the child's exit status, or -1 if it could not be run at all (both
// of which callers already treat as failure -- each tool returns 0 only on a
// fully-applied patch).
static int runPatchTool(const std::string& binary, const std::vector<std::string>& args) {

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(binary.c_str()));
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    int spawnErr = posix_spawn(&pid, binary.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawnErr != 0) {
        fprintf(stderr,
                "runPatchTool: could not run %s: %s. It is built alongside blackb0x -- see "
                "ResourcePath.hpp for the env var that overrides its location.\n",
                binary.c_str(), strerror(spawnErr));
        return -1;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "runPatchTool: waitpid() failed: %s\n", strerror(errno));
        return -1;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "runPatchTool: %s was killed by signal %d\n", binary.c_str(), WTERMSIG(status));
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int runIBoot32Patcher(const std::vector<std::string>& args) {
    return runPatchTool(resolveIBoot32PatcherPath(), args);
}

// CLI is a drop-in for the patch_kernel() call this used to link:
//   CBPatcher <infile> <outfile> <version> [--nosb]
// nukesb defaults to 1 upstream, which is exactly what the old in-tree
// wrapper hardcoded, so --nosb is deliberately never passed.
static int runCBPatcher(const std::vector<std::string>& args) {
    return runPatchTool(resolveCBPatcherPath(), args);
}

// ---------------------------------------------------------------------------
// Small path-string helpers (replace Patcher.mm's input:/decrypted:/patched:/
// output:/patchedDMG:/decryptedDMG:/downgrade:/preboot:/replaceExtension:with:)
// ---------------------------------------------------------------------------

static std::string replaceExtension(const std::string& path, const std::string& ext) {
    fs::path p(path);
    p.replace_extension(ext);
    return p.string();
}

static std::string withoutExtension(const std::string& path) {
    fs::path p(path);
    return p.replace_extension("").string();
}

static std::string decryptedPathFor(const std::string& path) { return replaceExtension(path, "dec"); }
static std::string patchedPathFor(const std::string& path) { return replaceExtension(path, "patched"); }
static std::string outputPathFor(const std::string& path) { return withoutExtension(path); }

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
    const std::string patchedDMG = "dist/" + deviceModel + "_" + buildID + "-Ramdisk.dmg";
    return !fs::exists(patchedDMG);
}

// ---------------------------------------------------------------------------
// Patcher
// ---------------------------------------------------------------------------

Patcher::Patcher() {
    clearComponents();
    TestByteOrder();
}

std::string Patcher::getRealVersion(const std::string& version) const {
    if (version.empty()) return "";

    std::istringstream ss(version);
    std::string part;
    std::vector<int> parts;
    while (std::getline(ss, part, '.')) {
        parts.push_back(atoi(part.c_str()));
    }
    int first = parts.size() > 0 ? parts[0] : 0;
    int second = parts.size() > 1 ? parts[1] : 0;
    int third = parts.size() > 2 ? parts[2] : 0;

    printf("Version: %d.%d.%d\n", first, second, third);
    switch (first) {
        case 4:
            return (second < 4) ? "4.0" : "5.0";
        case 5:
            return (second == 0) ? "5.0" : "6.0";
        case 6:
            return "7.0";
        case 7:
            return (second || third) ? "8.1" : "8.0";
        default:
            return "8.1";
    }
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

bool Patcher::patchiBSS(const std::string& path) {
    const FirmwareKeyPair* k = keyFor("iBSS");
    if (!k) {
        fprintf(stderr, "patchiBSS: no iBSS keys loaded\n");
        return false;
    }

    std::string decPath = decryptedPathFor(path);
    std::string patchedPath = patchedPathFor(path);
    std::string outPath = outputPathFor(path);

    printf("Patching iBSS...\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);
    // -r only: iBSS has no kernel-load routine, so boot-args/debug/KASLR
    // would be no-ops here even if asked for. Matches what the original
    // Patcher.mm asked for (RSA only).
    int patchResult = runIBoot32Patcher({decPath, patchedPath, "-r"});
    if (patchResult != 0) {
        // iBoot32Patcher never writes patchedPath on failure (e.g.
        // patch_rsa_check() couldn't find its target instruction pattern
        // in this specific iBSS build) -- same "decrypt() a nonexistent
        // file" heap corruption patchKernel() was already fixed for (see
        // its own comment), just never fixed here. Stop here instead of
        // silently shipping garbage/stale output that would still fail
        // signature verification once actually sent.
        fprintf(stderr, "patchiBSS: iBoot32Patcher failed for %s (exit %d)\n", path.c_str(), patchResult);
        std::error_code ec;
        fs::remove(decPath, ec);
        return false;
    }
    decrypt(const_cast<char*>(patchedPath.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));

    std::error_code ec;
    fs::remove(decPath, ec);

    if (path.find("j33i") != std::string::npos || path.find("j33ap") != std::string::npos) {
        // Apple TV 3
        fs::remove(outPath, ec);
        outputs_.iBSS = patchedPath;
    } else {
        // Apple TV 2
        fs::remove(patchedPath, ec);
        outputs_.iBSS = outPath;
    }

    checkPatching();
    return true;
}

// See Patcher.hpp's own comment. Deliberately minimal compared to
// patchiBSS() above: decrypt()s exactly the same way, but skips the
// iBootPatcher() call (and the AppleTV2-vs-AppleTV3 patchedPath/outPath
// filename-heuristic branch that only matters for picking which of
// patchiBSS()'s two *patched* outputs to keep) entirely -- there's only
// ever one output here, the plain decrypted file, since nothing about it
// differs by device model when unpatched.
bool Patcher::useStockIBSS(const std::string& path, bool stockSecurerom) {
    if (stockSecurerom) {
        // See this method's own comment in Patcher.hpp -- a genuinely
        // un-pwned device's SecureROM verifies the img3 signature over the
        // original encrypted bytes; decrypt()ing first (even without any
        // patch applied) would invalidate that signature before it's ever
        // checked.
        fprintf(stderr,
                "--stock-securerom: sending the original downloaded iBSS untouched (still encrypted, still "
                "img3-wrapped) -- decrypting it first would invalidate Apple's own signature before a real "
                "SecureROM ever gets to check it.\n");
        outputs_.iBSS = path;
        checkPatching();
        return true;
    }

    const FirmwareKeyPair* k = keyFor("iBSS");
    if (!k) {
        // --stock-recovery without --stock-securerom deliberately isn't
        // refused outright for this (see Cli.hpp's own comment on
        // stockRecovery) -- loadKeysForDevice() already resolved "latest"
        // to buildID_ and genuinely tried Blackb0x/ImageKeys/<device>_
        // <buildID_>.keys for it (see keysForDevice()'s own "cannot open"
        // line just above this one in the log); this build simply isn't
        // one blackb0x ships local decryption keys for (it only ever
        // pins kJailbreakTargetBuild's), not a bug. Drop a real .keys file
        // for this build at that exact path and rerun to get past this.
        fprintf(stderr,
                "useStockIBSS: no iBSS keys loaded for %s %s -- this build has no Blackb0x/ImageKeys/ entry, so "
                "the stock iBSS can't be decrypted for the checkm8/boot_client() upload path.\n",
                deviceModel_.c_str(), buildID_.c_str());
        return false;
    }

    std::string outPath = outputPathFor(path);

    fprintf(stderr,
            "--stock-recovery: decrypting the stock iBSS exactly as downloaded from Apple -- no boot-args/"
            "KASLR/ticket-check patches applied.\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    outputs_.iBSS = outPath;
    checkPatching();
    return true;
}

bool Patcher::patchiBEC(const std::string& path) {
    const FirmwareKeyPair* k = keyFor("iBEC");
    if (!k) {
        fprintf(stderr, "patchiBEC: no iBEC keys loaded\n");
        return false;
    }

    std::string decPath = decryptedPathFor(path);
    std::string patchedPath = patchedPathFor(path);
    // prebootPathFor()/downgradePathFor() are no longer used: patchiBEC()
    // produces one output, not a downgrade/boot pair (see below).
    std::string outPath = outputPathFor(path);

    printf("Patching iBEC...\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    // No -b: boot-args are NOT compiled into iBEC any more. They are set at
    // runtime with `setenv boot-args ...` + `saveenv` over the recovery
    // console, which is how the stock restore path (DeviceManager.cpp's
    // sendStockRestoreTail()) and real idevicerestore have always done it.
    //
    // Two reasons that is strictly better than patch_boot_args():
    //  1. The patch is the most invasive one in iBoot32Patcher and the only
    //     one that can mis-patch silently. Both boot-args strings blackb0x
    //     used were longer than iBoot's own "rd=md0 nand-enable-reformat=1
    //     -progress", which triggers its relocation path: it repoints the
    //     xref into the "Reliance on this certificate" string, strcpy()s
    //     there unbounded, scans byte-by-byte for an IT instruction with no
    //     end-of-buffer guard, and finally writes an 8-bit PC-relative
    //     immediate with no range check. Nothing about that reports failure
    //     if it lands wrong.
    //  2. `setenv` needs no iBEC rebuild to change, so the args can be
    //     matched to what the ramdisk actually does.
    //
    // No -d either: that patch forces `debug-enabled` to answer 1 forever,
    // and blackb0x never wanted it -- Patcher.mm passed debug="FALSE" too.
    // It was being applied anyway because zzanehip's iBootPatcher() entry
    // point tested its RSA argument twice (fixed on our fork's branch, but
    // the CLI never had the bug at all).
    // -r, -k and -t, all three unconditional.
    //
    // -k (patch_kaslr) disables iBoot's kernel-slide randomization. This is
    // what the original app did on every single iBEC patch
    // (Patcher.mm:248-249 passes kaslr="TRUE" both times, with no condition
    // on it), and the original is the only configuration this project has
    // ever seen boot. It was briefly dropped here on the reasoning that
    // nothing downstream reads the slide; that was wrong, and removing a
    // patch the known-working reference applied unconditionally is not a
    // change to make on an argument-from-first-principles basis.
    //
    // -t (patch_ticket_check) removes the APTicket requirement from
    // everything iBEC goes on to load, which the whole non-stock flow
    // depends on (see DeviceManager.hpp's sendStockRestoreTail() comment).
    // It is unconditional because there is no configuration in which a
    // patched iBEC wants the check left in: the question is only ever
    // whether a ticket gets SENT, and that is decided at send time, not
    // bake time -- sendStockTail() (Cli.cpp) is gated on --stock-recovery,
    // so --stock-firmware on its own already sends no ticket. A patched
    // iBEC that still enforced the check could never load blackb0x's own
    // patched kernel/ramdisk, ticket or not, since no real ticket can
    // authorize those.
    //
    // patchiBEC() therefore takes no flags/ticket parameters any more. The
    // old `flags` argument was already dead (an explicit (void)flags), and
    // the old `ticket=false` call site was a version heuristic carried over
    // from the original setIBECPath: -- if patch_ticket_check genuinely
    // cannot find its pattern on some early build, iBoot32Patcher exits
    // nonzero and this function fails loudly, which is a far better outcome
    // than silently shipping an iBEC that still wants a ticket.
    std::vector<std::string> iBECArgs = {"-r", "-k", "-t"};

    // ONE patched iBEC now, not two. The downgrade/boot pair only ever
    // differed by the boot-args compiled into each (args1 carried rd=md0,
    // args2 did not); every other patch flag was identical. With boot-args
    // moved to runtime `setenv`, the two builds are byte-for-byte the same
    // file, so producing both was pure duplication -- and worse, it made the
    // downgrade-vs-boot distinction look like a property of the binary when
    // it is really a property of which boot-args the sender sets.
    //
    // Both PatchedComponents fields therefore point at the same output, in
    // exactly the way useStockIBEC() below already does.
    std::vector<std::string> patchedArgs = {decPath, patchedPath};
    patchedArgs.insert(patchedArgs.end(), iBECArgs.begin(), iBECArgs.end());

    int patchedResult = runIBoot32Patcher(patchedArgs);
    if (patchedResult != 0) {
        // Same reasoning as patchiBSS()'s own comment -- iBoot32Patcher
        // never writes its output file on failure (e.g. patch_ticket_check()/
        // patch_rsa_check() couldn't find their target instruction pattern
        // in this specific iBEC build), and decrypt()ing a missing/stale
        // file next would silently ship garbage that still fails
        // verification once sent, rather than failing cleanly here.
        fprintf(stderr, "patchiBEC: iBoot32Patcher failed for %s (exit %d)\n", path.c_str(), patchedResult);
        std::error_code ec;
        fs::remove(decPath, ec);
        return false;
    }

    decrypt(const_cast<char*>(patchedPath.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));

    std::error_code ec;
    fs::remove(decPath, ec);
    fs::remove(patchedPath, ec);

    outputs_.iBEC = outPath;

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

bool Patcher::patchKernel(const std::string& path, const std::string& productVersion) {
    const FirmwareKeyPair* k = keyFor("Kernelcache");
    if (!k) {
        fprintf(stderr, "patchKernel: no Kernelcache keys loaded\n");
        return false;
    }

    std::string decPath = decryptedPathFor(path);
    std::string patchedPath = patchedPathFor(path);
    std::string outPath = outputPathFor(path);

    printf("Patching kernelcache...\n");

    // productVersion comes straight from BuildManifest.plist's own
    // ProductVersion (e.g. "6.1.3") — NOT derived from `path`. The original
    // versionString(path) helper (deleted) assumed a single flat
    // "<device>_<version>" directory name, matching neither this port's own
    // actual workDir layout (Cli.cpp's downloadAndPatchComponents():
    // ipswDataRoot()/deviceModel/buildID, two nested segments, no version
    // anywhere in the path at all) nor any real value in the manifest —
    // confirmed directly: it silently produced an empty string every time,
    // which CBPatcher (see below) correctly rejected as an unsupported
    // "iOS 0" — but the caller never checked *that* either, so it fell
    // through to decrypt()'ing a kernelcache.patched file that CBPatcher
    // never actually wrote, corrupting the heap in third_party/xpwn's own
    // decrypt()-with-template path instead of failing cleanly.
    //
    // Deliberately NOT run through getRealVersion() here, even though the
    // original Patcher.mm always did for every AppleTV path (verbatim same
    // switch/case, confirmed by reading Patcher.mm directly) — that
    // "real firmware version -> iOS-equivalent kernel-signature family"
    // bucketing maps our actual target (10B329a / "6.1.3") to "7.0", and
    // against a real AppleTV3,2 6.1.3 kernelcache, "7.0" finds ZERO
    // matching CBPatcher signatures ("[CBPatch] One or more patches not
    // found") while just passing productVersion straight through — which
    // CBPatcher's own kernPat()/kernPatOld() truncates to major
    // version 6 internally (versionInt < 8 -> versionFloat =
    // (float)versionInt, see CBPatcher.c) — finds and applies every
    // expected patch (tfp0, AMFI/memcmp bypass, sandbox policy, ...) and
    // reports success. Most likely explanation: this project's
    // libcbpatcher.a was lost and rebuilt from a third-party source fork
    // (see docs/HISTORY.md), whose "7.0"-family signatures don't exactly match
    // what shipped in the original 2020 binary getRealVersion() was tuned
    // against — not that the original bucketing logic was conceptually
    // wrong. getRealVersion() is kept (unused from here) as a reference in
    // case that signature drift ever gets fixed upstream; for the one real
    // firmware this tool has ever been tested against, the raw version
    // string is what's actually confirmed to work.
    std::string internalFirmware = productVersion;

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    // decrypt() reports failure only by printing (e.g. "error: cannot open
    // infile") -- it returns void, and it still leaves a zero-byte output
    // file behind. CBPatcher then SEGVs on that empty input.
    //
    // Found by bake-firmware's first full AppleTV3,2 sweep: 10B329a
    // patches fine, 10B144b dies here with exit 139 after iBSS and iBEC have
    // both already succeeded. The kernelcache downloads correctly (6006020
    // bytes) and the .keys file does have a Kernelcache entry, so this is a
    // decrypt-stage failure on that specific build, not a missing key or a
    // failed download -- and without this check it is an unexplained crash
    // rather than one skipped firmware.
    //
    // Same class of hazard the second decrypt() below was already fixed for
    // (see patchResult's comment); the fix was never applied to this one.
    std::error_code decEc;
    if (!fs::exists(decPath, decEc) || fs::file_size(decPath, decEc) == 0) {
        fprintf(stderr,
                "patchKernel: decrypt() produced no usable output for %s (wrote %s). Refusing to hand an "
                "empty file to CBPatcher, which crashes on one. Check this build's Kernelcache key/iv.\n",
                path.c_str(), decPath.c_str());
        fs::remove(decPath, decEc);
        return false;
    }

    int patchResult = runCBPatcher({decPath, patchedPath, internalFirmware});
    if (patchResult != 0) {
        // CBPatcher never writes patchedPath on failure — the previous
        // version of this code called decrypt() on it anyway, which corrupted
        // the heap in xpwntool's own decrypt()-with-template path when handed
        // a nonexistent input file rather than failing cleanly. Stop here
        // instead.
        fprintf(stderr, "patchKernel: CBPatcher failed for productVersion=\"%s\" (resolved to \"%s\", exit %d)\n",
                productVersion.c_str(), internalFirmware.c_str(), patchResult);
        std::error_code ec;
        fs::remove(decPath, ec);
        return false;
    }
    decrypt(const_cast<char*>(patchedPath.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));

    std::error_code ec;
    fs::remove(decPath, ec);

    outputs_.kernel = outPath;
    checkPatching();
    return true;
}

// See Patcher.hpp's own comment. Same decrypt()-only pattern as
// useStockIBSS()/useStockIBEC()/useStockRamdisk() -- no CBPatcher
// call at all.
bool Patcher::useStockKernel(const std::string& path, bool stockRecovery) {
    if (stockRecovery) {
        // See this method's own comment in Patcher.hpp -- whichever iBEC
        // is running (useStockIBEC()'s own unpatched output, since that's
        // the only iBEC stockRecovery ever produces) still verifies the
        // kernelcache's img3 signature over the original encrypted bytes.
        fprintf(stderr,
                "--stock-firmware --stock-recovery: sending the original downloaded kernelcache untouched "
                "(still encrypted, still img3-wrapped) -- the stock iBEC that's now running still verifies "
                "its signature.\n");
        outputs_.kernel = path;
        checkPatching();
        return true;
    }

    const FirmwareKeyPair* k = keyFor("Kernelcache");
    if (!k) {
        fprintf(stderr, "useStockKernel: no Kernelcache keys loaded\n");
        return false;
    }

    std::string outPath = outputPathFor(path);

    fprintf(stderr,
            "--stock-firmware: decrypting the stock kernelcache exactly as downloaded from Apple -- "
            "no tfp0/AMFI/sandbox patches applied.\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    outputs_.kernel = outPath;
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

    const std::string patchedDMG = "dist/" + deviceModel_ + "_" + buildID_ + "-Ramdisk.dmg";
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
                "Either update the device to the latest firmware Apple currently signs (the\n"
                "one bake-firmware --signed-only would have picked up), or bake every known\n"
                "combination instead, including older/unsigned ones, by re-running:\n"
                "  ./bake-firmware --only ramdisk\n"
                "(without --signed-only)\n",
                deviceModel_.c_str(), buildID_.c_str(), patchedDMG.c_str());
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

// See Patcher.hpp's own comment on why this exists. Deliberately minimal
// compared to patchRamdisk()/bake-firmware's own bakeRamdisk(): no
// dist/ lookup, no HFS+/loop-mount work at all -- just
// decrypt()'s the freshly-downloaded RestoreRamdisk exactly the way
// patchiBSS()/patchiBEC() decrypt their own components, and sends that
// straight through. This is byte-for-byte what a real, unmodified Apple
// restore would send the device.
// Called for either --stock-ramdisk or --stock-firmware (see Cli.cpp's
// downloadAndPatchComponents()) -- the stockRecovery branch below is only
// ever actually reachable in practice alongside --stock-securerom (runCli()
// requires --stock-firmware for --stock-recovery, and the only reason to
// combine those two without also testing against a real, un-pwned
// SecureROM is rare/contrived), so that message names --stock-securerom
// specifically rather than a generic internal function name a user has
// no way to connect back to anything they typed. The other (non-
// stockRecovery) branch is reachable via --stock-ramdisk or
// --stock-firmware alone -- named generically since either could be why.
bool Patcher::useStockRamdisk(const std::string& path, bool stockRecovery) {
    if (stockRecovery) {
        // See this method's own comment in Patcher.hpp -- whichever iBEC
        // is running (useStockIBEC()'s own unpatched output, since that's
        // the only iBEC stockRecovery ever produces) still verifies the
        // ramdisk's img3 signature over the original encrypted bytes.
        fprintf(stderr,
                "--stock-securerom: sending the original downloaded RestoreRamdisk untouched (still encrypted, "
                "still img3-wrapped) -- the stock iBEC that's now running still verifies its signature.\n");
        outputs_.ramdisk = path;
        checkPatching();
        return true;
    }

    const FirmwareKeyPair* k = keyFor("RestoreRamdisk");
    if (!k) {
        fprintf(stderr, "useStockRamdisk: no RestoreRamdisk keys loaded\n");
        return false;
    }

    std::string outPath = outputPathFor(path);

    fprintf(stderr,
            "--stock-ramdisk/--stock-firmware: decrypting the stock RestoreRamdisk exactly as downloaded from "
            "Apple -- skipping the blackb0x-patched dist/ ramdisk and entrypoint.c entirely, for isolating "
            "whether a boot failure is in blackb0x's own ramdisk patching or earlier in the chain.\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    outputs_.ramdisk = outPath;
    checkPatching();
    return true;
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
