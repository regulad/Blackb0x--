//
//  BakeAllRamdisks.cpp
//  bake-all-ramdisks
//
//  Batch driver for bakeRamdisk() (BakeRamdisk.hpp/.cpp) — the only binary
//  that actually calls it. Walks every .keys file under Blackb0x/ImageKeys/
//  (i.e. every (device model, firmware build) combination this port has
//  decryption keys for), downloads that firmware's RestoreRamdisk component
//  and bakes it, writing results to dist/<device>_<buildID>-Ramdisk.dmg.
//
//  This is the one-time-per-firmware step described in BakeRamdisk.hpp's
//  header comment, done for every known firmware at once rather than one
//  at a time — blackb0x itself never runs this; Patcher::patchRamdisk()
//  just checks whether the dist/ entry it needs already exists.
//
//  An already-existing dist/ entry is skipped; pass --force to rebuild it.
//  There used to be a .sum sidecar per output holding a fingerprint of
//  Blackb0x/ramdisk/, so that editing the overlay automatically forced a
//  re-bake. That whole system is gone — it was fragile and only ever
//  approximated staleness — so after changing anything that affects baked
//  output, pass --force.
//
//  Usage: sudo ./bake-all-ramdisks [--signed-only] [--device <model>] [--build <buildID>] [--force]
//  (needs CAP_SYS_ADMIN/CAP_CHOWN, same as bakeRamdisk() itself — see its
//  header comment for why)
//
//  --signed-only restricts the run to builds ipsw.me still reports Apple as
//  actively signing for that device right now — typically just the latest
//  one or two per device (7 out of 92 known builds, checked live 2026-09-11),
//  which is what the vast majority of real devices will actually be on.
//
//  --device <model> restricts the run to just that one device (e.g.
//  "AppleTV3,2" — the exact ImageKeys/ directory name, case-sensitive),
//  every known build for it. Meant for iterating on ramdisk/Debs/ content
//  (kNeverStageDebs, the prebake blacklist, etc.) without paying for all
//  92 known (device, firmware) combinations on every single test run —
//  combine with --signed-only to narrow to just that device's currently-
//  signed build(s).
//
//  --build <buildID> restricts the run to just that one firmware build
//  (e.g. "12H606"), across whichever known device(s) have it. Same
//  rationale as --device — iterating without paying for every known
//  combination — and combines with --device (and --signed-only) to pin
//  the run down to exactly one (device, buildID) pair.
//

#include "BakeRamdisk.hpp"
#include "IPSW.hpp"
#include "IPSWDownloader.hpp"
#include "ResourcePath.hpp"

