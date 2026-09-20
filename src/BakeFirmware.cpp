//
//  BakeFirmware.cpp
//  bake-firmware
//
//  The single ahead-of-time baker: for every (device model, firmware build)
//  combination this port has decryption keys for, download and patch the
//  whole firmware suite — iBSS, iBEC, KernelCache and DeviceTree into
//  dist/ as flat <Component>-<device>_<buildID> entries (Apple's own
//  BuildManifest component keys), the ramdisk as
//  dist/RestoreRamDisk-<device>_<buildID>.dmg.
//
//  This was two binaries, bake-all-bootloaders and bake-all-ramdisks, and
//  the split was load-bearing for exactly one reason: on Linux the ramdisk
//  bake loop-mounted a real HFS+ image and so needed CAP_SYS_ADMIN/CAP_CHOWN,
//  while nothing in the bootchain half needed root at all. Keeping them apart
//  meant the rootless half could be re-run freely while iterating on patch
//  logic without dragging sudo into it. That reason is gone: the ramdisk bake
//  is native `hdiutil` now, which needs neither a loop mount nor root, so
//  both halves are ordinary unprivileged work over the same target list and
//  there is nothing left to separate. Everything else they had in common —
//  the target enumeration, the --device/--build filters, the
//  skip-unless---force rule — was duplicated verbatim between the two files
//  and is now written once.
//
//  Neither half is what blackb0x itself runs against a live device:
//  Patcher::patchRamdisk() only checks whether the dist/ entry it needs
//  already exists, and Cli.cpp spawns this tool (`--only ramdisk`, one
//  tuple) in the background if it doesn't. The bootchain half is
//  `.claude/TODO.md` item 5 — blackb0x still patches those four components
//  live, inside the window the device is already sitting in pwned DFU, even
//  though everything this tool produces is knowable ahead of time.
//
//  It is also the only way to exercise Patcher::patchiBSS()/patchiBEC()/
//  patchKernel() — and therefore the iBoot32Patcher binary they fork/exec —
//  across every known firmware WITHOUT any hardware attached. A patch whose
//  instruction-pattern search fails on some specific build shows up here as
//  a plain failure row instead of as a device that will not boot.
//
//  Usage: ./bake-firmware [--device <model>] [--build <buildID>]
//                         [--only bootchain|ramdisk] [--bootchain-out <dir>]
//                         [--force] [--stop-early]
//
//  --device <model> restricts the run to just that one device (e.g.
//  "AppleTV3,2" — the exact ImageKeys/ directory name, case-sensitive),
//  every known build for it. Meant for iterating on bake content without
//  paying for all 92 known (device, firmware) combinations on every single
//  test run.
//
//  --build <buildID> restricts the run to just that one firmware build (e.g.
//  "12H606"), across whichever known device(s) have it. Combines with
//  --device to pin the run down to exactly one pair.
//
//  There used to be a --signed-only flag here, which asked ipsw.me which
//  builds Apple is still actively signing and baked only those. It is gone,
//  because the question it answered is not one this tool has any stake in.
//  What blackb0x can jailbreak is decided entirely by what has a .keys file
//  under keys/ and patches that work against it — and the live path pins
//  kJailbreakTargetBuild (Cli.cpp) regardless. Apple's current signing
//  window neither adds a bakeable target nor removes one, so filtering on it
//  only ever hid targets that were still perfectly valid to bake, at the
//  cost of a network round trip per device. Use --device/--build to narrow a
//  run. signedBuildsForDevice() (IPSW.hpp) itself stays: the --stock-*
//  diagnostic routes are real consumers of it, since a TSS ticket genuinely
//  can only be issued for a build Apple is still signing.
//
//  --only bootchain|ramdisk runs just that half. `--only bootchain` is the
//  fast iteration loop for patch logic: no entrypoint cross-compile, no
//  debcache, no HFS+ work at all. `--only ramdisk` is what Cli.cpp spawns
//  when a live run finds the dist/ entry it needs is missing.
//
//  --bootchain-out <dir> overrides the bootchain output root (default
//  dist). The ramdisk output location is NOT configurable: it is
//  the dist/ layout Patcher::patchRamdisk() looks in by name.
//
//  --force rebuilds targets whose output already exists. Without it an
//  already-complete target is skipped, which makes repeated runs cheap.
//  There is no staleness detection. For the bootchain half the inputs are
//  Apple's immutable per-build components, so the only thing that changes
//  output is this project's own patch code. For the ramdisk half there used
//  to be a .sum sidecar holding a content fingerprint of the overlay, so
//  that editing it forced a re-bake; that system is gone (fragile, and only
//  ever an approximation of staleness). Either way --force is the explicit
//  way to say "I changed something, rebuild it".
//
//  --stop-early aborts on the first target that doesn't come out complete
//  instead of working through the whole set. Off by default: a patch that
//  fails on ONE build is exactly the signal this tool exists to surface, and
//  the failure pattern across builds (one model? one iBoot version?
//  everything?) is usually more informative than the first failure alone.
//

