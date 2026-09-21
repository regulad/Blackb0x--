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
    printf("  --diag-ramdisk-repack     DIAGNOSTIC: send the ramdisk that is Apple's own\n");
    printf("                            image run through blackb0x's ENTIRE rebuild\n");
    printf("                            machinery (decrypt, resize, attach, detach,\n");
    printf("                            resize to minimum, IMG3 re-seal) with NOTHING\n");
    printf("                            added -- not one changed byte of content.\n");
    printf("                            ISOLATES THE REBUILD ITSELF: if this does not\n");
    printf("                            boot, size and content are both exonerated and\n");
    printf("                            the repack is the bug; if it boots, the repack\n");
    printf("                            is cleared. Needs a suite baked with\n");
    printf("                            `bake-firmware --diagnostic-ramdisks` (CI always\n");
    printf("                            passes it). NOT a jailbreak -- the image carries\n");
    printf("                            no /blackb0x and no entrypoint.\n");
    printf("  --diag-ramdisk-binary     DIAGNOSTIC: send the ramdisk carrying the\n");
    printf("                            entrypoint binary, its LaunchDaemon plist and\n");
    printf("                            /mnt, but NO /blackb0x overlay -- a few hundred\n");
    printf("                            KB over stock instead of ~28 MB over.\n");
    printf("                            ISOLATES THE OVERLAY'S SIZE from the install\n");
    printf("                            mechanism: if this boots while the real ramdisk\n");
    printf("                            does not, size is the answer; if it fails while\n");
    printf("                            --diag-ramdisk-repack boots, the install\n");
    printf("                            mechanism is. Same bake requirement as above.\n");
    printf("                            NOT a jailbreak -- there is nothing to install.\n");
    printf("  --diag-ramdisk-overlay    DIAGNOSTIC: the mirror of the flag above -- send the\n");
    printf("                            ramdisk carrying the FULL /blackb0x overlay and\n");
    printf("                            /mnt, but NO entrypoint binary and NO LaunchDaemon\n");
    printf("                            plist, so nothing on it ever tries to launch our\n");
    printf("                            code. Within a few tens of KB of the real image.\n");
    printf("                            ISOLATES THE OVERLAY'S BULK from our job being\n");
    printf("                            spawned at all: if this boots and stays, the overlay\n");
    printf("                            is fine and the problem is entirely our job (code\n");
    printf("                            signing, or what the binary does); if it does not\n");
    printf("                            boot, the overlay or its size is what stops the\n");
    printf("                            kernel rooting off the image. Same bake requirement\n");
    printf("                            as above. NOT a jailbreak -- with no entrypoint on\n");
    printf("                            the image nothing ever reads /blackb0x.\n");
    printf("  --stock-recovery          DIAGNOSTIC: send the stock iBSS/iBEC exactly\n");
    printf("                            as downloaded from Apple (still runs checkm8\n");
    printf("                            first -- SecureROM's own signature check still\n");
    printf("                            needs bypassing to accept any file at all -- but\n");
    printf("                            no boot-args/KASLR/ticket-check patches applied\n");
    printf("                            to the bootloader itself) -- to check whether a\n");
    printf("                            boot failure is in blackb0x's own iBSS/iBEC\n");
    printf("                            patches or elsewhere in the chain. REQUIRES\n");
    printf("                            --stock-firmware (refuses to start otherwise):\n");
    printf("                            the resulting stock iBEC still enforces real\n");
    printf("                            APTicket verification on whatever it loads next,\n");
    printf("                            and a real ticket can never authorize blackb0x's\n");
    printf("                            own patched kernel/ramdisk. The device will NOT be\n");
    printf("                            jailbroken by a run using this flag.\n");
    printf("  --stock-firmware          DIAGNOSTIC: send a stock kernelcache (no\n");
    printf("                            tfp0/AMFI/sandbox patches) and stock ramdisk from\n");
    printf("                            the SAME build the patched iBSS/iBEC are for, while\n");
    printf("                            keeping blackb0x's own patched iBSS/iBEC -- to\n");
    printf("                            check whether the patched bootloader can boot an\n");
    printf("                            otherwise-unmodified OS. If it boots, the iBSS/iBEC\n");
    printf("                            patches are OK and any jailbreak failure is in the\n");
    printf("                            kernel/ramdisk patches; if it fails the same way,\n");
    printf("                            the bootloader patches are implicated. Also the\n");
    printf("                            build --stock-recovery/--stock-securerom pair with.\n");
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
    printf("                            and --stock-firmware (refuses to start\n");
    printf("                            otherwise):\n");
    printf("                            those tickets are only ever valid for the exact,\n");
    printf("                            unmodified stock components, so anything blackb0x\n");
    printf("                            has patched can never pass.\n");
    printf("  --no-shell-attach         DIAGNOSTIC: after the final 'bootx', do NOT\n");
    printf("                            reconnect and read the recovery console -- exit\n");
    printf("                            and release USB immediately so you can inspect it\n");
    printf("                            interactively (e.g. `irecovery -s`). Boot\n");
    printf("                            success/failure is then not judged by this tool.\n");
    printf("  --no-send-restorelogo     DIAGNOSTIC: never send RestoreLogo or run the\n");
    printf("                            setpicture/bgcolor commands, on any path -- to\n");
    printf("                            isolate whether that step disturbs a later\n");
    printf("                            component load (e.g. the kernelcache).\n");
    printf("  --send-only ibss|ibec     DIAGNOSTIC: run the exploit and send only iBSS\n");
    printf("                            (ibss) or iBSS then iBEC (ibec), then exit\n");
    printf("                            immediately -- leaving the device where that stage\n");
    printf("                            put it and releasing USB so you can attach with\n");
    printf("                            `irecovery -s` to read the running iBoot version\n");
    printf("                            and confirm which stage is actually live (ours vs\n");
    printf("                            the device's own installed iBoot). Honors the same\n");
    printf("                            build-selection and stock flags. NOT a jailbreak.\n");
    printf("  --tether-boot             DIAGNOSTIC: tether-boot the OS on NAND with the\n");
    printf("                            patched kernel instead of installing. Sends iBSS,\n");
    printf("                            iBEC, RestoreLogo, DeviceTree, KernelCache -- no\n");
    printf("                            Ramdisk -- with NAND-root boot-args (rd=disk0s1s1).\n");
    printf("                            If the OS comes up on screen the boot chain/kernel\n");
    printf("                            are intact; isolates the kernel from the ramdisk path.\n");
    printf("                            NOT a jailbreak; conflicts with --stock-*.\n");
    printf("  --extra-boot-args \"<s>\"   REFUSED: this bootloader ignores runtime boot-args.\n");
    printf("                            Boot-args are compiled into iBEC, so changing them\n");
    printf("                            means re-baking it -- seconds, and no root:\n");
    printf("                              ./build/bake-iboot --device <m> --build <b> --force \\\n");
    printf("                                                 --extra-boot-args \"<s>\"\n");
    printf("                            Arbitrary kernel boot-args must be BAKED that way;\n");
    printf("                            there is no runtime channel for them on this\n");
    printf("                            hardware.\n");
    printf("  --help                    Show this message\n");
    printf("\n");
    // Was a Linux-era note telling the user to run under sudo or install a
    // udev rule for raw USB access. Both are wrong here: this is macOS-only
    // now, blackb0x reaches the device through IOKit with no special
    // privilege, and there is no udev. See AGENTS.md ("Root: blackb0x needs
    // none; bake-firmware requires it unconditionally") -- the privilege
    // requirement lives entirely in the authoring half, which mounts things.
    printf("blackb0x needs no special privileges: it reaches a DFU/Recovery-mode device\n");
    printf("through IOKit. Do NOT run it under sudo. Only the authoring tools need root,\n");
    printf("and only bake-firmware's ramdisk half, which mounts disk images -- bake-iboot\n");
    printf("and bake-kernel run rootless in seconds.\n");
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
        } else if (arg == "--diag-ramdisk-repack") {
            options.diagRamdiskRepack = true;
        } else if (arg == "--diag-ramdisk-binary") {
            options.diagRamdiskBinary = true;
        } else if (arg == "--diag-ramdisk-overlay") {
            options.diagRamdiskOverlay = true;
        } else if (arg == "--stock-recovery") {
            options.stockRecovery = true;
        } else if (arg == "--stock-firmware") {
            options.stockFirmware = true;
        } else if (arg == "--stock-securerom") {
            options.stockSecurerom = true;
        } else if (arg == "--no-shell-attach") {
            options.noShellAttach = true;
        } else if (arg == "--no-send-restorelogo") {
            options.noSendRestoreLogo = true;
        } else if (arg == "--tether-boot") {
            options.tetherBoot = true;
        } else if (arg == "--extra-boot-args") {
            options.extraBootArgs = nextArg("--extra-boot-args");
        } else if (arg == "--send-only") {
            options.sendOnly = nextArg("--send-only");
            if (options.sendOnly != "ibss" && options.sendOnly != "ibec") {
                fprintf(stderr, "--send-only takes 'ibss' or 'ibec', got '%s'\n", options.sendOnly.c_str());
                printCliUsage(argv[0]);
                exit(2);
            }
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
    // --stock-recovery is NOT redundant with it: --stock-firmware deliberately
    // keeps blackb0x's own patched iBSS/iBEC and only stocks the kernel/ramdisk
    // (isolating whether the *bootloader* patches themselves are the
    // problem); combining --stock-recovery is how you get a fully-stock
    // suite end to end, a real, distinct diagnostic of its own. --no-pwn is
    // unrelated to either -- it controls whether a pwntool runs at all.
    if (options.stockFirmware && options.stockRamdisk) {
        fprintf(stderr, "--stock-ramdisk is redundant with --stock-firmware\n");
    }
    // THE RAMDISK SLOT HOLDS EXACTLY ONE IMAGE, and every flag here names a
    // different one for it. Unlike the --stock-firmware/--stock-ramdisk pair
    // above (which agree on what to send, so redundancy is only a warning),
    // these genuinely disagree, and whichever one the code happened to test
    // first would silently win. A bisect whose answer depends on argument
    // order is worse than no bisect, so this REFUSES rather than warns.
    //
    // Listed explicitly rather than counted through a helper so the error can
    // name what the user actually typed; see CliOptions' own comments for what
    // each image isolates.
    {
        // The diagnostic images alone, kept separately from the stock ones
        // below: the two checks after the exclusivity test are about the
        // diagnostics specifically and each has to be able to NAME what the
        // user typed. A ternary did that while there were only two of them;
        // with three it would silently misreport the third.
        std::vector<std::string> diagRamdiskChoices;
        if (options.diagRamdiskRepack) diagRamdiskChoices.push_back("--diag-ramdisk-repack");
        if (options.diagRamdiskBinary) diagRamdiskChoices.push_back("--diag-ramdisk-binary");
        if (options.diagRamdiskOverlay) diagRamdiskChoices.push_back("--diag-ramdisk-overlay");
        auto joinChoices = [](const std::vector<std::string>& choices) {
            std::string joined;
            for (size_t i = 0; i < choices.size(); i++) joined += (i ? ", " : "") + choices[i];
            return joined;
        };

        std::vector<std::string> ramdiskChoices = diagRamdiskChoices;
        // --stock-ramdisk and --stock-firmware are ONE claim between them, not
        // two: --stock-firmware stocks the ramdisk as well (see
        // downloadAndPatchComponents()'s `stockRamdisk || stockFirmware`), so
        // they ask for the same image and combining them stays the mere
        // redundancy the warning below already reports. Collapsing them here
        // is what keeps that case a warning rather than turning it into a
        // hard failure as a side effect of adding the diagnostics.
        if (options.stockRamdisk || options.stockFirmware) {
            ramdiskChoices.push_back(options.stockFirmware ? "--stock-firmware (implies a stock ramdisk)"
                                                           : "--stock-ramdisk");
        }
        if (ramdiskChoices.size() > 1) {
            std::string joined = joinChoices(ramdiskChoices);
            fprintf(stderr,
                    "These flags each choose a DIFFERENT RestoreRamdisk and cannot be combined: %s.\n"
                    "Pick one per run -- the whole point of the diagnostic images is that everything\n"
                    "except the ramdisk stays identical between runs, so they are compared across runs,\n"
                    "never within one.\n",
                    joined.c_str());
            printCliUsage(argv[0]);
            exit(2);
        }
        // --tether-boot sends NO ramdisk at all (see its own comment), so
        // asking it to send a particular one is incoherent, the same way the
        // --stock-* combination below is. Only the diagnostic images are
        // checked here; the --stock-* side of the same problem has its own
        // check a few lines down and keeps its own wording.
        if (options.tetherBoot && !diagRamdiskChoices.empty()) {
            fprintf(stderr,
                    "--tether-boot sends no Ramdisk at all, so it cannot be combined with %s.\n",
                    joinChoices(diagRamdiskChoices).c_str());
            printCliUsage(argv[0]);
            exit(2);
        }
        // --stock-recovery/--stock-securerom drive their own single-connection
        // send path and both require --stock-firmware (which the exclusivity
        // check above already refuses alongside a diagnostic image). Named
        // here anyway, because reaching that error through "--stock-firmware
        // is required" would point at the wrong flag.
        if (!diagRamdiskChoices.empty() && (options.stockRecovery || options.stockSecurerom)) {
            fprintf(stderr,
                    "The --diag-ramdisk-* images are blackb0x's own baked output and can never satisfy the "
                    "real APTicket verification --stock-recovery/--stock-securerom leave in force.\n");
            printCliUsage(argv[0]);
            exit(2);
        }
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
    // --tether-boot drives its own send path (no RestoreLogo/Ramdisk, NAND-root
    // args) and boots the OS already on NAND. The --stock-* diagnostics each
    // drive a DIFFERENT send path (stockRecovery's single-connection tail, the
    // stock kernel/ramdisk suites) around the ramdisk install, so combining
    // them with tether-boot is incoherent rather than merely redundant.
    if (options.tetherBoot &&
        (options.stockRecovery || options.stockFirmware || options.stockRamdisk || options.stockSecurerom)) {
        fprintf(stderr, "--tether-boot cannot be combined with the --stock-* diagnostics.\n");
        printCliUsage(argv[0]);
        exit(2);
    }
    // --extra-boot-args FAILS HARD. It does not warn and proceed, and it is
    // not silently ignored.
    //
    // The flag used to append its value to a `setenv boot-args ...` command
    // sent just before 'bootx'. That channel is inert on this bootloader --
    // the variable is set and never read back (DeviceManager.cpp's
    // sendKernelCache() carries the disassembly) -- so boot-args are compiled
    // into iBEC now, and a compiled-in string cannot be extended at runtime
    // by anything. There is no send-time mechanism left for this flag to use.
    //
    // The alternative was to have blackb0x re-bake the iBEC itself on demand.
    // Rejected: the jailbreak binary deliberately links no decrypt path and
    // no GPL patch tools, has no keys/ and no IPSW, and consumes dist/
    // verbatim (AGENTS.md's Patcher/PatcherPatch split). Making one flag
    // reach across that boundary would undo the whole arrangement to save a
    // user one command.
    //
    // So it errors, and names the command that does work: arbitrary boot-args
    // have to be baked into the iBEC, not passed at jailbreak time.
    if (!options.extraBootArgs.empty()) {
        fprintf(stderr,
                "--extra-boot-args cannot work at jailbreak time on this bootloader.\n"
                "\n"
                "  Boot-args are COMPILED INTO iBEC (iBoot32Patcher -b), because AppleTV3,2's\n"
                "  iBoot-1537.9.55 never reads the boot-args environment variable on its\n"
                "  kernel-boot path -- `setenv boot-args ...` is accepted, stores the value,\n"
                "  and nothing ever reads it back. A compiled-in string cannot be appended to\n"
                "  over USB.\n"
                "\n"
                "  Re-bake the iBEC instead. It takes seconds and needs no root:\n"
                "    ./build/bake-iboot --device <model> --build <build> --force \\\n"
                "                       --extra-boot-args \"%s\"\n"
                "\n"
                "  That rewrites dist/iBEC-<model>_<build> and dist/iBECTether-<model>_<build>\n"
                "  with your args appended to the built-in ones; re-run blackb0x after.\n"
                "  Use --boot-args / --tether-boot-args there to replace the base string\n"
                "  outright. See `./build/bake-iboot` with no arguments for the full usage and\n"
                "  the length budget.\n",
                options.extraBootArgs.c_str());
        exit(2);
    }
    return options;
}

// ---------------------------------------------------------------------------
// Device selection / DFU-mode wait
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// The jailbreak target build — per device
// ---------------------------------------------------------------------------
//
// This was one global constant for most of this project's life
// (`kJailbreakTargetBuild = "10B329a"`, inherited verbatim from the original
// app's MainView.m `downloadComponentsForBuildID:@"10B329a"`). That was
// defensible only while a single device model was supported at a single
// build; it is not defensible now, because the three supported models did not
// all receive the same final firmware. There is no one build that is even
// AVAILABLE for all three, let alone the best target for all three:
// AppleTV2,1 never got anything past 7.1.2, while both Apple TV 3s ran on to
// 8.4.7. A single constant necessarily targets at least one device with a
// build it never shipped with.
//
// WHY THESE BUILDS — the newest build each device ever received:
//
//   * Both GPL patch tools handle them, measured rather than assumed.
//     iBoot32Patcher (iBSS/iBEC) and CBPatcher (kernelcache) are pattern
//     matchers, and whether a given signature survives into a given build is
//     not predictable from the version string — so every known tuple was run
//     through the real bake-iboot/bake-kernel pipeline. The results are in
//     misc/verified_patcher_compatible.txt; every build below is `ok` for
//     both tools there (AppleTV2,1 11D258's kernel needs the xpwn read-side
//     16-align fix, which is in the pinned regulad/xpwn@legacy — it is the
//     `kernel-readfix` column).
//   * The Apple TV 3 builds need no key hunt. Apple shipped the late
//     AppleTV3,x builds with NO KBAG element at all in iBSS, iBEC, the
//     kernelcache or the RestoreRamDisk, so an all-empty
//     keys/<device>/<device>_<build>.keys plist is complete, correct key
//     material for them rather than a placeholder — "nobody published keys"
//     was never the blocker it looked like. (docs/HISTORY.md, "The newest
//     Apple TV 3 builds are not key-blocked; they are unencrypted".)
//   * A matched suite has no version skew. iBSS/iBEC, kernelcache, DeviceTree
//     and ramdisk all come from the one build named here, so the iBoot that
//     co-authors the DeviceTree is the iBoot that shipped with the kernel
//     reading it. Retargeting the WHOLE chain is safe in a way that pairing
//     an old bootloader with a newer OS is not — see docs/HISTORY.md,
//     "Corollary for retargeting kJailbreakTargetBuild".
//
// THIS TABLE IS MEANT TO BE EDITED, and retargeting a device is meant to be a
// one-line change to it. No build ID is hardcoded anywhere else in the tree.
// Three things to re-check when you move one:
//   * misc/verified_patcher_compatible.txt must say the new tuple patches
//     (the `iboot` and `kernel-readfix` columns).
//   * keys/<device>/<device>_<build>.keys must exist — empty strings are
//     fine and correct for an unencrypted build, but bake-firmware enumerates
//     keys/ to decide what to bake, so a missing file means no baked suite.
//   * the persistence payload: BakeRamdisk.cpp's stageVersionBranch() picks
//     it off a ProductVersion, and only 8.4.x gets a real untether (7.x/8.x
//     below that is the tethered dirhelper branch, 6.1.4 is p0sixspwn,
//     anything else warns and stages common content only). And the finished
//     ramdisk still has to fit the hard 64 MiB ceiling at the new build.
struct JailbreakTarget {
    const char* deviceModel;
    const char* buildID;
};
// OWNER'S POLICY, and it constrains what may be put in this table: the target
// is EITHER each device's newest build OR 10B329a. Nothing in between.
//
// The reasoning is that intermediate builds buy nothing and cost real work.
// Every distinct ProductVersion drags its own matched iPhoneOS SDK with it
// (entrypoint links against the target firmware's own libSystem now -- see
// entrypoint/Makefile's DEVICE -> XCODE_VERSION -> IOS_MIN chain), so each
// extra target means another multi-gigabyte Xcode and another set of
// verification runs. Splitting the difference to dodge a bug is also how you
// end up debugging a configuration nobody has ever booted.
//
// The two sanctioned positions:
//
//   NEWEST (current)  AppleTV3,x -> 12H1006 (8.4.7), AppleTV2,1 -> 11D258
//                     (7.1.2). Needs iPhoneOS 8.4 (Xcode 6.4) and 7.1 (Xcode
//                     5.1.1); both are already downloaded. 8.4.x is also the
//                     only line with a real untether.
//
//   FALLBACK          10B329a on ALL THREE devices -- it is 6.1.3 on every one
//                     of them (confirmed per tuple against
//                     misc/firmware_versions.txt, not assumed), so it collapses
//                     to ONE SDK: iPhoneOS 6.1, from Xcode 4.6. That Xcode is
//                     NOT currently downloaded; it is ~1.61 GiB and available
//                     credential-free, the archive.org copy being bit-identical
//                     to Apple's own (published SHA-1 matches). Falling back
//                     also drops to the tethered persistence branch, since
//                     6.1.3 is not 8.4.x.
//
// If you are here because a target is misbehaving, fix the bug or take the
// fallback wholesale. Do not add a fourth row.
const JailbreakTarget kJailbreakTargets[] = {
    // A1469 (Apple TV 3 rev A, the checkm8 target). 12H1006 is tvOS 8.4.7,
    // the last build Apple ever shipped it. Unencrypted boot chain.
    {"AppleTV3,2", "12H1006"},
    // A1427 (Apple TV 3). Same final build as AppleTV3,2, same story —
    // unencrypted, both patchers verified.
    {"AppleTV3,1", "12H1006"},
    // A1378 (Apple TV 2, the SHAtter target). 11D258 is 7.1.2, the last build
    // it ever received; Apple never shipped this model 8.x at all, so it
    // cannot share the Apple TV 3 target no matter what that becomes.
    {"AppleTV2,1", "11D258"},
};

// The build blackb0x targets on `deviceModel`, or an EMPTY STRING if that
// model has no entry in the table above.
//
// Empty rather than a fallback, deliberately. Every candidate default is a
// build that SOME device never shipped with, and handing a device a build it
// never received fails far downstream and unrecognizably — a missing dist/
// entry, a missing keys/ file, or (worst) a patched-and-baked suite that
// simply hangs on hardware with no output. An unknown model here means this
// project has no measured answer for it, which is exactly the thing to say
// out loud; callers turn this into a hard error naming the models that do
// have one (see knownJailbreakTargetsDescription() and runCli()).
std::string jailbreakTargetBuildFor(const std::string& deviceModel) {
    for (const auto& target : kJailbreakTargets) {
        if (deviceModel == target.deviceModel) return target.buildID;
    }
    return std::string();
}

// "AppleTV3,2 (12H1006), AppleTV3,1 (12H1006), AppleTV2,1 (11D258)" — the
// table rendered for the error message the empty return above forces, so the
// failure names the real, current targets instead of a stale hardcoded list.
std::string knownJailbreakTargetsDescription() {
    std::string description;
    for (const auto& target : kJailbreakTargets) {
        if (!description.empty()) description += ", ";
        description += std::string(target.deviceModel) + " (" + target.buildID + ")";
    }
    return description;
}

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

// Run a command and return its stdout, trimmed. Empty on any failure.
//
// popen() runs its argument through /bin/sh, which everything else in this
// file deliberately avoids -- but the one caller builds no argument from user
// input (it is a fixed `gh run list` with a literal repo slug), so there is
// nothing to quote and nothing to inject. Keeping it to one tightly-scoped
// helper is cheaper than hand-rolling a pipe/fork/dup2 for a single string.
static std::string captureCommand(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return "";
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
    if (pclose(pipe) != 0) return "";
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

// The run id of the newest SUCCESSFUL CI run, or "" if it cannot be
// determined.
//
// WHY THIS EXISTS, because the obvious thing is broken. `gh run download` with
// no run id is documented as taking the most recent run carrying an artifact
// of that name, and it does NOT do that in practice: the artifacts endpoint is
// not ordered by creation time, and gh takes the first match it sees.
// Measured against this repository -- the list came back
//
//   artifact 10624290099  run=35564293107  created 05:28   <- what gh picked
//   artifact 10624059003  run=35566229208  created 06:00   <- actually newest
//
// so a fetch silently served an artifact two runs stale, missing a component
// that the newest run had built. That is the worst possible failure for this
// tool: the download SUCCEEDS, the files look right, and the missing piece
// only surfaces later as a component that will not load -- or, far worse, as a
// hardware test run against firmware that is not the firmware you just built.
//
// Resolving the run explicitly costs one extra `gh` invocation and removes the
// ambiguity entirely. `--status success` matters as much as the ordering: the
// newest run may be in progress or may have failed, and half a bake is not
// something to hand a device.
static std::string newestSuccessfulRunId(const std::string& repo) {
    return captureCommand("gh run list --repo " + repo +
                          " --status success --limit 1 --json databaseId "
                          "--jq '.[0].databaseId' 2>/dev/null");
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
// `extraRamdisk`, when non-empty, is the dist/ basename PREFIX of a ramdisk
// variant this run additionally needs -- i.e. one of the diagnostic images
// (see diagramdisk:: in Patcher.hpp). It has to be part of the presence test,
// not just downloaded and hoped for, for a reason that bit immediately:
//
// The test below short-circuits on the manifest plus the normal ramdisk. The
// diagnostic images are DELIBERATELY absent from Manifest-<tuple>.txt, because
// that index's presence is what means "a complete, jailbreakable suite". So a
// dist/ populated before the diagnostics existed -- which is every dist/ that
// predates them -- passes present() and returns early, and the download that
// would have brought them never runs. The run then fails much later, at
// Patcher's baked-component load, as a missing file with no explanation of
// why it is missing or how to get it.
//
// Including the variant here makes a stale dist/ re-fetch instead, which is
// correct: `gh run download` unpacks the whole artifact and CI publishes the
// diagnostics alongside everything else, so one fetch satisfies both.
static bool ensureBakedFirmware(const std::string& deviceModel, const std::string& buildID,
                                const std::string& extraRamdisk = "") {
    const std::string suffix = "-" + deviceModel + "_" + buildID;
    auto present = [&]() {
        if (!fs::exists("dist/Manifest" + suffix + ".txt") ||
            !fs::exists("dist/RestoreRamDisk" + suffix + ".dmg")) {
            return false;
        }
        if (!extraRamdisk.empty() && !fs::exists("dist/" + extraRamdisk + suffix + ".dmg")) {
            return false;
        }
        return true;
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

        // Download into a SCRATCH directory and move the files over, rather
        // than unpacking straight into dist/.
        //
        // `gh run download` has no --clobber/--force and REFUSES to run when
        // any file it would write already exists -- it fails with "already
        // exists" naming whichever component it hit first. Unpacking directly
        // into dist/ therefore only works on a genuinely empty dist/, which is
        // exactly the case that stopped being the common one when the presence
        // test started requiring the diagnostic ramdisks: a dist/ from before
        // they existed now correctly fails present(), reaches this download,
        // and then collides with its own older files. The user's only recourse
        // was `rm -rf dist`, which is not something this tool should make
        // anyone do.
        //
        // Staging and moving is also simply more correct: the move is
        // overwrite-by-default, so a re-fetch refreshes a partial or stale
        // dist/ in place instead of demanding it be empty first.
        const fs::path scratch = fs::path("dist") / ".fetch-tmp";
        fs::remove_all(scratch, mkEc);
        fs::create_directories(scratch, mkEc);

        // Pin the run explicitly -- see newestSuccessfulRunId() for why the
        // no-run-id form cannot be trusted. Falling back to it when the lookup
        // fails is still better than not fetching at all; the presence check
        // below is what actually decides whether what arrived is usable.
        const std::string runId = newestSuccessfulRunId(artifactRepo());
        std::vector<std::string> argv = {"gh", "run", "download"};
        if (!runId.empty()) {
            printf("Using CI run %s (newest successful).\n", runId.c_str());
            argv.push_back(runId);
        } else {
            printf("Could not resolve the newest successful CI run; letting gh choose.\n");
        }
        argv.insert(argv.end(), {"--repo", artifactRepo(), "-n", artifact, "-D", scratch.string()});
        fflush(stdout);

        // It unpacks the artifact's contents directly into -D, and the
        // artifact is the flat dist/ layout already, so no rearranging.
        bool fetched = runForeground(argv);

        if (fetched) {
            std::error_code moveEc;
            for (const auto& entry : fs::directory_iterator(scratch, moveEc)) {
                if (moveEc || !entry.is_regular_file()) continue;
                const fs::path dest = fs::path("dist") / entry.path().filename();
                // rename() first: same filesystem, atomic, cheap. It fails
                // across devices, so fall back to a copy that overwrites.
                std::error_code renameEc;
                fs::rename(entry.path(), dest, renameEc);
                if (renameEc) {
                    std::error_code copyEc;
                    fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, copyEc);
                    if (copyEc) {
                        fprintf(stderr, "Could not place %s into dist/: %s\n",
                                entry.path().filename().string().c_str(), copyEc.message().c_str());
                    }
                }
            }
        }
        fs::remove_all(scratch, mkEc);

        if (fetched && present()) {
            printf("Downloaded a complete suite for %s %s.\n", deviceModel.c_str(), buildID.c_str());
            return true;
        }
        fprintf(stderr,
                "The published artifact did not yield a complete suite for %s %s.\n"
                "  It may not have been baked for this device/build yet -- see the bake matrix in\n"
                "  .github/workflows/ci.yml.\n",
                deviceModel.c_str(), buildID.c_str());
        // The common case for this message is now a diagnostic image, not a
        // missing device. CI stopped passing --diagnostic-ramdisks once the
        // bisect they existed for was finished, so the published artifact has
        // the real ramdisk and nothing else. Say that plainly rather than let
        // a reader conclude their device is unsupported.
        if (!extraRamdisk.empty()) {
            fprintf(stderr,
                    "  Specifically, %s%s.dmg is absent. Diagnostic ramdisks are no longer\n"
                    "  published by CI -- bake one locally:\n"
                    "    sudo ./build/bake-firmware --device '%s' --build %s --diagnostic-ramdisks\n",
                    extraRamdisk.c_str(), suffix.c_str(), deviceModel.c_str(), buildID.c_str());
        }
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
    // --diagnostic-ramdisks is forwarded when one was asked for. Without it
    // the bake would succeed, present() would still fail on the missing
    // diagnostic image, and the user would be told the bake "did not produce a
    // complete suite" -- when what actually happened is that we never asked
    // for the thing they requested.
    std::vector<std::string> bakeArgv = {resolveBakeFirmwarePath(), "--device", deviceModel,
                                         "--build", buildID};
    if (!extraRamdisk.empty()) bakeArgv.push_back("--diagnostic-ramdisks");
    if (!runForeground(bakeArgv) || !present()) {
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
                                                              bool stockSecurerom, bool tetherBoot,
                                                              const std::string& diagRamdiskComponent) {

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

    // Keys are only ever needed to decrypt a stock iBSS for the checkm8 route
    // (useStockIBSS()). One build supplies the whole suite -- bootloader and OS
    // alike -- so there is one build to load keys for. (This used to take a
    // separate `bootloaderBuild` parameter, because --stock-firmware-new pulled
    // the OS suite from a newer build than the iBSS/iBEC that loaded it; that
    // flag is gone, see docs/HISTORY.md, and with it the two-build split.)
    patcher.loadKeysForDevice(device.deviceModel, buildToRequest);
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
    // Bake realBuildID, the manifest's own resolved build -- one build supplies
    // the entire suite, so that is where the baked iBSS/iBEC live too.
    // realBuildID rather than buildToRequest because the latter can be the
    // literal string "latest" on the stock-bootloader routes; those are exactly
    // the fullyStock ones that never reach here, but naming the resolved build
    // keeps this correct rather than accidentally correct.
    bool fullyStock = stockFirmware && stockRecovery;
    // diagRamdiskComponent is passed through so a --diag-ramdisk-* run treats
    // that image as REQUIRED, not optional. Without it a dist/ baked before
    // the diagnostics existed satisfies the presence test, no download
    // happens, and the run dies later at Patcher's component load with a bare
    // missing-file error.
    if (!fullyStock && !ensureBakedFirmware(device.deviceModel, manifest->realBuildID, diagRamdiskComponent)) {
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
    // One suffix for every baked component, bootloader and OS alike, named for
    // the manifest's resolved build. There were two (a bootSuffix for the baked
    // iBSS/iBEC and an osSuffix for the baked kernel/DeviceTree/RestoreLogo)
    // purely so --stock-firmware-new could run an old bootloader against a newer
    // OS suite; that flag is gone and the two were equal in every other mode.
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
        takeBaked("iBSS", "iBSS",
                  [&](const std::string& p) { patcher.setBakedIBSSPath(p); });
        // TWO baked iBECs exist, differing only in the boot-args compiled
        // into each; exactly one is sent. The mode is a property of the
        // BINARY now, not of anything the sender can say at 'bootx' -- see
        // Patcher.hpp's `bootargs` namespace for why a baked string cannot be
        // overridden at runtime on this bootloader.
        const char* iBECComponent =
            tetherBoot ? bootargs::kIBECTetherComponent : bootargs::kIBECComponent;
        takeBaked(iBECComponent, iBECComponent,
                  [&](const std::string& p) { patcher.setBakedIBECPath(p); });
    }

    if (stockFirmware) {
        downloadAndPatch("KernelCache", manifest->kernelCachePath,
                          [&](const std::string& path) { patcher.useStockKernel(path, stockRecovery); });
    } else {
        takeBaked("KernelCache", "KernelCache",
                  [&](const std::string& p) { patcher.setBakedKernelPath(p); });
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
    //
    // Three mutually exclusive ways to fill the ramdisk slot; parseCliOptions()
    // already refused any combination of them, so the order of these branches
    // cannot decide an argument (see its own comment for why that mattered
    // enough to be a hard error rather than a warning).
    if (!diagRamdiskComponent.empty()) {
        // Diagnostic bisect images. Nothing to download -- they are baked
        // dist/ entries like the real ramdisk, produced by
        // `bake-firmware --diagnostic-ramdisks`. Note ensureBakedFirmware()
        // above has already run (a diagnostic run is never fullyStock), but it
        // only guarantees the real suite; useDiagnosticRamdisk() does its own
        // existence check and names the re-bake command if the flag was never
        // passed.
        patcher.useDiagnosticRamdisk(diagRamdiskComponent);
    } else if (stockRamdisk || stockFirmware) {
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
// The normal (install) flow: iBSS -> iBEC -> RestoreLogo -> Ramdisk ->
// DeviceTree -> KernelCache('bootx'). tetherBoot is a diagnostic variant that
// skips ONLY the Ramdisk and boots the OS already on NAND off the patched
// kernel (NAND-root args, rd=disk0s1s1 instead of rd=md0) -- a fixed revival
// of the original app's tether-boot (`self.selected_device.jailbroken == 1`)
// that this port had removed; see CliOptions::tetherBoot and docs/HISTORY.md.
// It keeps DeviceTree (the kernel needs one; the original tether path wrongly
// sent none) AND RestoreLogo (its setpicture is what initializes the display,
// without which the boot is invisible -- see the RestoreLogo send below).
// stockRecovery and tetherBoot are mutually exclusive (parseCliOptions()
// enforces it), so their branches below never overlap.
bool sendComponentsToDevice(DeviceManager& deviceManager, AppleTVDevice& device, const PatchedComponents& components,
                             bool dryRun, bool stockRecovery, bool stockSecurerom, const std::string& sendOnly,
                             bool noShellAttach, bool noSendRestoreLogo, bool tetherBoot) {
    if (dryRun) {
        printf("(dry run) Would send:\n");
        printf("  iBSS%s\n", components.iBSS ? "" : " (missing, would fail here)");
        printf("  iBEC%s\n", components.iBEC ? "" : " (missing)");
        if (components.restoreLogo) printf("  RestoreLogo\n");
        if (!tetherBoot) printf("  Ramdisk%s\n", components.ramdisk ? "" : " (missing)");
        printf("  DeviceTree%s\n", components.deviceTree ? "" : " (missing)");
        printf("  KernelCache%s%s\n", components.kernel ? "" : " (missing, would fail here)",
               tetherBoot ? " (NAND-root boot-args, no rd=md0)" : "");
        printf("(dry run) Would then wait for the Apple TV to %s\n", tetherBoot ? "boot" : "reboot");
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
    if (sendOnly == "ibss") {
        console::out("--send-only ibss: stopping here so the running stage can be inspected.\n");
        return true;
    }
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
        int i = deviceManager.sendStockRestoreTail(device.ecid, components, device.deviceModel, noShellAttach,
                                                    noSendRestoreLogo);
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
    if (sendOnly == "ibec") {
        console::out("--send-only ibec: stopping here so the running stage can be inspected.\n");
        return true;
    }

    if (stockRecovery) {
        if (!sendStockTail()) return false;
        device.needsPostInstall = 1;
        device.waitForRecovery = 1;
        console::out("Waiting for Apple TV to reboot\n");
        return true;
    }

    // RestoreLogo, when the manifest had one (downloadAndPatchComponents()
    // only sets components.restoreLogo if a real Path existed). This path
    // used to skip it entirely even though the documented flow and the
    // stockRecovery path (sendStockRestoreTail()) both send it here, between
    // iBEC and Ramdisk. Non-fatal on failure: RestoreLogo is a cosmetic boot
    // image, not something the boot depends on, so a failed send shouldn't
    // abort a run that would otherwise proceed.
    // RestoreLogo is sent on BOTH the install and tether-boot paths (only the
    // Ramdisk below is tether-specific). It is not merely cosmetic here: on
    // these devices iBoot only brings the display/framebuffer up when it has a
    // picture to draw, and its `setpicture` is what initializes that panel.
    // The kernel then renders verbose (`-v`) boot text into the same
    // framebuffer, and a NAND boot with no framebuffer is invisible whether it
    // succeeds, hangs, or panics -- which is exactly the "nothing happens on
    // --tether-boot" symptom. redsn0w injects its own boot logo for the same
    // reason. It must go BEFORE the DeviceTree, which loads over the logo's
    // memory once drawn -- this ordering already holds. See docs/HISTORY.md.
    if (components.restoreLogo && !noSendRestoreLogo) {
        console::out("Sending RestoreLogo...\n");
        if (deviceManager.sendRestoreLogo(*components.restoreLogo, device.ecid) != 0) {
            console::err("Failed to send RestoreLogo (continuing -- it is a cosmetic boot image).\n");
        } else {
            console::out("RestoreLogo sent.\n");
        }
    }

    if (!tetherBoot) {
        console::out("Sending Ramdisk...\n");
        i = components.ramdisk ? deviceManager.sendRamdisk(*components.ramdisk, device.ecid) : -1;
        if (i != 0) {
            console::err("Failed to send Ramdisk. Re-enter DFU mode and try again.\n");
            return false;
        }
        console::out("Ramdisk sent.\n");
    }

    console::out("Sending DeviceTree...\n");
    i = components.deviceTree ? deviceManager.sendDeviceTree(*components.deviceTree, device.ecid) : -1;
    if (i != 0) {
        console::err("Failed to send DeviceTree. Re-enter DFU mode and try again.\n");
        return false;
    }
    console::out("DeviceTree sent.\n");

    // needsPostInstall drives the post-boot install steps; a tether-boot
    // installs nothing (it just boots the OS on NAND), so leave it unset there.
    if (!tetherBoot) {
        device.needsPostInstall = 1;
    }

    // Only reached when stockRecovery is unset -- sendStockTail() above
    // already includes KernelCache and returns directly otherwise.
    //
    // ramdiskBoot = !tetherBoot. The install path just sent a Ramdisk and
    // DeviceTree, so the kernel roots off the ramdisk (rd=md0). --tether-boot
    // sent a DeviceTree but no Ramdisk and roots off NAND (no rd=md0). This is
    // the arg selection the original app got backwards -- it put rd=md0 into
    // the iBEC its tether path sent and non-rd=md0 into the install path, so
    // the install ramdisk's entrypoint.c could never have run as PID 1. See
    // docs/HISTORY.md.
    console::out("Sending KernelCache...\n");
    int kernelResult = components.kernel
                           ? deviceManager.sendKernelCache(*components.kernel, device.ecid, noShellAttach,
                                                           /*ramdiskBoot=*/!tetherBoot)
                           : -1;
    if (kernelResult != 0) {
        console::err("Failed to send KernelCache.\n");
        return false;
    }
    console::out("KernelCache sent.\n");

    device.waitForRecovery = 1;
    console::out("Waiting for Apple TV to %s\n", tetherBoot ? "boot" : "reboot");
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
    // do anything else. A real SecureROM run is one self-consistent signed
    // build end to end, which --stock-firmware gives it.
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
    // --stock-recovery stocks the bootloader too, so the whole suite has to be
    // one self-consistent build its ticket verification accepts, which is
    // exactly what --stock-firmware supplies.
    if (options.stockRecovery && !options.stockFirmware) {
        fprintf(stderr,
                "--stock-recovery requires --stock-firmware: a stock iBEC verifies a real APTicket "
                "against the exact, unmodified component digests BuildManifest.plist lists, and blackb0x's "
                "own patched kernel/ramdisk can never match those regardless of which build gets requested. "
                "Pass --stock-recovery --stock-firmware together.\n");
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
    // No setExtraBootArgs() here any more: DeviceManager sends no boot-args
    // at all. They are compiled into the iBEC chosen below. parseCliOptions()
    // already refused --extra-boot-args outright, with a pointer at
    // `bake-iboot --extra-boot-args`.
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
    if (options.stockFirmware)
        stockFlags.push_back("--stock-firmware (patched bootloader, stock kernel/ramdisk, same build)");
    if (options.stockRecovery) stockFlags.push_back("--stock-recovery (stock iBSS/iBEC)");
    if (options.stockRamdisk && !options.stockFirmware)
        stockFlags.push_back("--stock-ramdisk (stock RestoreRamdisk)");
    if (options.diagRamdiskRepack)
        stockFlags.push_back("--diag-ramdisk-repack (pristine ramdisk, repacked, nothing added)");
    if (options.diagRamdiskBinary)
        stockFlags.push_back("--diag-ramdisk-binary (entrypoint + plist + /mnt, no /blackb0x overlay)");
    if (options.diagRamdiskOverlay)
        stockFlags.push_back("--diag-ramdisk-overlay (/blackb0x overlay + /mnt, no entrypoint, no plist)");
    if (!options.sendOnly.empty())
        stockFlags.push_back("--send-only " + options.sendOnly + " (send that stage, then exit for inspection)");
    if (!stockFlags.empty()) {
        std::string joined;
        for (size_t i = 0; i < stockFlags.size(); i++) {
            joined += (i ? ", " : " ") + stockFlags[i];
        }
        fprintf(stderr, "DIAGNOSTIC run:%s -- the device will NOT be jailbroken even if everything "
                        "below succeeds.\n",
                joined.c_str());
    }

    // One build for the whole run: the iBSS/iBEC that execute AND the OS suite
    // they hand off to (kernel/ramdisk/DeviceTree/RestoreLogo, and the manifest
    // downloaded here). There were briefly two, so --stock-firmware-new could
    // run the old patched bootloader against a newer stock OS suite; that is
    // removed (docs/HISTORY.md: an older iBoot cannot populate the DeviceTree
    // properties a newer kernel expects, and fails silently when it can't), and
    // a matched suite is what every real tool sends. blackb0x only bakes/keys
    // this device's own jailbreak target build, so that is the default;
    // device.buildID only matters when re-running the real jailbreak against an
    // already-jailbroken device (no stock flag).
    //
    // The target is per DEVICE (jailbreakTargetBuildFor(), see its table's own
    // comment) — the three supported models topped out at different firmwares,
    // so there is no single build to fall back to. Resolving it here is
    // deliberate: this is the first point in the run where the device is
    // actually known, and it is the only point either consumer needs it.
    const std::string targetBuild = jailbreakTargetBuildFor(device.deviceModel);
    if (targetBuild.empty()) {
        // No silent default. See jailbreakTargetBuildFor()'s comment for why a
        // guessed build is worse than no build at all. Note checkExploit()
        // above already rejects unrecognized models — except for a device that
        // arrives ALREADY in pwned DFU, which it lets straight through, so this
        // really is reachable.
        fprintf(stderr,
                "No jailbreak target build is known for %s. blackb0x targets: %s. If %s should be "
                "supported, add it to kJailbreakTargets (Cli.cpp) with a build that "
                "misc/verified_patcher_compatible.txt says both patchers handle.\n",
                device.deviceModel.c_str(), knownJailbreakTargetsDescription().c_str(),
                device.deviceModel.c_str());
        return 1;
    }
    std::string buildToRequest = targetBuild;
    // The --diag-ramdisk-* images are baked per tuple exactly like the real
    // ramdisk, so they are listed here for the same reason the --stock-* flags
    // are: a diagnostic run must stay on the build this project baked a suite
    // for, not follow an already-jailbroken device's own installed build.
    if (device.jailbroken && !options.stockFirmware && !options.stockRecovery && !options.stockSecurerom &&
        !options.stockRamdisk && !options.diagRamdiskRepack && !options.diagRamdiskBinary &&
        !options.diagRamdiskOverlay)
        buildToRequest = device.buildID;
    printf("Targeting %s %s for this run.\n", device.deviceModel.c_str(), buildToRequest.c_str());
    // Both --stock-securerom (real SecureROM, no checkm8) AND --stock-recovery
    // (real, unpatched iBEC via useStockIBEC() -- patch_ticket_check()
    // never runs against it, checkm8 or not) need a build the device's
    // real signature/ticket verification will actually accept -- either
    // one alone means SOMETHING in this boot chain is enforcing real
    // Apple signing, not just SecureROM specifically. The per-device
    // jailbreak target resolved above is a specific build this project's own
    // patches/keys/baked ramdisks are tuned for — the newest that device ever
    // received, which for this long-EOL hardware is still years outside
    // Apple's current signing window. Apple always has at least one currently-
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
        // iBSS/iBEC are stock here too (useStockIBSS()/useStockIBEC()), so the
        // WHOLE suite -- bootloader included -- is this one signed build. That
        // is now true of every route, not just this one: buildToRequest is the
        // only build there is.
    }

    // Empty unless one of the diagnostic images was asked for; they are
    // mutually exclusive (parseCliOptions()), so one string is the whole
    // channel. The names live in Patcher.hpp's `diagramdisk` namespace,
    // shared with the baker that writes them.
    const std::string diagRamdiskComponent = options.diagRamdiskRepack    ? diagramdisk::kRepackComponent
                                             : options.diagRamdiskBinary  ? diagramdisk::kBinaryComponent
                                             : options.diagRamdiskOverlay ? diagramdisk::kOverlayComponent
                                                                          : "";

    auto components = downloadAndPatchComponents(patcher, device, buildToRequest,
                                                   options.stockRamdisk,
                                                   options.stockRecovery, options.stockFirmware,
                                                   options.stockSecurerom, options.tetherBoot,
                                                   diagRamdiskComponent);
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

    // --tether-boot boots the OS ALREADY on NAND off our patched kernel, which
    // is built for exactly one build: buildToRequest, i.e. this DEVICE's own
    // jailbreak target (or, when re-running against an already-jailbroken
    // device, the build already installed on it -- which is why the comparison
    // is against buildToRequest and not the target directly: buildToRequest is
    // what actually gets sent). If the installed OS is a different build, the
    // kernel/kext/userspace mismatch hangs at (or after) the NAND root-mount
    // with no visible output -- the classic "nothing happens" tether-boot
    // symptom. We can read the installed build via lockdownd, but ONLY if the
    // device has been seen in Normal mode (that is what populates
    // device.buildID); in DFU/Recovery there is no way to read the on-NAND
    // version. So warn on mismatch, and when it is simply unknown tell the user
    // how to make it knowable (connect once in Normal mode). Purely advisory --
    // never refuses: a nearby build whose kernel ABI did not change may well
    // still boot, so we always proceed and let the hardware decide.
    //
    // The per-device target makes this test sharper, not vaguer. Against the
    // old single global build, an AppleTV2,1 running the newest OS Apple ever
    // gave it was reported as a mismatch purely because the constant named an
    // Apple TV 3 build; now each device is compared against the build blackb0x
    // really targets for it, and the message names that same build.
    if (options.tetherBoot) {
        if (device.buildID.empty()) {
            printf("\n--tether-boot: didn't see the device in Normal mode to read its OS version -- tether "
                   "boot may not work (it needs the installed OS on this %s to be %s). Proceeding anyway.\n",
                   device.deviceModel.c_str(), buildToRequest.c_str());
        } else if (device.buildID != buildToRequest) {
            printf("\n--tether-boot: installed OS is %s%s but tether boot loads this %s's %s kernel -- "
                   "version mismatch, tether boot may not work (though it can still boot if the kernel ABI "
                   "didn't change between these builds). Proceeding anyway.\n",
                   device.version.empty() ? "" : (device.version + " / ").c_str(), device.buildID.c_str(),
                   device.deviceModel.c_str(), buildToRequest.c_str());
        } else {
            printf("\n--tether-boot: installed OS build %s matches the kernel being sent -- the NAND root "
                   "and the patched kernel should be compatible.\n",
                   device.buildID.c_str());
        }
    }

    // Echo the boot-args this run will actually boot with. They live in the
    // iBEC that was just resolved out of dist/, not in anything sent over
    // USB, so this is the only place the run's own log records them -- and
    // on a device with no console it is the only record there is. A dist/
    // baked with a custom `bake-iboot --extra-boot-args` prints the built-in
    // string here rather than the custom one; the baker's own output is
    // authoritative for that case.
    printf("\nBoot-args (compiled into %s): %s\n",
           options.tetherBoot ? bootargs::kIBECTetherComponent : bootargs::kIBECComponent,
           options.tetherBoot ? bootargs::kTetherBootArgs : bootargs::kRamdiskBootArgs);

    if (!sendComponentsToDevice(deviceManager, device, *components, options.dryRun,
                                 options.stockRecovery, options.stockSecurerom, options.sendOnly,
                                 options.noShellAttach, options.noSendRestoreLogo, options.tetherBoot)) {
        return 1;
    }

    if (options.dryRun) {
        printf("\n(dry run) Done — nothing was written to the device.\n");
        return 0;
    }

    if (!options.sendOnly.empty()) {
        printf("\n--send-only %s: sent that stage and stopped. USB is released -- attach now with\n"
               "  irecovery -s\n"
               "and read the iBoot version/SRTG to see which stage is actually running (ours, or the\n"
               "device's own installed iBoot after a failed handoff). The Apple TV was NOT jailbroken.\n",
               options.sendOnly.c_str());
        return 0;
    }

    if (options.tetherBoot) {
        printf("\nDone. --tether-boot DIAGNOSTIC: the Apple TV should now boot the OS already on\n"
               "NAND using the patched kernel (no ramdisk, no rd=md0). If the OS comes up on the\n"
               "TV screen, checkm8 -> iBEC -> the patched/-z KernelCache is intact end to end and a\n"
               "jailbreak failure is downstream in the ramdisk/entrypoint.c path; if it hangs the\n"
               "same way, the kernel/DeviceTree is implicated. The Apple TV was NOT jailbroken.\n");
    } else if (!stockFlags.empty()) {
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
