//
//  BakeIboot.cpp
//  Blackb0x
//
//  Standalone `bake-iboot`: decrypt + iBoot32Patcher + re-encrypt the iBSS and
//  iBEC into <out>/iBSS-<device>_<buildID> and <out>/iBEC-<device>_<buildID>.
//
//  Split out of bake-firmware for the same reasons as bake-kernel: the
//  bootloader patch path can be run and instrumented on its own, with NO root
//  (it mounts nothing). bake-firmware shells out to it per tuple when the dist/
//  iBSS/iBEC aren't already present. AUTHORING-only: it links the same
//  decrypt()/iBoot32Patcher path as bake-firmware's bootchain half
//  (PatcherPatch.cpp), which blackb0x itself never links.
//
//  Usage: bake-iboot --device <model> --build <buildID> [--out <dir>] [--force]
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
            "  Decrypts, patches (iBoot32Patcher) and re-encrypts iBSS + iBEC into\n"
            "  <out>/iBSS-<device>_<buildID> and <out>/iBEC-<device>_<buildID>\n"
            "  (default <out> = dist). No root needed; reuses bake-firmware's IPSW cache.\n",
            kProg);
}

int main(int argc, char** argv) {
    std::string device;
    std::string build;
    std::string outDir = "dist";
    bool force = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--build") == 0 && i + 1 < argc) {
            build = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0) {
            force = true;
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

    const std::string tupleSuffix = std::string("-") + device + "_" + build;
    const std::string iBSSDest = outDir + "/iBSS" + tupleSuffix;
    const std::string iBECDest = outDir + "/iBEC" + tupleSuffix;

    std::error_code ec;
    if (!force && fs::exists(iBSSDest, ec) && fs::exists(iBECDest, ec)) {
        printf("%s: %s and %s already exist (use --force to rebuild)\n", kProg, iBSSDest.c_str(),
               iBECDest.c_str());
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
    if (!patcher.patchiBEC(iBECLocal)) {
        fprintf(stderr, "%s: patchiBEC failed for %s %s\n", kProg, device.c_str(), build.c_str());
        return 1;
    }

    // patchiBSS()/patchiBEC() only fire onComponentsReady (which clears
    // outputs_) once the WHOLE suite is present -- here just iBSS+iBEC are, so
    // they stay set.
    const PatchedComponents& out = patcher.components();
    if (!out.iBSS || !out.iBEC) {
        fprintf(stderr, "%s: patch reported success but iBSS/iBEC output missing\n", kProg);
        return 1;
    }

    fs::create_directories(outDir, ec);
    fs::copy_file(*out.iBSS, iBSSDest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fprintf(stderr, "%s: failed to publish %s -> %s: %s\n", kProg, out.iBSS->c_str(), iBSSDest.c_str(),
                ec.message().c_str());
        return 1;
    }
    fs::copy_file(*out.iBEC, iBECDest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fprintf(stderr, "%s: failed to publish %s -> %s: %s\n", kProg, out.iBEC->c_str(), iBECDest.c_str(),
                ec.message().c_str());
        return 1;
    }

    printf("%s: wrote %s and %s\n", kProg, iBSSDest.c_str(), iBECDest.c_str());
    return 0;
}