#include "BakeRamdisk.hpp"
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
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

static const char* kProg = "bake-firmware";

// Every (device, buildID) pair this port has a .keys file for — read
// straight from the directory structure (keys/<device>/
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

// ---------------------------------------------------------------------------
// Bootchain half: iBSS, iBEC, KernelCache, DeviceTree
// ---------------------------------------------------------------------------

// What one target produced. Each bootchain component is tracked separately
// rather than collapsed into a single pass/fail, because which ones failed is
// the diagnostic: iBSS-but-not-iBEC points somewhere entirely different from
// "every patch on this build failed".
enum class RamdiskOutcome {
    NotRequested,
    Skipped,
    Baked,
    BakedWithSizeWarning,
    DownloadFailed,
    BakeFailed,
};

struct TargetResult {
    std::string device;
    std::string buildID;

    // Bootchain
    bool bootchainRequested = false;
    bool downloaded = false;
    bool iBSS = false;
    bool iBEC = false;
    bool kernel = false;
    bool deviceTree = false;
    bool bootchainSkipped = false;

    RamdiskOutcome ramdisk = RamdiskOutcome::NotRequested;

    std::string note;

    bool bootchainComplete() const { return !bootchainRequested || (iBSS && iBEC && kernel && deviceTree); }

    bool ramdiskComplete() const {
        return ramdisk == RamdiskOutcome::NotRequested || ramdisk == RamdiskOutcome::Skipped ||
               ramdisk == RamdiskOutcome::Baked || ramdisk == RamdiskOutcome::BakedWithSizeWarning;
    }

    bool complete() const { return bootchainComplete() && ramdiskComplete(); }
};

static const char* mark(bool ok) { return ok ? "ok" : "FAIL"; }

static void addNote(std::string& note, const std::string& what) {
    note += (note.empty() ? "" : "; ") + what;
}

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
static std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

static bool publish(const std::string& from, const std::string& toDir, const std::string& name,
                     std::string& noteOut) {
    std::error_code dirEc;
    fs::create_directories(toDir, dirEc);
    if (dirEc && !fs::is_directory(toDir)) {
        addNote(noteOut, "could not create " + toDir + ": " + dirEc.message());
        return false;
    }
    std::error_code copyEc;
    const fs::path dest = fs::path(toDir) / name;
    fs::copy_file(from, dest, fs::copy_options::overwrite_existing, copyEc);
    if (copyEc) {
        addNote(noteOut, "could not publish " + name + ": " + copyEc.message());
        return false;
    }
    // This process is root (see main()'s geteuid() check), so everything it
    // writes would otherwise land root-owned. dist/ is ordinary build output
    // the invoking user wants to read, move and delete without sudo.
    chownToSudoCaller(toDir);
    chownToSudoCaller(dest.string());
    return true;
}

