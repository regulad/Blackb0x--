//
//  Patcher.cpp
//  Blackb0x
//

#include "Patcher.hpp"
#include "ResourcePath.hpp"
#include "StockIBSSCrypt.hpp"

// NOTE: this TU links no xpwn/decrypt() code. Everything that decrypts or
// re-encrypts firmware -- patchiBSS()/patchiBEC()/patchKernel() and the
// GPLv3 patch-tool exec helpers -- lives in PatcherPatch.cpp, which is
// compiled ONLY into bake-firmware. The blackb0x jailbreak binary consumes
// bake-firmware's already-encrypted dist/ output verbatim and does no
// decryption at all. See PatcherPatch.cpp's header and docs/HISTORY.md.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// img3ValidateFile() -- iBoot's own "is this image well-formed" gate, re-run
// against every IMG3 this project writes
// ---------------------------------------------------------------------------
//
// WHAT THIS IS. The five checks below are not a plausible-looking invariant
// invented here. They are iBoot's, recovered by disassembling a real
// AppleTV3,2 12H1006 iBEC: the gate at 0x9ff18288 runs, in this order,
//
//     len          >= 20                 (the root header must fit)
//     magic        == '3gmI'
//     sizeNoPack   <= len - 20           (the body fits in the file)
//     sigCheckArea <= sizeNoPack         (the signed region fits in the body)
//     sizeNoPack + 20 <= fullSize        (the header agrees with the body)
//
// and returns error 0x16 ("malformed") the moment one fails -- BEFORE any
// signature, ticket or KBAG logic runs. That ordering is the whole reason
// this matters: no iBoot32Patcher patch defeats a check that happens before
// the checks iBoot32Patcher patches, and the device says nothing more useful
// than a generic "<Component> image not valid".
//
// The 20-byte root header is
//     char magic[4]; uint32 fullSize; uint32 sizeNoPack;
//     uint32 sigCheckArea; char ident[4];
// little-endian on disk, which is why the magic reads as '3gmI' and the
// ident of a kernelcache reads as 'lnrk'. xpwn spells sigCheckArea
// `shshOffset` and sizeNoPack `dataSize` (third_party/xpwn includes/xpwn/img3.h).
//
// THE SIXTH CHECK is ours, not iBoot's: walk the tag chain from offset 20 and
// require it to terminate EXACTLY at 20 + sizeNoPack. A chain that overruns
// or underruns means the root header and the body disagree about where the
// body ends -- precisely the class of defect this guard exists for, caught
// one layer earlier than iBoot would catch it.
//
// WHY IT EXISTS. A repacked 12H1006 kernelcache was rejected on hardware with
// "Kernelcache image not valid". Cause: xpwn's writeImg3Root() only recomputed
// sigCheckArea inside the branch that fires when an SHSH element is written,
// so an UNSIGNED image (TYPE DATA SEPO, no SHSH) kept the value cloned from
// the stock template -- which, once our payload came out 112 bytes smaller,
// pointed past the end of our own file. Fixed in third_party/xpwn, but the
// fix is not the point:
//
// THIS WAS THE THIRD DEFECT OF IDENTICAL CHARACTER in that one file in a
// week (the DATA element's 16-alignment on the write side, then the same on
// the read side, then this). Every one of them produced an artifact that was
// internally self-consistent, that every check the pipeline made accepted,
// and that was wrong. Checking bake-firmware's EXIT STATUS caught none of
// them -- the bake genuinely succeeded; it succeeded at writing a bad file.
// Each was found only by hexdumping the output after a failed hardware run,
// at the cost of a DFU cycle apiece. An assertion that re-reads the bytes we
// just wrote and re-runs the consumer's own predicate on them is the only
// check in this pipeline that could have failed instead.
//
// So: cheap (20 bytes plus a tag walk, no dependencies), applied to EVERY
// IMG3 republished anywhere in this project -- not just kernelcaches. The
// RestoreRamDisk carried the identical stale field and survived only because
// it happened to grow rather than shrink.

