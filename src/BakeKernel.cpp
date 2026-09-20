//
//  BakeKernel.cpp
//  Blackb0x
//
//  Standalone `bake-kernel`: decrypt + CBPatcher + re-encrypt ONE kernelcache
//  into <out>/KernelCache-<device>_<buildID>.
//
//  Split out of bake-firmware so the kernel patch pipeline can be run and
//  instrumented entirely on its own -- and with NO root, since patching a
//  kernelcache mounts nothing (unlike the ramdisk half). bake-firmware shells
//  out to this per tuple when the dist/ kernel isn't already present (see
//  BakeFirmware.cpp). AUTHORING-only: it links the same decrypt()/CBPatcher
//  path as bake-firmware's bootchain half (PatcherPatch.cpp), which blackb0x
//  itself never links.
//
//  Usage: bake-kernel --device <model> --build <buildID> [--out <dir>] [--force]
//

#include "IPSW.hpp"
#include "IPSWDownloader.hpp"
#include "Patcher.hpp"
#include "ResourcePath.hpp"

#include <xpwn/libxpwn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static const char* kProg = "bake-kernel";

static void usage() {
    fprintf(stderr,
            "usage: %s --device <model> --build <buildID> [--out <dir>] [--force]\n"
            "          [--uncompressed-size <N>]\n"
            "  Decrypts, patches (CBPatcher) and re-encrypts one kernelcache into\n"
            "  <out>/KernelCache-<device>_<buildID> (default <out> = dist). No root needed;\n"
            "  reuses the same IPSW download cache as bake-firmware.\n"
            "  --uncompressed-size <N>  DIAGNOSTIC: trim the decompressed kernel to N bytes\n"
            "                           (hex 0x.. or decimal) before re-compressing, so the\n"
            "                           complzss header reports that size. For testing\n"
            "                           whether iBoot accepts a kernelcache whose declared\n"
            "                           uncompressed size matches what it decodes.\n",
            kProg);
}

int main(int argc, char** argv) {
    std::string device;
    std::string build;
    std::string outDir = "dist";
    bool force = false;
    size_t uncompressedSize = 0;  // 0 = normal; else trim the kernel to this before re-compress

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--build") == 0 && i + 1 < argc) {
            build = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (strcmp(argv[i], "--uncompressed-size") == 0 && i + 1 < argc) {
            // accepts hex (0x...) or decimal
            uncompressedSize = (size_t)strtoull(argv[++i], nullptr, 0);
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
    const std::string dest = outDir + "/KernelCache" + tupleSuffix;

    std::error_code ec;
    if (!force && fs::exists(dest, ec)) {
        printf("%s: %s already exists (use --force to rebuild)\n", kProg, dest.c_str());
        return 0;
    }

    // xpwn byte-order globals, exactly as bake-firmware does up front -- the
    // decrypt()/lzss path in PatcherPatch.cpp depends on it.
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
    if (manifest->kernelCachePath.empty()) {
        fprintf(stderr, "%s: BuildManifest.plist has no KernelCache path\n", kProg);
        return 1;
    }

    Patcher patcher;
    patcher.loadKeysForDevice(device, build);
    patcher.setBuildIdentity(manifest->buildIdentity);
    patcher.setBuildID(manifest->realBuildID);

    const std::string localPath = workDir + "/" + fs::path(manifest->kernelCachePath).filename().string();
    if (!fs::exists(localPath, ec) &&
        !downloader.downloadComponent(manifest->kernelCachePath, localPath, nullptr)) {
        fprintf(stderr, "%s: failed to download kernelcache (%s)\n", kProg, manifest->kernelCachePath.c_str());
        return 1;
    }

    if (uncompressedSize > 0) {
        fprintf(stderr, "%s: --uncompressed-size 0x%llx: will trim the kernel to that before re-compress\n",
                kProg, (unsigned long long)uncompressedSize);
    }
    if (!patcher.patchKernel(localPath, manifest->productVersion, uncompressedSize)) {
        fprintf(stderr, "%s: patchKernel failed for %s %s\n", kProg, device.c_str(), build.c_str());
        return 1;
    }
    // patchKernel() only fires onComponentsReady (which clears outputs_) once
    // the WHOLE suite is present -- here just the kernel is, so it stays set.
    const PatchedComponents& out = patcher.components();
    if (!out.kernel) {
        fprintf(stderr, "%s: patchKernel reported success but produced no kernel output\n", kProg);
        return 1;
    }

    fs::create_directories(outDir, ec);
    fs::copy_file(*out.kernel, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fprintf(stderr, "%s: failed to publish %s -> %s: %s\n", kProg, out.kernel->c_str(), dest.c_str(),
                ec.message().c_str());
        return 1;
    }

    printf("%s: wrote %s (%ju bytes)\n", kProg, dest.c_str(), (uintmax_t)fs::file_size(dest, ec));
    return 0;
}
