//
//  Cli.hpp
//  Blackb0x
//
//  Replaces AppDelegate.h/.m + MainView.h/.m + Blackb0x.h/.m + main.m — the
//  Cocoa app shell, the jailbreak button handler, and the
//  Objective-C `Blackb0x` singleton (which just held one DeviceManager) all
//  collapse into a single linear CLI session. TaskManager.h/.m is dropped
//  entirely — it only ever drove NSProgressIndicator widgets, no logic of
//  its own to port.
//

#pragma once

#include <cstdint>
#include <string>

// Parsed from argv. `--ecid`/`--udid` pre-select a device (skipping the
// interactive numbered menu) for scripting/automation; if neither is given
// and more than one device is connected, runCli() prompts interactively.
struct CliOptions {
    uint64_t ecid = 0;    // 0 = not specified
    std::string udid;     // empty = not specified
    bool dryRun = false;
    // Never attempt to run the pwntool (blackb0x-pwn) at all -- if the
    // connected device isn't already reporting a pwned DFU serial string,
    // fail instead of attempting the exploit. For iterating on the
    // post-exploit send flow against an already-pwned device without
    // spawning a pwntool again. (Was named --no-checkm8; renamed once
    // "pwntool" became this project's general term for the exploit binary.)
    bool noPwn = false;
    // Sends the stock RestoreRamdisk exactly as downloaded from Apple
    // instead of the blackb0x-patched dist/ one -- a diagnostic for
    // narrowing down whether a boot failure is in blackb0x's own ramdisk
    // patching/entrypoint.c or earlier in the chain (see
    // Patcher::useStockRamdisk()'s own comment).
    bool stockRamdisk = false;
    // DIAGNOSTIC: send dist/RestoreRamDiskDiagRepack-<tuple>.dmg -- Apple's
    // own pristine ramdisk run through this project's ENTIRE image-rebuilding
    // machinery with NOTHING ADDED. Same decrypt, same hdiutil resize up, same
    // `attach -owners on`, same detach, same `resize -size min`, same IMG3
    // re-seal against the original as template, same img3ValidateFile() guard,
    // same 64 MiB ceiling. The only thing that does not happen is the staging.
    //
    // WHAT IT ISOLATES, and why the question is worth a flag. On real
    // AppleTV3,2 hardware today: --stock-ramdisk shows the Apple logo,
    // --tether-boot boots the installed OS, and the real baked ramdisk does
    // NOTHING AT ALL -- no logo. That last result is evidence in itself,
    // because the bake leaves Apple's /sbin/launchd byte-identical and leaves
    // com.apple.restored_external.plist (RunAtLoad) alone, and restored_external
    // is what draws the logo. If the kernel had mounted our image and run
    // launchd, the logo would appear whether or not our own binary was ever
    // accepted. No logo means the failure is BEFORE launchd -- the kernel is
    // not rooting off our image at all. Two candidates remain: the image is
    // too big (~44.3 MB against stock's ~16.6 MB, and the 64 MiB ceiling comes
    // from a measured USB upload short-write, not from anything the kernel
    // said), or the rebuild itself produces something unmountable.
    //
    // This flag answers the second. If THIS does not boot, size and content
    // are both exonerated -- the image carries not one changed byte of
    // content -- and the repack machinery is the bug. If it does boot, the
    // machinery is cleared and --diag-ramdisk-binary is the next point.
    //
    // Requires a suite baked with `bake-firmware --diagnostic-ramdisks`; CI
    // always passes it, so a downloaded artifact has them. Mutually exclusive
    // with --diag-ramdisk-binary, --stock-ramdisk and the other ramdisk-path
    // diagnostics (parseCliOptions() refuses the combination) -- each names a
    // different image for the same slot, and silently letting one win would
    // make the bisect report the wrong answer. NOT a jailbreak: this image has
    // no /blackb0x and no entrypoint on it, so a successful boot installs
    // nothing.
    bool diagRamdiskRepack = false;
    // DIAGNOSTIC: send dist/RestoreRamDiskDiagBinary-<tuple>.dmg -- the
    // entrypoint binary, its LaunchDaemon plist and our /mnt mountpoint, and
    // NO /blackb0x OVERLAY AT ALL. A few hundred KB over stock rather than the
    // ~28 MB the real overlay adds.
    //
    // WHAT IT ISOLATES: the overlay's SIZE, separately from the install
    // mechanism. Everything the bake does to Apple's tree is present here
    // except the one thing that makes the image large. Read against the other
    // three points:
    //   * boots, while the real bake does not -> SIZE is the answer; the
    //     install mechanism is fine and the work is shedding content.
    //   * does not boot, while --diag-ramdisk-repack does -> the INSTALL
    //     MECHANISM is the answer (the entrypoint install, the plist, or
    //     creating /mnt), and size is irrelevant.
    //   * neither boots -> the repack itself, which --diag-ramdisk-repack
    //     will already have said.
    //
    // Same requirements and same exclusivity as --diag-ramdisk-repack above.
    // NOT a jailbreak: entrypoint's merge_tree() has nothing to merge, so even
    // a perfect boot installs nothing -- a device that reaches the Apple logo
    // and reboots is this flag SUCCEEDING.
    bool diagRamdiskBinary = false;
    // Sends the stock iBSS/iBEC exactly as downloaded from Apple instead
    // of blackb0x's own patched versions -- checkm8/the pwntool still runs
    // first (SecureROM's own signature check still needs bypassing to
    // accept any file at all), but no boot-args/KASLR/ticket-check
    // patches get applied to the bootloader itself (see
    // Patcher::useStockIBSS()/useStockIBEC()'s own comment). (Was named
    // --no-pwn; renamed to avoid colliding with the new, differently-
    // scoped --no-pwn above once that name became available.)
    //
    // REQUIRES stockFirmware below (runCli() refuses to start otherwise):
    // the resulting stock iBEC still enforces real APTicket verification
    // on whatever it loads next, checkm8 or not (patch_ticket_check()
    // only ever runs against blackb0x's own patched iBEC), and a real
    // ticket only ever authorizes the exact, unmodified component
    // digests BuildManifest.plist lists -- blackb0x's own patched
    // kernel/ramdisk can never match those. This also means stockRecovery
    // requests whatever build Apple currently signs ("latest") instead of
    // this project's own per-device jailbreak-target build -- see runCli()'s
    // buildToRequest comment.
    //
    // Deliberately NOT required to also carry stockSecurerom: useStockIBSS()
    // still needs a real local .keys entry to decrypt the stock iBSS for
    // checkm8's boot_client() path when stockSecurerom isn't set, and
    // blackb0x ships keys only for the builds it actually targets -- one per
    // device model (jailbreakTargetBuildFor(), Cli.cpp), plus the older
    // builds keys/ happens to carry, essentially
    // never whatever "latest" resolves to -- but that's a per-build data
    // gap, not an incoherent combination, and it's a legitimate
    // troubleshooting run in its own right (does checkm8 + a stock
    // iBEC/kernel/ramdisk/ticket chain work at all, independent of
    // whether iBSS itself is stock or blackb0x-patched). So this is left
    // to fail at runtime with a specific "no iBSS keys loaded for <device>
    // <build>" (Patcher::useStockIBSS()) rather than refused upfront here
    // -- drop a real .keys file for that build under keys/
    // and rerun to get past it.
    bool stockRecovery = false;
    // The stock-kernel/ramdisk diagnostic. Sends a stock kernelcache (see
    // Patcher::useStockKernel()) and stock ramdisk (same as stockRamdisk
    // above) while KEEPING blackb0x's own patched iBSS/iBEC -- so it asks
    // exactly one question: "can blackb0x's patched bootloader boot an
    // otherwise-unmodified OS?". If it boots, the iBSS/iBEC patches are
    // confirmed OK and any jailbreak failure is in blackb0x's own
    // kernel/ramdisk patches; if it fails the same way, the bootloader
    // patches are implicated.
    //
    // The whole suite (kernelcache, ramdisk, DeviceTree, RestoreLogo) comes
    // from the SAME build the patched iBSS/iBEC are for -- this device's own
    // jailbreak target build (jailbreakTargetBuildFor(), Cli.cpp; it is per
    // device model, since the three supported models topped out at different
    // firmwares) -- self-consistent end to end, and the build stockRecovery/
    // stockSecurerom below also pair with.
    //
    // There used to be a second flag here, --stock-firmware-new, which pulled
    // that stock OS suite from the newest currently-signed build instead while
    // still loading it with the OLD patched iBSS/iBEC, to test whether an old
    // bootloader could hand off to a newer OS at all. It is gone, and
    // deliberately so: an iBoot is not a passive loader, it is the DeviceTree's
    // co-author. The IPSW's DeviceTree is a template whose runtime-filled
    // properties iBoot populates on the way to XNU, and the set of properties
    // any given iBoot knows how to fill is frozen at its own build. A newer
    // kernel wanting a newer runtime-filled property (the documented 32-bit
    // example is /chosen/nvram-proxy-data, added in iOS 6) gets an empty one
    // from an older iBoot and hangs very early -- no signature error, no bootx
    // rejection, no boot-failure-count increment, i.e. a result
    // indistinguishable from every other failure this flag was supposed to help
    // tell apart. The one published success at booting a newer kernel under an
    // older 32-bit iBoot needed a hand-merged DeviceTree AND a patch to iBoot's
    // own UpdateDeviceTree() -- far outside what iBoot32Patcher does. See
    // docs/HISTORY.md, "Can an old iBoot boot a NEWER build's kernelcache/
    // DeviceTree/ramdisk?", for the full evidence and confidence level. Note
    // this says nothing against retargeting the WHOLE chain (bootloader and OS
    // together) to a newer build: skew between iBoot and the tree it hands off
    // is the hazard, and a matched suite has none.
    //
    // stockRamdisk is redundant with this (warned, not rejected).
    bool stockFirmware = false;
    // Like noPwn above (never attempt to run a pwntool), but for the
    // opposite scenario: a genuinely un-exploited device, still running
    // real, un-bypassed SecureROM signature enforcement. Errors out if the
    // device IS already pwned (PWND: in its serial string) instead of
    // proceeding -- an already-pwned device contradicts what this flag is
    // for, so silently continuing would give a meaningless result. If the
    // device is confirmed not pwned, skips the pwntool and proceeds into
    // the rest of the boot chain, personalizing iBSS with a real,
    // ECID-bound SHSH ticket fetched from Apple's TSS server before
    // sending it (see Personalize.hpp/DeviceManager::sendiBSS()) --
    // REQUIRES both stockRecovery and stockFirmware (runCli() refuses to
    // start otherwise): that ticket (and the combined APTicket
    // DeviceManager::sendStockRestoreTail() sends afterward) is only ever
    // valid for the exact, unmodified stock components BuildManifest.plist
    // lists, so anything blackb0x has patched can never pass a real
    // SecureROM/iBEC's check regardless.
    bool stockSecurerom = false;
    // Diagnostic: send only the named stage and then exit immediately,
    // leaving the device wherever that stage put it so it can be inspected
    // with `irecovery -s` (e.g. to read the running iBoot version / SRTG and
    // confirm which bootloader stage is actually live -- ours or the device's
    // own installed iBoot after a failed handoff). "ibss" stops right after
    // iBSS; "ibec" stops right after iBEC (iBSS then iBEC). Empty = the normal
    // full sequence. checkm8/the exploit still runs first either way, and the
    // usual build-selection / stock flags still apply to which iBSS/iBEC is
    // sent. The device is NOT jailbroken by a --send-only run.
    std::string sendOnly;
    // Diagnostic: after the final 'bootx', do NOT reconnect and read the
    // device's recovery console (checkDeviceLeftRecoveryModeAfterBoot() /
    // captureConsoleLog()). That readout holds the USB device for ~5s and
    // consumes whatever iBoot printed; skipping it makes blackb0x exit right
    // after 'bootx' and release USB, so the console can be read interactively
    // instead (e.g. `irecovery -s`). Boot success/failure is then not judged
    // by this tool -- you inspect it yourself.
    bool noShellAttach = false;
    // Diagnostic: never send RestoreLogo, and never issue the "setpicture"/
    // "bgcolor" commands that go with it, on ANY path (the reconnect-per-step
    // flow and the stockRecovery single-connection tail). The stock recovery
    // flow this project was modeled against did not always send a logo;
    // this isolates whether the RestoreLogo upload / setpicture step is what
    // disturbs a later component (e.g. the kernelcache load).
    bool noSendRestoreLogo = false;
    // DIAGNOSTIC: tether-boot instead of the ramdisk install. Sends
    // iBSS -> iBEC -> RestoreLogo -> DeviceTree -> KernelCache('bootx') -- NO
    // Ramdisk. It sends a DIFFERENT iBEC: dist/iBECTether-<tuple> rather than
    // dist/iBEC-<tuple>. The two are the same patched bootloader with
    // different boot-args compiled in, and they have to be separate files
    // because a baked string cannot be overridden at runtime on this
    // bootloader (Patcher.hpp's `bootargs` namespace). The tether image
    // carries NAND-root boot-args (rd=disk0s1s1 instead of rd=md0), so
    // the patched kernel boots the OS already on the device's NAND rather than
    // the install ramdisk. RestoreLogo is kept because its setpicture is what
    // initializes the display -- without it a NAND boot is invisible whether
    // it succeeds or hangs. The point is to test the boot chain in
    // isolation: if the device leaves Recovery and the real OS comes up on the
    // TV screen, checkm8 -> iBEC -> the -z/patched KernelCache is proven intact
    // end to end and any jailbreak failure is downstream in the ramdisk/
    // entrypoint.c path; if it hangs the same way, the kernel/DeviceTree/-z
    // patch is implicated. Nothing is installed -- the device is NOT
    // jailbroken by a --tether-boot run.
    //
    // This revives (and fixes) the original app's `tetherbootClick`/
    // `self.selected_device.jailbroken == 1` flow that this port had removed;
    // see docs/HISTORY.md. Two corrections over the original: the kernel gets
    // a DeviceTree (the original tether path sent none, and the kernel needs
    // one), and the NAND-root args are actually the ones WITHOUT rd=md0 (the
    // original had the two arg sets wired up backwards).
    //
    // Boots whatever build blackb0x targets for THIS device model
    // (jailbreakTargetBuildFor(), Cli.cpp), so the device's NAND should be
    // running that same build for a clean boot -- a large kernel/userspace
    // version skew may panic. runCli() reads the installed build over
    // lockdownd where it can and says which build it is comparing against, so
    // the preflight warning is now specific to the connected model rather
    // than to one project-wide constant. Even a partial boot
    // (screen lights up / boot logo) still proves the kernel decompressed and
    // executed. Mutually exclusive with the --stock-* diagnostics (they drive
    // their own send paths); parseCliOptions() refuses the combination.
    bool tetherBoot = false;
    // --extra-boot-args. ACCEPTED BY THE PARSER, THEN REFUSED WITH AN ERROR.
    // Never empty-and-ignored, never warned-about-and-honoured: if it is set
    // at all, parseCliOptions() prints the working alternative and exits 2.
    //
    // WHY IT CANNOT WORK, which is the whole story of this project's central
    // bug. The flag used to append its value to a `setenv boot-args ...`
    // command sent over the recovery protocol just before 'bootx'. That
    // command is accepted by AppleTV3,2's iBoot-1537.9.55 and really does set
    // the variable -- "boot-args" is a legitimate entry in its settable
    // env-var name table. Nothing ever reads it back: there is no
    // env_get("boot-args") on the kernel-boot path, which selects between an
    // empty string and iBoot's own hardcoded "rd=md0 nand-enable-reformat=1
    // -progress" and snprintf()s that into the kernel command line. Stored
    // and never consulted -- ignored silently rather than rejected, which is
    // why it took weeks to find. Boot-args are compiled into iBEC now
    // (Patcher.hpp's `bootargs` namespace), and a compiled-in string wins
    // unconditionally, so there is nothing left at send time to append to.
    //
    // WHERE ARBITRARY BOOT-ARGS GO INSTEAD. They are baked, not passed at
    // jailbreak time. An iBEC re-bake takes seconds and needs no privilege:
    //
    //     ./build/bake-iboot --device AppleTV3,2 --build 10B329a --force \
    //                        --extra-boot-args "debug=0x14e"
    //
    // That appends to the built-in args in BOTH baked iBECs; --boot-args /
    // --tether-boot-args replace a base string outright. Ordinary kernel args
    // (nand-enable-reformat=1, serial=3 debug-uarts=3) all go through that
    // route, and nothing on the ramdisk side reads boot-args at all --
    // entrypoint.c consumes none (see its header comment).
    //
    // LENGTH IS CHECKED THERE, NOT HERE, AND AGAINST A DIFFERENT NUMBER.
    // bake-iboot measures against bootargs::kMaxBakedBootArgsLength (179),
    // which comes from the size of the string patch_boot_args() relocates
    // into and from iBoot's 256-byte kernel command line -- NOT from
    // DeviceManager::kMaxRecoveryCommandLength (127), which bounds recovery
    // COMMANDS that a baked string never passes through. Over-long is an
    // error there too; nothing is ever truncated.
    std::string extraBootArgs;
    bool help = false;
};

CliOptions parseCliOptions(int argc, char** argv);
void printCliUsage(const char* argv0);

// Runs the full interactive session (device wait/select, DFU-mode wait,
// exploit, firmware download+patch, component upload) to completion.
// Returns a process exit code.
int runCli(const CliOptions& options);