extern "C" {
#include <xpwn/libxpwn.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// Every (device, buildID) pair this port has a .keys file for — read
// straight from the directory structure (Blackb0x/ImageKeys/<device>/
// <device>_<buildID>.keys), not some separate hardcoded list, so this
// tracks ImageKeys/ automatically as devices/builds are added or removed.
static std::vector<std::pair<std::string, std::string>> knownFirmwareTargets() {
    std::vector<std::pair<std::string, std::string>> targets;
    std::string keysRoot = resolveImageKeyPath("");

    std::error_code deviceEc;
    for (const auto& deviceDir : fs::directory_iterator(keysRoot, deviceEc)) {
        if (deviceEc || !deviceDir.is_directory()) continue;
        std::string device = deviceDir.path().filename().string();
        std::string prefix = device + "_";

        std::error_code fileEc;
        for (const auto& entry : fs::directory_iterator(deviceDir.path(), fileEc)) {
            if (fileEc || entry.path().extension() != ".keys") continue;
            std::string stem = entry.path().stem().string();  // "<device>_<buildID>"
            if (stem.rfind(prefix, 0) != 0) continue;
            targets.emplace_back(device, stem.substr(prefix.size()));
        }
    }
    std::sort(targets.begin(), targets.end());
    return targets;
}

int main(int argc, char** argv) {
    bool signedOnly = false;
    bool force = false;
    std::string deviceFilter;
    std::string buildFilter;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--signed-only") == 0) {
            signedOnly = true;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (strcmp(argv[i], "--device") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bake-all-ramdisks: --device requires a value (e.g. --device AppleTV3,2)\n");
                return 2;
            }
            deviceFilter = argv[++i];
        } else if (strcmp(argv[i], "--build") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bake-all-ramdisks: --build requires a value (e.g. --build 12H606)\n");
                return 2;
            }
            buildFilter = argv[++i];
        } else {
            fprintf(stderr, "bake-all-ramdisks: unrecognized argument %s\n", argv[i]);
            fprintf(stderr,
                    "usage: bake-all-ramdisks [--signed-only] [--device <model>] [--build <buildID>]\n"
                    "                         [--force]\n");
            return 2;
        }
    }

    TestByteOrder();

    auto targets = knownFirmwareTargets();
    if (targets.empty()) {
        fprintf(stderr, "bake-all-ramdisks: no .keys files found under %s\n", resolveImageKeyPath("").c_str());
        return 1;
    }

    if (!deviceFilter.empty()) {
        size_t before = targets.size();
        std::vector<std::pair<std::string, std::string>> filtered;
        for (auto& target : targets) {
            if (target.first == deviceFilter) filtered.push_back(target);
        }
        targets = std::move(filtered);
        printf("--device %s: %zu of %zu known combinations match.\n", deviceFilter.c_str(), targets.size(), before);
        if (targets.empty()) {
            fprintf(stderr, "bake-all-ramdisks: no known (device, firmware) combinations for device %s\n",
                    deviceFilter.c_str());
            return 1;
        }
    }

    if (!buildFilter.empty()) {
        size_t before = targets.size();
        std::vector<std::pair<std::string, std::string>> filtered;
        for (auto& target : targets) {
            if (target.second == buildFilter) filtered.push_back(target);
        }
        targets = std::move(filtered);
        printf("--build %s: %zu of %zu known combinations match.\n", buildFilter.c_str(), targets.size(), before);
        if (targets.empty()) {
            fprintf(stderr, "bake-all-ramdisks: no known (device, firmware) combinations for build %s\n",
                    buildFilter.c_str());
            return 1;
        }
    }

    if (signedOnly) {
        size_t before = targets.size();
        // One ipsw.me lookup per unique device, not per (device, buildID)
        // pair — every build of the same device shares one signed-set query.
        std::map<std::string, std::set<std::string>> signedByDevice;
        std::vector<std::pair<std::string, std::string>> filtered;
        for (auto& [device, buildID] : targets) {
            auto found = signedByDevice.find(device);
            if (found == signedByDevice.end()) {
                found = signedByDevice.emplace(device, signedBuildsForDevice(device)).first;
            }
            if (found->second.count(buildID)) filtered.push_back({device, buildID});
        }
        targets = std::move(filtered);
        printf("--signed-only: %zu of %zu known combinations are currently signed by Apple.\n", targets.size(),
               before);
        if (targets.empty()) {
            fprintf(stderr, "bake-all-ramdisks: nothing to do (ipsw.me reports nothing currently signed)\n");
            return 1;
        }
    }

    fs::create_directories("dist");
    printf("Found %zu known (device, firmware) combinations.\n", targets.size());


    // Built once — same binary gets spliced into every firmware's ramdisk
    // (see BakeRamdisk.hpp's bakeRamdisk() comment), so there's no reason
    // to pay for the podman build again per target.
    std::string entrypointBinaryPath = buildEntrypointBinary();
    if (entrypointBinaryPath.empty()) {
        fprintf(stderr, "bake-all-ramdisks: failed to build entrypoint/ — see stderr above\n");
        return 1;
    }

    // Fail-fast, deliberately, for the actual bake step: computeGlobalDebcacheOnce()
    // (BakeRamdisk.cpp) caches its result across every firmware target in this
    // run, so a real failure in it (a broken dpkg/apt state, a bad .deb, etc.)
    // isn't "this one target had a problem" — every remaining target shares
    // the exact same cached failure and would fail identically. Continuing
    // the loop after any failure here just re-demonstrates the same root
    // cause several more times before reporting it; dying immediately on the
    // first one gets to the real error faster.
    //
    // Fetching the firmware itself is a different story: Apple pulling an
    // old build's signing, a flaky mirror, etc. only affects that one
    // target, so those failures are downgraded to a warning and the run
    // moves on to the next target instead of aborting the whole batch.
    size_t succeeded = 0;
    size_t warned = 0;
    size_t skippedDownload = 0;

    // Memoized per device MODEL (not per (device, buildID) tuple) — a full
    // sweep bakes dozens of buildIDs for the same handful of device
    // models, and newestVersionForDevice() is a live ipsw.me network call
    // with no caching of its own. Without this, every tuple for the same
    // model would re-fetch and get the identical answer.
    std::map<std::string, std::string> newestVersionCache;

    for (size_t i = 0; i < targets.size(); i++) {
        const std::string& device = targets[i].first;
        const std::string& buildID = targets[i].second;
        std::string label = device + " " + buildID;
        std::string outputPath = "dist/" + device + "_" + buildID + "-Ramdisk.dmg";

        printf("[%zu/%zu] %s: ", i + 1, targets.size(), label.c_str());
        fflush(stdout);

        // Existence is the whole check. There used to be a .sum sidecar
        // holding a content fingerprint of the ramdisk/ overlay, compared
        // here so an edit to ramdisk/ forced a re-bake -- that system is gone
        // (fragile, and only ever an approximation of staleness). --force is
        // the explicit way to say "I changed something, rebuild it".
        if (!force && fs::exists(outputPath)) {
            printf("already baked, skipping (use --force to rebuild)\n");
            succeeded++;
            continue;
        }

        IpswFetch fetcher;
        std::string firmwareURL = fetcher.firmwareURLForDevice(device, buildID);
        if (firmwareURL.empty()) {
            printf("WARNING: could not download ramdisk from apple.\n");
            fprintf(stderr, "bake-all-ramdisks: %s: no firmware URL from ipsw.me\n", label.c_str());
            skippedDownload++;
            continue;
        }

        FragmentDownloader downloader(firmwareURL);
        if (!downloader.open()) {
            printf("WARNING: could not download ramdisk from apple.\n");
            fprintf(stderr, "bake-all-ramdisks: %s: could not open remote IPSW\n", label.c_str());
            skippedDownload++;
            continue;
        }

        std::string workDir = ipswDataRoot() + "/" + device + "/" + buildID;
        fs::create_directories(workDir);

        std::string manifestPath = workDir + "/BuildManifest.plist";
        if (!downloader.downloadComponent("BuildManifest.plist", manifestPath, nullptr)) {
            printf("WARNING: could not download ramdisk from apple.\n");
            fprintf(stderr, "bake-all-ramdisks: %s: failed to download BuildManifest.plist\n", label.c_str());
            skippedDownload++;
            continue;
        }

        auto manifest = parseManifest(manifestPath);
        if (!manifest || manifest->restoreRamdiskPath.empty()) {
            printf("WARNING: could not download ramdisk from apple.\n");
            fprintf(stderr, "bake-all-ramdisks: %s: no RestoreRamDisk component in BuildManifest.plist\n",
                    label.c_str());
            skippedDownload++;
            continue;
        }

        std::string localRamdiskPath = workDir + "/" + fs::path(manifest->restoreRamdiskPath).filename().string();
        if (!downloader.downloadComponent(manifest->restoreRamdiskPath, localRamdiskPath, nullptr)) {
            printf("WARNING: could not download ramdisk from apple.\n");
            fprintf(stderr, "bake-all-ramdisks: %s: failed to download RestoreRamDisk\n", label.c_str());
            skippedDownload++;
            continue;
        }

        // Deliberately keyed by the buildID from the .keys filename we're
        // already processing, not manifest->realBuildID — they usually
        // agree, but if they ever don't, re-deriving the lookup from
        // realBuildID risks asking keysForDevice() for a .keys file other
        // than the one we started from.
        auto keys = fetcher.keysForDevice(device, buildID);
        auto it = keys.find("RestoreRamdisk");
        if (it == keys.end()) {
            printf("FAILED (no RestoreRamdisk key)\n");
            fprintf(stderr, "bake-all-ramdisks: %s: no RestoreRamdisk entry in .keys file\n", label.c_str());
            return 1;
        }

        // What bakeRamdisk() threads down to the synthetic "firmware" dpkg
        // stanza (scripts/build_deb_cache.py's sandbox and
        // BakeRamdisk.cpp's own bake-time preinstall declare it — see
        // those files' own comments) AND to stageVersionBranch()'s choice
        // of persistence/untether payload must be the real target DEVICE's
        // likely OS version, NOT manifest->productVersion (this specific
        // (device, buildID) tuple's own ramdisk vehicle version). Those are
        // two different things: kJailbreakTargetBuild (Cli.cpp) pins one
        // fixed, old ramdisk vehicle build used for every real jailbreak
        // run regardless of device model or what OS the actual device is
        // running — e.g. baking AppleTV3,2's "10B329a" tuple has a real
        // ProductVersion around 6.x, but a real AppleTV3,2 being jailbroken
        // today is almost certainly running something much newer (most
        // real devices auto-update to the latest available). Persistence
        // payloads and firmware-gated Depends: lines need to match what's
        // actually installed on the device's own NAND, which this baked
        // ramdisk never touches or reflects — so assume the newest known
        // OS for this device MODEL. Real per-device precision isn't
        // available at bake time at all now: the tether-boot path was the
        // only one that ever knew a specific connected device's exact
        // version (and did not use this baked persistence content anyway),
        // and it has been removed — see docs/HISTORY.md.
        // manifest->productVersion (this tuple's own version) is
        // only the last-resort fallback now, if even the newest-known-
        // version lookup fails.
        std::string productVersion;
        auto cacheIt = newestVersionCache.find(device);
        if (cacheIt != newestVersionCache.end()) {
            productVersion = cacheIt->second;
        } else {
            productVersion = newestVersionForDevice(device);
            newestVersionCache[device] = productVersion;
        }
        if (productVersion.empty()) {
            fprintf(stderr,
                    "bake-all-ramdisks: %s: ipsw.me lookup for %s's newest known version failed; falling back "
                    "to this tuple's own BuildManifest.plist ProductVersion (less accurate — reflects the "
                    "ramdisk vehicle's version, not the likely target device's)\n",
                    label.c_str(), device.c_str());
            productVersion = manifest->productVersion;
            if (productVersion.empty()) {
                fprintf(stderr, "bake-all-ramdisks: %s: BuildManifest.plist fallback also had no ProductVersion\n",
                        label.c_str());
            }
        }

        bool sizeWarning = false;
        if (!bakeRamdisk(localRamdiskPath, it->second.key, it->second.iv, productVersion, outputPath,
                          entrypointBinaryPath, sizeWarning)) {
            printf("FAILED (bake)\n");
            fprintf(stderr, "bake-all-ramdisks: %s: bakeRamdisk() failed — see stderr above\n", label.c_str());
            return 1;
        }

        if (sizeWarning) {
            printf("OK (with size warning, see stderr) -> %s\n", outputPath.c_str());
            warned++;
        } else {
            printf("OK -> %s\n", outputPath.c_str());
        }
        succeeded++;
    }

    printf("\n%zu/%zu succeeded", succeeded, targets.size());
    if (warned > 0) {
        printf(" (%zu with a size warning, see stderr above)", warned);
    }
    if (skippedDownload > 0) {
        printf(", %zu skipped (could not download ramdisk from apple, see stderr above)", skippedDownload);
    }
    printf(".\n");

    return 0;
}
