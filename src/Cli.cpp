//
//  Cli.cpp
//  Blackb0x
//
//  See Cli.hpp for what this replaces. The overall flow (select a device,
//  make sure it's in DFU mode, run the model-appropriate exploit, download
//  firmware components for the right build, patch them, send them to the
//  device) is a direct, linear port of MainView.m's
//  jailbreakClick/tetherbootClick/checkExploit/downloadComponentsForBuildID/
//  componentsReady chain — the original's dispatch_async-driven UI updates
//  become plain sequential C++ (there's no GUI event loop to marshal onto),
//  and MainView's `spawnDFUHelper` popup (which just displayed instructions
//  and waited for the user to notice and re-click Jailbreak/Boot) becomes an
//  actual blocking poll loop, since a CLI has no button to re-click.
//

#include "Cli.hpp"

#include "Console.hpp"
#include "DeviceManager.hpp"
#include "IPSW.hpp"
#include "IPSWDownloader.hpp"
#include "Patcher.hpp"
#include "ResourcePath.hpp"

extern "C" {
#include <plist/plist.h>
}

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------

void printCliUsage(const char* argv0) {
    printf("Usage: %s [options]\n", argv0);
    printf("  --ecid <hex-or-decimal>   Pre-select a device by ECID (skips the menu)\n");
    printf("  --udid <udid>             Pre-select a device by UDID (Normal mode only)\n");
    printf("  --dry-run                 Do everything up to but not including the\n");
    printf("                            exploit and the USB upload to the device —\n");
    printf("                            prints what would run/be sent instead\n");
    printf("  --no-pwn                  Never attempt to run the pwntool\n");
    printf("                            (blackb0x-pwn) at all — if the connected device\n");
    printf("                            isn't already reporting a pwned DFU serial\n");
    printf("                            string, fail instead of attempting the exploit.\n");
    printf("                            For iterating on the post-exploit send flow\n");
    printf("                            against an already-pwned device without\n");
    printf("                            spawning a pwntool again.\n");
    printf("  --stock-ramdisk           DIAGNOSTIC: send the stock RestoreRamdisk exactly\n");
    printf("                            as downloaded from Apple, instead of the\n");
    printf("                            blackb0x-patched one -- to check whether a boot\n");
    printf("                            failure is in blackb0x's own ramdisk\n");
    printf("                            patching/entrypoint.c or earlier in the chain.\n");
    printf("                            The device will NOT be jailbroken by a run\n");
    printf("                            using this flag.\n");
    printf("  --stock-recovery          DIAGNOSTIC: send the stock iBSS/iBEC exactly\n");
    printf("                            as downloaded from Apple (still runs checkm8\n");
    printf("                            first -- SecureROM's own signature check still\n");
    printf("                            needs bypassing to accept any file at all -- but\n");
    printf("                            no boot-args/KASLR/ticket-check patches applied\n");
    printf("                            to the bootloader itself) -- to check whether a\n");
    printf("                            boot failure is in blackb0x's own iBSS/iBEC\n");
    printf("                            patches or elsewhere in the chain. REQUIRES\n");
    printf("                            --stock-firmware (refuses to start otherwise): the\n");
    printf("                            resulting stock iBEC still enforces real APTicket\n");
    printf("                            verification on whatever it loads next, and a real\n");
    printf("                            ticket can never authorize blackb0x's own patched\n");
    printf("                            kernel/ramdisk. The device will NOT be jailbroken\n");
    printf("                            by a run using this flag.\n");
    printf("  --stock-firmware          DIAGNOSTIC: send a stock kernelcache (no\n");
    printf("                            tfp0/AMFI/sandbox patches) and stock ramdisk\n");
    printf("                            (same as --stock-ramdisk) -- meaningful alone\n");
    printf("                            (keeps blackb0x's own patched iBSS/iBEC) to check\n");
    printf("                            whether blackb0x's own patched bootloader can\n");
    printf("                            still boot an otherwise-unmodified OS: if this\n");
    printf("                            boots fine, the iBSS/iBEC patches are confirmed OK\n");
    printf("                            and the failure is in blackb0x's own kernel/\n");
    printf("                            ramdisk patches specifically; if it fails the same\n");
    printf("                            way, the iBSS/iBEC patches themselves are\n");
    printf("                            implicated. Also required alongside\n");
    printf("                            --stock-recovery for a fully-stock suite end to\n");
    printf("                            end (see that flag's own entry for why). The\n");
    printf("                            device will NOT be jailbroken by a run using this\n");
    printf("                            flag.\n");
    printf("  --stock-securerom           DIAGNOSTIC: never attempt to run a pwntool, for a\n");
    printf("                            genuinely un-exploited device still running real,\n");
    printf("                            un-bypassed SecureROM signature enforcement --\n");
    printf("                            ERRORS if the device already reports PWND: in its\n");
    printf("                            serial string (contradicts what this flag is for),\n");
    printf("                            instead of skipping a pwntool. iBSS gets personalized\n");
    printf("                            with a real, ECID-bound SHSH ticket fetched from\n");
    printf("                            Apple's TSS server before being sent, and a combined\n");
    printf("                            APTicket covering everything after it (see\n");
    printf("                            Personalize.hpp) -- REQUIRES both --stock-recovery\n");
    printf("                            and --stock-firmware (refuses to start otherwise):\n");
    printf("                            those tickets are only ever valid for the exact,\n");
    printf("                            unmodified stock components, so anything blackb0x\n");
    printf("                            has patched can never pass.\n");
    printf("  --help                    Show this message\n");
    printf("\n");
    printf("blackb0x needs root by default: talking to a DFU/Recovery-mode device needs\n");
    printf("raw USB access, which the kernel restricts to root unless a udev rule grants\n");
    printf("it to your own user (see the README's own setup section). Run it via\n");
    printf("`sudo blackb0x ...` if you haven't set that up.\n");
}

CliOptions parseCliOptions(int argc, char** argv) {
    CliOptions options;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto nextArg = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", flag);
                exit(2);
            }
            return argv[++i];
        };

        if (arg == "--ecid") {
            options.ecid = strtoull(nextArg("--ecid").c_str(), nullptr, 0);
        } else if (arg == "--udid") {
            options.udid = nextArg("--udid");
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else if (arg == "--no-pwn") {
            options.noPwn = true;
        } else if (arg == "--stock-ramdisk") {
            options.stockRamdisk = true;
        } else if (arg == "--stock-recovery") {
            options.stockRecovery = true;
        } else if (arg == "--stock-firmware") {
            options.stockFirmware = true;
        } else if (arg == "--stock-securerom") {
            options.stockSecurerom = true;
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printCliUsage(argv[0]);
            exit(2);
        }
    }
    // --stock-firmware already implies stock ramdisk (see
    // downloadAndPatchComponents()'s own `stockRamdisk || stockFirmware`
    // check) -- not an error, just redundant, so warn rather than reject.
    // --stock-recovery is NOT redundant with --stock-firmware: --stock-
    // firmware deliberately keeps blackb0x's own patched iBSS/iBEC and
    // only stocks the kernel/ramdisk (isolating whether the *bootloader*
    // patches themselves are the problem); combining both flags is how
    // you get a fully-stock suite end to end, a real, distinct diagnostic
    // of its own, not a redundant restatement. --no-pwn is unrelated to
    // either -- it controls whether a pwntool runs at all.
    if (options.stockFirmware && options.stockRamdisk) {
        fprintf(stderr, "--stock-ramdisk is redundant with --stock-firmware\n");
    }
    // Opposite PWND-state requirements (noPwn hard-fails if NOT already
    // pwned; stockSecurerom hard-fails if it IS) -- not useful together,
    // and checkExploit() checks stockSecurerom first, then noPwn, so
    // combining them just means noPwn's hard-fail wins once stockSecurerom's
    // own check passes.
    if (options.noPwn && options.stockSecurerom) {
        fprintf(stderr,
                "--no-pwn and --stock-securerom require opposite device states (already pwned vs. not) -- "
                "combining them is not useful.\n");
    }
    return options;
}

