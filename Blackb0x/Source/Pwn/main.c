//
//  main.c
//  blackb0x-pwn
//
//  Standalone CLI for Blackb0x's own original checkm8/SHAtter exploit
//  implementations (see Checkm8Pwn.c) -- the same code the main `blackb0x`
//  binary's checkm8Attempt() shells out to. Builds on every platform:
//  nothing here is Apple-specific, only libirecovery's own
//  backend-independent irecv_* API, which resolves to IOKit on Darwin and
//  libusb elsewhere. See this target's own block in CMakeLists.txt.
//

#include "Checkm8Pwn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void printUsage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s <checkm8|shatter> [--ecid <hex-or-decimal>]\n"
        "\n"
        "  checkm8  Apple TV 3,1/3,2 (S5L8947X) SecureROM DFU exploit\n"
        "  shatter  Apple TV 2,1 SecureROM DFU exploit\n"
        "\n"
        "Runs Blackb0x's own original, hand-ported exploit implementations\n"
        "directly over libirecovery (its native IOKit backend on macOS, libusb\n"
        "elsewhere). Waits for a single already-connected DFU-mode device; if\n"
        "--ecid is omitted, the first one found is used.\n"
        "\n"
        "env (all DEBUG_-prefixed knobs exist for investigating why this does\n"
        "not work on Linux -- every default is the macOS-confirmed behaviour,\n"
        "so leaving them unset changes nothing):\n"
        "  DEBUG_CANCEL_DELAY_US\n"
        "      Microseconds to let checkm8's bug-setup DFU_DNLOAD run before\n"
        "      aborting it. Default 100, from the macOS original. See\n"
        "      scripts/sweep_pwn_cancel_delay.py.\n"
        "  DEBUG_OVERWRITE_TIMEOUT_MS\n"
        "      Milliseconds to let the overwrite transfer deliver its payload.\n"
        "      Default 100, likewise from the macOS original.\n"
        "  DEBUG_KEEP_CONNECTION\n"
        "      Hold one connection from the bug setup through the overwrite\n"
        "      instead of closing, sleeping 500ms and reopening in between.\n"
        "      Unset by default (the macOS behaviour).\n"
        "  DEBUG_RECONNECT_ATTEMPTS\n"
        "      One-second reconnect retries per stage. Default 30, from the\n"
        "      macOS original; lower it to fail fast while sweeping.\n"
        "  DEBUG_IGNORE_GROOM_ERRORS\n"
        "      Report heap-groom requests that answer unexpectedly instead of\n"
        "      aborting the run. Unset by default (the macOS behaviour).\n"
        "  DEBUG_BUGSETUP_RETRIES\n"
        "      Retry the bug setup in-process, up to N times. Default 1 (single\n"
        "      shot, the original behaviour). Pair with\n"
        "      DEBUG_BUGSETUP_TARGET_CONSUMED; alone it just takes the first\n"
        "      attempt. Note repeated attempts degrade the device's EP0 after\n"
        "      roughly 35 tries, so keep N modest.\n"
        "  DEBUG_BUGSETUP_TARGET_CONSUMED\n"
        "      Retry the bug setup until it consumes exactly this many bytes,\n"
        "      then proceed. The count is non-deterministic at a fixed delay (0\n"
        "      and 64 both observed at 100us), so this pins the one observable\n"
        "      the race offers. A confirmed-working macOS run consumes 0.\n"
        "      There is NO known oracle for the exploit precondition itself --\n"
        "      dfuIDLE afterwards is normal and proves nothing, because the\n"
        "      leaked buffer is by definition untracked by the DFU state\n"
        "      machine. If the target is never hit, the run proceeds anyway.\n"
        "      On Linux the bug setup's SETUP often never reaches the device at\n"
        "      all -- it reports dfuIDLE afterwards, so it processed no DNLOAD\n"
        "      and holds no buffer. libusb cancels with USBDEVFS_DISCARDURB,\n"
        "      which dequeues a URB the controller has not started, so getting\n"
        "      the SETUP out first is a race against a frame boundary. A lost\n"
        "      race leaves the device untouched in dfuIDLE, so retrying needs no\n"
        "      reset, no power cycle and no manual DFU re-entry -- which is what\n"
        "      makes this loopable, since the stages AFTER the bug setup are the\n"
        "      ones that wedge the device.\n"
        "      Costs one DFU_GETSTATUS per attempt, which advances the DFU state\n"
        "      machine. Polling GETSTATUS after a DNLOAD is what a compliant DFU\n"
        "      host does, so it probably does not disturb the dangling buffer --\n"
        "      but that is unverified, hence opt-in. Try: 500.\n"
        "  DEBUG_BUGSETUP_RETRY_DELAY_US\n"
        "      Pause between bug-setup retries. Default 2000. Back-to-back\n"
        "      attempts degraded the device's EP0 within three iterations in an\n"
        "      earlier version of the retry loop; this lets the endpoint settle.\n"
        "  DEBUG_DFU_STATUS\n"
        "      Query DFU_GETSTATUS at four points and print the device's own\n"
        "      bStatus/bState. Answers what the transfer trace cannot: the bug\n"
        "      setup reports 'consumed 0' on a WORKING macOS run and a FAILING\n"
        "      Linux one alike, because IOKit cancels a pipe whose SETUP already\n"
        "      went out while libusb may dequeue the URB before it transmits at\n"
        "      all. dfuDNLOAD_IDLE/dfuDNBUSY after the bug setup means the device\n"
        "      is holding a buffer; dfuIDLE means it never saw the request.\n"
        "      NOT PASSIVE -- GETSTATUS advances the DFU state machine, so a run\n"
        "      with this set is not evidence about a run without it, and it may\n"
        "      break an exploit that would otherwise work. Set it on BOTH\n"
        "      platforms and compare; never leave it on.\n"
        "  DEBUG_TRACE_TRANSFERS\n"
        "      Print a table of every control transfer the run made, after it\n"
        "      finishes: request fields, return value, how many bytes the host\n"
        "      controller believes moved (kept even on a stall or timeout, which\n"
        "      neither USB backend's ordinary wrapper does), and elapsed\n"
        "      microseconds. Buffered -- nothing is printed while the exploit is\n"
        "      running, since a write() between the bug setup and the overwrite\n"
        "      would land inside the window being measured.\n"
        "      This exists because macOS will not give up the wire: its USB\n"
        "      capture interfaces need SIP fully disabled and are reported broken\n"
        "      on 15.6.1+ regardless. Run this on both platforms and diff.\n",
        argv0);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const char* exploit = argv[1];
    uint64_t ecid = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ecid") == 0 && i + 1 < argc) {
            ecid = strtoull(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "Unknown argument: %s\n\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    int ok;
    if (strcmp(exploit, "checkm8") == 0) {
        ok = runCheckm8(ecid);
    } else if (strcmp(exploit, "shatter") == 0) {
        ok = runSHAtter(ecid);
    } else {
        printUsage(argv[0]);
        return 1;
    }

    return ok ? 0 : 1;
}
