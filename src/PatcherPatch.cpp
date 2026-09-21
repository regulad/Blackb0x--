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

    // POST-REPUBLISH GUARD. Re-runs iBoot's own img3 gate on the bytes we just
    // wrote (see img3ValidateFile() in Patcher.cpp for the predicate, where it
    // came from, and why bake-firmware's exit status was never evidence that
    // the image was usable). Checked on BOTH branches below even though the
    // Apple TV 3 branch publishes the RAW patched iBSS rather than this
    // re-wrapped one: the img3 writer is the same either way, so this is a
    // canary for every other component of the same bake.
    if (!img3ValidateFile(outPath, "patchiBSS: re-encrypted iBSS")) {
        fs::remove(patchedPath, ec);
        fs::remove(outPath, ec);
        return false;
    }

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
bool Patcher::patchiBEC(const std::string& path, const std::string& bootArgs,
                        const std::string& tetherBootArgs) {
    const FirmwareKeyPair* k = keyFor("iBEC");
    if (!k) {
        fprintf(stderr, "patchiBEC: no iBEC keys loaded\n");
        return false;
    }

    // Refuse rather than let patch_boot_args() strcpy() past the end of the
    // certificate boilerplate it relocates into. See
    // bootargs::kMaxBakedBootArgsLength for how that ceiling was measured --
    // it is NOT the 127-byte recovery-command budget, which applies to a
    // different channel entirely.
    for (const auto& [label, args] : {std::pair<const char*, const std::string&>{"boot-args", bootArgs},
                                      std::pair<const char*, const std::string&>{"tether boot-args",
                                                                                 tetherBootArgs}}) {
        if (args.empty()) {
            fprintf(stderr, "patchiBEC: %s is empty -- an iBEC with no baked boot-args cannot boot our "
                            "ramdisk (see Patcher.hpp's bootargs namespace)\n",
                    label);
            return false;
        }
        if (args.size() > bootargs::kMaxBakedBootArgsLength) {
            fprintf(stderr,
                    "patchiBEC: %s is %zu bytes and the baked limit is %zu. iBoot32Patcher would strcpy() "
                    "it over the embedded certificate blob with no bounds check, and iBoot would truncate "
                    "the kernel command line without reporting anything. Shorten it.\n",
                    label, args.size(), bootargs::kMaxBakedBootArgsLength);
            return false;
        }
    }

    std::string decPath = decryptedPathFor(path);
    std::string patchedPath = patchedPathFor(path);
    std::string outPath = outputPathFor(path);
    // The second image. Named off the same base so both land beside the
    // downloaded original in the IPSW cache; BakeIboot.cpp publishes them as
    // dist/iBEC-<tuple> and dist/iBECTether-<tuple>.
    std::string tetherPatchedPath = replaceExtension(path, "tether.patched");
    std::string tetherOutPath = replaceExtension(path, "tether");

    printf("Patching iBEC...\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    // -b <args>: the boot-args are COMPILED IN. This is not an optimization
    // or a belt-and-braces default -- it is the only channel that works.
    // AppleTV3,2's iBoot-1537.9.55 never reads the boot-args environment
    // variable on its kernel-boot path at all; the full disassembly evidence
    // (the single literal-pool xref in the env-var name table, the
    // kernel-boot literal pool, the absence of any MOVW/MOVT materialization)
    // is written up in Patcher.hpp's `bootargs` namespace comment, and the
    // original NSSpiral/Blackb0x -- the only configuration ever seen to boot
    // on real hardware -- baked its args the same way.
    //
    // This overturns the reasoning that used to sit here, which claimed
    // runtime `setenv boot-args` was "strictly better" than patch_boot_args()
    // and collapsed a former two-iBEC design into one. Both of its premises
    // were wrong in the same way: `setenv` does not reach the kernel on this
    // bootloader, so what it actually bought was a kernel booting with NONE
    // of amfi=0xff / cs_enforcement_disable=1 / amfi_get_out_of_my_way=1 --
    // which is exactly why an ad-hoc-signed entrypoint could never exec as
    // PID 1 while Apple's own signed launchd (--stock-ramdisk) booted fine.
    //
    // The old comment's ONE correct observation is kept, because it is still
    // a live hazard: patch_boot_args() is the most invasive patch in
    // iBoot32Patcher and the only one that can mis-apply without saying so.
    // It strcpy()s unbounded into the "Reliance on this certificate" string,
    // scans byte-by-byte for an IT instruction with no end-of-buffer guard,
    // and writes an 8-bit PC-relative immediate with no range check. The
    // mitigations are the length check above and, more importantly,
    // VERIFYING THE RESULT: decrypt the published iBEC and confirm the
    // injected string is present and that the null-string LDR now points at
    // the boot-args literal. A -b that silently fails reproduces this exact
    // bug class.
    //
    // -d (patch_debug_enabled) IS NOW PASSED. It used to be deliberately
    // withheld -- the original app passed debug="FALSE" too, and it was only
    // ever applied here by accident, because zzanehip's iBootPatcher() entry
    // point tested its RSA argument twice (fixed on our fork's branch; the
    // CLI never had the bug).
    //
    // It is the enabling half of `debug=0x14e` in the baked boot-args.
    // DB_LOG_PI_SCRN (0x100) makes the kernel render PANIC info onto the
    // framebuffer, which is the only panic-visibility channel this device has
    // -- -v covers ordinary printf, not the panic UI, and the UART is on
    // internal test points. The kernel gates `debug=` on
    // PE_i_can_has_debugger / the device-tree `debug-enabled` property, which
    // is 0 on a production-fused retail unit; patch_debug_enabled() rewrites
    // the `BL get_value_for_dtre_var("debug-enabled")` call site to
    // `MOVS R0,#1; MOVS R0,#1` so it answers 1 unconditionally.
    //
    // It was held back from the -b change on purpose -- landing a second
    // behavioural change alongside the boot-args-CHANNEL fix would have made
    // a hardware failure uninterpretable -- and lands on its own now that -b
    // is confirmed working on hardware (the kernel is accepted and boot
    // handoff succeeds with both --stock-ramdisk and --tether-boot).
    //
    // Failure is loud: iBoot32Patcher returns -1 and writes no output file if
    // find_dtre_get_value_bl_insn() cannot find its pattern, and runOne()
    // below treats a nonzero exit as fatal. That is necessary and not
    // sufficient -- this project has been burned three times by tools that
    // exited zero and produced a wrong artifact -- so the landing of this
    // patch is PROVEN by decoding the produced iBEC, exactly the way -b, -r
    // and -t were. See docs/HISTORY.md and Patcher.hpp's `bootargs`
    // namespace for the byte-level evidence.
    //
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
    const std::vector<std::string> iBECArgs = {"-r", "-k", "-t", "-d"};

    // TWO patched iBECs, deliberately, from the one decrypted input -- the
    // design the original app used and this port had collapsed. They differ
    // ONLY in the -b string; every other flag is identical.
    //
    // The collapse was sound ONLY while boot-args were a runtime `setenv`,
    // because then the two builds really were byte-identical and the
    // install-vs-tether distinction really was a property of the sender. Now
    // that the string is compiled in and wins unconditionally, one image
    // physically cannot serve both rd=md0 and rd=disk0s1s1 -- the mode is a
    // property of the binary again, so there have to be two binaries.
    //
    // The alternative considered and rejected: bake one iBEC and have
    // blackb0x re-bake on demand when the mode changes. That needs the IPSW,
    // keys/ and the GPL patch tools, none of which the jailbreak binary has
    // (see AGENTS.md's Patcher/PatcherPatch split), and it would turn a
    // diagnostic flag into a network round trip. Two files in dist/ cost a
    // few hundred KB and one extra iBoot32Patcher exec.
    std::error_code ec;
    auto runOne = [&](const char* what, const std::string& args, const std::string& toPatched,
                      const std::string& toOut) -> bool {
        std::vector<std::string> argv = {decPath, toPatched, "-b", args};
        argv.insert(argv.end(), iBECArgs.begin(), iBECArgs.end());
        int rc = runIBoot32Patcher(argv);
        if (rc != 0) {
            // Same reasoning as patchiBSS()'s own comment -- iBoot32Patcher
            // never writes its output file on failure (e.g.
            // patch_ticket_check()/patch_rsa_check()/patch_boot_args()
            // couldn't find their target instruction pattern in this
            // specific iBEC build), and decrypt()ing a missing/stale file
            // next would silently ship garbage that still fails verification
            // once sent, rather than failing cleanly here.
            fprintf(stderr, "patchiBEC: iBoot32Patcher failed for the %s image of %s (exit %d)\n", what,
                    path.c_str(), rc);
            return false;
        }
        decrypt(const_cast<char*>(toPatched.c_str()), const_cast<char*>(toOut.c_str()),
                const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
                const_cast<char*>(path.c_str()));
        if (!fs::exists(toOut, ec) || fs::file_size(toOut, ec) == 0) {
            fprintf(stderr, "patchiBEC: re-encrypt produced no usable %s image at %s\n", what,
                    toOut.c_str());
            return false;
        }
        // POST-REPUBLISH GUARD -- see img3ValidateFile() in Patcher.cpp. Both
        // iBECs get it: they are two separate republishes of the same input
        // differing only in the baked -b string, and nothing guarantees a
        // header defect lands on both.
        const std::string stage = std::string("patchiBEC: re-encrypted ") + what + " iBEC";
        if (!img3ValidateFile(toOut, stage.c_str())) return false;
        printf("patchiBEC: %s image baked with boot-args \"%s\"\n", what, args.c_str());
        return true;
    };

    const bool ok = runOne("install", bootArgs, patchedPath, outPath) &&
                    runOne("tether", tetherBootArgs, tetherPatchedPath, tetherOutPath);

    fs::remove(decPath, ec);
    fs::remove(patchedPath, ec);
    fs::remove(tetherPatchedPath, ec);
    if (!ok) return false;

    outputs_.iBEC = outPath;
    outputs_.iBECTether = tetherOutPath;

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

    // POST-REPUBLISH GUARD. This is the exact image, and the exact stage, that
    // shipped a stale sigCheckArea and cost a hardware cycle to "Kernelcache
    // image not valid" -- the kernelcache is the component that SHRANK when
    // repacked, which is the only reason the defect surfaced here first rather
    // than on the ramdisk, which carried it too. See img3ValidateFile() in
    // Patcher.cpp. A failure here is fatal: publishing a kernelcache iBoot
    // will reject only moves the failure to a DFU cycle on real hardware,
    // where it presents as an exploit problem rather than a header problem.
    if (!img3ValidateFile(outPath, "patchKernel: re-encrypted kernelcache")) {
        std::error_code vEc;
        fs::remove(decPath, vEc);
        fs::remove(outPath, vEc);
        return false;
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