// ---------------------------------------------------------------------------
// Device selection / DFU-mode wait
// ---------------------------------------------------------------------------

namespace {

// The original hardcoded this exact firmware build for a fresh jailbreak
// install (MainView.m's downloadInstall calling
// downloadComponentsForBuildID:@"10B329a") — the jailbreak patches are
// built/tested against this specific build, not a UI default to change
// lightly.
const std::string kJailbreakTargetBuild = "10B329a";

void printDeviceLine(const AppleTVDevice& d, int index) {
    printf("  [%d] %s (%llu) in %s", index, d.deviceModel.c_str(), (unsigned long long)d.ecid, d.mode.c_str());
    if (!d.version.empty()) printf(", %s", d.version.c_str());
    if (d.jailbroken) printf(", jailbroken");
    printf("\n");
}

// Blocks until at least one AppleTV is connected, then either auto-selects
// the one matching --ecid/--udid, the sole connected device, or prompts an
// interactive numbered menu — replacing MainView's icon click-to-select.
// Prints a one-time reminder if nothing shows up within 5 seconds: a device
// left mid-exploit by a failed checkm8/SHAtter attempt (some of its early
// failure paths return without ever calling irecv_reset()/irecv_close(),
// see docs/HISTORY.md) can end up wedged at the USB level and stop enumerating
// entirely until it's fully power-cycled, not just re-DFU'd.
std::optional<uint64_t> selectDevice(DeviceManager& deviceManager, const CliOptions& options) {
    printf("Waiting for an Apple TV 2 or 3 (any mode)...\n");
    auto waitStart = std::chrono::steady_clock::now();
    bool remindedToPowerCycle = false;
    for (;;) {
        auto devices = deviceManager.devicesSnapshot();

        if (!remindedToPowerCycle && std::chrono::steady_clock::now() - waitStart >= std::chrono::seconds(5)) {
            printf("Still searching... Please power-cycle your Apple TV after a failed exploit attempt.\n");
            remindedToPowerCycle = true;
        }

        if (options.ecid != 0) {
            for (auto& d : devices) {
                if (d.ecid == options.ecid) return d.ecid;
            }
        } else if (!options.udid.empty()) {
            for (auto& d : devices) {
                if (d.udid == options.udid) return d.ecid;
            }
        } else if (devices.size() == 1) {
            return devices[0].ecid;
        } else if (devices.size() > 1) {
            // printDeviceLine() reports whether each device is jailbroken,
            // which needs a real AFC handshake to know. Done here rather
            // than in the background, so the menu isn't printed before the
            // answers it shows are in. checkJailbreak() is a no-op after the
            // first call per device, so re-entering this loop is free.
            for (auto& d : devices) {
                if (d.mode == "Normal" && !d.udid.empty()) deviceManager.checkJailbreak(d.udid);
            }
            devices = deviceManager.devicesSnapshot();

            {
                // Scoped to the printing only. Holding the console lock
                // across the scanf() below would block a device-event thread
                // for as long as the user takes to answer.
                console::Block block;
                console::out("\nMultiple devices connected:\n");
                for (size_t i = 0; i < devices.size(); i++) printDeviceLine(devices[i], (int)i);
                console::out("Select a device number: ");
            }
            int choice = -1;
            if (scanf("%d", &choice) == 1 && choice >= 0 && (size_t)choice < devices.size()) {
                return devices[(size_t)choice].ecid;
            }
            console::out("Invalid selection, still waiting.\n");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// Replaces MainView's spawnDFUHelper — prints the same instructions, but
// actually polls for the transition rather than requiring the user to
// notice a popup and re-click a button, since a CLI has no button.
bool waitForDFUMode(DeviceManager& deviceManager, uint64_t ecid, AppleTVDevice& outDevice) {
    for (;;) {
        AppleTVDevice* d = deviceManager.deviceWithUDID("", ecid);
        if (d) {
            if (d->mode == "DFU") {
                outDevice = *d;
                return true;
            }
        } else {
            // Device with this ECID isn't currently connected at all (it may
            // be mid-reboot into DFU) — keep waiting rather than failing.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// Replaces MainView's checkExploit: — per-model exploit dispatch. Verbatim
// device-model branching from the original, not simplified. `dryRun` skips
// the actual SHAtter/checkm8 USB call (the point where this function stops
// being observation and starts writing exploit payloads into the device),
// printing what would have run instead. `noPwn`/`stockSecurerom` only gate
// the AppleTV3,2/checkm8 branch below (the one that actually spawns a
// pwntool) — SHAtter (AppleTV2,1) is a separate, hand-rolled exploit that
// never touches a pwntool at all, so there's nothing for these flags to
// refuse there. (`noPwn` was named noCheckm8/--no-checkm8; renamed once
// "pwntool" became this project's general term for the exploit binary,
// keeping its original hard-fail-if-not-already-pwned behavior.)
//
// stockSecurerom is checked before the early pwnedDFU return below, not
// after: its whole point is a genuinely un-exploited device (real,
// Apple-signed SecureROM DFU, not checkm8'd) -- if the device is already
// pwned, that contradicts the test setup this flag exists for, so it
// needs to error out even though checkExploit() would otherwise treat an
// already-pwned device as trivially "done" and return success.
bool checkExploit(DeviceManager& deviceManager, const AppleTVDevice& device, bool dryRun, bool noPwn,
                   bool stockSecurerom) {
    if (stockSecurerom && device.pwnedDFU) {
        fprintf(stderr,
                "--stock-securerom: device is already in pwned DFU (PWND: in its serial string) -- this "
                "flag requires a genuinely un-exploited device (real SecureROM signature enforcement "
                "still intact) to be a meaningful test. Re-enter DFU mode on a device that hasn't been "
                "pwned, or drop --stock-securerom.\n");
        return false;
    }

    if (device.pwnedDFU) return true;

    if (device.deviceModel == "AppleTV2,1") {
        if (dryRun) {
            printf("(dry run) Would try SHAtter\n");
            return true;
        }
        printf("Trying SHAtter...\n");
        if (deviceManager.SHAtter(device.ecid) == 0) {
            fprintf(stderr, "Exploit failed.\n");
            return false;
        }
        return true;
    }

    if (device.deviceModel == "AppleTV3,1") {
        fprintf(stderr,
                "AppleTV3,1 needs external hardware for checkm8: plug in an Arduino and use\n"
                "synackuk's checkm8 tool to put the device into pwned DFU first, then re-run\n"
                "blackb0x with --ecid %llu.\n",
                (unsigned long long)device.ecid);
        return false;
    }

    if (device.deviceModel == "AppleTV3,2") {
        if (noPwn) {
            fprintf(stderr,
                    "--no-pwn: device is not already in pwned DFU (no PWND: in its serial string) — "
                    "refusing to run checkm8. Pwn it separately first (`blackb0x-pwn checkm8`), or "
                    "drop --no-pwn to let blackb0x do it.\n");
            return false;
        }
        if (stockSecurerom) {
            // Already confirmed not pwned (the check at the top of this
            // function would have errored out otherwise) -- skip the
            // pwntool entirely and proceed straight into the rest of the
            // boot chain, relying on the device's own real, un-bypassed
            // SecureROM signature verification (backed by a real,
            // TSS-issued personalization ticket -- see
            // Personalize.hpp/sendiBSS()'s own comments) the whole way.
            // runCli() already hard-refuses --stock-securerom without
            // --stock-recovery before this ever runs, so there's nothing
            // left to caveat here.
            fprintf(stderr,
                    "--stock-securerom: device confirmed not already pwned -- skipping blackb0x-pwn "
                    "entirely and proceeding into the rest of the boot chain, relying on the device's "
                    "own real SecureROM signature verification.\n");
            return true;
        }
        if (dryRun) {
            printf("(dry run) Would try checkm8\n");
            return true;
        }
        printf("Trying checkm8...\n");
        if (deviceManager.checkm8(device.ecid) == 0) {
            fprintf(stderr, "Exploit failed.\n");
            return false;
        }
        return true;
    }

    fprintf(stderr, "Unrecognized device model: %s\n", device.deviceModel.c_str());
    return false;
}

// ---------------------------------------------------------------------------
// Firmware download + patch
// ---------------------------------------------------------------------------

// Non-blocking spawn of `bake-firmware --only ramdisk --device <deviceModel>
// --build <buildID>` for exactly the one (device, firmware) combination this
// run needs — returns the child's pid immediately without waiting for it to
// exit. Unlike DeviceManager.cpp's runLineBufferedSubprocess() (blocking:
// spawns, streams output, and waits for the child before returning), this
// needs to let the rest of downloadAndPatchComponents() keep running
// concurrently with the bake — see that function's own call site for why.
// Manual PATH search for an executable, matching the no-shell convention used
// elsewhere in this project (DeviceManager.cpp has the same helper).
static bool commandExistsOnPath(const char* name) {
    const char* pathEnv = getenv("PATH");
    if (!pathEnv) return false;
    std::string path(pathEnv);
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

// fork/exec, wait, report. No shell, same as everywhere else here.
static bool runForeground(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Failed to fork() for %s: %s\n", argv[0].c_str(), strerror(errno));
        return false;
    }
    if (pid == 0) {
        execvp(cargv[0], cargv.data());
        fprintf(stderr, "Cannot execute %s: %s\n", cargv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Which repository's CI artifacts to pull a prebuilt firmware suite from.
// Overridable so a fork, or a private mirror, does not need a code change.
static std::string artifactRepo() {
    if (const char* override_ = getenv("BLACKB0X_ARTIFACT_REPO")) return std::string(override_);
    return "regulad/Blackb0x--";
}

// Makes sure dist/ holds a complete baked suite for this tuple, by whichever
// of three routes is actually available. Returns false only when none of them
// can produce one.
//
// The ordering is about what the user has to supply, cheapest first:
//
//   1. Already baked. Nothing to do -- and nothing is ever re-fetched or
//      re-baked over an existing suite, so a hand-built dist/ always wins.
//   2. `gh` on PATH. Download the suite .github/workflows/ci.yml already
//      published. This is the normal end-user path and is the entire reason
//      that pipeline exists: no root, no Theos, no apt, no ~30-minute bake.
//   3. Running as root. Bake locally, which needs the whole authoring
//      toolchain but no network beyond Apple's own servers.
//
// Not root and no gh is the one combination that cannot work, and it gets a
// real explanation rather than a bare failure: the two things that would fix
// it are genuinely different (install a CLI vs. re-run under sudo) and the
// user has to pick.
//
// Deliberately synchronous. This used to fork a background bake and join it
// later, overlapping it with the download/patch pipeline -- but that pipeline
// is gone (blackb0x patches nothing now), so there is no concurrent work left
// to hide it behind, and a jailbreak silently turning into a multi-minute
// root-requiring bake was never a good surprise anyway.
static bool ensureBakedFirmware(const std::string& deviceModel, const std::string& buildID) {
    const std::string suffix = "-" + deviceModel + "_" + buildID;
    auto present = [&]() {
        return fs::exists("dist/Manifest" + suffix + ".txt") &&
               fs::exists("dist/RestoreRamDisk" + suffix + ".dmg");
    };

    if (present()) return true;

    printf("No baked firmware in dist/ for %s %s.\n", deviceModel.c_str(), buildID.c_str());
    fflush(stdout);

    if (commandExistsOnPath("gh")) {
        const std::string artifact = "firmware-" + deviceModel;
        printf("Fetching the prebuilt suite published by CI (%s, artifact %s)...\n",
               artifactRepo().c_str(), artifact.c_str());
        fflush(stdout);
        std::error_code mkEc;
        fs::create_directories("dist", mkEc);
        // `gh run download` with no run id takes the most recent run that has
        // an artifact by this name, which is what the monthly refresh
        // produces. It unpacks the artifact's contents directly into -D, and
        // the artifact is the flat dist/ layout already, so no rearranging.
        if (runForeground({"gh", "run", "download", "--repo", artifactRepo(), "-n", artifact, "-D", "dist"}) &&
            present()) {
            printf("Downloaded a complete suite for %s %s.\n", deviceModel.c_str(), buildID.c_str());
            return true;
        }
        fprintf(stderr,
                "The published artifact did not yield a complete suite for %s %s.\n"
                "  It may not have been baked for this device/build yet -- see the bake matrix in\n"
                "  .github/workflows/ci.yml.\n",
                deviceModel.c_str(), buildID.c_str());
    }

    if (geteuid() != 0) {
        fprintf(stderr,
                "\nCannot obtain a firmware suite for %s %s. Two ways forward:\n"
                "\n"
                "  Download one built by CI (no root, no toolchain):\n"
                "    install GitHub's CLI and sign in --  brew install gh && gh auth login\n"
                "    then re-run this command.\n"
                "\n"
                "  Or bake one yourself (needs root and the authoring tools):\n"
                "    cmake --build build --target authoring -j$(sysctl -n hw.ncpu)\n"
                "    sudo ./build/bake-firmware --device '%s' --build %s\n",
                deviceModel.c_str(), buildID.c_str(), deviceModel.c_str(), buildID.c_str());
        return false;
    }

    printf("Running as root and no gh available -- baking locally instead. This takes a few minutes.\n");
    fflush(stdout);
    if (!runForeground({resolveBakeFirmwarePath(), "--device", deviceModel, "--build", buildID}) || !present()) {
        fprintf(stderr, "bake-firmware did not produce a complete suite for %s %s.\n",
                deviceModel.c_str(), buildID.c_str());
        return false;
    }
    return true;
}

// ManifestInfo / parseManifest() now live in IPSW.hpp/.cpp — shared with
// bake-firmware (BakeFirmware.cpp), which needs the exact same
// BuildManifest.plist parsing to locate RestoreRamDisk across every known
// firmware, not just the one connected device this CLI flow targets.

// Replaces MainView's downloadComponentsForBuildID: — downloads every
// component sequentially (see the note in Cli.hpp/this file's header
// comment on why this isn't parallelized like the original's dispatch_async
// fire-and-forget) into ipswDataRoot(), patching each immediately after it
// lands (matching the original's setXPath: custom setters, which triggered
// the same patch* calls as a side effect of assignment).
std::optional<PatchedComponents> downloadAndPatchComponents(Patcher& patcher, const AppleTVDevice& device,
                                                              const std::string& buildToRequest,
                                                              bool stockRamdisk,
                                                              bool stockRecovery, bool stockFirmware,
                                                              bool stockSecurerom) {

    printf("Downloading firmware for %s %s...\n", device.deviceModel.c_str(), buildToRequest.c_str());
    IpswFetch fetcher;
    std::string firmwareURL = fetcher.firmwareURLForDevice(device.deviceModel, buildToRequest);
    if (firmwareURL.empty()) {
        fprintf(stderr, "Could not resolve a firmware URL for %s %s\n", device.deviceModel.c_str(),
                buildToRequest.c_str());
        return std::nullopt;
    }

    FragmentDownloader downloader(firmwareURL);
    if (!downloader.open()) {
        fprintf(stderr, "Failed to open remote IPSW\n");
        return std::nullopt;
    }

    std::string workDir = ipswDataRoot() + "/" + device.deviceModel + "/" + buildToRequest;
    fs::create_directories(workDir);

    std::string manifestPath = workDir + "/BuildManifest.plist";
    if (!downloader.downloadComponent("BuildManifest.plist", manifestPath, nullptr)) {
        fprintf(stderr, "Failed to download BuildManifest.plist\n");
        return std::nullopt;
    }

    auto manifest = parseManifest(manifestPath);
    if (!manifest) {
        fprintf(stderr, "Failed to parse BuildManifest.plist\n");
        return std::nullopt;
    }

    patcher.loadKeysForDevice(device.deviceModel, manifest->realBuildID);
    patcher.setBuildIdentity(manifest->buildIdentity);
    patcher.setBuildID(manifest->realBuildID);

    // Resolve the firmware suite before touching any component, so a missing
    // one fails here rather than several layers down as a generic "component
    // not patched". ensureBakedFirmware() does the real work: already-present,
    // else downloaded from CI, else baked locally if root -- see its own
    // comment.
    //
    // This used to kick off a background bake here and waitpid() it much later,
    // overlapping it with this function's own downloads and patches. That
    // pipeline is gone (blackb0x patches nothing now), so there is nothing left
    // to overlap, and a synchronous call says plainly what is happening.
    // A bake is needed whenever ANY component below comes from dist/ rather
    // than a straight IPSW download -- i.e. any component blackb0x keeps in
    // its patched form. Only a fully-stock suite (--stock-firmware AND
    // --stock-recovery together, the sole way every one of
    // iBSS/iBEC/kernel/ramdisk/DeviceTree is taken from Apple's IPSW
    // verbatim) needs nothing from dist/, so it alone skips the bake. In
    // every other mode at least the patched bootloader (iBSS/iBEC, when
    // !stockRecovery) or the patched kernel/ramdisk (when !stockFirmware)
    // still comes from the bake -- including --stock-firmware ALONE, which
    // deliberately keeps blackb0x's own patched iBSS/iBEC (they exist only
    // in dist/). The old gate here (`!stockRamdisk && !stockFirmware`)
    // wrongly skipped the bake for --stock-firmware too, so its takeBaked()
    // iBSS/iBEC/DeviceTree calls hit a dist/ that was never produced.
    bool fullyStock = stockFirmware && stockRecovery;
    if (!fullyStock && !ensureBakedFirmware(device.deviceModel, manifest->realBuildID)) {
        return std::nullopt;
    }

    std::optional<PatchedComponents> result;
    patcher.onComponentsReady = [&](const PatchedComponents& c) { result = c; };

    // required=false for the genuinely-optional components (RestoreLogo,
    // loaded-by-iBoot entries -- some builds' manifests just don't have
    // them, see their own call sites below): silently skipping those is
    // correct, expected behavior, not a bug. For every other (required)
    // component, an empty remotePath means BuildManifest.plist itself
    // has no Path for it at all -- a real, surprising problem
    // (Patcher::missingRequiredComponents() would otherwise report this
    // component as "missing" with no explanation anywhere in the output
    // at all, since neither the download-failure nor the patch-failure
    // branches below ever ran).
    auto downloadAndPatch = [&](const char* label, const std::string& remotePath, auto&& patchFn,
                                 bool required = true) {
        if (remotePath.empty()) {
            if (required) {
                fprintf(stderr,
                        "%s: BuildManifest.plist has no Path for this component -- can't download it at all.\n",
                        label);
            }
            return;
        }
        std::string localPath = workDir + "/" + fs::path(remotePath).filename().string();
        printf("Downloading %s...\n", label);
        // The underlying progress callback fires far more often than the
        // percentage actually changes (once per chunk received, not once
        // per percentage point) — without tracking the last value printed,
        // "pct % 25 == 0" reprints the same "0%"/"25%"/etc line every time
        // a chunk happens to land while still at that percentage, which is
        // most of them.
        unsigned int lastPrinted = 101;
        if (!downloader.downloadComponent(remotePath, localPath, [label, &lastPrinted](unsigned int pct) {
                if (pct % 25 == 0 && pct != lastPrinted) {
                    printf("  %s: %u%%\n", label, pct);
                    lastPrinted = pct;
                }
            })) {
            fprintf(stderr, "Failed to download %s\n", label);
            return;
        }
        patchFn(localPath);
    };

    // stockRecovery, not stockFirmware, gates iBSS/iBEC: --stock-firmware
    // alone deliberately keeps blackb0x's own patched bootloader and only
    // stocks the kernel/ramdisk it hands off to (see that flag's own
    // comment in Cli.hpp). --stock-recovery is the complementary test
    // (stock bootloader too) -- runCli() requires --stock-firmware
    // alongside it (a stock bootloader's own real APTicket verification
    // can never authorize blackb0x's own patched kernel/ramdisk), so in
    // practice --stock-recovery never appears here without
    // --stock-firmware also stocking the rest of the suite.
    // blackb0x does not patch. Anything not explicitly being stocked comes out
    // of dist/ exactly as bake-firmware produced it, which is what removed
    // iBoot32Patcher and CBPatcher from this binary entirely -- patchiBSS(),
    // patchiBEC() and patchKernel() were their only callers here.
    //
    // The IPSW downloader stays, and is still reached: a --stock-* run needs
    // Apple's own unmodified component for whichever piece it is stocking, and
    // that can only come from the IPSW. So those branches download (populating
    // the usual cache under ipswDataRoot()) while every other component is
    // taken from the bake.
    const std::string bakedSuffix = "-" + device.deviceModel + "_" + manifest->realBuildID;
    auto takeBaked = [&](const char* label, const std::string& name, auto&& setter) {
        const std::string path = "dist/" + name + bakedSuffix;
        if (!fs::exists(path)) {
            fprintf(stderr,
                    "%s: no baked component at %s.\n"
                    "  Bake it first:\n"
                    "    sudo ./build/bake-firmware --device '%s' --build %s\n",
                    label, path.c_str(), device.deviceModel.c_str(), manifest->realBuildID.c_str());
            return;
        }
        setter(path);
    };

    if (stockRecovery) {
        downloadAndPatch("iBSS", manifest->iBSSPath,
                          [&](const std::string& path) { patcher.useStockIBSS(path, stockSecurerom); });
        downloadAndPatch("iBEC", manifest->iBECPath,
                          [&](const std::string& path) { patcher.useStockIBEC(path); });
    } else {
        takeBaked("iBSS", "iBSS", [&](const std::string& p) { patcher.setBakedIBSSPath(p); });
        takeBaked("iBEC", "iBEC", [&](const std::string& p) { patcher.setBakedIBECPath(p); });
    }

    if (stockFirmware) {
        downloadAndPatch("KernelCache", manifest->kernelCachePath,
                          [&](const std::string& path) { patcher.useStockKernel(path, stockRecovery); });
    } else {
        takeBaked("KernelCache", "KernelCache", [&](const std::string& p) { patcher.setBakedKernelPath(p); });
    }

    // DeviceTree is sent unmodified (see Patcher::setDeviceTreePath()), so
    // Apple's own IPSW copy and bake-firmware's published copy are byte-for-
    // byte identical. Prefer the baked one when it's there (the jailbreak and
    // --stock-firmware paths, where dist/ was just resolved above), but fall
    // back to downloading it straight from the IPSW: a fully-stock run
    // (--stock-firmware --stock-recovery) skips the bake entirely, so there
    // is no dist/DeviceTree to take. DeviceTree was previously the ONLY
    // required component with no download path at all, which is exactly why
    // that route failed with a hard "missing DeviceTree". Same
    // bake-first/download-fallback shape as RestoreLogo just below.
    const std::string bakedDeviceTree = "dist/DeviceTree" + bakedSuffix;
    if (fs::exists(bakedDeviceTree)) {
        patcher.setDeviceTreePath(bakedDeviceTree);
    } else {
        downloadAndPatch("DeviceTree", manifest->deviceTreePath,
                          [&](const std::string& path) { patcher.setDeviceTreePath(path); });
    }

    // required=false: confirmed genuinely optional, not just "usually
    // present" -- idevicerestore's own recovery_send_applelogo() checks
    // build_identity_has_component() first and returns success outright
    // if the manifest doesn't have one at all (see
    // ManifestInfo::restoreLogoPath's own comment). downloadAndPatch()
    // skips the callback entirely when the path is empty either way, so
    // this is a silent no-op wherever it's absent -- correctly so here.
    //
    // Preferred from the bake, which publishes it verbatim, and downloaded only
    // as a fallback -- a dist/ produced before bake-firmware started publishing
    // the unmodified components will not have it.
    if (fs::exists("dist/RestoreLogo" + bakedSuffix)) {
        patcher.setRestoreLogoPath("dist/RestoreLogo" + bakedSuffix);
    } else {
        downloadAndPatch(
            "RestoreLogo", manifest->restoreLogoPath,
            [&](const std::string& path) { patcher.setRestoreLogoPath(path); },
            /*required=*/false);
    }

    // Almost always empty (see ManifestInfo::loadedByIBootComponents' own
    // comment) -- one downloadAndPatch() call per manifest entry flagged
    // Info.IsLoadedByiBoot, matching idevicerestore's own generic
    // iteration instead of a fixed component list.
    // Same bake-first, download-as-fallback rule as RestoreLogo above.
    for (const auto& [name, remotePath] : manifest->loadedByIBootComponents) {
        const std::string baked = "dist/" + name + bakedSuffix;
        if (fs::exists(baked)) {
            patcher.addLoadedByIBootComponent(name, baked);
            continue;
        }
        downloadAndPatch(name.c_str(), remotePath,
                          [&](const std::string& path) { patcher.addLoadedByIBootComponent(name, path); });
    }

    // onlyBootComponents is gone with the tether-boot path -- this was
    // its only guard here, and it is now unconditionally taken.
    if (stockRamdisk || stockFirmware) {
        // Diagnostic routes: useStockRamdisk() genuinely needs the
        // downloaded file (see its own comment) -- unchanged from
        // before.
        downloadAndPatch("RestoreRamdisk", manifest->restoreRamdiskPath,
                          [&](const std::string& path) { patcher.useStockRamdisk(path, stockRecovery); });
    } else {
        // Real jailbreak path: nothing to download here at all. The suite
        // was resolved up front by ensureBakedFirmware() -- downloaded from
        // CI, baked locally as root, or already present -- so this only has
        // to point patchRamdisk() at it.
        patcher.patchRamdisk();
    }

    if (!result) {
        std::vector<std::string> missing = patcher.missingRequiredComponents();
        std::string joined;
        for (size_t i = 0; i < missing.size(); i++) joined += (i ? ", " : "") + missing[i];
        // Not "...patched successfully" -- on any --stock-* route
        // nothing here actually patches anything (useStockIBSS()/
        // useStockIBEC()/etc. just decrypt or pass the original file
        // through untouched), so that wording pointed at the wrong half
        // of the problem when the real cause was e.g. a missing
        // decryption key or a download failure instead.
        fprintf(stderr,
                "Not all required components were prepared successfully -- still missing: %s. Look further up "
                "for the actual reason that component's own step failed (a download failure prints \"Failed "
                "to download <name>\"; a missing decryption key prints \"no ... keys loaded\"; an actual patch "
                "failure -- only possible on a non-stock route -- prints its own iBootPatcher()/patch_kernel() "
                "error).\n",
                joined.empty() ? "(nothing? this shouldn't happen)" : joined.c_str());
        return std::nullopt;
    }
    return result;
}

// Replaces MainView's componentsReady: — the final upload sequence. Sends
// iBSS first; aborts back to a fresh DFU wait if that fails (matching the
// original's "spawn the DFU helper again" recovery path).
//
// There is exactly one flow now: iBSS -> iBEC -> RestoreLogo -> Ramdisk ->
// DeviceTree -> KernelCache('bootx'). The original app also had a
// tether-boot variant that skipped Ramdisk/DeviceTree and booted the
// installed OS off NAND (its `self.selected_device.jailbroken == 1`
// branch); that whole path is gone -- see docs/HISTORY.md.
bool sendComponentsToDevice(DeviceManager& deviceManager, AppleTVDevice& device, const PatchedComponents& components,
                             bool dryRun, bool stockRecovery, bool stockSecurerom) {
    if (dryRun) {
        printf("(dry run) Would send:\n");
        printf("  iBSS%s\n", components.iBSS ? "" : " (missing, would fail here)");
        printf("  iBEC%s\n", components.iBEC ? "" : " (missing)");
        if (components.restoreLogo) printf("  RestoreLogo\n");
        printf("  Ramdisk%s\n", components.ramdisk ? "" : " (missing)");
        printf("  DeviceTree%s\n", components.deviceTree ? "" : " (missing)");
        printf("  KernelCache%s\n", components.kernel ? "" : " (missing, would fail here)");
        printf("(dry run) Would then wait for the Apple TV to reboot\n");
        return true;
    }

    // Whole lines, never "Sending X -> " followed by a bare "Sent" once the
    // send returns: each of these sends takes seconds, during which the
    // device re-enumerates and get_tv() reports its reconnect attempts, so
    // an unterminated line left open across that span got everything else
    // spliced onto the end of it ("Sending iBSS -> Disconnected (123)").
    console::out("Sending iBSS...\n");
    if (deviceManager.sendiBSS(*components.iBSS, device.ecid, stockRecovery, stockSecurerom,
                                components.buildIdentity, device.deviceModel, components.buildID) != 0) {
        console::err("Failed to send iBSS. Please re-enter DFU mode and try again.%s\n",
                stockSecurerom ? " (--stock-securerom personalizes the image with a real TSS-issued SHSH ticket "
                               "before sending it via the standard DFU route -- if this still fails, it's a "
                               "real signal about the image/device/firmware match itself, not an artifact of "
                               "this tool's own delivery mechanism)"
                             : "");
        return false;
    }
    console::out("iBSS sent.\n");
    // iBSS running successfully means the device is about to reboot and
    // re-enumerate in Recovery mode -- sendiBEC() below (get_tv_patient())
    // blocks retrying for up to ~30s waiting for exactly that, with no
    // status of its own in between. Without this, that whole window reads
    // as silently stuck rather than an expected, normal wait.
    console::out("Waiting for device to come back up in Recovery mode...\n");

    // Gated on stockRecovery, NOT stockSecurerom: this is about whether
    // useStockIBEC()'s genuinely-unpatched iBEC is what's running, not
    // whether checkm8 ran to get there. patch_ticket_check() (patchiBEC(),
    // applied by default) is what makes a ticket unnecessary, and it only
    // ever runs against blackb0x's own patched iBEC -- stockRecovery's
    // stock iBEC has no such patch applied regardless of stockSecurerom, so
    // it enforces real ticket verification on whatever it loads next
    // (DeviceTree/Ramdisk/KernelCache) either way.
    //
    // Everything from the ticket through KernelCache runs as ONE call
    // (DeviceManager::sendStockRestoreTail()) on a single persistent
    // connection when stockRecovery is set, rather than this function's
    // usual reconnect-per-step calls -- confirmed directly on real
    // hardware that reconnecting right after the ticket specifically
    // (not after any of the OTHER resets in this chain) drops the device
    // all the way back to DFU mode, matching real idevicerestore's own
    // recovery_enter_restore(), which never closes its connection across
    // this same span either. See sendStockRestoreTail()'s own comment.
    auto sendStockTail = [&]() -> bool {
        const char* what = "APTicket + RestoreLogo + Ramdisk + DeviceTree + KernelCache";
        console::out("Sending %s...\n", what);
        int i = deviceManager.sendStockRestoreTail(device.ecid, components, device.deviceModel);
        if (i != 0) {
            console::err("Failed to send the post-iBEC stock restore sequence. Re-enter DFU mode and try "
                         "again.\n");
            return false;
        }
        console::out("%s sent.\n", what);
        return true;
    };

    console::out("Sending iBEC...\n");
    int i = components.iBEC ? deviceManager.sendiBEC(*components.iBEC, device.ecid) : -1;
    if (i != 0) {
        console::err("Failed to send iBEC -- the device may have rebooted out of the exploited state\n"
                     "instead of staying put (see any reconnect-attempt lines above for detail). Re-enter DFU\n"
                     "mode and try again.\n");
        return false;
    }
    console::out("iBEC sent.\n");

    if (stockRecovery) {
        if (!sendStockTail()) return false;
        device.needsPostInstall = 1;
        device.waitForRecovery = 1;
        console::out("Waiting for Apple TV to reboot\n");
        return true;
    }

    console::out("Sending Ramdisk...\n");
    i = components.ramdisk ? deviceManager.sendRamdisk(*components.ramdisk, device.ecid) : -1;
    if (i != 0) {
        console::err("Failed to send Ramdisk. Re-enter DFU mode and try again.\n");
        return false;
    }
    console::out("Ramdisk sent.\n");

    console::out("Sending DeviceTree...\n");
    i = components.deviceTree ? deviceManager.sendDeviceTree(*components.deviceTree, device.ecid) : -1;
    if (i != 0) {
        console::err("Failed to send DeviceTree. Re-enter DFU mode and try again.\n");
        return false;
    }
    console::out("DeviceTree sent.\n");

    device.needsPostInstall = 1;

    // Only reached when stockRecovery is unset -- sendStockTail() above
    // already includes KernelCache and returns directly otherwise.
    //
    // A Ramdisk and DeviceTree were just sent, so the kernel boots from the
    // ramdisk: sendKernelCache() sets `rd=md0` unconditionally now. The old
    // tether-boot path was the only caller that wanted anything else, and it
    // had the two boot-args sets wired up backwards anyway -- the compiled-in
    // args WITH rd=md0 went into the iBEC that sent no ramdisk, and the ones
    // WITHOUT it into this path, so entrypoint.c could never have run as
    // PID 1. See docs/HISTORY.md.
    console::out("Sending KernelCache...\n");
    int kernelResult = components.kernel ? deviceManager.sendKernelCache(*components.kernel, device.ecid) : -1;
    if (kernelResult != 0) {
        console::err("Failed to send KernelCache.\n");
        return false;
    }
    console::out("KernelCache sent.\n");

    device.waitForRecovery = 1;
    console::out("Waiting for Apple TV to reboot\n");
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Top-level session
// ---------------------------------------------------------------------------

int runCli(const CliOptions& options) {
    console::init();

    if (options.help) {
        printCliUsage("blackb0x");
        return 0;
    }

    // --stock-securerom without --stock-recovery would send blackb0x's own
    // PATCHED iBSS through personalizeIMG3Component() (Personalize.cpp) --
    // the TSS ticket that fetches is only ever valid for the exact,
    // unmodified component digest BuildManifest.plist lists, so stitching
    // it into anything blackb0x has patched can never pass a real
    // SecureROM's verification, regardless of how correctly everything
    // else here behaves. Same reasoning transitively requires
    // --stock-firmware too (see that check just below) -- checked
    // directly here as well, rather than only relying on that second
    // check to catch it, so this specific combination gets a message
    // that actually names --stock-securerom as the reason. Refuse outright
    // rather than attempting (and failing) a combination that can never
    // do anything else.
    if (options.stockSecurerom && !(options.stockRecovery && options.stockFirmware)) {
        fprintf(stderr,
                "--stock-securerom requires both --stock-recovery and --stock-firmware: personalizing "
                "anything other than the unmodified, stock iBSS/iBEC/kernel/ramdisk against a real TSS "
                "ticket can never pass a genuine SecureROM's signature check. Pass --stock-recovery "
                "--stock-firmware --stock-securerom together.\n");
        return 1;
    }

    // --stock-recovery's own stock iBEC (useStockIBEC(), never patched --
    // patch_ticket_check() only ever runs against blackb0x's own patched
    // iBEC) enforces real APTicket verification on whatever it loads
    // next, checkm8 or not (see DeviceManager::sendStockRestoreTail()'s
    // own comment). A real APTicket only ever authorizes the exact,
    // unmodified component digests BuildManifest.plist lists -- blackb0x's
    // own patched kernel/ramdisk (patchKernel()/patchRamdisk(), what
    // --stock-recovery without --stock-firmware would still send) have
    // different digests by definition, so a stock iBEC can never accept
    // them regardless of which build gets requested. Refuse outright
    // rather than attempting (and failing) a combination that can never
    // do anything else -- this also sidesteps a real, previously-silent
    // failure mode: --stock-recovery alone still points buildToRequest at
    // "latest" (needed for the ticket itself), but patchKernel()/
    // patchRamdisk() have zero support for whatever build that resolves
    // to (no keys/ entry, no baked dist/ ramdisk), so they
    // fail with no output component set at all, surfacing several layers
    // away as a generic "Not all required components patched
    // successfully".
    if (options.stockRecovery && !options.stockFirmware) {
        fprintf(stderr,
                "--stock-recovery requires --stock-firmware: a stock iBEC verifies a real APTicket against "
                "the exact, unmodified component digests BuildManifest.plist lists, and blackb0x's own "
                "patched kernel/ramdisk can never match those regardless of which build gets requested. Pass "
                "--stock-recovery --stock-firmware together.\n");
        return 1;
    }

    printf("blackb0x (regulad's portable port) — Apple TV 2/3 jailbreak tool\n");

    if (options.dryRun) {
        printf("(dry run) Discovery, DFU wait, download, and patch all happen for real.\n");
        printf("(dry run) Only the exploit and the USB upload are skipped.\n\n");
    }
    fflush(stdout);

    // No hard root requirement: raw DFU/Recovery-mode USB access is a kernel
    // device-node permission, not something this process can determine in
    // advance for every possible udev/group setup. Running as root always
    // works; running as a normal user works too, given the right udev rule
    // (see README's own setup section) — either way, libirecovery's own
    // device-open calls are what actually surface a real permission
    // error, with a real errno behind it, if access genuinely isn't there.

    // Nothing this tool can ever do succeeds without a baked ramdisk sitting
    // in dist/ for whichever specific device+firmware turns out to be
    // needed — patchRamdisk() (Patcher.cpp) checks for that specific entry
    // once a device is actually connected and its firmware is known. An
    // entirely empty dist/ here used to always mean bake-firmware was
    // simply never run at all, worth failing on immediately rather than
    // waiting for a device to show up first — but that's no longer true:
    // this process can always bake one itself, so an empty dist/ is
    // recoverable on demand later (downloadAndPatchComponents() in this file
    // bakes exactly the one tuple actually needed, once a device is
    // connected and its firmware resolved). There is nothing to fail on
    // here any more; this only says what is about to happen.
    {
        bool haveAnyRamdisk = false;
        std::error_code ec;
        if (fs::exists("dist", ec) && fs::is_directory("dist", ec)) {
            for (const auto& entry : fs::directory_iterator("dist", ec)) {
                if (ec) break;
                if (entry.path().extension() == ".dmg") {
                    haveAnyRamdisk = true;
                    break;
                }
            }
        }
        if (!haveAnyRamdisk) {
            printf(
                "dist/ has no baked ramdisks yet -- will bake whatever this run specifically needs on demand "
                "once a device is connected and its firmware build is known.\n");
            fflush(stdout);
        }
    }

    // Collapses the original Blackb0x.h/.m singleton (which just held one
    // DeviceManager) — this session IS that single instance, function-local
    // rather than a dispatch_once class method.
    DeviceManager deviceManager;
    Patcher patcher;

    // One line per real CHANGE, not per event — this is the only place
    // device connection/exploit status gets printed; DeviceManager itself
    // stays quiet on success and only writes to stderr for genuine,
    // otherwise-unexplained failures.
    //
    // Per-event was too noisy to read: the boot chain re-enumerates the
    // device deliberately, several times, and DeviceManager re-reports it in
    // full each time — so "is jailbroken" got reprinted on every reset, while
    // a mode change (the one thing worth seeing) printed nothing at all,
    // leaving a bare "Disconnected" with no matching reconnect. Tracking what
    // was last said about each device fixes both ends of that.
    //
    // Held in a shared_ptr rather than a plain local: these lambdas run on
    // libirecovery's and libimobiledevice's event threads, which are not
    // torn down when runCli() returns.
    struct AnnouncedState {
        std::string mode;
        int jailbroken = -1;
        int jailbreakRunning = -1;
        bool sshHintShown = false;
    };
    struct Announced {
        std::mutex mutex;
        std::map<uint64_t, AnnouncedState> byEcid;
    };
    auto announced = std::make_shared<Announced>();

    DeviceEventSink sink;
    sink.onDeviceAdded = [announced](const AppleTVDevice& d) {
        {
            std::lock_guard<std::mutex> lock(announced->mutex);
            announced->byEcid[d.ecid].mode = d.mode;
        }
        console::out("Connected to %s (%llu) in %s mode.\n", d.deviceModel.c_str(),
                     (unsigned long long)d.ecid, d.mode.c_str());
    };
    sink.onDeviceRemoved = [](uint64_t ecid, const std::string& udid) {
        // A usbmuxd-side removal carries a UDID and no ECID at all --
        // DeviceManager's disconnectDevice() passes (uint64_t)-1 for it,
        // which this used to print verbatim as 18446744073709551615.
        if (ecid == (uint64_t)-1) console::out("Disconnected (%s).\n", udid.c_str());
        else console::out("Disconnected (%llu).\n", (unsigned long long)ecid);
    };
    sink.onStatus = [](const std::string& status) { console::out("%s\n", status.c_str()); };
    sink.onDeviceUpdated = [announced](const AppleTVDevice& d) {
        // Composed under announced->mutex but printed outside it: console's
        // own lock is the one that has to be held across a whole block, and
        // nesting the two in opposite orders elsewhere would be a deadlock
        // waiting to happen.
        std::string modeLine, jailbreakLine, sshLine;
        {
            std::lock_guard<std::mutex> lock(announced->mutex);
            AnnouncedState& state = announced->byEcid[d.ecid];
            if (!d.mode.empty() && d.mode != state.mode) {
                state.mode = d.mode;
                modeLine = d.deviceModel + " (" + std::to_string(d.ecid) + ") is now in " + d.mode + " mode.";
            }
            if (d.jailbroken != state.jailbroken || d.jailbreakRunning != state.jailbreakRunning) {
                state.jailbroken = d.jailbroken;
                state.jailbreakRunning = d.jailbreakRunning;
                if (d.jailbroken) {
                    jailbreakLine = d.deviceModel + " is jailbroken" +
                                    (d.jailbreakRunning == 1 ? " and running." : ".");
                }
            }
            if (d.jailbreakRunning == 1 && !state.sshHintShown) {
                state.sshHintShown = true;
                sshLine = "Run scripts/push_authorized_keys.sh if you want SSH access to " + d.deviceModel + ".";
            }
        }
        console::Block block;
        if (!modeLine.empty()) console::out("%s\n", modeLine.c_str());
        if (!jailbreakLine.empty()) console::out("%s\n", jailbreakLine.c_str());
        if (!sshLine.empty()) console::out("%s\n", sshLine.c_str());
    };
    deviceManager.setEventSink(sink);

    auto ecidOpt = selectDevice(deviceManager, options);
    if (!ecidOpt) {
        fprintf(stderr, "No device selected.\n");
        return 1;
    }
    uint64_t ecid = *ecidOpt;

    AppleTVDevice device;
    {
        AppleTVDevice* d = deviceManager.deviceWithUDID("", ecid);
        if (!d) {
            fprintf(stderr, "Selected device disconnected before it could be used.\n");
            return 1;
        }
        device = *d;
    }

    // Has to happen here, synchronously, and only while the device is still
    // in Normal mode: it needs a lockdownd/AFC handshake, which nothing can
    // do once the device is sitting in DFU. device.jailbroken decides which
    // build gets requested further down, so it has to be a settled answer by
    // then rather than whatever a background check happened to have written
    // by the time that line ran.
    if (device.mode == "Normal" && !device.udid.empty()) {
        deviceManager.checkJailbreak(device.udid);
        if (AppleTVDevice* d = deviceManager.deviceWithUDID("", ecid)) device = *d;
    }

    if (device.mode != "DFU") {
        {
            // Instructions the user has to follow step by step -- a device
            // event landing in the middle of them makes them much harder to
            // read than an extra line after them does.
            console::Block block;
            console::out("\nTo enter DFU mode, on the Apple TV's remote:\n\n");
            console::out("  1. Hold MENU + DOWN together until the LED starts flashing rapidly\n");
            console::out("     (~6 seconds), then let go of BOTH buttons completely.\n");
            console::out("     This alone only reaches Recovery Mode, which blinks the same\n");
            console::out("     way DFU does -- it is not DFU mode yet.\n");
            console::out("  2. Immediately hold MENU + PLAY together until the LED starts\n");
            console::out("     flashing rapidly again (~6-7 seconds), then let go. This second\n");
            console::out("     step is what actually puts it in DFU mode.\n\n");
            console::out("If this repeatedly doesn't take: try a different micro-USB cable\n");
            console::out("(a bad cable is a common silent failure) and make sure the remote\n");
            console::out("has a clear line of sight to the Apple TV -- a missed button edge\n");
            console::out("during the handoff between steps 1 and 2 just leaves it in Recovery\n");
            console::out("Mode instead.\n\n");
            console::out("Waiting for the device to enter DFU mode...\n");
        }
        if (!waitForDFUMode(deviceManager, ecid, device)) {
            fprintf(stderr, "Device never entered DFU mode.\n");
            return 1;
        }
    }

    if (!checkExploit(deviceManager, device, options.dryRun, options.noPwn, options.stockSecurerom)) {
        return 1;
    }

    // Which flags in this run guarantee the device stays non-jailbroken
    // (any use of unpatched/stock content) -- --no-pwn is deliberately NOT
    // one of these: it only skips re-running the exploit, which is
    // perfectly compatible with a real, successful jailbreak if the
    // device was already pwned by a separate run.
    std::vector<std::string> stockFlags;
    if (options.stockFirmware) stockFlags.push_back("--stock-firmware (patched bootloader, stock kernel/ramdisk)");
    if (options.stockRecovery) stockFlags.push_back("--stock-recovery (stock iBSS/iBEC)");
    if (options.stockRamdisk && !options.stockFirmware) stockFlags.push_back("--stock-ramdisk (stock RestoreRamdisk)");
    if (!stockFlags.empty()) {
        std::string joined;
        for (size_t i = 0; i < stockFlags.size(); i++) {
            joined += (i ? ", " : " ") + stockFlags[i];
        }
        fprintf(stderr, "DIAGNOSTIC run:%s -- the device will NOT be jailbroken even if everything "
                        "below succeeds.\n",
                joined.c_str());
    }

    std::string buildToRequest = kJailbreakTargetBuild;
    if (device.jailbroken) buildToRequest = device.buildID;
    printf("Targeting %s %s for this run.\n", device.deviceModel.c_str(), buildToRequest.c_str());
    // Both --stock-securerom (real SecureROM, no checkm8) AND --stock-recovery
    // (real, unpatched iBEC via useStockIBEC() -- patch_ticket_check()
    // never runs against it, checkm8 or not) need a build the device's
    // real signature/ticket verification will actually accept -- either
    // one alone means SOMETHING in this boot chain is enforcing real
    // Apple signing, not just SecureROM specifically. kJailbreakTargetBuild
    // is fixed to the specific old build this project's own jailbreak
    // patches/ImageKeys/baked ramdisks are tuned for, almost never Apple's
    // current signing window. Apple always has at least one currently-
    // signed build for any still-supported device (this is what makes
    // ipsw.me's "latest" endpoint meaningful at all) --
    // IpswFetch::firmwareURLForDevice() already treats the literal string
    // "latest" as a request for exactly that (see its own implementation).
    if (options.stockSecurerom || options.stockRecovery) {
        buildToRequest = "latest";

        // Only useStockIBSS()'s decrypt-only branch (stockRecovery
        // *without* stockSecurerom) ever needs a real local
        // keys/ entry -- when stockSecurerom is also set,
        // every single component (iBSS/iBEC/kernel/ramdisk) goes out
        // untouched, still encrypted, still img3-wrapped (see
        // useStockIBSS()/useStockIBEC()/useStockKernel()/useStockRamdisk()'s
        // own comments), so decrypt() never runs anywhere in that chain
        // and local keys are irrelevant. Preferring an older, locally-
        // keyed-but-still-signed build over the real "latest" would only
        // be actively counterproductive there -- a fully-stock run should
        // always test against whatever Apple actually currently signs,
        // not an older build that merely happens to have a local .keys
        // file blackb0x will never use.
        if (!options.stockSecurerom) {
            // whatever build ipsw.me's "latest" resolves to often has no
            // published keys at all yet (see Cli.hpp's own comment on
            // stockRecovery) even though an OLDER build is still
            // currently signed and DOES have a local .keys file.
            // Enumerate every currently-signed build via
            // signedBuildsForDevice() and prefer whichever one(s)
            // blackb0x already has local keys for, picking the
            // lexicographically-greatest match as a best-effort "newest"
            // (safe in practice: candidates here are always both
            // currently-signed AND already locally keyed, which in this
            // project's own ImageKeys/ history has never spanned the
            // 9.x/10.x digit-count boundary where plain string
            // comparison would misorder). Falls straight through to the
            // literal "latest" above, unchanged, if none of the
            // currently-signed builds have local keys either -- strictly
            // better than guessing blind, never worse than before this
            // existed.
            std::set<std::string> signedBuilds = signedBuildsForDevice(device.deviceModel);
            std::string preferred;
            for (const auto& build : signedBuilds) {
                std::error_code ec;
                std::string keysPath =
                    resolveImageKeyPath(device.deviceModel + "/" + device.deviceModel + "_" + build + ".keys");
                if (fs::exists(keysPath, ec)) preferred = build;
            }
            if (!preferred.empty()) {
                printf("%s has local keys for currently-signed build %s -- requesting that instead of blindly "
                       "resolving \"latest\".\n",
                       device.deviceModel.c_str(), preferred.c_str());
                buildToRequest = preferred;
            }
        }
    }

    auto components = downloadAndPatchComponents(patcher, device, buildToRequest,
                                                   options.stockRamdisk,
                                                   options.stockRecovery, options.stockFirmware,
                                                   options.stockSecurerom);
    if (!components) {
        // Not "...to patch..." -- on any --stock-* route nothing here
        // actually patches anything (useStockIBSS()/useStockIBEC()/etc.
        // just decrypt or pass the original file through untouched), so
        // that wording pointed at the wrong half of the problem. The
        // real reason is already printed above (see
        // downloadAndPatchComponents()'s own message).
        fprintf(stderr, "Failed to download or prepare firmware components.\n");
        return 1;
    }

    if (!sendComponentsToDevice(deviceManager, device, *components, options.dryRun,
                                 options.stockRecovery, options.stockSecurerom)) {
        return 1;
    }

    if (options.dryRun) {
        printf("\n(dry run) Done — nothing was written to the device.\n");
        return 0;
    }

    if (!stockFlags.empty()) {
        std::string joined;
        for (size_t i = 0; i < stockFlags.size(); i++) {
            joined += (i ? ", " : "") + stockFlags[i];
        }
        printf("\nDone. DIAGNOSTIC run (%s) -- the Apple TV should reboot into a stock, "
               "non-jailbroken state if this run succeeded.\n",
               joined.c_str());
    } else {
        printf("\nDone. The Apple TV should now reboot into the jailbroken system.\n");
    }
    return 0;
}
