//
//  PatcherPatch.cpp
//  Blackb0x
//
//  AUTHORING-ONLY translation unit. Everything here decrypts and re-encrypts
//  firmware with xpwntool's decrypt(), and/or fork/execs the GPLv3 patch
//  tools (iBoot32Patcher, CBPatcher). It is compiled into bake-firmware ONLY,
//  never into the blackb0x jailbreak binary -- which is why blackb0x links
//  neither blackb0x_xpwntool nor xpwn (see CMakeLists.txt). The jailbreak
//  binary consumes bake-firmware's already-encrypted dist/ output verbatim
//  and does no decryption at all: iBoot32Patcher only ever defeats iBoot's
//  signature/ticket/KASLR checks, never its img3 parse or AES-decrypt path
//  (verified against the patcher source -- see docs/HISTORY.md), so every
//  component except the raw iBSS must reach iBoot still encrypted and
//  img3-wrapped. All of that wrapping happens here, ahead of time.
//
//  These are Patcher member functions defined in a second TU (the rest live
//  in Patcher.cpp); C++ permits a class's methods to be split across
//  translation units, and only bake-firmware links this one.
//

#include "Patcher.hpp"
#include "ResourcePath.hpp"

// decrypt() (AES-CBC decrypt / IMG3 re-encrypt) is our own first-party glue
// over third_party/xpwn's public API now, not the old vendored xpwntool.c.
#include "Img3Crypt.hpp"

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
    // NO -z. The -z patch (patch_lzss_check in our iBoot32Patcher fork) neuters
    // iBoot's kernelcache complzss size + adler32 checks, on the theory that the
    // 38-byte-short decode was harmless trailing padding. That theory is WRONG:
    // with -z applied, --tether-boot (which boots the NAND OS off the patched
    // kernel, no ramdisk) still fails, and iBoot's *Boot Failure Count*
    // increments while its *Panic Fail Count* does NOT -- i.e. iBoot never hands
    // control to the kernel (no panic), it aborts the boot because the kernel
    // image it produced is invalid. Suppressing the size/adler check just hid
    // the complaint; the kernelcache is genuinely malformed as iBoot receives
    // it. So -z is walked back to restore the check as our oracle while the real
    // cause (why iBoot decodes 38 bytes short of a stream that decodes fully
    // off-device) is tracked down. The fork keeps the -z capability, unused;
    // see docs/HISTORY.md.
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
bool Patcher::patchKernel(const std::string& path, const std::string& productVersion,
                          size_t uncompressedSizeOverride) {
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

    // [size instrumentation] The kernelcache boot failure ("Size mismatch from
    // lzss ..., should be ...") is a complzss length problem: iBoot decompresses
    // fewer bytes than the header claims. Log the byte size at each stage so a
    // bake pinpoints where the length diverges -- decPath is xpwn's
    // decompression of the original kernelcache; patchedPath is CBPatcher's
    // output; outPath is the re-encrypted result. See docs/HISTORY.md.
    fprintf(stderr, "patchKernel: [size] decrypted/decompressed kernel = %llu bytes (decPath)\n",
            (unsigned long long)fs::file_size(decPath, decEc));

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
    {
        std::error_code szEc;
        unsigned long long decSz = (unsigned long long)fs::file_size(decPath, szEc);
        unsigned long long patSz = (unsigned long long)fs::file_size(patchedPath, szEc);
        fprintf(stderr,
                "patchKernel: [size] CBPatcher output = %llu bytes (patchedPath); input was %llu -- %s\n",
                patSz, decSz,
                (patSz == decSz) ? "unchanged (in-place patch, as expected)"
                                 : "*** SIZE CHANGED -- CBPatcher altered the kernel length ***");
    }

    // DIAGNOSTIC: --uncompressed-size. Trim the decompressed patched kernel to
    // the requested length BEFORE re-compressing, so the complzss header the
    // re-encrypt writes reports that (smaller) length_uncompressed and the
    // stream decodes to exactly it -- a self-consistent kernelcache whose
    // declared size matches what iBoot actually decoded on hardware (dropping
    // trailing padding). Only shrinks, never grows.
    if (uncompressedSizeOverride > 0) {
        std::error_code trEc;
        auto cur = fs::file_size(patchedPath, trEc);
        if (!trEc && uncompressedSizeOverride < cur) {
            fs::resize_file(patchedPath, uncompressedSizeOverride, trEc);
            fprintf(stderr,
                    "patchKernel: [--uncompressed-size] trimmed patched kernel %llu -> %llu bytes before "
                    "re-compress\n",
                    (unsigned long long)cur, (unsigned long long)uncompressedSizeOverride);
        } else {
            fprintf(stderr,
                    "patchKernel: [--uncompressed-size] %llu not applied (kernel is %llu bytes; override must "
                    "be smaller and nonzero)\n",
                    (unsigned long long)uncompressedSizeOverride, (unsigned long long)cur);
        }
    }

    decrypt(const_cast<char*>(patchedPath.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));

    {
        std::error_code szEc;
        fprintf(stderr, "patchKernel: [size] re-encrypted kernelcache = %llu bytes (outPath)\n",
                (unsigned long long)fs::file_size(outPath, szEc));
    }

    std::error_code ec;
    // Debug: BLACKB0X_KEEP_KERNEL_TEMPS keeps decPath (the decrypted/
    // decompressed, UNPATCHED kernel fed into CBPatcher) for inspection --
    // used when diffing patcher output or the complzss pipeline. patchedPath
    // (CBPatcher's output) is already left in place regardless. Off by default.
    if (!getenv("BLACKB0X_KEEP_KERNEL_TEMPS")) {
        fs::remove(decPath, ec);
    }

    outputs_.kernel = outPath;
    checkPatching();
    return true;
}