// Fork/exec/wait a foreground subprocess, inheriting stdio so its output
// (e.g. bake-kernel's [size] instrumentation) prints straight through. Same
// shape as Cli.cpp's runForeground(); kept local rather than shared to avoid
// pulling Cli.cpp's other machinery into this authoring binary.
static bool runSubprocess(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "runSubprocess: fork() failed for %s: %s\n", argv[0].c_str(), strerror(errno));
        return false;
    }
    if (pid == 0) {
        execvp(cargv[0], cargv.data());
        fprintf(stderr, "runSubprocess: cannot execute %s: %s\n", cargv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static TargetResult bakeBootchainInProcess(const std::string& device, const std::string& buildID,
                                            const std::string& outRoot, bool force) {
    TargetResult result;
    result.device = device;
    result.buildID = buildID;
    result.bootchainRequested = true;

    // dist/ is FLAT. Components are named <Component>-<device>_<buildID>, using
    // Apple's own BuildManifest component keys (iBSS, iBEC, KernelCache,
    // DeviceTree, RestoreRamDisk) rather than lowercase inventions of ours, so
    // a file here is recognisable as the thing the manifest calls it.
    //
    // This used to be dist/bootchain/<device>_<buildID>/{iBSS,iBEC,
    // kernelcache,devicetree} -- a directory per tuple, plus a separate
    // dist/<device>_<buildID>-Ramdisk.dmg sitting outside it. Two layouts for
    // one firmware suite, with the ramdisk the odd one out. One flat directory
    // keyed by component and tuple is easier to glob, easier to publish
    // (.claude/TODO.md item 7), and puts every artifact for a tuple next to
    // its siblings.
    const std::string outDir = outRoot;
    const std::string tupleSuffix = std::string("-") + device + "_" + buildID;

    // Four files, all present, is what "already done" means. A partial
    // directory from an interrupted or half-failing earlier run is NOT
    // treated as done -- that would make a failure sticky and invisible on
    // the next run, which is the opposite of what this tool is for.
    if (!force && fs::exists(outDir + "/iBSS" + tupleSuffix) &&
        fs::exists(outDir + "/iBEC" + tupleSuffix) &&
        fs::exists(outDir + "/KernelCache" + tupleSuffix) &&
        fs::exists(outDir + "/DeviceTree" + tupleSuffix)) {
        printf("already built, skipping (use --force to rebuild)\n");
        result.bootchainSkipped = true;
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
    // manifest->realBuildID -- they usually agree, and re-deriving from
    // realBuildID risks loading a different .keys file than the one this
    // target came from. The ramdisk half keys its own .keys lookup the same
    // way, for the same reason.
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
            addNote(result.note, std::string(what) + ": not in manifest");
            return false;
        }
        std::string localPath = workDir + "/" + fs::path(remotePath).filename().string();
        if (!fs::exists(localPath) && !downloader.downloadComponent(remotePath, localPath, nullptr)) {
            addNote(result.note, std::string(what) + ": download failed");
            return false;
        }
        if (!apply(localPath)) {
            addNote(result.note, std::string(what) + ": patch failed");
            return false;
        }
        return true;
    };

    // iBSS + iBEC are baked by the standalone, rootless `bake-iboot` tool
    // (BakeIboot.cpp), and the kernelcache by `bake-kernel`, rather than
    // in-process -- so each patch pipeline can be run and instrumented
    // independently of a full bake. Shell out unless the dist/ outputs are
    // already present; each tool reuses this same ipswDataRoot() download cache
    // and publishes straight to outDir, so there are no in-process patch*()
    // calls or publish() calls for these below.
    if (!force && fs::exists(outDir + "/iBSS" + tupleSuffix) && fs::exists(outDir + "/iBEC" + tupleSuffix)) {
        result.iBSS = result.iBEC = true;
    } else {
        std::vector<std::string> iargs = {resolveBakeIbootPath(), "--device", device, "--build", buildID,
                                          "--out", outDir};
        if (force) iargs.push_back("--force");
        bool ok = runSubprocess(iargs);
        result.iBSS = ok && fs::exists(outDir + "/iBSS" + tupleSuffix);
        result.iBEC = ok && fs::exists(outDir + "/iBEC" + tupleSuffix);
        if (!result.iBSS || !result.iBEC) addNote(result.note, "iBSS/iBEC: bake-iboot failed");
    }
    // KernelCache is baked by the standalone, rootless `bake-kernel` tool
    // (BakeKernel.cpp) rather than in-process, so the kernel patch pipeline
    // can be run and instrumented independently of a full bake. Shell out to
    // it unless the dist/ kernel is already present -- it reuses this same
    // ipswDataRoot() download cache and publishes straight to outDir, so
    // there is no in-process patchKernel() call or kernel publish() below.
    if (!force && fs::exists(outDir + "/KernelCache" + tupleSuffix)) {
        result.kernel = true;
    } else {
        std::vector<std::string> kargs = {resolveBakeKernelPath(), "--device", device, "--build", buildID,
                                          "--out", outDir};
        if (force) kargs.push_back("--force");
        result.kernel = runSubprocess(kargs) && fs::exists(outDir + "/KernelCache" + tupleSuffix);
        if (!result.kernel) addNote(result.note, "KernelCache: bake-kernel failed");
    }
    // DeviceTree is sent unmodified (see Patcher.hpp's setDeviceTreePath()) --
    // there is no patch step to fail, only the download.
    result.deviceTree = fetchAndPatch("DeviceTree", manifest->deviceTreePath, [&](const std::string& p) {
        patcher.setDeviceTreePath(p);
        return true;
    });

    // Components this tool does not need to modify are still published, byte
    // for byte as Apple shipped them. blackb0x consumes dist/ and nothing else
    // now -- it has no IPSW downloader, no decryption keys and no network --
    // so anything it might need to send has to be here, whether or not the
    // bake had a reason to touch it.
    //
    // RestoreLogo is genuinely optional (idevicerestore's own
    // recovery_send_applelogo() returns success outright when the manifest
    // has no logo at all), and loadedByIBootComponents is usually empty. Both
    // are published when present and simply omitted when not; the index below
    // is what tells blackb0x which of them exist, so it never has to guess or
    // parse a BuildManifest it no longer downloads.
    auto publishVerbatim = [&](const std::string& name, const std::string& remotePath) -> bool {
        if (remotePath.empty()) return false;
        std::string localPath = workDir + "/" + fs::path(remotePath).filename().string();
        if (!fs::exists(localPath) && !downloader.downloadComponent(remotePath, localPath, nullptr)) {
            addNote(result.note, name + ": download failed");
            return false;
        }
        return publish(localPath, outDir, name + tupleSuffix, result.note);
    };
    std::vector<std::string> extraComponents;
    if (publishVerbatim("RestoreLogo", manifest->restoreLogoPath)) {
        extraComponents.push_back("RestoreLogo");
    }
    for (const auto& [name, remotePath] : manifest->loadedByIBootComponents) {
        if (publishVerbatim(name, remotePath)) extraComponents.push_back(name);
    }

    const PatchedComponents& out = patcher.components();
    // iBSS/iBEC are published by bake-iboot, and KernelCache by bake-kernel
    // (both shelled out above), not here.
    if (result.deviceTree && out.deviceTree)
        result.deviceTree = publish(*out.deviceTree, outDir, "DeviceTree" + tupleSuffix, result.note);

    // The index. blackb0x reads this instead of a BuildManifest: it names every
    // component present for this tuple, so optional pieces (RestoreLogo) and
    // manifest-driven ones (loadedByIBoot) need no out-of-band agreement
    // between the two binaries beyond the file format itself.
    //
    // Written last, and only when the four required components all succeeded,
    // so its presence is exactly the "this tuple is complete" signal. A
    // half-baked tuple leaves no index and blackb0x refuses it by name.
    if (result.iBSS && result.iBEC && result.kernel && result.deviceTree) {
        std::ofstream index(outDir + "/Manifest" + tupleSuffix + ".txt");
        if (index) {
            index << "# Components baked for " << device << " " << buildID << ".\n"
                  << "# One name per line; the file is <name>" << tupleSuffix << ".\n"
                  << "# Read by Cli.cpp's loadComponentsFromDist(). RestoreRamDisk is\n"
                  << "# listed here but baked separately by the ramdisk half.\n"
                  << "iBSS\niBEC\nKernelCache\nDeviceTree\n";
            for (const auto& name : extraComponents) index << name << "\n";
        }
        index.close();
        chownToSudoCaller(outDir + "/Manifest" + tupleSuffix + ".txt");
    }

    printf("iBSS %s, iBEC %s, kernel %s, devicetree %s%s%s%s\n", mark(result.iBSS), mark(result.iBEC),
           mark(result.kernel), mark(result.deviceTree),
           extraComponents.empty() ? "" : " (+verbatim: ",
           extraComponents.empty() ? "" : joinNames(extraComponents).c_str(),
           extraComponents.empty() ? "" : ")");
    if (!result.note.empty()) printf("  note: %s\n", result.note.c_str());
    return result;
}

// Runs one target's bootchain half in a forked child.
//
// Not defensive programming for its own sake -- this is load-bearing. A full
// sweep drives the vendored decrypt()/patch_kernel() code 4x per target over
// dozens of targets in one process, and it does not survive that: a
// 29-target AppleTV2,1 run died partway through with no summary at all,
// while every target it died on patches fine when run by itself in a fresh
// process. Something in that code (which has already produced one confirmed
// NULL-deref crash and carries a documented history of heap corruption) does
// not tolerate being reused across many inputs.
//
// Rather than chase that through third_party, each target gets its own
// process. A crash then costs exactly one row instead of the whole run and
// everything after it, which is the difference between this tool being
// usable for a full sweep and not. It also means a crash is REPORTED -- the
// parent sees the signal and says so -- instead of manifesting as a log that
// just stops.
//
// The ramdisk half deliberately does NOT get the same treatment; see
// bakeRamdiskForTarget()'s own comment for why it has to stay in-process.
//
// The child writes its result back as one line: six flags, a tab, then the
// note. Deliberately trivial to parse; there is no reason for a richer
// channel when the parent only needs what the summary table prints.
static TargetResult bakeBootchainForked(const std::string& device, const std::string& buildID,
                                         const std::string& outRoot, bool force) {
    TargetResult result;
    result.device = device;
    result.buildID = buildID;
    result.bootchainRequested = true;

    int fds[2];
    if (pipe(fds) != 0) {
        result.note = std::string("pipe() failed: ") + strerror(errno);
        printf("%s\n", result.note.c_str());
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        result.note = std::string("fork() failed: ") + strerror(errno);
        printf("%s\n", result.note.c_str());
        return result;
    }

    if (pid == 0) {
        close(fds[0]);
        TargetResult child = bakeBootchainInProcess(device, buildID, outRoot, force);
        std::string line = std::string(child.downloaded ? "1" : "0") + (child.iBSS ? "1" : "0") +
                           (child.iBEC ? "1" : "0") + (child.kernel ? "1" : "0") +
                           (child.deviceTree ? "1" : "0") + (child.bootchainSkipped ? "1" : "0") + "\t" +
                           child.note + "\n";
        ssize_t ignored = write(fds[1], line.c_str(), line.size());
        (void)ignored;
        close(fds[1]);
        // _exit, not exit: the child must not run atexit handlers or flush
        // the stdio buffers it inherited from the parent, or every line the
        // parent had buffered gets duplicated into the output.
        _exit(0);
    }

    close(fds[1]);
    std::string payload;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) payload.append(buf, (size_t)n);
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFSIGNALED(status)) {
        result.note = "child killed by signal " + std::to_string(WTERMSIG(status)) +
                      " -- this target crashes the patch code; run it alone to investigate";
        printf("CRASHED (signal %d)\n", WTERMSIG(status));
        return result;
    }

    size_t tab = payload.find('\t');
    if (payload.size() < 6 || tab == std::string::npos) {
        result.note = "child produced no usable result (exit " + std::to_string(WEXITSTATUS(status)) + ")";
        printf("no result from child\n");
        return result;
    }
    result.downloaded = payload[0] == '1';
    result.iBSS = payload[1] == '1';
    result.iBEC = payload[2] == '1';
    result.kernel = payload[3] == '1';
    result.deviceTree = payload[4] == '1';
    result.bootchainSkipped = payload[5] == '1';
    result.note = payload.substr(tab + 1);
    if (!result.note.empty() && result.note.back() == '\n') result.note.pop_back();
    return result;
}

