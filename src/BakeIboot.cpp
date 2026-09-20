//
//  BakeIboot.cpp
//  Blackb0x
//
//  Standalone `bake-iboot`: decrypt + iBoot32Patcher + re-encrypt the iBSS and
//  BOTH iBECs into <out>/iBSS-<device>_<buildID>,
//  <out>/iBEC-<device>_<buildID> and <out>/iBECTether-<device>_<buildID>.
//
//  Two iBECs because the boot-args are BAKED IN (iBoot32Patcher's -b) and a
//  baked string wins unconditionally -- this bootloader never reads the
//  boot-args environment variable on its kernel-boot path, so `setenv
//  boot-args` cannot override one. The install path needs rd=md0 and
//  --tether-boot needs rd=disk0s1s1, which is two images. The full
//  disassembly evidence is in Patcher.hpp's `bootargs` namespace comment.
//
//  This is also the escape hatch for a custom boot-args string: --boot-args /
//  --tether-boot-args / --extra-boot-args re-bake an iBEC in seconds with no
//  root, which is what `blackb0x --extra-boot-args` now points users at.
//
//  Split out of bake-firmware for the same reasons as bake-kernel: the
//  bootloader patch path can be run and instrumented on its own, with NO root
//  (it mounts nothing). bake-firmware shells out to it per tuple when the dist/
//  iBSS/iBEC aren't already present. AUTHORING-only: it links the same
//  decrypt()/iBoot32Patcher path as bake-firmware's bootchain half
//  (PatcherPatch.cpp), which blackb0x itself never links.
//
//  Usage: bake-iboot --device <model> --build <buildID> [--out <dir>] [--force]
//                    [--boot-args <s>] [--tether-boot-args <s>]
//                    [--extra-boot-args <s>]
//

#include "IPSW.hpp"
#include "IPSWDownloader.hpp"
#include "Patcher.hpp"
#include "ResourcePath.hpp"

#include <xpwn/libxpwn.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static const char* kProg = "bake-iboot";

static void usage() {
    fprintf(stderr,
            "usage: %s --device <model> --build <buildID> [--out <dir>] [--force]\n"
            "           [--boot-args <s>] [--tether-boot-args <s>] [--extra-boot-args <s>]\n"
            "  Decrypts, patches (iBoot32Patcher) and re-encrypts iBSS + both iBECs into\n"
            "  <out>/iBSS-<device>_<buildID>, <out>/iBEC-<device>_<buildID> and\n"
            "  <out>/iBECTether-<device>_<buildID> (default <out> = dist).\n"
            "  No root needed; reuses bake-firmware's IPSW cache.\n"
            "\n"
            "  The boot-args are COMPILED INTO each iBEC (iBoot32Patcher -b). This\n"
            "  bootloader never reads the boot-args environment variable on its\n"
            "  kernel-boot path, so `setenv boot-args` at jailbreak time reaches\n"
            "  nothing -- re-baking here is the only way to change them. It takes\n"
            "  seconds and needs no privilege.\n"
            "    --boot-args <s>        replace the install image's args\n"
            "                           (default: %s)\n"
            "    --tether-boot-args <s> replace the --tether-boot image's args\n"
            "                           (default: %s)\n"
            "    --extra-boot-args <s>  append <s> to BOTH of whichever bases are in\n"
            "                           effect -- the ergonomic way to arm a\n"
            "                           `blackb0x.*` directive without retyping the\n"
            "                           AMFI/code-signing set\n"
            "  Each final string must be at most %zu bytes; see Patcher.hpp's bootargs\n"
            "  namespace for where that ceiling comes from. Over-long is an error, never\n"
            "  a truncation. Note there are TWO regimes, not one number: at or under 39\n"
            "  bytes iBoot32Patcher writes the string in place over iBoot's own default;\n"
            "  above that it relocates onto the certificate boilerplate, which is where\n"
            "  the %zu-byte ceiling comes from. Both defaults above are already in the\n"
            "  relocation regime (81 and 87 bytes) -- nothing useful fits under 39.\n",
            kProg, bootargs::kRamdiskBootArgs, bootargs::kTetherBootArgs,
            bootargs::kMaxBakedBootArgsLength, bootargs::kMaxBakedBootArgsLength);
}

