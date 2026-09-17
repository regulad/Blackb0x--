//
//  BakeAllBootloaders.cpp
//  bake-all-bootloaders
//
//  Batch driver that downloads and patches every firmware component EXCEPT
//  the ramdisk — iBSS, iBEC, KernelCache and DeviceTree — for every (device
//  model, firmware build) combination this port has decryption keys for,
//  writing results to dist/bootchain/<device>_<buildID>/.
//
//  This is `.claude/TODO.md` item 5 ("Pre-patch firmware components ahead of
//  time, instead of after device enumeration") for the non-ramdisk half of
//  the suite. blackb0x itself does all of this live today, inside the window
//  the device is already sitting in pwned DFU waiting for the next
//  component: network I/O and CPU-bound patching both happen while the clock
//  is running. Everything this tool produces is knowable ahead of time.
//
//  It is deliberately the counterpart to bake-all-ramdisks, NOT part of it:
//
//    - bake-all-ramdisks needs CAP_SYS_ADMIN/CAP_CHOWN (it loop-mounts a
//      real HFS+ image) and so has to run under sudo.
//    - NOTHING here needs root. Every step is download, decrypt, and
//      byte-level patching of ordinary files. That is the whole reason this
//      is a separate binary rather than a flag on the other one: it can be
//      run and re-run freely while iterating on the patch logic itself,
//      which is exactly what is wanted while the iBoot32Patcher invocation
//      is under investigation (see docs/HISTORY.md).
//
//  It is also the only way to exercise Patcher::patchiBSS()/patchiBEC()/
//  patchKernel() — and therefore the iBoot32Patcher binary they now
//  fork/exec — across every known firmware WITHOUT any hardware attached.
//  A patch whose instruction-pattern search fails on some specific build
//  shows up here as a plain failure row instead of as a device that will
//  not boot.
//
//  Usage: ./bake-all-bootloaders [--signed-only] [--device <model>]
//                                [--build <buildID>] [--out <dir>]
//                                [--force] [--stop-early]
//
//  --out <dir> overrides the output root (default dist/bootchain). Worth
//  knowing about: dist/ is typically created by `sudo ./bake-all-ramdisks`
//  and therefore owned by root, which makes the default location
//  unwritable for this deliberately-rootless tool. It detects that case
//  and says so rather than reporting a confusing per-file error.
//
//  --signed-only / --device / --build behave exactly as they do in
//  bake-all-ramdisks (see its header) — same filters, same semantics, so
//  the two tools can be pointed at the same subset.
//
//  --force re-patches targets whose output directory already exists.
//  Without it, an already-complete target is skipped, which makes repeated
//  runs cheap. There is no content-hash sidecar here (unlike the ramdisk's
//  .sum): the inputs are Apple's immutable per-build components, so the
//  only thing that changes output is this project's own patch code, and
//  --force is the explicit way to say "I changed that".
//
//  --stop-early aborts on the first target that fails instead of working
//  through the whole set. Off by default: a patch that fails on ONE build
//  is exactly the signal this tool exists to surface, and the failure
//  pattern across builds (one model? one iBoot version? everything?) is
//  usually more informative than the first failure alone.
//

#include "IPSW.hpp"
#include "IPSWDownloader.hpp"
#include "Patcher.hpp"
#include "ResourcePath.hpp"