// ---------------------------------------------------------------------------
// Ramdisk half
// ---------------------------------------------------------------------------

// Deliberately NOT forked per target, unlike the bootchain half above.
// bakeRamdisk() leans on two things that only pay off when they are shared
// across every target in one process: computeGlobalDebcacheOnce()
// (BakeRamdisk.cpp) resolves and caches the whole apt closure once, and
// buildEntrypointBinary() pays for one entrypoint cross-compile. Forking per target
// would throw both away and re-do them dozens of times. The bootchain half
// forks because the vendored patch code it drives 4x per target does not
// survive being reused; the ramdisk half drives one decrypt and one encrypt
// per target and has never shown that problem.
//
// `newestVersionCache` is memoized per device MODEL (not per (device,
// buildID) tuple) -- a full sweep bakes dozens of buildIDs for the same
// handful of device models, and newestVersionForDevice() is a live ipsw.me
// network call with no caching of its own. Without this, every tuple for the
// same model would re-fetch and get the identical answer.
static RamdiskOutcome bakeRamdiskForTarget(const std::string& device, const std::string& buildID,
                                            const std::string& entrypointBinaryPath, bool force,
                                            std::map<std::string, std::string>& newestVersionCache,
                                            std::string& noteOut) {
    const std::string label = device + " " + buildID;
    // Same flat convention as the bootchain half above, and the same Apple
    // manifest key: RestoreRamDisk, not "Ramdisk".
    const std::string outputPath = "dist/RestoreRamDisk-" + device + "_" + buildID + ".dmg";

    // Existence is the whole check -- see --force in this file's header for
    // why there is no staleness detection any more.
    if (!force && fs::exists(outputPath)) {
        printf("already baked, skipping (use --force to rebuild)\n");
        return RamdiskOutcome::Skipped;
    }

    IpswFetch fetcher;
    std::string firmwareURL = fetcher.firmwareURLForDevice(device, buildID);
    if (firmwareURL.empty()) {
        printf("WARNING: could not download ramdisk from apple.\n");
        fprintf(stderr, "%s: %s: no firmware URL from ipsw.me\n", kProg, label.c_str());
        addNote(noteOut, "ramdisk: no firmware URL");
        return RamdiskOutcome::DownloadFailed;
    }

    FragmentDownloader downloader(firmwareURL);
    if (!downloader.open()) {
        printf("WARNING: could not download ramdisk from apple.\n");
        fprintf(stderr, "%s: %s: could not open remote IPSW\n", kProg, label.c_str());
        addNote(noteOut, "ramdisk: remote IPSW unreachable");
        return RamdiskOutcome::DownloadFailed;
    }

    std::string workDir = ipswDataRoot() + "/" + device + "/" + buildID;
    fs::create_directories(workDir);

    std::string manifestPath = workDir + "/BuildManifest.plist";
    if (!downloader.downloadComponent("BuildManifest.plist", manifestPath, nullptr)) {
        printf("WARNING: could not download ramdisk from apple.\n");
        fprintf(stderr, "%s: %s: failed to download BuildManifest.plist\n", kProg, label.c_str());
        addNote(noteOut, "ramdisk: no BuildManifest");
        return RamdiskOutcome::DownloadFailed;
    }

    auto manifest = parseManifest(manifestPath);
    if (!manifest || manifest->restoreRamdiskPath.empty()) {
        printf("WARNING: could not download ramdisk from apple.\n");
        fprintf(stderr, "%s: %s: no RestoreRamDisk component in BuildManifest.plist\n", kProg, label.c_str());
        addNote(noteOut, "ramdisk: no RestoreRamDisk in manifest");
        return RamdiskOutcome::DownloadFailed;
    }

    std::string localRamdiskPath = workDir + "/" + fs::path(manifest->restoreRamdiskPath).filename().string();
    if (!downloader.downloadComponent(manifest->restoreRamdiskPath, localRamdiskPath, nullptr)) {
        printf("WARNING: could not download ramdisk from apple.\n");
        fprintf(stderr, "%s: %s: failed to download RestoreRamDisk\n", kProg, label.c_str());
        addNote(noteOut, "ramdisk: RestoreRamDisk download failed");
        return RamdiskOutcome::DownloadFailed;
    }

    // Deliberately keyed by the buildID from the .keys filename we're
    // already processing, not manifest->realBuildID -- they usually
    // agree, but if they ever don't, re-deriving the lookup from
    // realBuildID risks asking keysForDevice() for a .keys file other
    // than the one we started from.
    auto keys = fetcher.keysForDevice(device, buildID);
    auto it = keys.find("RestoreRamdisk");
    if (it == keys.end()) {
        printf("FAILED (no RestoreRamdisk key)\n");
        fprintf(stderr, "%s: %s: no RestoreRamdisk entry in .keys file\n", kProg, label.c_str());
        addNote(noteOut, "ramdisk: no RestoreRamdisk key");
        return RamdiskOutcome::BakeFailed;
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
                "%s: %s: ipsw.me lookup for %s's newest known version failed; falling back "
                "to this tuple's own BuildManifest.plist ProductVersion (less accurate — reflects the "
                "ramdisk vehicle's version, not the likely target device's)\n",
                kProg, label.c_str(), device.c_str());
        productVersion = manifest->productVersion;
        if (productVersion.empty()) {
            fprintf(stderr, "%s: %s: BuildManifest.plist fallback also had no ProductVersion\n", kProg,
                    label.c_str());
        }
    }

    bool sizeWarning = false;
    if (!bakeRamdisk(localRamdiskPath, it->second.key, it->second.iv, productVersion, outputPath,
                      entrypointBinaryPath, sizeWarning)) {
        printf("FAILED (bake)\n");
        fprintf(stderr, "%s: %s: bakeRamdisk() failed — see stderr above\n", kProg, label.c_str());
        addNote(noteOut, "ramdisk: bake failed");
        return RamdiskOutcome::BakeFailed;
    }

    if (sizeWarning) {
        printf("OK (with size warning, see stderr) -> %s\n", outputPath.c_str());
        return RamdiskOutcome::BakedWithSizeWarning;
    }
    printf("OK -> %s\n", outputPath.c_str());
    return RamdiskOutcome::Baked;
}

