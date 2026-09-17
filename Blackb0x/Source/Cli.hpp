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
    // Never attempt to run a pwntool (gaster/blackb0x-pwn) at all -- if the
    // connected device isn't already reporting a pwned DFU serial string,
    // fail instead of attempting the exploit. For iterating on the
    // post-exploit send flow against an already-pwned device without
    // spawning a pwntool again. (Was named --no-checkm8; renamed once
    // "pwntool" became the general term for gaster/blackb0x-pwn both.)
    bool noPwn = false;
    bool dontCheckFirmwareSums = false;
    // Sends the stock RestoreRamdisk exactly as downloaded from Apple
    // instead of the blackb0x-patched dist/ one -- a diagnostic for
    // narrowing down whether a boot failure is in blackb0x's own ramdisk
    // patching/entrypoint.c or earlier in the chain (see
    // Patcher::useStockRamdisk()'s own comment).
    bool stockRamdisk = false;
    // Sends the stock iBSS/iBEC exactly as downloaded from Apple instead
    // of blackb0x's own patched versions -- checkm8/pwnTool still runs
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
    // this project's own fixed jailbreak-target build -- see runCli()'s
    // buildToRequest comment.
    //
    // Deliberately NOT required to also carry stockSecurerom: useStockIBSS()
    // still needs a real local .keys entry to decrypt the stock iBSS for
    // checkm8's boot_client() path when stockSecurerom isn't set, and
    // blackb0x only ever ships one for kJailbreakTargetBuild, essentially
    // never whatever "latest" resolves to -- but that's a per-build data
    // gap, not an incoherent combination, and it's a legitimate
    // troubleshooting run in its own right (does checkm8 + a stock
    // iBEC/kernel/ramdisk/ticket chain work at all, independent of
    // whether iBSS itself is stock or blackb0x-patched). So this is left
    // to fail at runtime with a specific "no iBSS keys loaded for <device>
    // <build>" (Patcher::useStockIBSS()) rather than refused upfront here
    // -- drop a real .keys file for that build under Blackb0x/ImageKeys/
    // and rerun to get past it.
    bool stockRecovery = false;
    // Sends a stock kernelcache (see Patcher::useStockKernel()) and stock
    // ramdisk (same as stockRamdisk above) -- devicetree is already always
    // sent unmodified either way. Meaningful alone (blackb0x's own patched
    // iBSS/iBEC, ticket checks already bypassed there, booting an
    // otherwise-unmodified OS -- if this boots fine, the iBSS/iBEC patches
    // are confirmed OK and the failure is in blackb0x's own kernel/ramdisk
    // patches specifically; if it fails the same way, the iBSS/iBEC
    // patches themselves are implicated instead) or combined with
    // stockRecovery above for a fully-stock suite end to end (checkm8/
    // pwnTool still runs regardless, unless noPwn above also skips it) --
    // stockRecovery above REQUIRES this combination specifically (see its
    // own comment for why).
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
    bool help = false;
    // Which tool actually runs the checkm8 exploit -- "gaster" or
    // "blackb0x-pwn". Only ever meaningfully choosable on Apple platforms
    // (--pwntool, see printCliUsage()/parseCliOptions()): gaster does not
    // work on macOS no matter what has been tried, blackb0x-pwn does (see
    // README.md/docs/HISTORY.md), so blackb0x-pwn is the Apple default;
    // blackb0x-pwn itself is never built at all on Linux, so gaster is the
    // only option there, unconditionally.
#if defined(__APPLE__)
    std::string pwnTool = "blackb0x-pwn";
#else
    std::string pwnTool = "gaster";
#endif
};

CliOptions parseCliOptions(int argc, char** argv);
void printCliUsage(const char* argv0);

// Runs the full interactive session (device wait/select, DFU-mode wait,
// exploit, firmware download+patch, component upload) to completion.
// Returns a process exit code.
int runCli(const CliOptions& options);