namespace {

uint32_t le32At(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The 4 bytes exactly as they sit in the file. Used for the magic, which is
// the one field whose on-disk byte ORDER is what the check is about --
// reversing it for readability there would print "expected '3gmI', got
// '3gmI'".
std::string fourCCRaw(const unsigned char* p) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        unsigned char c = p[i];
        s += (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    return s;
}

// A 4-byte on-disk tag/ident printed the way people write it ('krnl', not
// the 'lnrk' the little-endian bytes spell out).
std::string fourCC(const unsigned char* p) {
    std::string s;
    for (int i = 3; i >= 0; --i) {
        unsigned char c = p[i];
        s += (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    return s;
}

// The one failure path. Loud on purpose: a warning buried in a bake log is a
// silent death deferred to the next hardware run, which is exactly how the
// three bugs above got as far as they did. Everything the next person needs
// -- which file, which predicate, the actual value, the expected value, and
// where the predicate came from -- is here, so nobody has to go back to a
// disassembler to interpret it.
bool img3Reject(const std::string& path, const char* context, const char* predicate, const char* actual,
                const char* expected) {
    fprintf(stderr,
            "\n"
            "*** IMG3 VALIDATION FAILED -- refusing to publish this image ***\n"
            "    file:      %s\n"
            "    stage:     %s\n"
            "    predicate: %s\n"
            "    actual:    %s\n"
            "    expected:  %s\n"
            "\n"
            "    This is iBoot's OWN img3 gate, recovered by disassembling a real\n"
            "    AppleTV3,2 12H1006 iBEC (the check at 0x9ff18288). iBoot returns error\n"
            "    0x16 (malformed) here, before any signature, ticket or KBAG logic runs,\n"
            "    so no iBoot32Patcher patch can rescue this image and the device would\n"
            "    report only a generic \"image not valid\". The bake \"succeeding\" is not\n"
            "    evidence of anything -- see img3ValidateFile() in src/Patcher.cpp.\n"
            "\n",
            path.c_str(), context, predicate, actual, expected);
    return false;
}

} // namespace

bool img3FileHasMagic(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char magic[4] = {0, 0, 0, 0};
    const bool read4 = fread(magic, 1, sizeof(magic), f) == sizeof(magic);
    fclose(f);
    return read4 && memcmp(magic, "3gmI", 4) == 0;
}

bool img3ValidateFile(const std::string& path, const char* context) {
    if (!context) context = "(unspecified)";

    char actual[256];
    char expected[256];

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        snprintf(actual, sizeof(actual), "cannot open the file (%s)", strerror(errno));
        return img3Reject(path, context, "the image exists and is readable", actual,
                          "a file written by this step");
    }

    if (fseeko(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(actual, sizeof(actual), "cannot seek the file (%s)", strerror(errno));
        return img3Reject(path, context, "the image is a seekable regular file", actual, "a regular file");
    }
    const off_t lenOff = ftello(f);
    if (lenOff < 0) {
        fclose(f);
        return img3Reject(path, context, "the image has a knowable length", "ftello() failed",
                          "a regular file");
    }
    const uint64_t len = (uint64_t)lenOff;

    // 1. len >= 20
    if (len < 20) {
        fclose(f);
        snprintf(actual, sizeof(actual), "len = %llu bytes", (unsigned long long)len);
        snprintf(expected, sizeof(expected), "len >= 20 (the img3 root header alone is 20 bytes)");
        return img3Reject(path, context, "len >= 20", actual, expected);
    }

    unsigned char hdr[20];
    if (fseeko(f, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return img3Reject(path, context, "the 20-byte root header is readable", "short read", "20 bytes");
    }

    // 2. magic == '3gmI'
    if (memcmp(hdr, "3gmI", 4) != 0) {
        fclose(f);
        snprintf(actual, sizeof(actual), "the first 4 bytes are '%s' (%02x %02x %02x %02x)",
                 fourCCRaw(hdr).c_str(), hdr[0], hdr[1], hdr[2], hdr[3]);
        snprintf(expected, sizeof(expected), "'3gmI' (33 67 6d 49) -- the file is not an IMG3 at all");
        return img3Reject(path, context, "magic == '3gmI'", actual, expected);
    }

    const uint32_t fullSize = le32At(hdr + 4);
    const uint32_t sizeNoPack = le32At(hdr + 8);
    const uint32_t sigCheckArea = le32At(hdr + 12);
    const std::string ident = fourCC(hdr + 16);

    // 3. sizeNoPack <= len - 20
    if ((uint64_t)sizeNoPack > len - 20) {
        fclose(f);
        snprintf(actual, sizeof(actual), "sizeNoPack = %u (0x%x)", sizeNoPack, sizeNoPack);
        snprintf(expected, sizeof(expected), "<= len - 20 = %llu (0x%llx), len = %llu",
                 (unsigned long long)(len - 20), (unsigned long long)(len - 20), (unsigned long long)len);
        return img3Reject(path, context, "sizeNoPack <= len - 20", actual, expected);
    }

    // 4. sigCheckArea <= sizeNoPack -- the one that shipped. xpwn calls this
    //    field shshOffset; it was left holding the stock template's value on
    //    every unsigned image we repacked smaller than Apple's.
    if (sigCheckArea > sizeNoPack) {
        fclose(f);
        snprintf(actual, sizeof(actual), "sigCheckArea = %u (0x%x) -- %llu bytes past the end of the body",
                 sigCheckArea, sigCheckArea, (unsigned long long)((uint64_t)sigCheckArea - sizeNoPack));
        snprintf(expected, sizeof(expected), "<= sizeNoPack = %u (0x%x); Apple ships unsigned img3s with the two equal",
                 sizeNoPack, sizeNoPack);
        return img3Reject(path, context, "sigCheckArea <= sizeNoPack", actual, expected);
    }

    // 5. sizeNoPack + 20 <= fullSize
    if ((uint64_t)sizeNoPack + 20 > (uint64_t)fullSize) {
        fclose(f);
        snprintf(actual, sizeof(actual), "fullSize = %u (0x%x)", fullSize, fullSize);
        snprintf(expected, sizeof(expected), ">= sizeNoPack + 20 = %llu (0x%llx)",
                 (unsigned long long)((uint64_t)sizeNoPack + 20), (unsigned long long)((uint64_t)sizeNoPack + 20));
        return img3Reject(path, context, "sizeNoPack + 20 <= fullSize", actual, expected);
    }

    // 6. OURS, not iBoot's: the tag chain must terminate exactly at the end of
    //    the body the root header declares. Each element is a 12-byte header
    //    (magic, total size, data size) followed by its data and any padding,
    //    with the total size covering all three.
    const uint64_t bodyEnd = 20 + (uint64_t)sizeNoPack;
    uint64_t pos = 20;
    std::string chain;
    while (pos < bodyEnd) {
        if (pos + 12 > len) {
            fclose(f);
            snprintf(actual, sizeof(actual), "a tag header at offset %llu runs past the end of the file (len = %llu)",
                     (unsigned long long)pos, (unsigned long long)len);
            snprintf(expected, sizeof(expected), "every tag header to lie inside the file; chain so far: %s",
                     chain.empty() ? "(none)" : chain.c_str());
            return img3Reject(path, context, "the tag chain stays inside the file", actual, expected);
        }
        unsigned char tag[12];
        if (fseeko(f, (off_t)pos, SEEK_SET) != 0 || fread(tag, 1, sizeof(tag), f) != sizeof(tag)) {
            fclose(f);
            snprintf(actual, sizeof(actual), "short read of the tag header at offset %llu",
                     (unsigned long long)pos);
            return img3Reject(path, context, "every tag header is readable", actual, "12 readable bytes");
        }
        const std::string tagName = fourCC(tag);
        const uint32_t tagTotal = le32At(tag + 4);
        const uint32_t tagData = le32At(tag + 8);
        if (!chain.empty()) chain += " ";
        chain += tagName;

        if (tagTotal < 12 || (uint64_t)tagTotal < 12 + (uint64_t)tagData) {
            fclose(f);
            snprintf(actual, sizeof(actual), "tag '%s' at offset %llu has size = %u but dataSize = %u",
                     tagName.c_str(), (unsigned long long)pos, tagTotal, tagData);
            snprintf(expected, sizeof(expected), "size >= 12 + dataSize (12-byte tag header + payload + padding)");
            return img3Reject(path, context, "each tag's size covers its own header and data", actual, expected);
        }
        if (pos + (uint64_t)tagTotal > len) {
            fclose(f);
            snprintf(actual, sizeof(actual), "tag '%s' at offset %llu claims %u bytes, ending at %llu",
                     tagName.c_str(), (unsigned long long)pos, tagTotal,
                     (unsigned long long)(pos + tagTotal));
            snprintf(expected, sizeof(expected), "to end at or before len = %llu", (unsigned long long)len);
            return img3Reject(path, context, "the tag chain stays inside the file", actual, expected);
        }
        pos += tagTotal;
    }
    fclose(f);

    if (pos != bodyEnd) {
        snprintf(actual, sizeof(actual), "the tag chain ends at %llu (%s by %llu bytes); chain: %s",
                 (unsigned long long)pos, (pos > bodyEnd) ? "overruns" : "underruns",
                 (unsigned long long)(pos > bodyEnd ? pos - bodyEnd : bodyEnd - pos), chain.c_str());
        snprintf(expected, sizeof(expected), "to end at 20 + sizeNoPack = %llu (sizeNoPack = %u)",
                 (unsigned long long)bodyEnd, sizeNoPack);
        return img3Reject(path, context, "the tag chain terminates exactly at 20 + sizeNoPack (ours, not iBoot's)",
                          actual, expected);
    }

    printf("img3: %s OK (%s: ident '%s', len %llu, fullSize %u, sizeNoPack %u, sigCheckArea %u, tags: %s)\n",
           fs::path(path).filename().string().c_str(), context, ident.c_str(), (unsigned long long)len, fullSize,
           sizeNoPack, sigCheckArea, chain.c_str());
    return true;
}

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
bool Patcher::useStockIBSS(const std::string& path, bool stockSecurerom) {
    // A5 (AppleTV3,x) with no --stock-securerom is the one delivery route
    // that cannot run an encrypted iBSS: checkm8's boot_client()
    // (DeviceManager.cpp) uploads the img3 DATA payload directly as code, so
    // it needs plaintext. Every OTHER route loads it through a real (or
    // pwned) SecureROM that AES-decrypts the img3 itself -- --stock-securerom
    // (standard DFU, un-pwned SecureROM) and AppleTV2,1/A4 (irecv_send_file to
    // a limera1n/SHAtter-pwned SecureROM) both want the encrypted original,
    // untouched, so its signature/KBAG stay intact.
    //
    // So decrypt here -- and ONLY here -- for the boot_client route, using the
    // local .keys entry blackb0x already loaded. This is the deliberate,
    // narrow exception to "blackb0x does no decryption": the same reason the
    // BAKED iBSS is the one dist/ component published decrypted rather than
    // re-encrypted (see PatcherPatch.cpp's patchiBSS()). decryptStockIBSS()
    // uses wolfSSL, not xpwn, so no GPL code reaches blackb0x's link line
    // (StockIBSSCrypt.cpp). Matches the design the CliOptions::stockRecovery
    // comment already documented for this path.
    const bool isA5 = deviceModel_.rfind("AppleTV3", 0) == 0;
    if (isA5 && !stockSecurerom) {
        const FirmwareKeyPair* k = keyFor("iBSS");
        if (!k || k->key.empty() || k->iv.empty()) {
            fprintf(stderr,
                    "useStockIBSS: no iBSS decryption keys loaded for %s %s -- the checkm8 boot_client() route "
                    "uploads the img3 DATA as code and cannot run an encrypted iBSS. Drop a real keys/%s/"
                    "%s_%s.keys with an iBSS entry, or add --stock-securerom to route through a real "
                    "SecureROM (which decrypts the img3 itself and needs no local key).\n",
                    deviceModel_.c_str(), buildID_.c_str(), deviceModel_.c_str(), deviceModel_.c_str(),
                    buildID_.c_str());
            return false;
        }
        const std::string decPath = path + ".dec";
        if (!decryptStockIBSS(path, decPath, k->key, k->iv)) {
            fprintf(stderr, "useStockIBSS: failed to decrypt the stock iBSS at %s\n", path.c_str());
            return false;
        }
        fprintf(stderr,
                "--stock-recovery: decrypted Apple's stock iBSS for the checkm8 boot_client() route (it "
                "uploads the DATA payload as code, so it needs plaintext); no patches applied.\n");
        outputs_.iBSS = decPath;
        checkPatching();
        return true;
    }

    fprintf(stderr,
            "--stock-recovery: sending the original downloaded iBSS untouched (still encrypted, still "
            "img3-wrapped) -- %s decrypts the img3 itself.\n",
            stockSecurerom ? "the real SecureROM (standard DFU)" : "the pwned SecureROM");
    outputs_.iBSS = path;
    checkPatching();
    return true;
}


// See Patcher.hpp's own comment. Unlike patchiBEC(), this produces ONE image
// and leaves outputs_.iBECTether unset -- there is no install-vs-tether
// distinction to make here, because nothing is patched and Apple's own iBEC
// carries Apple's own compiled-in boot-args either way. A --stock-recovery
// run therefore cannot tether-boot meaningfully; that is inherent to sending
// an unmodified bootloader, not a gap in this function.
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

// See Patcher.hpp's own comment, and BakeRamdisk.hpp's RamdiskVariant for what
// each of the two images actually isolates. Sent exactly like the real baked
// ramdisk -- still encrypted, still img3-wrapped, straight out of dist/ -- so
// the ONLY difference between this and a normal jailbreak run is which of the
// three images the kernel is handed. That is what makes the comparison mean
// something: everything else in the chain (checkm8, iBSS, iBEC, boot-args,
// kernelcache, DeviceTree, RestoreLogo) is bit-for-bit the same run.
bool Patcher::useDiagnosticRamdisk(const std::string& component) {
    const std::string diagDMG = "dist/" + component + "-" + deviceModel_ + "_" + buildID_ + ".dmg";
    if (!fs::exists(diagDMG)) {
        // Hard stop, same as patchRamdisk(). The distinctive failure here is
        // "the suite was baked, but without --diagnostic-ramdisks", which is
        // the DEFAULT -- so this is the expected first experience of the flag
        // and the message has to name the fix rather than the symptom.
        // ensureBakedFirmware() (Cli.cpp) cannot help either: it keys
        // "complete" off the Manifest index and the real ramdisk, neither of
        // which says anything about the diagnostics.
        fprintf(stderr,
                "blackb0x: PANIC: no diagnostic ramdisk for %s %s (%s doesn't exist).\n"
                "The diagnostic images are NOT baked by default -- they cost two extra ramdisk\n"
                "bakes per tuple. Produce them with:\n"
                "  sudo ./build/bake-firmware --only ramdisk --diagnostic-ramdisks \\\n"
                "                             --device %s --build %s\n"
                "Or pull a CI artifact new enough to contain them (CI always passes that flag;\n"
                "see .github/workflows/ci.yml).\n",
                deviceModel_.c_str(), buildID_.c_str(), diagDMG.c_str(), deviceModel_.c_str(),
                buildID_.c_str());
        std::exit(1);
    }

    fprintf(stderr,
            "DIAGNOSTIC ramdisk: sending %s instead of the real baked RestoreRamdisk. Everything else\n"
            "  about this run -- iBSS, iBEC, boot-args, kernelcache, DeviceTree -- is unchanged, so a\n"
            "  difference in behaviour is attributable to this image alone. The device will NOT be\n"
            "  jailbroken.\n",
            diagDMG.c_str());
    outputs_.ramdisk = diagDMG;
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