int main(int argc, char** argv) {
    std::string device;
    std::string build;
    std::string outDir = "dist";
    bool force = false;
    std::string bootArgs = bootargs::kRamdiskBootArgs;
    std::string tetherBootArgs = bootargs::kTetherBootArgs;
    std::string extraBootArgs;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--build") == 0 && i + 1 < argc) {
            build = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (strcmp(argv[i], "--boot-args") == 0 && i + 1 < argc) {
            bootArgs = argv[++i];
        } else if (strcmp(argv[i], "--tether-boot-args") == 0 && i + 1 < argc) {
            tetherBootArgs = argv[++i];
        } else if (strcmp(argv[i], "--extra-boot-args") == 0 && i + 1 < argc) {
            extraBootArgs = argv[++i];
        } else {
            fprintf(stderr, "%s: unrecognized or incomplete argument: %s\n", kProg, argv[i]);
            usage();
            return 2;
        }
    }
    if (device.empty() || build.empty()) {
        usage();
        return 2;
    }

    if (!extraBootArgs.empty()) {
        // Control characters would land inside the kernel command line as
        // garbage, or terminate the C string patch_boot_args() strcpy()s
        // early. Neither is recoverable on a device with no console.
        for (unsigned char c : extraBootArgs) {
            if (c < 0x20 || c == 0x7f) {
                fprintf(stderr, "%s: --extra-boot-args contains a control character\n", kProg);
                return 2;
            }
        }
        bootArgs += ' ';
        bootArgs += extraBootArgs;
        tetherBootArgs += ' ';
        tetherBootArgs += extraBootArgs;
    }
    // patchiBEC() checks these too (the invariant must not depend on the
    // caller), but failing here costs no IPSW download.
    for (const auto& [label, s] : {std::pair<const char*, const std::string&>{"--boot-args", bootArgs},
                                   std::pair<const char*, const std::string&>{"--tether-boot-args",
                                                                              tetherBootArgs}}) {
        if (s.size() > bootargs::kMaxBakedBootArgsLength) {
            fprintf(stderr, "%s: the composed %s string is %zu bytes; the limit is %zu. Drop %zu byte(s).\n",
                    kProg, label, s.size(), bootargs::kMaxBakedBootArgsLength,
                    s.size() - bootargs::kMaxBakedBootArgsLength);
            return 2;
        }
    }

    const std::string tupleSuffix = std::string("-") + device + "_" + build;
    const std::string iBSSDest = outDir + "/iBSS" + tupleSuffix;
    const std::string iBECDest = outDir + "/" + bootargs::kIBECComponent + tupleSuffix;
    const std::string iBECTetherDest = outDir + "/" + bootargs::kIBECTetherComponent + tupleSuffix;

    std::error_code ec;
    // All THREE, or it is not done. A dist/ carrying only the install iBEC is
    // exactly the half-baked state that would make --tether-boot fail with a
    // missing-component error on a tree that looks complete.
    if (!force && fs::exists(iBSSDest, ec) && fs::exists(iBECDest, ec) && fs::exists(iBECTetherDest, ec)) {
        printf("%s: %s, %s and %s already exist (use --force to rebuild)\n", kProg, iBSSDest.c_str(),
               iBECDest.c_str(), iBECTetherDest.c_str());
        return 0;
    }

    TestByteOrder();

    IpswFetch fetcher;
    std::string firmwareURL = fetcher.firmwareURLForDevice(device, build);
    if (firmwareURL.empty()) {
        fprintf(stderr, "%s: no firmware URL from ipsw.me for %s %s\n", kProg, device.c_str(), build.c_str());
        return 1;
    }

    FragmentDownloader downloader(firmwareURL);
    if (!downloader.open()) {
        fprintf(stderr, "%s: could not open remote IPSW\n", kProg);
        return 1;
    }

    const std::string workDir = ipswDataRoot() + "/" + device + "/" + build;
    fs::create_directories(workDir, ec);

    const std::string manifestPath = workDir + "/BuildManifest.plist";
    if (!fs::exists(manifestPath, ec) &&
        !downloader.downloadComponent("BuildManifest.plist", manifestPath, nullptr)) {
        fprintf(stderr, "%s: failed to download BuildManifest.plist\n", kProg);
        return 1;
    }
    auto manifest = parseManifest(manifestPath);
    if (!manifest) {
        fprintf(stderr, "%s: failed to parse BuildManifest.plist\n", kProg);
        return 1;
    }
    if (manifest->iBSSPath.empty() || manifest->iBECPath.empty()) {
        fprintf(stderr, "%s: BuildManifest.plist has no iBSS/iBEC path\n", kProg);
        return 1;
    }

    Patcher patcher;
    patcher.loadKeysForDevice(device, build);
    patcher.setBuildIdentity(manifest->buildIdentity);
    patcher.setBuildID(manifest->realBuildID);

    // --- iBSS ---
    const std::string iBSSLocal = workDir + "/" + fs::path(manifest->iBSSPath).filename().string();
    if (!fs::exists(iBSSLocal, ec) && !downloader.downloadComponent(manifest->iBSSPath, iBSSLocal, nullptr)) {
        fprintf(stderr, "%s: failed to download iBSS (%s)\n", kProg, manifest->iBSSPath.c_str());
        return 1;
    }
    if (!patcher.patchiBSS(iBSSLocal)) {
        fprintf(stderr, "%s: patchiBSS failed for %s %s\n", kProg, device.c_str(), build.c_str());
        return 1;
    }

    // --- iBEC ---
    const std::string iBECLocal = workDir + "/" + fs::path(manifest->iBECPath).filename().string();
    if (!fs::exists(iBECLocal, ec) && !downloader.downloadComponent(manifest->iBECPath, iBECLocal, nullptr)) {
        fprintf(stderr, "%s: failed to download iBEC (%s)\n", kProg, manifest->iBECPath.c_str());
        return 1;
    }
    if (!patcher.patchiBEC(iBECLocal, bootArgs, tetherBootArgs)) {
        fprintf(stderr, "%s: patchiBEC failed for %s %s\n", kProg, device.c_str(), build.c_str());
        return 1;
    }

    // patchiBSS()/patchiBEC() only fire onComponentsReady (which clears
    // outputs_) once the WHOLE suite is present -- here just iBSS+iBECs are,
    // so they stay set.
    const PatchedComponents& out = patcher.components();
    if (!out.iBSS || !out.iBEC || !out.iBECTether) {
        fprintf(stderr, "%s: patch reported success but an iBSS/iBEC output is missing\n", kProg);
        return 1;
    }

    fs::create_directories(outDir, ec);
    auto publish = [&](const std::string& from, const std::string& to) -> bool {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fprintf(stderr, "%s: failed to publish %s -> %s: %s\n", kProg, from.c_str(), to.c_str(),
                    ec.message().c_str());
            return false;
        }
        // bake-firmware shells out to this tool and is itself run under sudo,
        // so hand the result back to the invoking user the same way its own
        // publish() does -- a dist/ that needs root to read or delete is the
        // exact surprise AGENTS.md says not to leave behind.
        chownToSudoCaller(to);
        return true;
    };
    if (!publish(*out.iBSS, iBSSDest)) return 1;
    if (!publish(*out.iBEC, iBECDest)) return 1;
    if (!publish(*out.iBECTether, iBECTetherDest)) return 1;

    printf("%s: wrote %s, %s and %s\n", kProg, iBSSDest.c_str(), iBECDest.c_str(), iBECTetherDest.c_str());
    printf("%s: install boot-args: %s\n", kProg, bootArgs.c_str());
    printf("%s: tether  boot-args: %s\n", kProg, tetherBootArgs.c_str());
    return 0;
}