extern "C" {
#include <xpwn/libxpwn.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// Same derivation as bake-all-ramdisks' knownFirmwareTargets(): read the
// (device, buildID) set straight out of Blackb0x/ImageKeys/<device>/
// <device>_<buildID>.keys rather than a separate hardcoded list, so both
// tools track ImageKeys/ automatically and always agree on what "every
// known firmware" means.
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

// What one target produced. Each component is tracked separately rather
// than collapsed into a single pass/fail, because which ones failed is the
// diagnostic: iBSS-but-not-iBEC points somewhere entirely different from
// "every patch on this build failed".
struct TargetResult {
    std::string device;
    std::string buildID;
    bool downloaded = false;
    bool iBSS = false;
    bool iBEC = false;
    bool kernel = false;
    bool deviceTree = false;
    bool skipped = false;
    std::string note;

    bool complete() const { return iBSS && iBEC && kernel && deviceTree; }
};

static const char* mark(bool ok) { return ok ? "ok" : "FAIL"; }

// Copies one patched output into the target's output directory under a
// stable name. Patcher writes its outputs next to the downloaded input (in
// the IPSW work directory), which is cache-shaped and gets reused across
// runs; the output directory is the durable, predictable location something
// else can consume later.
//
// Deliberately NO file extension on the published names. The container
// format genuinely differs per component and per device model, so asserting
// one in the filename would be a lie: patchiBSS() keeps the RAW patched
// iBoot for Apple TV 3 (it deletes the re-wrapped img3 and hands back the
// .patched file -- see its own j33i branch) but the re-wrapped img3 for
// Apple TV 2, while DeviceTree is passed through as Apple's original img3
// untouched.
//
// create_directories() is checked on its own rather than sharing an
// error_code with the copy: the two fail for completely different reasons,
// and conflating them turns "your output directory is not writable" into a
// misleading per-file ENOENT on every component.
static bool publish(const std::string& from, const std::string& toDir, const std::string& name,
                     std::string& noteOut) {
    std::error_code dirEc;
    fs::create_directories(toDir, dirEc);
    if (dirEc && !fs::is_directory(toDir)) {
        noteOut += (noteOut.empty() ? "" : "; ") + ("could not create " + toDir + ": " + dirEc.message());
        return false;
    }
    std::error_code copyEc;
    fs::copy_file(from, fs::path(toDir) / name, fs::copy_options::overwrite_existing, copyEc);
    if (copyEc) {
        noteOut += (noteOut.empty() ? "" : "; ") + ("could not publish " + name + ": " + copyEc.message());
        return false;
    }
    return true;
}

static TargetResult bakeOne(const std::string& device, const std::string& buildID,
                             const std::string& outRoot, bool force) {
    TargetResult result;
    result.device = device;
    result.buildID = buildID;

    const std::string label = device + " " + buildID;
    const std::string outDir = outRoot + "/" + device + "_" + buildID;

    // Four files, all present, is what "already done" means. A partial
    // directory from an interrupted or half-failing earlier run is NOT
    // treated as done -- that would make a failure sticky and invisible on
    // the next run, which is the opposite of what this tool is for.
    if (!force && fs::exists(outDir + "/iBSS") && fs::exists(outDir + "/iBEC") &&
        fs::exists(outDir + "/kernelcache") && fs::exists(outDir + "/devicetree")) {
        printf("already built, skipping (use --force to rebuild)\n");
        result.skipped = true;
        result.downloaded = true;
        result.iBSS = result.iBEC = result.kernel = result.deviceTree = true;
        return result;
    }

    IpswFetch fetcher;
    std::string firmwareURL = fetcher.firmwareURLForDevice(device, buildID);
    if (firmwareURL.empty()) {
        printf("no firmware URL from ipsw.me\n");
        result.note = "no firmware URL";
        return result;
    }

    FragmentDownloader downloader(firmwareURL);
    if (!downloader.open()) {
        printf("could not open remote IPSW\n");
        result.note = "remote IPSW unreachable";
        return result;
    }

    std::string workDir = ipswDataRoot() + "/" + device + "/" + buildID;
    std::error_code ec;
    fs::create_directories(workDir, ec);

    std::string manifestPath = workDir + "/BuildManifest.plist";
    if (!downloader.downloadComponent("BuildManifest.plist", manifestPath, nullptr)) {
        printf("failed to download BuildManifest.plist\n");
        result.note = "no BuildManifest";
        return result;
    }

    auto manifest = parseManifest(manifestPath);
    if (!manifest) {
        printf("could not parse BuildManifest.plist\n");
        result.note = "unparseable BuildManifest";
        return result;
    }
    result.downloaded = true;

    Patcher patcher;
    // Keyed by the buildID from the .keys filename being processed, not
    // manifest->realBuildID -- same reasoning as bake-all-ramdisks': they
    // usually agree, and re-deriving from realBuildID risks loading a
    // different .keys file than the one this target came from.
    patcher.loadKeysForDevice(device, buildID);
    patcher.setBuildIdentity(manifest->buildIdentity);
    patcher.setBuildID(manifest->realBuildID);

    // Fetch one component into workDir and hand its local path to `apply`.
    // Download failure and patch failure are reported separately by the
    // caller via the per-component flags; this just reports whether the
    // pair succeeded.
    auto fetchAndPatch = [&](const char* what, const std::string& remotePath,
                             const std::function<bool(const std::string&)>& apply) -> bool {
        if (remotePath.empty()) {
            result.note += (result.note.empty() ? "" : "; ") + std::string(what) + ": not in manifest";
            return false;
        }
        std::string localPath = workDir + "/" + fs::path(remotePath).filename().string();
        if (!fs::exists(localPath) && !downloader.downloadComponent(remotePath, localPath, nullptr)) {
            result.note += (result.note.empty() ? "" : "; ") + std::string(what) + ": download failed";
            return false;
        }
        if (!apply(localPath)) {
            result.note += (result.note.empty() ? "" : "; ") + std::string(what) + ": patch failed";
            return false;
        }
        return true;
    };

    // Matches the non-stock branches of Cli.cpp's downloadAndPatchComponents()
    // exactly -- this tool only ever produces the real jailbreak suite, so
    // none of the --stock-* diagnostic routes apply. The "4." version
    // heuristic for iBEC is the same one carried over from the original
    // setIBECPath: (4.x-era iBEC needs empty flags and no ticket patch).
    result.iBSS = fetchAndPatch("iBSS", manifest->iBSSPath,
                                 [&](const std::string& p) { return patcher.patchiBSS(p); });
    result.iBEC = fetchAndPatch("iBEC", manifest->iBECPath, [&](const std::string& p) {
        if (p.find("4.") != std::string::npos) return patcher.patchiBEC(p, "", false);
        return patcher.patchiBEC(p);
    });
    result.kernel = fetchAndPatch("KernelCache", manifest->kernelCachePath, [&](const std::string& p) {
        return patcher.patchKernel(p, manifest->productVersion);
    });
    // DeviceTree is sent unmodified (see Patcher.hpp's setDeviceTreePath()) --
    // there is no patch step to fail, only the download.
    result.deviceTree = fetchAndPatch("DeviceTree", manifest->deviceTreePath, [&](const std::string& p) {
        patcher.setDeviceTreePath(p);
        return true;
    });

    const PatchedComponents& out = patcher.components();
    if (result.iBSS && out.iBSS) result.iBSS = publish(*out.iBSS, outDir, "iBSS", result.note);
    if (result.iBEC && out.iBEC) result.iBEC = publish(*out.iBEC, outDir, "iBEC", result.note);
    if (result.kernel && out.kernel) result.kernel = publish(*out.kernel, outDir, "kernelcache", result.note);
    if (result.deviceTree && out.deviceTree)
        result.deviceTree = publish(*out.deviceTree, outDir, "devicetree", result.note);

    printf("iBSS %s, iBEC %s, kernel %s, devicetree %s%s%s\n", mark(result.iBSS), mark(result.iBEC),
           mark(result.kernel), mark(result.deviceTree), result.note.empty() ? "" : " -- ",
           result.note.c_str());
    return result;
}

int main(int argc, char** argv) {
    bool signedOnly = false;
    bool force = false;
    bool stopEarly = false;
    std::string deviceFilter;
    std::string buildFilter;
    std::string outRoot = "dist/bootchain";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--signed-only") == 0) {
            signedOnly = true;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (strcmp(argv[i], "--stop-early") == 0) {
            stopEarly = true;
        } else if (strcmp(argv[i], "--device") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bake-all-bootloaders: --device requires a value (e.g. --device AppleTV3,2)\n");
                return 2;
            }
            deviceFilter = argv[++i];
        } else if (strcmp(argv[i], "--build") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bake-all-bootloaders: --build requires a value (e.g. --build 12H606)\n");
                return 2;
            }
            buildFilter = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bake-all-bootloaders: --out requires a value (e.g. --out /tmp/bootchain)\n");
                return 2;
            }
            outRoot = argv[++i];
        } else {
            fprintf(stderr, "bake-all-bootloaders: unrecognized argument %s\n", argv[i]);
            fprintf(stderr,
                    "usage: bake-all-bootloaders [--signed-only] [--device <model>] [--build <buildID>]\n"
                    "                            [--out <dir>] [--force] [--stop-early]\n");
            return 2;
        }
    }

    TestByteOrder();

    auto targets = knownFirmwareTargets();
    if (targets.empty()) {
        fprintf(stderr, "bake-all-bootloaders: no .keys files found under %s\n", resolveImageKeyPath("").c_str());
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
            fprintf(stderr, "bake-all-bootloaders: no known (device, firmware) combinations for device %s\n",
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
            fprintf(stderr, "bake-all-bootloaders: no known (device, firmware) combinations for build %s\n",
                    buildFilter.c_str());
            return 1;
        }
    }

    if (signedOnly) {
        size_t before = targets.size();
        // One ipsw.me lookup per unique device, not per (device, buildID)
        // pair -- same memoization bake-all-ramdisks does, same reason.
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
            fprintf(stderr, "bake-all-bootloaders: nothing to do (ipsw.me reports nothing currently signed)\n");
            return 1;
        }
    }

    printf("Found %zu known (device, firmware) combinations.\n", targets.size());
    printf("No root needed: every step here is download, decrypt and file patching.\n");

    // Checked once, up front, rather than discovered per component after
    // every download and patch has already been paid for. The overwhelmingly
    // likely cause is a root-owned dist/ left behind by
    // `sudo ./bake-all-ramdisks`, so say that explicitly instead of letting
    // an EACCES surface as four identical unexplained per-file errors.
    std::error_code outEc;
    fs::create_directories(outRoot, outEc);
    if (outEc && !fs::is_directory(outRoot)) {
        fprintf(stderr,
                "\nbake-all-bootloaders: cannot create output directory %s: %s\n"
                "  dist/ is usually created by `sudo ./bake-all-ramdisks` and owned by root, which\n"
                "  makes it unwritable for this tool -- which deliberately does not need root.\n"
                "  Either chown it (sudo chown -R \"$(id -un)\" dist) or pick another location\n"
                "  with --out <dir>.\n",
                outRoot.c_str(), outEc.message().c_str());
        return 1;
    }
    printf("Output root: %s\n\n", outRoot.c_str());

    std::vector<TargetResult> results;
    for (size_t i = 0; i < targets.size(); i++) {
        printf("[%zu/%zu] %s %s: ", i + 1, targets.size(), targets[i].first.c_str(), targets[i].second.c_str());
        fflush(stdout);
        results.push_back(bakeOne(targets[i].first, targets[i].second, outRoot, force));
        if (stopEarly && !results.back().complete()) {
            printf("\n--stop-early: stopping at the first incomplete target.\n");
            break;
        }
    }

    // Per-component totals, not just a pass count: the point of running this
    // across every build is to see WHICH patch fails WHERE. A run where
    // every iBEC fails and everything else succeeds says something very
    // different from one where a single build fails everything.
    size_t complete = 0, skipped = 0, noDownload = 0;
    size_t iBSSFail = 0, iBECFail = 0, kernelFail = 0, dtFail = 0;
    for (const auto& r : results) {
        if (r.skipped) skipped++;
        if (r.complete()) complete++;
        if (!r.downloaded) noDownload++;
        if (r.downloaded && !r.iBSS) iBSSFail++;
        if (r.downloaded && !r.iBEC) iBECFail++;
        if (r.downloaded && !r.kernel) kernelFail++;
        if (r.downloaded && !r.deviceTree) dtFail++;
    }

    printf("\n%-14s %-10s %-6s %-6s %-8s %-11s %s\n", "device", "build", "iBSS", "iBEC", "kernel", "devicetree",
           "note");
    for (const auto& r : results) {
        if (r.skipped) continue;
        printf("%-14s %-10s %-6s %-6s %-8s %-11s %s\n", r.device.c_str(), r.buildID.c_str(),
               r.downloaded ? mark(r.iBSS) : "-", r.downloaded ? mark(r.iBEC) : "-",
               r.downloaded ? mark(r.kernel) : "-", r.downloaded ? mark(r.deviceTree) : "-", r.note.c_str());
    }

    printf("\n%zu/%zu complete (%zu already built and skipped).\n", complete, results.size(), skipped);
    if (noDownload) printf("%zu could not be downloaded at all (not a patch failure).\n", noDownload);
    if (iBSSFail || iBECFail || kernelFail || dtFail) {
        printf("Patch failures among downloaded targets: iBSS %zu, iBEC %zu, kernel %zu, devicetree %zu.\n",
               iBSSFail, iBECFail, kernelFail, dtFail);
    }
    printf("Output: %s/<device>_<buildID>/\n", outRoot.c_str());

    // A target that could not be downloaded is not this tool's failure --
    // Apple pulling an old build, a flaky mirror. A target that downloaded
    // and then failed to patch is, and that is what the exit status
    // reports, so a CI/scripted run can tell the two apart.
    bool anyPatchFailure = iBSSFail || iBECFail || kernelFail || dtFail;
    return anyPatchFailure ? 1 : 0;
}