static const char* ramdiskMark(RamdiskOutcome outcome) {
    switch (outcome) {
        case RamdiskOutcome::NotRequested: return "-";
        case RamdiskOutcome::Skipped: return "skip";
        case RamdiskOutcome::Baked: return "ok";
        case RamdiskOutcome::BakedWithSizeWarning: return "ok*";
        case RamdiskOutcome::DownloadFailed: return "dl";
        case RamdiskOutcome::BakeFailed: return "FAIL";
    }
    return "?";
}

// ---------------------------------------------------------------------------

static void usage() {
    fprintf(stderr,
            "usage: bake-firmware [--device <model>] [--build <buildID>]\n"
            "                     [--only bootchain|ramdisk] [--bootchain-out <dir>]\n"
            "                     [--force] [--stop-early]\n");
}

int main(int argc, char** argv) {
    bool force = false;
    bool stopEarly = false;
    bool doBootchain = true;
    bool doRamdisk = true;
    std::string deviceFilter;
    std::string buildFilter;
    std::string bootchainOut = "dist";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (strcmp(argv[i], "--stop-early") == 0) {
            stopEarly = true;
        } else if (strcmp(argv[i], "--device") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --device requires a value (e.g. --device AppleTV3,2)\n", kProg);
                return 2;
            }
            deviceFilter = argv[++i];
        } else if (strcmp(argv[i], "--build") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --build requires a value (e.g. --build 12H606)\n", kProg);
                return 2;
            }
            buildFilter = argv[++i];
        } else if (strcmp(argv[i], "--only") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --only requires a value (bootchain or ramdisk)\n", kProg);
                return 2;
            }
            const char* what = argv[++i];
            if (strcmp(what, "bootchain") == 0) {
                doRamdisk = false;
            } else if (strcmp(what, "ramdisk") == 0) {
                doBootchain = false;
            } else {
                fprintf(stderr, "%s: --only takes 'bootchain' or 'ramdisk', not '%s'\n", kProg, what);
                return 2;
            }
        } else if (strcmp(argv[i], "--bootchain-out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --bootchain-out requires a value (e.g. --bootchain-out /tmp/bootchain)\n",
                        kProg);
                return 2;
            }
            bootchainOut = argv[++i];
        } else {
            fprintf(stderr, "%s: unrecognized argument %s\n", kProg, argv[i]);
            usage();
            return 2;
        }
    }

    TestByteOrder();

    auto targets = knownFirmwareTargets();
    if (targets.empty()) {
        fprintf(stderr, "%s: no .keys files found under %s\n", kProg, resolveImageKeyPath("").c_str());
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
            fprintf(stderr, "%s: no known (device, firmware) combinations for device %s\n", kProg,
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
            fprintf(stderr, "%s: no known (device, firmware) combinations for build %s\n", kProg,
                    buildFilter.c_str());
            return 1;
        }
    }

    printf("Found %zu known (device, firmware) combinations.\n", targets.size());
    printf("Baking: %s.\n", doBootchain && doRamdisk ? "bootchain and ramdisk"
                            : doBootchain           ? "bootchain only"
                                                    : "ramdisk only");
    // Root, unconditionally, checked here rather than per-half.
    //
    // The ramdisk half genuinely cannot work without it, and the reasons are
    // measured rather than assumed: chown() to root:wheel/mobile:staff returns
    // EPERM for a normal user; the pristine /sbin/launchd this splices into is
    // root-owned and mode 0555 on some firmwares, so even opening it O_WRONLY
    // fails with EACCES without root's mode bypass; and files created by a
    // non-root writer land owned by the invoking user, which would silently
    // bake a ramdisk whose entire tree belongs to uid 501. hdiutil itself needs
    // no privilege at all -- attach and resize both work fine unprivileged --
    // so it is specifically the ownership work that forces this.
    //
    // The bootchain half needs none of that, and this used to be split
    // accordingly. Requiring it for both is the deliberate choice: the two
    // halves run together by default, a bake takes minutes, and discovering
    // the requirement only when the ramdisk half starts means having already
    // paid for every download and patch. One uniform rule up front beats a
    // conditional one that lets a long run fail late. bakeRamdisk() keeps its
    // own identical check, since it is callable on its own.
    if (geteuid() != 0) {
        fprintf(stderr,
                "%s: must run as root (try: sudo %s ...)\n"
                "  The ramdisk half chown()s staged content to root:wheel and writes into\n"
                "  root-owned files on the mounted volume; neither is possible unprivileged.\n"
                "  hdiutil itself needs no privilege -- the ownership work does.\n",
                kProg, kProg);
        return 1;
    }
    printf("Running as root (required: staged content is chown()ed to root:wheel).\n");

    // Checked once, up front, rather than discovered per component after
    // every download and patch has already been paid for -- an unwritable
    // output directory otherwise surfaces as four identical unexplained
    // per-file errors.
    if (doBootchain) {
        std::error_code outEc;
        fs::create_directories(bootchainOut, outEc);
        if (outEc && !fs::is_directory(bootchainOut)) {
            fprintf(stderr, "\n%s: cannot create bootchain output directory %s: %s\n", kProg,
                    bootchainOut.c_str(), outEc.message().c_str());
            return 1;
        }
        printf("Bootchain output root: %s\n", bootchainOut.c_str());
    }

    std::string entrypointBinaryPath;
    if (doRamdisk) {
        std::error_code distEc;
        fs::create_directories("dist", distEc);
        if (distEc && !fs::is_directory("dist")) {
            fprintf(stderr, "\n%s: cannot create dist/: %s\n", kProg, distEc.message().c_str());
            return 1;
        }

        // Built once — the same binary gets spliced into every firmware's
        // ramdisk (see BakeRamdisk.hpp's bakeRamdisk() comment), so there's
        // no reason to pay for the cross-compile again per target. Skipped
        // entirely under --only bootchain, which is most of why that flag is
        // worth having.
        entrypointBinaryPath = buildEntrypointBinary();
        if (entrypointBinaryPath.empty()) {
            fprintf(stderr, "%s: failed to build entrypoint/ — see stderr above\n", kProg);
            return 1;
        }
    }
    printf("\n");

    // Memoized per device MODEL across the whole run; see
    // bakeRamdiskForTarget()'s own comment.
    std::map<std::string, std::string> newestVersionCache;

    std::vector<TargetResult> results;

    // Fail-fast, deliberately, for a ramdisk BAKE failure (as opposed to a
    // download failure): computeGlobalDebcacheOnce() (BakeRamdisk.cpp)
    // caches its result across every target in this run, so a real failure
    // in it (a broken dpkg/apt state, a bad .deb, etc.) isn't "this one
    // target had a problem" — every remaining target shares the exact same
    // cached failure and would fail identically. Continuing the loop just
    // re-demonstrates the same root cause dozens more times before reporting
    // it. Unlike the old bake-all-ramdisks this doesn't return straight out
    // of the loop, because that would also throw away the bootchain results
    // already collected: it breaks, prints the summary, and exits nonzero.
    //
    // Fetching the firmware itself is a different story: Apple pulling an
    // old build's signing, a flaky mirror, etc. only affects that one
    // target, so those are downgraded to a warning and the run moves on.
    bool fatalRamdiskFailure = false;

    for (size_t i = 0; i < targets.size(); i++) {
        const std::string& device = targets[i].first;
        const std::string& buildID = targets[i].second;

        printf("[%zu/%zu] %s %s\n", i + 1, targets.size(), device.c_str(), buildID.c_str());

        TargetResult result;
        result.device = device;
        result.buildID = buildID;

        if (doBootchain) {
            printf("  bootchain: ");
            fflush(stdout);
            result = bakeBootchainForked(device, buildID, bootchainOut, force);
        }

        if (doRamdisk) {
            printf("  ramdisk:   ");
            fflush(stdout);
            result.ramdisk =
                bakeRamdiskForTarget(device, buildID, entrypointBinaryPath, force, newestVersionCache, result.note);
        }

        results.push_back(result);

        if (result.ramdisk == RamdiskOutcome::BakeFailed) {
            printf("\nStopping: a ramdisk bake failure is shared by every remaining target (see above).\n");
            fatalRamdiskFailure = true;
            break;
        }
        if (stopEarly && !result.complete()) {
            printf("\n--stop-early: stopping at the first incomplete target.\n");
            break;
        }
    }

    // Per-component totals, not just a pass count: the point of running this
    // across every build is to see WHICH step fails WHERE. A run where every
    // iBEC fails and everything else succeeds says something very different
    // from one where a single build fails everything.
    size_t complete = 0, skipped = 0, noDownload = 0;
    size_t iBSSFail = 0, iBECFail = 0, kernelFail = 0, dtFail = 0;
    size_t ramdiskOk = 0, ramdiskWarned = 0, ramdiskSkipped = 0, ramdiskNoDownload = 0, ramdiskFailed = 0;
    for (const auto& r : results) {
        if (r.bootchainSkipped) skipped++;
        if (r.complete()) complete++;
        if (r.bootchainRequested) {
            if (!r.downloaded) noDownload++;
            if (r.downloaded && !r.iBSS) iBSSFail++;
            if (r.downloaded && !r.iBEC) iBECFail++;
            if (r.downloaded && !r.kernel) kernelFail++;
            if (r.downloaded && !r.deviceTree) dtFail++;
        }
        switch (r.ramdisk) {
            case RamdiskOutcome::Baked: ramdiskOk++; break;
            case RamdiskOutcome::BakedWithSizeWarning: ramdiskWarned++; break;
            case RamdiskOutcome::Skipped: ramdiskSkipped++; break;
            case RamdiskOutcome::DownloadFailed: ramdiskNoDownload++; break;
            case RamdiskOutcome::BakeFailed: ramdiskFailed++; break;
            case RamdiskOutcome::NotRequested: break;
        }
    }

    printf("\n%-14s %-10s %-6s %-6s %-8s %-11s %-8s %s\n", "device", "build", "iBSS", "iBEC", "kernel",
           "devicetree", "ramdisk", "note");
    for (const auto& r : results) {
        // A row where everything that was actually requested got skipped
        // says nothing; it is already counted in the "already built and
        // skipped" lines below.
        bool bootchainQuiet = !r.bootchainRequested || r.bootchainSkipped;
        bool ramdiskQuiet = r.ramdisk == RamdiskOutcome::NotRequested || r.ramdisk == RamdiskOutcome::Skipped;
        if (bootchainQuiet && ramdiskQuiet) continue;
        const char* dash = "-";
        printf("%-14s %-10s %-6s %-6s %-8s %-11s %-8s %s\n", r.device.c_str(), r.buildID.c_str(),
               !r.bootchainRequested ? dash : (r.downloaded ? mark(r.iBSS) : dash),
               !r.bootchainRequested ? dash : (r.downloaded ? mark(r.iBEC) : dash),
               !r.bootchainRequested ? dash : (r.downloaded ? mark(r.kernel) : dash),
               !r.bootchainRequested ? dash : (r.downloaded ? mark(r.deviceTree) : dash),
               ramdiskMark(r.ramdisk), r.note.c_str());
    }

    printf("\n%zu/%zu complete.\n", complete, results.size());
    if (doBootchain) {
        printf("Bootchain: %zu already built and skipped.\n", skipped);
        if (noDownload) printf("Bootchain: %zu could not be downloaded at all (not a patch failure).\n", noDownload);
        if (iBSSFail || iBECFail || kernelFail || dtFail) {
            printf("Bootchain patch failures among downloaded targets: iBSS %zu, iBEC %zu, kernel %zu, "
                   "devicetree %zu.\n",
                   iBSSFail, iBECFail, kernelFail, dtFail);
        }
        printf("Bootchain output: %s/<Component>-<device>_<buildID>\n", bootchainOut.c_str());
    }
    if (doRamdisk) {
        printf("Ramdisk: %zu baked", ramdiskOk + ramdiskWarned);
        if (ramdiskWarned) printf(" (%zu with a size warning, see stderr above)", ramdiskWarned);
        printf(", %zu already baked and skipped", ramdiskSkipped);
        if (ramdiskNoDownload) printf(", %zu skipped (could not download from apple, see stderr above)", ramdiskNoDownload);
        if (ramdiskFailed) printf(", %zu FAILED to bake", ramdiskFailed);
        printf(".\n");
        printf("Ramdisk output: dist/RestoreRamDisk-<device>_<buildID>.dmg\n");
    }

    // A target that could not be downloaded is not this tool's failure --
    // Apple pulling an old build, a flaky mirror. A target that downloaded
    // and then failed to patch or bake is, and that is what the exit status
    // reports, so a CI/scripted run can tell the two apart.
    bool anyRealFailure = iBSSFail || iBECFail || kernelFail || dtFail || ramdiskFailed || fatalRamdiskFailure;
    return anyRealFailure ? 1 : 0;
}
