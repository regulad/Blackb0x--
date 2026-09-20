//
//  DeviceManager.cpp
//  Blackb0x
//
//  CLI port of DeviceManager.m. The checkm8/SHAtter exploit bodies and
//  the iRecovery USB helpers are ported VERBATIM from the original — this is
//  exploit-critical hardware-timing code and is not "improved" during
//  conversion, only translated from Objective-C method syntax to C++ and
//  from dispatch_async(..., ^{ view.xxx = ... }) to direct DeviceEventSink
//  callback invocations (there is no GUI event loop to marshal onto here).
//
//  Dropped entirely: AppleTVIcon's Cocoa rendering (NSImageView/NSColor/
//  NSProgressIndicator), arrangeIcons() (pure icon-grid layout math), and
//  the "is this the currently GUI-selected device" auto-continue logic in
//  the original newDevice() (view.selected_ecid / dfuHelper.isVisible /
//  jailbreakClick / tetherbootClick) — that decision belongs to the CLI
//  front end (a later phase), which can make it by observing
//  onDeviceAdded/onDeviceUpdated instead.
//

#include "DeviceManager.hpp"
#include "Console.hpp"
#include "Patcher.hpp"
#include "Personalize.hpp"
#include "ResourcePath.hpp"
#include "SHAtter.h"
#include "bootkit.h"

#include <plist/plist.h>

extern "C" {
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>
}

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>

// ---------------------------------------------------------------------------
// Forward declarations of free functions (ported verbatim from the original
// C-linkage globals in DeviceManager.m — these were never Objective-C
// methods to begin with).
// ---------------------------------------------------------------------------

static irecv_client_t get_tv(uint64_t ecid);
static void reset_counters(irecv_client_t client);
static void usb_reset(irecv_client_t client);
static void send_buffer(irecv_client_t client, unsigned char* data, unsigned long size);
static void get_data(irecv_client_t client, char* buffer, unsigned long length);
static void request_image_validation(irecv_client_t client);
static int msleep(long msec);
static const char* mode_to_str(int mode);
static bool serialStringIndicatesRealDFU(const char* serialString);
static bool isPwnedDFU(const struct irecv_device_info* info);
static int send_data(irecv_client_t client, unsigned char* data, size_t size);
static bool commandExistsOnPath(const char* name);
static int runBlackb0xPwn(const std::vector<std::string>& args, int timeoutSeconds = 0);
static int boot_client(irecv_client_t client, void* buf, size_t sz, bool allowUnpwned = false);
static int check_img3_file_format(irecv_client_t client, void* file, size_t sz, void** out, size_t* outsz);
static int sendiBSS_ATV31(uint64_t ecid, const char* iBSSpath, bool allowUnpwned = false);
static int sendiBSS_ATV32(uint64_t ecid, const char* path, bool allowUnpwned = false);
static void send_progress(double progress);
static int progress_cb(irecv_client_t client, const irecv_event_t* event);

extern "C" void blackb0x_irecv_device_event_cb(const irecv_device_event_t* event, void* user_data);
extern "C" void blackb0x_idevice_event_cb(const idevice_event_t* event, void* user_data);
extern "C" int blackb0x_irecv_progress_cb(irecv_client_t client, const irecv_event_t* event);

DeviceManager* DeviceManager::instance_ = nullptr;

// ---------------------------------------------------------------------------
// AppleTVDevice
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// DeviceManager construction / device bookkeeping
// ---------------------------------------------------------------------------

DeviceManager::DeviceManager() {
    instance_ = this;

    irecv_device_event_subscribe(&irecvEventCtx_, blackb0x_irecv_device_event_cb, nullptr);
    // Normal-mode discovery goes through the real system usbmuxd again (see
    // docs/HISTORY.md) — it must be run with --no-preflight for this project's
    // target hardware (pre-2013 Apple TV/iOS), whose lockdownd predates
    // what usbmuxd's own preflight step can negotiate and would otherwise
    // never be exposed via idevice_get_device_list()/this callback at all.
    idevice_event_subscribe(blackb0x_idevice_event_cb, nullptr);
}

std::vector<AppleTVDevice> DeviceManager::devicesSnapshot() const {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    return std::vector<AppleTVDevice>(devices_.begin(), devices_.end());
}

AppleTVDevice* DeviceManager::deviceWithUDID(const std::string& udid, uint64_t ecid) {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    for (auto& icon : devices_) {
        if (!udid.empty() && icon.udid == udid) return &icon;
        if (ecid != 0 && icon.ecid == ecid) return &icon;
    }
    return nullptr;
}

void DeviceManager::newDevice(const std::string& productType, const std::string& modeStr,
                               const std::string& version, const std::string& buildID,
                               uint64_t ecid, const std::string& udid, int pwnedDFU) {
    if (productType.empty()) return;
    if (productType.find("AppleTV3") == std::string::npos &&
        productType.find("AppleTV2") == std::string::npos) return;

    AppleTVDevice* icon = deviceWithUDID(udid, ecid);
    bool isNew = false;
    if (!icon) {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        devices_.push_back(AppleTVDevice{});
        icon = &devices_.back();
        icon->udid = udid;
        icon->ecid = ecid;
        isNew = true;
    }

    if (productType == "AppleTV3,2" || productType == "AppleTV3,1") {
        if (modeStr == "Recovery") {
            icon->buildID.clear();
            icon->version.clear();
            if (icon->waitForRecovery == 1) {
                if (sink_.onStatus) {
                    sink_.onStatus("Done! Connect your AppleTV to a TV. You will be able to "
                                   "use it once it automatically restarts itself.");
                }
                icon->version = "Recovery - Jailbroken";
            } else {
                icon->version = "Recovery";
            }
        }
    }

    if (!version.empty()) {
        icon->buildID = buildID;
        icon->version = version;
    }

    icon->deviceModel = productType;
    icon->mode = modeStr;
    icon->connected = 1;
    icon->pwnedDFU = pwnedDFU;

    if (isNew && sink_.onDeviceAdded) {
        sink_.onDeviceAdded(*icon);
    } else if (sink_.onDeviceUpdated) {
        sink_.onDeviceUpdated(*icon);
    }

    // No jailbreak check fired from here. It used to be dispatched onto a
    // detached thread, which meant it ran concurrently with (and usually
    // finished after) the main flow's own read of icon->jailbroken -- so the
    // value that decides which build gets requested was racing the check
    // that produces it. Cli.cpp now calls checkJailbreak() synchronously at
    // the point it actually needs the answer, which is also what keeps this
    // process off USB entirely while a pwntool owns the device (see
    // UsbQuietWindow): there is no longer any background thread left that
    // could reach lockdownd/AFC on its own schedule.
}

void DeviceManager::disconnectDevice(uint64_t ecid, const std::string& udid) {
    AppleTVDevice* match = deviceWithUDID(udid, (ecid != (uint64_t)-1) ? ecid : 0);
    if (match) {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        match->connected = 0;
    }
    if (sink_.onDeviceRemoved) sink_.onDeviceRemoved(ecid, udid);
}

// ---------------------------------------------------------------------------
// USB quiet window
// ---------------------------------------------------------------------------

// While an external pwntool owns the device, this process has no business
// touching USB at all: blackb0x-pwn claims the DFU interface for the
// whole exploit and deliberately resets the device several times along the
// way, and the pwn is the one operation here whose USB timing actually
// matters. Left alone, this process talks to the same device from two
// places of its own -- libirecovery's event-handler thread, which
// libusb_open()s every Apple device it sees just to read the serial string,
// and blackb0x_irecv_device_event_cb() calling get_tv() on top of that,
// which opens the device again and retries for up to six seconds -- plus
// libimobiledevice's own event thread, which reaches the device through
// usbmuxd. None of the three is of any use during the exploit either: every
// disconnect/reconnect they report is the pwntool's own normal operation,
// announced against a device this process is not the one driving.
//
// Unsubscribing, rather than just ignoring the callbacks, is what actually
// stops the traffic: both libraries tear their event thread down once the
// last listener is gone (libirecovery.c's irecv_device_event_unsubscribe(),
// idevice.c's idevice_event_unsubscribe()), so for the duration of the
// window this process is single-threaded and genuinely silent on USB.
// libirecovery's unsubscribe additionally blocks until any callback still
// in progress has returned, since the same listener_mutex guards both.
// deviceEventsSuspended_ covers the narrow window before each teardown
// completes.
DeviceManager::UsbQuietWindow::UsbQuietWindow(DeviceManager& deviceManager) : deviceManager_(deviceManager) {
    deviceManager_.deviceEventsSuspended_.store(true);
    if (deviceManager_.irecvEventCtx_) {
        irecv_device_event_unsubscribe(deviceManager_.irecvEventCtx_);
        deviceManager_.irecvEventCtx_ = nullptr;
    }
    idevice_event_unsubscribe();
}

// Re-subscribing replays IRECV_DEVICE_ADD for whatever is present now, which
// is how the device's post-exploit state (the "PWND:[" in its serial string
// in particular) gets back into devices_. It does not re-announce anything:
// newDevice() finds the existing entry by ECID and updates it in place.
DeviceManager::UsbQuietWindow::~UsbQuietWindow() {
    deviceManager_.deviceEventsSuspended_.store(false);
    if (!deviceManager_.irecvEventCtx_) {
        irecv_device_event_subscribe(&deviceManager_.irecvEventCtx_, blackb0x_irecv_device_event_cb, nullptr);
    }
    idevice_event_subscribe(blackb0x_idevice_event_cb, nullptr);
}

// ---------------------------------------------------------------------------
// Exploits
// ---------------------------------------------------------------------------
// SHAtter and checkm8 are ported byte-for-byte from the original: same
// sequence of USB control transfers, same buffer sizes, same sleep amounts.
// The only change is routing status/progress through sink_ instead of
// dispatch_async(dispatch_get_main_queue(), ^{ view.xxx = ... }).

int DeviceManager::SHAtter(uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);

    auto status = [this](const char* s) { if (sink_.onStatus) sink_.onStatus(s); };
    auto progress = [this](double p) { if (sink_.onProgress) sink_.onProgress(p); };

    status("Preparing buffer overflow");
    progress(5.0);

    char data_one[0x40];
    memset(data_one, 0, 0x40);
    reset_counters(client);
    get_data(client, data_one, 0x40);
    progress(15.0);

    usb_reset(client);
    irecv_close(client);

    status("Requesting validation");

    client = get_tv(ecid);
    request_image_validation(client);
    irecv_close(client);

    status("Filling buffer with zeros");
    progress(25.0);
    char data_two[0x2C000];
    memset(data_two, 0, 0x2C000);

    client = get_tv(ecid);
    get_data(client, data_two, 0x2C000);
    progress(30.0);
    irecv_close(client);

    msleep(500);

    status("Overwriting SHA1 registers");
    progress(35.0);
    char data_three[0x140];
    memset(data_three, 0, 0x140);

    client = get_tv(ecid);
    reset_counters(client);
    progress(40.0);
    get_data(client, data_three, 0x140);
    usb_reset(client);
    irecv_close(client);

    client = get_tv(ecid);
    progress(55.0);
    request_image_validation(client);
    irecv_close(client);

    status("Sending SHAtter payload");

    char data_four[0x2C000];
    memset(data_four, 0, 0x2C000);

    client = get_tv(ecid);
    progress(65.0);

    send_buffer(client, SHAtter_payload, 0x800);
    progress(85.0);
    status("Overwriting exception vectors");

    get_data(client, data_four, 0x2C000);
    progress(90.0);

    irecv_close(client);

    msleep(500);

    progress(100.0);
    status("SHAtter successful");

    return 1;
}

// get_tv() gives up after ~6 one-second attempts — fine for reconnecting to
// a device already sitting quietly in a known DFU/Recovery state, but not
// patient enough for the moments in checkm8() where the device is actively
// re-initializing its own USB stack (after a bus reset, or after running
// injected SecureROM-level code) and may need meaningfully longer than 6
// seconds to reappear, especially through some xHCI/Thunderbolt host
// controllers. Confirmed necessary directly: a real run got all the way
// through "Executing payload" and still hit get_tv()'s own give-up path
// right after. Other checkm8 implementations take this to its logical
// extreme — gaster's USB-wait helper (back when this project vendored it)
// never gave up at all, just polled in an unbounded loop until the device
// reappeared or the user killed the process. attempts=30 here isn't
// unbounded (the CLI should
// still eventually report a real failure instead of hanging forever) but
// is deliberately far more patient than get_tv()'s own default.
static irecv_client_t get_tv_patient(uint64_t ecid, int attempts = 30) {
    irecv_client_t client = nullptr;
    for (int attempt = 0; attempt < attempts && !client; attempt++) {
        if (attempt > 0) sleep(1);
        client = get_tv(ecid);
    }
    return client;
}

// No /proc on Darwin, and proc_pidinfo()'s TASK_BASIC_INFO exposes no
// equivalent "genuinely uninterruptible" distinction. `ps -o state=` is the
// standard diagnostic here instead — BSD ps reports 'U' for uninterruptible
// wait. Shelled out via fork/exec+pipe (no popen()/system()) to match this
// file's no-shell convention elsewhere.
static bool isUninterruptible(pid_t pid) {
    int outPipe[2];
    if (pipe(outPipe) != 0) return false;
    pid_t child = fork();
    if (child < 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return false;
    }
    if (child == 0) {
        close(outPipe[0]);
        dup2(outPipe[1], STDOUT_FILENO);
        close(outPipe[1]);
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) dup2(devNull, STDERR_FILENO);
        char pidBuf[32];
        snprintf(pidBuf, sizeof(pidBuf), "%d", (int)pid);
        execlp("ps", "ps", "-o", "state=", "-p", pidBuf, (char*)nullptr);
        _exit(127);
    }
    close(outPipe[1]);
    char buf[64] = {0};
    ssize_t n = read(outPipe[0], buf, sizeof(buf) - 1);
    close(outPipe[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (n <= 0) return false;
    return strchr(buf, 'U') != nullptr;
}

// Manual PATH search (no shell/system() — matches this file's own
// no-shell convention elsewhere) for whether a bare command name resolves
// to an executable file. Used to require `stdbuf` up front rather than
// discovering its absence mid-exploit.
static bool commandExistsOnPath(const char* name) {
    const char* pathEnv = getenv("PATH");
    if (!pathEnv) return false;
    std::string path(pathEnv);
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return true;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

// Resolves the real binary name for GNU coreutils' `stdbuf`. Homebrew's
// `coreutils` formula installs it prefixed (`gstdbuf`) to avoid shadowing the
// BSD toolset, unless the user has separately opted into coreutils' optional
// "gnubin" PATH shim, which exposes it unprefixed — so try the unprefixed
// name first, then fall back to the prefixed one.
static std::string resolveStdbufBinary() {
    if (commandExistsOnPath("stdbuf")) return "stdbuf";
    if (commandExistsOnPath("gstdbuf")) return "gstdbuf";
    return "";
}

// Spawns a pwntool binary, streaming its stdout/stderr straight through to
// blackb0x's own stdout/stderr, verbatim and live rather than
// buffered-then-dumped-on-failure — genuinely useful for this exploit
// specifically, since a stuck or failing run is exactly the kind of thing
// worth watching happen in real time. Bounded by timeoutSeconds (0 = no
// timeout): a pwntool's own wait-for-device and pwn-retry loops are
// genuinely unbounded, and this project's CLI needs to eventually give up
// instead of hanging forever if the device is gone for good — SIGTERM, then
// SIGKILL, on timeout.
//
// SIGKILL is not actually guaranteed to work here: a process blocked
// inside a kernel-level USB control-transfer syscall sits in
// uninterruptible sleep ("D" state, isUninterruptible() above) until that
// specific syscall returns — confirmed to happen for real against this
// exact hardware (a pwntool stuck in D state, unresponsive to
// SIGTERM/SIGKILL, for well over a minute, while a completely independent
// fresh libusb session against the same device worked fine moments
// earlier — see docs/HISTORY.md). No signal can interrupt that; the only real
// fixes are the kernel's own I/O eventually giving up, or physically
// unplugging the device to force it. Rather than block blackb0x itself
// waiting on an unkillable child (a real bug in an earlier version of this
// function: its post-SIGKILL cleanup used a *blocking* waitpid(), which
// just moved the hang from the pwntool into blackb0x itself, confirmed
// live against this same stuck process), this gives the kill a short bounded
// grace period, diagnoses+reports a D-state explicitly if it's still
// there, and moves on regardless — leaving the child to be reaped
// whenever/if the kernel call it's stuck in ever actually returns.
// Wrapped by runBlackb0xPwn() below: spawns binaryPath with args, streaming
// stdout/stderr live via the stdbuf/timeout/D-state machinery described
// above. toolName is used only in diagnostic messages.
//
// Prefixed with `stdbuf -oL -eL`: glibc's (and Darwin libc's) stdio only
// line-buffers stdout/stderr when they're attached to a terminal —
// attached to a pipe (exactly what this function does below), it
// silently switches to full block buffering instead, and neither
// blackb0x-pwn's own main.c nor Checkm8Pwn.c ever call
// setvbuf()/fflush() themselves. Confirmed directly: a short-lived
// invocation (no args, prints usage then exits) shows its output fine
// either way, since exit() flushes stdio regardless — but a long-running
// exploit attempt that never exits until it's done (exactly the "stuck"
// scenario this timeout/D-state handling exists for) would never flush
// its progress lines to the pipe at all, making the live-streaming below
// silently useless for the one case it actually matters for. `stdbuf`
// (GNU coreutils, LD_PRELOADs a constructor that calls setvbuf() before
// the child's own main() runs) fixes this without needing to patch
// either child just to add a setvbuf() call.
//
// Treated as a hard requirement, not a nice-to-have: an earlier version
// of this function fell back to running unbuffered if stdbuf wasn't
// found, which silently reintroduces exactly the "blind the whole time"
// blind spot documented in docs/HISTORY.md — the one this whole
// mechanism exists to fix, and precisely when it would matter most (a
// stuck/hanging exploit run). Fail loudly and immediately instead,
// before ever forking the child.
static int runLineBufferedSubprocess(const std::string& binaryPath, const std::vector<std::string>& args,
                                      int timeoutSeconds, const char* toolName) {
    std::string stdbufBin = resolveStdbufBinary();
    if (stdbufBin.empty()) {
        fprintf(stderr,
                "checkm8: `stdbuf` (GNU coreutils) is required but not found on PATH -- "
                "without it, %s's exploit progress can't be streamed live, which makes "
                "a stuck/hanging run indistinguishable from a silently-working one. "
                "Install it with `brew install coreutils` (this program looks for both the "
                "unprefixed `stdbuf` and Homebrew's default `gstdbuf` name) and try again.\n"
                , toolName);
        return -1;
    }
    std::vector<std::string> argvStrings = { stdbufBin, "-oL", "-eL", binaryPath };
    argvStrings.insert(argvStrings.end(), args.begin(), args.end());
    std::vector<char*> cargv;
    cargv.reserve(argvStrings.size() + 1);
    for (auto& a : argvStrings) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0) return -1;
    if (pipe(errPipe) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[1]);
        close(errPipe[1]);
        execvp(stdbufBin.c_str(), cargv.data());
        // Only reachable if stdbuf disappeared between the PATH check above
        // and this exec (a real TOCTOU window, not expected in practice) --
        // no silent fallback here, per the "required" reasoning above.
        _exit(127);
    }
    close(outPipe[1]);
    close(errPipe[1]);
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    auto pump = [&]() {
        // read() hands back arbitrary chunk boundaries, which routinely fall
        // mid-line -- holding the console lock across the whole drain keeps
        // anything else that prints from landing inside one of those partial
        // lines. In practice checkm8Attempt() already silences every other
        // source for the duration of the pwn (see UsbQuietWindow); this is
        // just so the helper stays safe if it ever gets used somewhere that
        // doesn't.
        console::Block block;
        char buf[512];
        ssize_t n;
        while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)n, stdout);
        while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)n, stderr);
        fflush(stdout);
        fflush(stderr);
    };

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    bool timedOut = false;
    int status = 0;
    for (;;) {
        pump();
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (timeoutSeconds > 0 && std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            break;
        }
        struct pollfd pfds[2] = { { outPipe[0], POLLIN, 0 }, { errPipe[0], POLLIN, 0 } };
        poll(pfds, 2, 200);
    }

    if (timedOut) {
        kill(pid, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 10 && !reaped; i++) {
            usleep(100000);
            pump();
            reaped = waitpid(pid, &status, WNOHANG) == pid;
        }
        if (!reaped) {
            kill(pid, SIGKILL);
            for (int i = 0; i < 10 && !reaped; i++) {
                usleep(100000);
                pump();
                reaped = waitpid(pid, &status, WNOHANG) == pid;
            }
        }
        if (!reaped) {
            if (isUninterruptible(pid)) {
                fprintf(stderr,
                        "checkm8: %s (pid %d) is stuck in an uninterruptible kernel USB "
                        "wait and can't be killed by software. It will keep running in the "
                        "background until whatever syscall it's blocked in returns on its own "
                        "-- this usually needs the Apple TV physically unplugged from USB to "
                        "clear. Continuing without waiting for it further.\n",
                        toolName, (int)pid);
            } else {
                fprintf(stderr, "checkm8: %s (pid %d) did not exit after SIGKILL.\n", toolName, (int)pid);
            }
        }
    }

    pump();
    close(outPipe[0]);
    close(errPipe[0]);

    if (timedOut) return -2;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int runBlackb0xPwn(const std::vector<std::string>& args, int timeoutSeconds) {
    return runLineBufferedSubprocess(resolvePwnPath(), args, timeoutSeconds, "blackb0x-pwn");
}

// The low-level USB request sequence/payload/timing that used to live
// directly in this function (hand-ported from the original
// DeviceManager.m) is gone from HERE, but not from the project: it now
// lives in src/Pwn/ and runs as the standalone `blackb0x-pwn`
// binary, which this function shells out to for the actual pwn step.
//
// For a long stretch this shelled out to a vendored `gaster` instead, with
// --pwntool to pick between the two. gaster is gone entirely: it was never
// made to pwn an AppleTV3,2 on either Linux 7.1.x or macOS 26, over an
// investigation long enough to have its own section in docs/HISTORY.md.
// Don't reintroduce it without new evidence.
//
// After a successful pwn this still reconnects and checks for "PWND:[" in
// the serial string itself — blackb0x-pwn reports its own success/failure
// via exit status, but that's worth confirming independently before handing
// control back to the rest of this project's own boot-chain code
// (sendiBSS/sendiBEC/etc., all unchanged).
bool DeviceManager::checkm8Attempt(uint64_t ecid) {
    auto status = [this](const char* s) { if (sink_.onStatus) sink_.onStatus(s); };
    auto progress = [this](double p) { if (sink_.onProgress) sink_.onProgress(p); };

    // The device may already be sitting in pwned DFU before the exploit
    // tool is ever invoked: checkm8()'s own retry loop calling this
    // function again, a previous CLI run that got killed/crashed after the
    // pwn actually succeeded but before this function's own post-pwn
    // verification ran, or the tool itself finishing the pwn in the
    // background after this project gave up waiting on it (see
    // runLineBufferedSubprocess()'s timeout/D-state handling above — this
    // is exactly the scenario that produces: confirmed live, a pwntool
    // stuck past its own timeout while the device had, per this same
    // check, already rebooted into pwned DFU). Re-running the exploit
    // against an already-pwned device is pointless at best; check first,
    // quickly (get_tv(), not get_tv_patient() — if it's not there within
    // get_tv()'s own short default, it's almost certainly not already
    // pwned and yet-unconnected, not worth 30 seconds of patience just to
    // rule that out), and skip straight to success if it's already there.
    {
        irecv_client_t already = get_tv(ecid);
        if (already) {
            const struct irecv_device_info* info = irecv_get_device_info(already);
            bool alreadyPwned = isPwnedDFU(info);
            irecv_close(already);
            if (alreadyPwned) {
                status("Device already in pwned DFU");
                status("Checkm8 successful");
                progress(100.0);
                return 1;
            }
        }
    }

    status("Exploiting with checkm8");
    progress(10.0);

    // Held across the exploit AND its verification below, not just the
    // subprocess: letting libirecovery's event thread back up the moment the
    // pwntool exits would put a second libusb_open() of the device in flight
    // against get_tv_patient()'s own reconnect retries, at exactly the moment
    // the device is still settling from the exploit's last reset.
    UsbQuietWindow quiet(*this);

    // blackb0x-pwn's own CLI takes an explicit --ecid, so pass it through
    // for precision rather than letting it grab whichever DFU device it
    // finds first.
    int exitCode = runBlackb0xPwn({"checkm8", "--ecid", std::to_string(ecid)}, 180);
    if (exitCode != 0) {
        if (exitCode == -2) {
            fprintf(stderr, "checkm8: blackb0x-pwn timed out waiting for the device.\n");
        } else {
            fprintf(stderr, "checkm8: blackb0x-pwn failed (exit %d).\n", exitCode);
        }
        status("Checkm8 unsuccessful");
        progress(100.0);
        return 0;
    }

    progress(80.0);

    irecv_client_t client = get_tv_patient(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device did not reappear after blackb0x-pwn.\n");
        status("Checkm8 unsuccessful");
        progress(100.0);
        return 0;
    }

    const struct irecv_device_info* info = irecv_get_device_info(client);
    if (!isPwnedDFU(info)) {
        irecv_close(client);
        fprintf(stderr, "checkm8: device did not report pwned DFU after blackb0x-pwn.\n");
        status("Checkm8 unsuccessful");
        progress(100.0);
        return 0;
    }

    status("Checkm8 successful");
    progress(100.0);

    irecv_close(client);
    return 1;
}

// blackb0x-pwn already retries internally and unboundedly within a single
// invocation (its get_tv() is patient by design) — this outer retry is
// mostly a safety net for the rarer case of a hard failure/timeout out of
// runLineBufferedSubprocess() itself (e.g. the child exiting outright, or
// this project's own 180s ceiling on top of the child's patience being
// hit). kMaxAttempts stays finite so the CLI still eventually reports a
// real, actionable failure rather than retrying forever.
int DeviceManager::checkm8(uint64_t ecid) {
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
        if (checkm8Attempt(ecid)) return 1;
        if (attempt < kMaxAttempts) {
            if (sink_.onStatus) sink_.onStatus("checkm8 attempt failed, retrying...");
            sleep(1);
        }
    }
    return 0;
}

static void reset_counters(irecv_client_t client) {
    irecv_reset_counters(client);
}

static void usb_reset(irecv_client_t client) {
    irecv_reset(client);
}

static void send_buffer(irecv_client_t client, unsigned char* data, unsigned long size) {
    irecv_send_buffer(client, data, size, 0);
}

static void get_data(irecv_client_t client, char* buffer, unsigned long length) {
    irecv_recv_buffer(client, buffer, length);
}

static void request_image_validation(irecv_client_t client) {
    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, nullptr, 0, 1000);

    unsigned char blank[16];
    memset(blank, 0, 16);

    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
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

// ---------------------------------------------------------------------------
// AFC connection (jailbreak detection)
// ---------------------------------------------------------------------------

// Uses the from-scratch AfcClient (see AfcClient.hpp) over the in-process
// mux — this tool's target hardware is never reachable through the system
// usbmuxd, so there is no other path to try (see docs/HISTORY.md).
int isJailbroken(const std::string& udid) {
    idevice_t device = nullptr;
    if (idevice_new(&device, udid.c_str()) != IDEVICE_E_SUCCESS) {
        return -1;
    }

    lockdownd_client_t lockdown_client = nullptr;
    if (lockdownd_client_new_with_handshake(device, &lockdown_client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
        idevice_free(device);
        return -1;
    }

    lockdownd_service_descriptor_t port = nullptr;
    if (lockdownd_start_service(lockdown_client, "com.apple.afc", &port) != LOCKDOWN_E_SUCCESS) {
        lockdownd_client_free(lockdown_client);
        idevice_free(device);
        return -1;
    }

    afc_client_t afc_client = nullptr;
    if (afc_client_new(device, port, &afc_client) != AFC_E_SUCCESS) {
        lockdownd_client_free(lockdown_client);
        idevice_free(device);
        return -1;
    }

    int jailbroken = 0;
    char** dirs = nullptr;
    afc_read_directory(afc_client, "/", &dirs);

    if (dirs) {
        for (int i = 0; dirs[i]; i++) {
            if (strcmp(dirs[i], ".blackb0x") == 0) jailbroken = 1;
            free(dirs[i]);
        }
        free(dirs);
    }

    afc_client_free(afc_client);
    lockdownd_client_free(lockdown_client);
    idevice_free(device);

    return dirs ? jailbroken : -1;
}

int isJailbreakRunning(const std::string& udid) {
    idevice_t device = nullptr;
    if (idevice_new(&device, udid.c_str()) != IDEVICE_E_SUCCESS) {
        return -1;
    }

    lockdownd_client_t lockdown_client = nullptr;
    if (lockdownd_client_new_with_handshake(device, &lockdown_client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
        idevice_free(device);
        return -1;
    }

    lockdownd_service_descriptor_t port = nullptr;
    lockdownd_error_t lderr = lockdownd_start_service(lockdown_client, "com.apple.afc2", &port);

    lockdownd_client_free(lockdown_client);
    idevice_free(device);

    if (lderr != LOCKDOWN_E_SUCCESS) {
        return 0;
    }

    return 1;
}

// waitForAFC2's original recursive NSThread-sleep retry, now a plain bounded
// blocking loop — called only from checkJailbreak(), which the main flow
// drives synchronously at the point it needs the answer.
void DeviceManager::checkJailbreakRunning(const std::string& udid) {
    int jb = -1;
    for (int attempts = 5; attempts > 0; attempts--) {
        jb = isJailbreakRunning(udid);
        if (jb != -1) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    AppleTVDevice* icon = deviceWithUDID(udid);
    if (!icon) return;

    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        icon->jailbreakRunning = jb;
    }

    if (sink_.onDeviceUpdated) sink_.onDeviceUpdated(*icon);
}

// Runs synchronously on whatever thread asks, which in practice is only ever
// Cli.cpp's main flow (see newDevice()'s own comment on why this is no longer
// dispatched onto a thread of its own).
//
// The retry is bounded, unlike the original's recursive dispatch_after, which
// re-armed itself indefinitely: a device that never answers over AFC at all
// has to end this call rather than wedge the one thread there is. Running out
// of attempts leaves the device's jailbroken flag untouched at its default of
// 0, which is the same answer the caller was already getting in practice --
// the detached version almost never finished before the main flow read it.
//
// Idempotent per UDID once it has an answer: selectDevice() can reach this
// once per pass through its own poll loop, and re-running a full AFC
// handshake each time would be several seconds of USB traffic for something
// that cannot have changed. A pass that ran out of attempts without getting
// an answer is deliberately not recorded, so a device that was merely slow
// to come up over AFC still gets looked at again on the next pass.
void DeviceManager::checkJailbreak(const std::string& udid) {
    if (udid.empty()) return;
    if (jailbreakChecked_.count(udid)) return;

    int jailbroken = -1;
    for (int attempts = 20; attempts > 0 && jailbroken == -1; attempts--) {
        jailbroken = isJailbroken(udid);
        if (jailbroken == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    if (jailbroken == -1) return;
    jailbreakChecked_.insert(udid);

    AppleTVDevice* icon = deviceWithUDID(udid);
    if (!icon) return;

    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        if (icon->jailbroken != 1) icon->jailbroken = jailbroken;
    }

    if (sink_.onDeviceUpdated) sink_.onDeviceUpdated(*icon);

    if (jailbroken) {
        checkJailbreakRunning(udid);
    } else {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        icon->jailbreakRunning = 0;
    }
}

// ---------------------------------------------------------------------------
// Events and callbacks
// ---------------------------------------------------------------------------

static int progress_cb(irecv_client_t client, const irecv_event_t* event) {
    (void)client;
    if (DeviceManager::instance() && DeviceManager::instance()->sink().onProgress) {
        DeviceManager::instance()->sink().onProgress(event->progress);
    }
    return 0;
}

extern "C" int blackb0x_irecv_progress_cb(irecv_client_t client, const irecv_event_t* event) {
    return progress_cb(client, event);
}

extern "C" void blackb0x_irecv_device_event_cb(const irecv_device_event_t* event, void* user_data) {
    (void)user_data;
    if (!DeviceManager::instance()) return;
    // A pwntool owns the device right now -- see UsbQuietWindow. Belt-and-
    // braces against a callback already in flight when it unsubscribed: the
    // get_tv() below would otherwise open the device out from under the
    // exploit.
    if (DeviceManager::instance()->deviceEventsSuspended_.load()) return;

    uint64_t ecid = event->device_info->ecid;

    if (event->type == IRECV_DEVICE_ADD) {
        irecv_client_t client = get_tv(ecid);
        irecv_device_t device = nullptr;
        irecv_devices_get_device_by_client(client, &device);

        int mode = 0;
        irecv_get_mode(client, &mode);

        const char* modeStr = mode_to_str(mode);
        const char* productType = device ? device->product_type : "";

        const char* serial = event->device_info->serial_string;

        // irecv_get_mode()'s PID read is unreliable at exactly this
        // DFU<->Recovery transition (see serialStringIndicatesRealDFU()'s
        // own comment for the documented upstream reports and a real
        // capture pair) -- this is the device-classification point
        // waitForDFUMode()/checkm8Attempt() (Cli.cpp) actually gate on, so
        // a misreported "DFU" here means checkm8 gets attempted against a
        // device that's still genuinely in Recovery mode underneath (no
        // "SRTG:[iBoot-...]" in its serial string at all). Correct it back
        // to "Recovery" before it ever reaches icon->mode below, the same
        // way the later stockSecurerom send-file branch already trusts the
        // serial string over this same PID read.
        if (modeStr && strcmp(modeStr, "DFU") == 0 && !serialStringIndicatesRealDFU(serial)) {
            modeStr = "Recovery";
        }

        int pwnedDFU = 0;
        if (serial && strstr(serial, "SHAtter")) pwnedDFU = 1;
        if (serial && strstr(serial, "checkm8")) pwnedDFU = 2;

        DeviceManager::instance()->newDevice(productType ? productType : "", modeStr, "", "",
                                              ecid, "", pwnedDFU);

        irecv_close(client);
    } else {
        DeviceManager::instance()->disconnectDevice(ecid, "");
    }
}

extern "C" void blackb0x_idevice_event_cb(const idevice_event_t* event, void* user_data) {
    (void)user_data;
    if (!DeviceManager::instance()) return;
    if (DeviceManager::instance()->deviceEventsSuspended_.load()) return;
    if (event->udid == nullptr) return;
    if (event->conn_type == CONNECTION_NETWORK) return;

    std::string udid = event->udid;

    switch (event->event) {
        case IDEVICE_DEVICE_ADD: {
            NormalModeInfo info = plistInfoForDeviceUUID(udid);
            DeviceManager::instance()->newDevice(info.productType, "Normal", info.productVersion,
                                                  info.buildVersion, info.uniqueChipID,
                                                  info.uniqueDeviceID, 0);
            break;
        }
        case IDEVICE_DEVICE_PAIRED:
            break;
        case IDEVICE_DEVICE_REMOVE:
            DeviceManager::instance()->disconnectDevice((uint64_t)-1, udid);
            break;
        default:
            break;
    }
}

static const char* mode_to_str(int mode) {
    switch (mode) {
        case IRECV_K_RECOVERY_MODE_1:
        case IRECV_K_RECOVERY_MODE_2:
        case IRECV_K_RECOVERY_MODE_3:
        case IRECV_K_RECOVERY_MODE_4:
            return "Recovery";
        case IRECV_K_DFU_MODE:
            return "DFU";
        case IRECV_K_WTF_MODE:
            return "WTF";
        default:
            return "Unknown";
    }
}

// irecv_get_mode()'s own PID-based detection is a well-documented,
// multi-year bug/quirk across libirecovery/idevicerestore for exactly
// the DFU<->Recovery transition this project cares about -- confirmed
// via real GitHub reports of the identical symptom (idevicerestore#78,
// libirecovery#50, tr4mpass#61: irecv_get_mode() reports DFU_MODE right
// after a reconnect that's actually still fully functional as Recovery
// mode underneath, serial string included) and idevicerestore's own
// source carrying a literal "TODO: verify if it actually goes from
// 0x1222 -> 0x1227" comment on this exact transition. Confirmed directly
// on real hardware here too: --stock-securerom's iBSS send silently took
// the wrong wire protocol (irecv_send_buffer()'s DFU-vs-Recovery branch
// is chosen by the SAME misdetected client->mode) because of this.
//
// The device's own serial string is a more trustworthy signal: real
// SecureROM DFU mode's serial string embeds "SRTG:[iBoot-x.x.x]" (the
// running SecureROM/iBoot build identifier); Recovery mode's has no such
// field, carrying "SRNM:[<serial>]" (the device's real hardware serial
// number) instead. Confirmed against real captures of both:
//   DFU:      CPID:8010 CPRV:11 CPFM:03 SCEP:01 BDID:0C ECID:... IBFL:3C SRTG:[iBoot-2696.0.0.1.33]
//   Recovery: CPID:8947 CPRV:00 CPFM:03 SCEP:10 BDID:00 ECID:000002713C84D50E IBFL:1B SRNM:[F6KM4D1TFF54]
static bool serialStringIndicatesRealDFU(const char* serialString) {
    return serialString && strstr(serialString, "iBoot") != nullptr;
}

// Shared by checkm8Attempt()/boot_client() below (the three call sites
// this used to be duplicated at, one of which -- boot_client() -- was
// missing this function's own null checks entirely, dereferencing
// info->serial_string completely unguarded): true exactly when this
// device's serial string carries "PWND:[", the only signal this project
// has for telling a checkm8/SHAtter-exploited DFU device apart from a
// genuinely un-pwned one. Both `info` itself and `info->serial_string`
// need checking -- irecv_get_device_info() can return null, and even
// when it doesn't, serial_string can still be null (see
// serialStringIndicatesRealDFU()'s own null check just above for the
// same reason).
static bool isPwnedDFU(const struct irecv_device_info* info) {
    return info && info->serial_string && strstr(info->serial_string, "PWND:[") != nullptr;
}

// ---------------------------------------------------------------------------
// iRecovery functions (shared)
// ---------------------------------------------------------------------------

irecv_client_t DeviceManager::get_tv(uint64_t ecid) {
    return ::get_tv(ecid);
}

static irecv_client_t get_tv(uint64_t ecid) {
    irecv_client_t client = nullptr;

    for (int i = 0; i <= 5; i++) {
        irecv_error_t err = irecv_open_with_ecid(&client, ecid);

        if (err == IRECV_E_UNSUPPORTED) {
            console::err("ERROR: %s\n", irecv_strerror(err));
            return nullptr;
        } else if (err != IRECV_E_SUCCESS) {
            // Visible retry progress: without this, a device that dropped
            // off USB mid-sequence (e.g. it rebooted to Normal Mode instead
            // of staying in the exploited state after an iBEC send) looks
            // identical, from the log alone, to one that's simply slow to
            // re-enumerate -- both just sit silent for ~6 seconds before the
            // final "Unable to connect to device". Surfacing each attempt's
            // own error lets that distinction actually be made from the log.
            console::err("  (reconnect attempt %d/6: %s, retrying...)\n", i + 1, irecv_strerror(err));
            sleep(1);
        } else {
            break;
        }

        if (i == 5) {
            console::err("ERROR: %s\n", irecv_strerror(err));
            return nullptr;
        }
    }

    irecv_event_subscribe(client, IRECV_PROGRESS, &blackb0x_irecv_progress_cb, nullptr);
    return client;
}

// ---------------------------------------------------------------------------
// MobileDevice plist handling
// ---------------------------------------------------------------------------
// Replaces dictionaryFromPlist:'s generic NSDictionary conversion with a
// direct extraction of just the fields Blackb0x actually consumes (see
// addDeviceWithInfo: in the original).

// Uses LegacyLockdownClient over the in-process mux -- this tool's target
// hardware is never reachable through the system usbmuxd, so there is no
// other path to try (see docs/HISTORY.md).
NormalModeInfo plistInfoForDeviceUUID(const std::string& udid) {
    NormalModeInfo out;

    idevice_t device = nullptr;
    if (idevice_new_with_options(&device, udid.c_str(), IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS) {
        return out;
    }

    lockdownd_client_t client = nullptr;
    if (lockdownd_client_new_with_handshake(device, &client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (lockdownd_client_new_with_handshake(device, &client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
            console::err("ERROR: Could not connect to lockdownd\n");
            idevice_free(device);
            return out;
        }
    }

    plist_t node = nullptr;
    if (lockdownd_get_value(client, nullptr, nullptr, &node) == LOCKDOWN_E_SUCCESS && node) {
        auto getString = [&](const char* key) -> std::string {
            plist_t item = plist_dict_get_item(node, key);
            if (!item || plist_get_node_type(item) != PLIST_STRING) return "";
            char* s = nullptr;
            plist_get_string_val(item, &s);
            std::string result = s ? s : "";
            free(s);
            return result;
        };

        out.productType = getString("ProductType");
        out.productVersion = getString("ProductVersion");
        out.buildVersion = getString("BuildVersion");
        out.uniqueDeviceID = getString("UniqueDeviceID");

        plist_t chipId = plist_dict_get_item(node, "UniqueChipID");
        if (chipId) {
            uint64_t u = 0;
            plist_get_uint_val(chipId, &u);
            out.uniqueChipID = u;
        }

        out.valid = true;
        plist_free(node);
    }

    lockdownd_client_free(client);
    idevice_free(device);

    return out;
}

// ---------------------------------------------------------------------------
// iRecovery (iBSS, iBEC, Ramdisk, Kernel, DeviceTree)
// ---------------------------------------------------------------------------

// Status for all send*() calls is reported by the caller (Cli.cpp's
// sendComponentsToDevice(), which already knows exactly when each one
// starts and what its result was) — these stay quiet on success and only
// report genuine, otherwise-unexplained failures.
int DeviceManager::sendiBSS(const std::string& iBSSpath, uint64_t ecid, bool stockRecovery, bool stockSecurerom,
                             std::shared_ptr<void> buildIdentity, const std::string& deviceModel,
                             const std::string& buildID) {
    irecv_client_t client = get_tv(ecid);
    if (!client) {
        return -1;
    }

    irecv_device_t device = nullptr;
    irecv_devices_get_device_by_client(client, &device);

    bool isATV31 = strstr(device->product_type, "AppleTV3,1") != nullptr;
    bool isATV32 = strstr(device->product_type, "AppleTV3,2") != nullptr;

    // Unambiguous, always-printed record of which of the two very
    // differently-shaped upload paths below this run actually took --
    // there was previously no way to tell from the terminal output alone
    // whether a --stock-securerom run really exercised the standard
    // irecv_send_file() route or silently fell through to boot_client().
    fprintf(stderr,
            "sendiBSS: device reports product_type \"%s\" (isATV31=%d, isATV32=%d), stockRecovery=%d, "
            "stockSecurerom=%d -> using %s route.\n",
            device->product_type, isATV31, isATV32, stockRecovery, stockSecurerom,
            (isATV31 || isATV32) ? (stockSecurerom ? "standard irecv_send_file()" : "checkm8 soft-DFU boot_client()")
                                  : "AppleTV2,1 irecv_send_file()");

    if ((isATV31 || isATV32) && stockSecurerom) {
        // --stock-securerom: boot_client() below (used by sendiBSS_ATV31()/
        // sendiBSS_ATV32()) is a custom soft-DFU sequence shaped around
        // checkm8's own post-exploit memory-corruption state, not the real
        // USB DFU class protocol SecureROM itself implements -- it has no
        // reason to work against a device that was never exploited,
        // independent of whether the iBSS content is valid. Use the same
        // standard irecv_send_file() route every other component (iBEC,
        // ramdisk, kernelcache) already uses instead, so this is at least
        // a real attempt at real DFU-protocol delivery rather than one
        // that fails at the USB state-machine level regardless of content.
        //
        // Skipping boot_client() also means skipping its own "PWND:[" gate
        // entirely -- but iBSS can only ever be accepted in DFU mode
        // (Recovery-mode iBoot has no use for another iBSS), pwned or not,
        // so that gate still needs a DFU-mode replacement here rather than
        // no gate at all. serialStringIndicatesRealDFU(), not
        // irecv_get_mode() -- see that function's own comment for why.
        const struct irecv_device_info* dfuCheckInfo = irecv_get_device_info(client);
        const char* dfuCheckSerial = dfuCheckInfo ? dfuCheckInfo->serial_string : nullptr;
        if (!serialStringIndicatesRealDFU(dfuCheckSerial)) {
            int mode = 0;
            irecv_get_mode(client, &mode);
            fprintf(stderr,
                    "sendiBSS: device is not in DFU mode -- iBSS can only be sent to a device in DFU mode. "
                    "(irecv_get_mode() reports: %s; serial string: %s)\n",
                    mode_to_str(mode), dfuCheckSerial ? dfuCheckSerial : "(none)");
            irecv_close(client);
            return -1;
        }

        // A real, un-pwned SecureROM also needs a live, ECID/nonce-bound
        // SHSH ticket for iBSS before it will accept it at all -- see
        // Personalize.hpp's own comment. The nonce is read here, from
        // this same DFU-mode session, since it's what the ticket has to
        // be bound to.
        const struct irecv_device_info* info = irecv_get_device_info(client);
        auto personalized = personalizeIMG3Component("iBSS", iBSSpath, buildIdentity, ecid, info->ap_nonce,
                                                       info->ap_nonce_size, deviceModel, buildID);
        if (!personalized) {
            irecv_close(client);
            return -1;
        }

        std::string personalizedPath = iBSSpath + ".personalized";
        {
            std::ofstream out(personalizedPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                fprintf(stderr, "sendiBSS: failed to write %s\n", personalizedPath.c_str());
                irecv_close(client);
                return -1;
            }
            out.write(reinterpret_cast<const char*>(personalized->data()), personalized->size());
        }

        irecv_error_t err = irecv_send_file(client, personalizedPath.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
        irecv_close(client);
        return (err == IRECV_E_SUCCESS) ? 0 : -1;
    }

    // The stock-iBSS-on-checkm8 case is handled upstream, not here: on A5
    // without --stock-securerom, Patcher::useStockIBSS() has already
    // decrypted Apple's stock iBSS to plaintext DATA before this point,
    // precisely because boot_client() below uploads the DATA payload directly
    // as code. So the file arriving here is runnable either way -- baked
    // (blackb0x-patched, decrypted) or stock (decrypted by useStockIBSS()).
    bool allowUnpwned = stockRecovery || stockSecurerom;

    if (isATV31) {
        irecv_close(client);
        return sendiBSS_ATV31(ecid, iBSSpath.c_str(), allowUnpwned);
    }

    if (isATV32) {
        irecv_close(client);
        return sendiBSS_ATV32(ecid, iBSSpath.c_str(), allowUnpwned);
    }

    // AppleTV2,1
    irecv_error_t err = irecv_send_file(client, iBSSpath.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    irecv_close(client);
    return (err == IRECV_E_SUCCESS) ? 0 : -1;
}

int DeviceManager::sendiBEC(const std::string& iBECpath, uint64_t ecid) {
    // get_tv_patient(), not plain get_tv(): this reconnect follows iBSS's
    // boot_client() actually manifesting/resetting the device now (see the
    // DFU fix above) -- like checkm8's own post-payload reconnect, a device
    // that just started executing freshly-uploaded code can take longer to
    // re-enumerate than get_tv()'s own plain ~6-second retry budget.
    irecv_client_t client = get_tv_patient(ecid);
    if (!client) {
        fprintf(stderr, "sendiBEC: device did not reconnect for %s\n", iBECpath.c_str());
        return -1;
    }
    // Real idevicerestore's own dfu_enter_recovery() (dfu.c) explicitly
    // sets the USB configuration back to 1 right after this same
    // post-iBSS reconnect, before sending iBEC -- blackb0x never did this
    // at all. Harmless if the device was already on configuration 1 (the
    // only one these DFU/Recovery-mode devices ever expose), but matches
    // idevicerestore exactly rather than assuming libirecovery's own
    // reconnect always leaves it set.
    irecv_usb_set_configuration(client, 1);
    irecv_error_t err = irecv_send_file(client, iBECpath.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "sendiBEC: failed to send %s: %s\n", iBECpath.c_str(), irecv_strerror(err));
        irecv_close(client);
        sleep(2);
        return -1;
    }

    // Execute the just-uploaded iBEC, matching real idevicerestore's
    // dfu_enter_recovery() (dfu.c) / recovery_send_ibec() (recovery.c) rather
    // than the original blackb0x, which sent nothing here and relied on
    // IRECV_SEND_OPT_DFU_NOTIFY_FINISH alone to jump into the image. Favouring
    // idevicerestore, the maintained reference: after the upload it does
    // sleep(1), then `go` as a bRequest=1 command (irecv_send_command_breq),
    // then a zero-length DFU_DNLOAD-class control transfer (0x21/1, wLength=0)
    // -- the "commit/execute" step, gated on build_major < 20 there, always
    // true for this A5 hardware. NOTIFY_FINISH finalizes the download; `go` is
    // the actual jump. `go` is best-effort here (logged, not fatal): the
    // device re-enumerates into Recovery mode as it executes iBEC, so the
    // command can legitimately return before the follow-up transfer lands, and
    // the next step (RestoreLogo) reconnects with get_tv_patient() regardless.
    sleep(1);
    irecv_error_t goErr = irecv_send_command_breq(client, "go", 1);
    if (goErr != IRECV_E_SUCCESS) {
        fprintf(stderr, "sendiBEC: 'go' after iBEC returned %s (continuing -- iBEC likely already executing)\n",
                irecv_strerror(goErr));
    }
    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, nullptr, 0, 5000);

    irecv_close(client);
    sleep(2);
    return 0;
}

// Shared by sendRamdisk()/sendKernelCache()/sendDeviceTree() below: send a
// file, then a follow-up command that tells the device what to do with it
// (e.g. "ramdisk", "bootx", "devicetree"). Sending the command after a
// failed/short file transfer would just be asking an already-gone device to
// act on data it never fully received, so this bails (and reports exactly
// which of the two steps failed, and why) rather than sending it anyway and
// unconditionally reporting success like the three callers used to.
// keepOpen: for sendStockRestoreTail() below -- when true, never closes
// `client` (success or failure), leaving connection lifecycle entirely
// to the caller. Every other caller here leaves this at its default
// (false, unchanged behavior).
// commandBreq: the USB control transfer's own bRequest field for the
// follow-up command -- irecv_send_command()'s default (still what every
// OTHER command here uses: "ticket"/"setpicture"/"bgcolor"/"ramdisk"/
// "devicetree"/"firmware") is bRequest=0, but real idevicerestore
// (recovery.c) sends its two actual boot-triggering commands ("go" for
// iBEC, "bootx" for the kernelcache) with bRequest=1 specifically via
// irecv_send_command_breq() -- confirmed against a real AppleTV3,2: a
// plain bRequest=0 "bootx" gets acknowledged over USB same as any other
// command, but the device never actually executes the boot, instead
// resetting back to iBoot's own command prompt ("Boot Failure Count"
// climbing on every attempt). sendKernelCache()/sendStockRestoreTail()
// below both pass 1 for their own "bootx" call for exactly this reason.
// dnloadFinish: real idevicerestore's own recovery_send_kernelcache()
// (recovery.c) sends a zero-length, DFU-class control transfer
// (bmRequestType 0x21, bRequest 1 -- literal DFU_DNLOAD, the same
// "that upload is finished, do something with it" signal boot_client()'s
// own checkm8 soft-DFU code elsewhere in this file already relies on)
// right after the kernelcache file upload, before its own boot-args/
// bootx commands -- completely missing here before this fix, on top of
// the bReq=1 bootx fix above. Confirmed on real hardware: bReq=1 alone
// (without this) still left the device resetting back to iBoot's own
// prompt every time. sendKernelCache() below passes true for exactly
// this reason; every other caller leaves this at its default (false).
// extraCommandBeforeMain: real idevicerestore's own recovery_send_ramdisk()
// fires "getenv ramdisk-delay" (fire-and-forget, plain irecv_send_command()
// -- no response read at all, unlike irecv_getenv()) right after the
// ramdisk file upload, before the actual "ramdisk" command. sendRamdisk()/
// sendStockRestoreTail() below both pass "getenv ramdisk-delay" for
// exactly this component; every other caller leaves this at its default
// (nullptr, no extra command sent).
static int sendFileThenCommand(irecv_client_t client, const char* what, const std::string& path,
                                const char* command, bool keepOpen = false, uint8_t commandBreq = 0,
                                bool dnloadFinish = false, const char* extraCommandBeforeMain = nullptr,
                                bool extraCommandMustSucceed = false) {
    if (!client) {
        fprintf(stderr, "%s: device did not reconnect for %s\n", what, path.c_str());
        return -1;
    }
    irecv_error_t err = irecv_send_file(client, path.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "%s: failed to send %s: %s\n", what, path.c_str(), irecv_strerror(err));
        if (!keepOpen) irecv_close(client);
        return -1;
    }
    if (dnloadFinish) {
        irecv_usb_control_transfer(client, 0x21, 1, 0, 0, nullptr, 0, 5000);
    }
    if (extraCommandBeforeMain) {
        // "getenv ramdisk-delay" is genuinely fire-and-forget -- real
        // idevicerestore ignores its result too, and nothing depends on it.
        // "setenv boot-args ..." is not: it is the ONLY thing that puts
        // rd=md0 and the AMFI/code-signing args in front of the kernel now
        // that patch_boot_args() is no longer used. If it silently fails,
        // iBoot falls back to its own compiled-in default
        // ("rd=md0 nand-enable-reformat=1 -progress"), which has no
        // amfi=0xff/cs_enforcement_disable=1 at all -- so entrypoint.c, an
        // unsigned binary, could not exec as PID 1 and the boot would fail
        // for a reason nothing in the log would explain.
        irecv_error_t extraErr = irecv_send_command(client, extraCommandBeforeMain);
        if (extraErr != IRECV_E_SUCCESS) {
            fprintf(stderr, "%s: '%s' failed: %s\n", what, extraCommandBeforeMain, irecv_strerror(extraErr));
            if (extraCommandMustSucceed) {
                if (!keepOpen) irecv_close(client);
                return -1;
            }
        }
    }
    err = irecv_send_command_breq(client, command, commandBreq);
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "%s: failed to send '%s' command: %s\n", what, command, irecv_strerror(err));
        if (!keepOpen) irecv_close(client);
        return -1;
    }
    // Previously silent on success -- this step's own USB-level outcome
    // (file transferred, command acknowledged) is worth logging
    // unconditionally, not just its failures, since it's the only
    // ground-truth this project has for "did the device actually do what
    // we asked" without a serial console. Notably: this only confirms the
    // COMMAND was accepted over USB, not that whatever the device does in
    // response to it (mounting a ramdisk, booting a kernel, etc.) actually
    // succeeds -- sendKernelCache() below adds an explicit post-'bootx'
    // check for exactly that gap, since USB-level success and an actual
    // successful boot are two different, genuinely distinguishable things.
    fprintf(stderr, "%s: sent %s and device acknowledged the '%s' command.\n", what, path.c_str(), command);
    if (!keepOpen) irecv_close(client);
    return 0;
}

// warnIfRamdiskExceedsDeviceLimit() USED TO LIVE HERE. It queried
// `getenv ramdisk-size` right before the ramdisk upload and warned if the
// baked ramdisk was bigger than the device's reported limit. It is gone
// because **that variable does not exist on any device this project
// supports**, so the check could never fire -- it took its
// "device did not report a ramdisk-size" branch on every single run, on
// every device, and read as a protective guard while doing nothing.
//
// Measured, not assumed. Both iBECs for build 10B329a were decrypted with
// the checked-in ImageKeys and unwrapped with xpwntool, then searched:
//
//   AppleTV2,1 (A4, iBoot-1537.9.55)   `ramdisk-size` occurrences: 0
//   AppleTV3,2 (A5, iBoot-1458.2)      `ramdisk-size` occurrences: 0
//
// That matches The Apple Wiki's own note that 32-bit iBoot has no such
// variable: the limit there was a compiled-in kRamdiskMaxSize (0x2000000 on
// iPhone 3GS iBoot-636.66) whose over-size path prints "Ramdisk too large".
// Neither of our iBECs contains that string either, so even that older
// mechanism is absent -- their only size-check strings are for Combo image,
// Device Tree, Kernelcache, and a generic `image_load: image too large`.
// The 256MB/512MB ramdisk-size values seen in the wild are all 64-bit
// devices.
//
// Note this is NOT the reason it was removed once before. That earlier
// removal rested on a belief that idevicerestore doesn't query the variable;
// it does, in recovery_send_ramdisk(), and it was correctly re-added then.
// idevicerestore only READS and LOGS the value -- it never sets it -- so on
// 32-bit iBoot it is logging a variable that isn't there, which is harmless
// for it and equally uninformative for us.
//
// The COMPARISON is what went away. The `getenv ramdisk-size` ROUND TRIP is
// still here, deliberately, in sendRamdiskSizeGetenv() below: it is the exact
// request real idevicerestore makes at this exact point, and this project has
// been bitten by wire-level differences before (the bReq=1 `bootx`, the
// zero-length DFU_DNLOAD). Keeping the traffic identical costs one no-op
// command and removes a whole category of "is it because our USB sequence
// differs?" from any future investigation. It is kept for that reason ALONE
// -- the response is discarded, and on this hardware there is no response to
// speak of.
//
// What replaced the check: a real size limit enforced at BAKE time in
// BakeRamdisk.cpp, where the number is knowable and the failure is cheap.
// See that limit's own comment (and DEBUG_RAMDISK_LIMIT_MIB).

// Fire-and-forget, purely for wire fidelity with idevicerestore's
// recovery_send_ramdisk(). The value is read and dropped on the floor; see
// the block comment above for why there is nothing to read on this hardware
// and why we send it anyway.
static void sendRamdiskSizeGetenv(irecv_client_t client) {
    if (!client) return;
    char* value = nullptr;
    irecv_getenv(client, "ramdisk-size", &value);
    free(value);
}

int DeviceManager::sendRamdisk(const std::string& Ramdisk_Path, uint64_t ecid) {
    // get_tv_patient(): same reasoning as sendiBEC() above -- this reconnect
    // follows DeviceTree's own NOTIFY_FINISH-triggered reset.
    irecv_client_t client = get_tv_patient(ecid);
    // Round trip only, result discarded -- see its own comment above.
    sendRamdiskSizeGetenv(client);
    int result = sendFileThenCommand(client, "sendRamdisk", Ramdisk_Path, "ramdisk", false, 0, false,
                                      "getenv ramdisk-delay");
    sleep(2);
    return result;
}

// Reads whatever the device is currently streaming on its console/serial-
// like interface -- the exact same mechanism `irecovery -s`'s own passive
// read uses (irecv_receive() bulk-reads endpoint 0x81 on interface 1,
// alternate setting 1, then switches back to interface 0 for normal DFU/
// Recovery commands). iBoot prints its full startup banner -- including
// "Boot Failure Count: N Panic Fail Count: N" -- here unconditionally on
// every boot, not behind any getenv variable; this is the only way to see
// it (confirmed: no boot-count/failure-count-shaped getenv variable
// anywhere in libirecovery, and this exact banner is what a real
// `irecovery -s` capture showed after a real 'bootx' attempt). Global
// accumulator, not a lambda capture: irecv_event_cb_t's signature has no
// user_data parameter, matching this file's own existing progress_cb
// convention below.
//
// IMPORTANT: the device only ever streams this splash/banner ONCE per
// Recovery-mode boot session -- confirmed directly (real run): only the
// very first connection after landing in Recovery mode gets it, every
// later reconnect in that same session (including from this function's
// own repeated polling below) reads back nothing at all. So within the
// polling loop below, only the very first captureConsoleLog() call that
// happens to land after the device has actually re-entered Recovery mode
// is expected to return anything -- every later poll in the same run
// coming back empty is normal, not a sign the capture mechanism broke.
static std::string g_consoleCaptureBuffer;
static int consoleReceivedCallback(irecv_client_t /*client*/, const irecv_event_t* event) {
    if (event->type == IRECV_RECEIVED && event->data && event->size > 0) {
        g_consoleCaptureBuffer.append(event->data, (size_t)event->size);
    }
    return 0;
}
static std::string captureConsoleLog(irecv_client_t client) {
    g_consoleCaptureBuffer.clear();
    irecv_event_subscribe(client, IRECV_RECEIVED, consoleReceivedCallback, nullptr);
    irecv_receive(client);
    irecv_event_unsubscribe(client, IRECV_RECEIVED);
    return g_consoleCaptureBuffer;
}

// 'bootx' is the actual boot trigger for the whole chain (ramdisk/
// devicetree's own commands just stage data for the kernel to use once it
// boots) -- but sendFileThenCommand()'s own success only means the USB
// control transfer that DELIVERS 'bootx' was acknowledged, not that the
// device went on to actually boot the kernel it names.
//
// CORRECTED (an earlier version of this function assumed staying in
// Recovery mode for up to 60s after 'bootx' was normal, reasoning that
// entrypoint.c's own install work takes real time before it reboots the
// device a second time): a real run's `irecovery -s` console capture,
// taken right after a 'bootx' attempt, showed iBoot's own fresh startup
// banner and NAND probe sequence ending in "Boot Failure Count: 1 ...
// Entering recovery mode, starting command prompt" -- i.e. a real,
// independent iBoot boot-failure fallback, not entrypoint.c quietly still
// working. A working handoff leaves Recovery mode in well under 5
// seconds; still being reachable past that point means iBoot rejected or
// failed to boot the uploaded kernelcache/ramdisk/devicetree combination
// outright (signature/format validation, a bad boot-args/KASLR patch,
// etc.), not that anything is legitimately still running. Returns true if
// the device left Recovery mode within that window, false otherwise --
// the caller now treats false as a real failure, not just a log line.
static bool checkDeviceLeftRecoveryModeAfterBoot(uint64_t ecid) {
    fprintf(stderr,
            "sendKernelCache: 'bootx' acknowledged. A working boot hands off well under 5s -- checking "
            "whether the device actually leaves Recovery mode within that window.\n");
    for (int i = 1; i <= 5; i++) {
        sleep(1);
        irecv_client_t check = get_tv(ecid);
        if (!check) {
            fprintf(stderr, "sendKernelCache: device left Recovery mode after %ds -- boot handoff succeeded.\n",
                    i);
            return true;
        }
        // Raw numeric mode, not just mode_to_str()'s collapsed "Recovery"
        // string -- IRECV_K_RECOVERY_MODE_1..4 (0x1280-0x1283) are four
        // genuinely different USB PIDs mode_to_str() folds into one label
        // (looked up directly: they're iBoot *protocol-version* IDs tied
        // to firmware generation, not live boot-state indicators, so this
        // is expected to stay constant across polls on a single device --
        // logged anyway since it's free, real data. Serial string logged
        // too, for the same reason.
        int mode = 0;
        irecv_get_mode(check, &mode);
        const struct irecv_device_info* info = irecv_get_device_info(check);
        fprintf(stderr,
                "sendKernelCache: still responding in Recovery mode after %ds. Raw mode: 0x%04x. Serial "
                "string: %s\n",
                i, mode, (info && info->serial_string) ? info->serial_string : "(none)");

        std::string console = captureConsoleLog(check);
        irecv_close(check);
        if (!console.empty()) {
            fprintf(stderr, "sendKernelCache: console output:\n%s\n", console.c_str());
            // "Boot Failure Count:" only ever appears in this capture at
            // all because it's iBoot's own fresh startup banner, printed
            // exactly once per Recovery-mode session (see this function's
            // own comment above) -- seeing it here already means the
            // device reset and iBoot gave up and dropped back to its own
            // command prompt. That's a definitive, already-final answer,
            // not a "maybe still booting" state, so there's no reason to
            // keep burning through the rest of the 5s budget waiting for
            // a boot that has already failed -- fail immediately instead.
            if (console.find("Boot Failure Count:") != std::string::npos) {
                fprintf(stderr,
                        "sendKernelCache: *** iBoot reports a boot failure count -- this is iBoot's own "
                        "startup banner, meaning the device actually reset and iBoot ran its own boot "
                        "sequence again. Treating this as an immediate failure instead of waiting out the "
                        "rest of the 5s window -- iBoot has already rejected the uploaded kernelcache/"
                        "ramdisk/devicetree combination. ***\n");
                return false;
            }
        }
    }
    // Reaching here means the device stayed in Recovery mode for the full
    // 5s without ever reporting a boot failure count above -- per this
    // function's own reasoning, that combination should be impossible (a
    // working boot leaves well under 5s; a failing one reprints iBoot's
    // startup banner, caught above). Still a failure either way, just an
    // unexplained one worth calling out as such rather than implying the
    // usual "iBoot rejected it" diagnosis applies.
    fprintf(stderr,
            "sendKernelCache: device is STILL in Recovery mode 5s after 'bootx' was acknowledged, without "
            "ever reporting a boot failure count -- treating this as a failure too, but this specific "
            "outcome (neither a clean handoff nor an explicit iBoot failure banner) shouldn't be possible "
            "and is worth investigating on its own.\n");
    return false;
}

// Boot-args for the two kinds of boot this function triggers. These used to
// be compiled into iBEC by iBoot32Patcher's patch_boot_args() (Patcher.cpp's
// old args1/args2); they are set at runtime now, which is how the stock path
// (sendStockRestoreTail() below) and real idevicerestore have always done it.
// See patchiBEC() for why the compiled-in version was worth getting rid of.
//
// Deliberately NOT followed by `saveenv`: boot-args must apply to this one
// boot only. The stock path saves `auto-boot false` because that is meant to
// persist, and pointedly does not save boot-args -- persisting `rd=md0` into
// NVRAM would leave the device trying to root-mount a ramdisk that is no
// longer there on every subsequent normal boot.
//
// kRamdiskBootArgs matches what the ramdisk actually does. entrypoint.c is
// the ramdisk's /sbin/launchd: it execs as PID 1, writes its files, and
// reboots -- nothing else. So:
//   rd=md0                     root device IS the ramdisk. Without this the
//                              kernel mounts the real NAND root and runs
//                              Apple's own launchd, and entrypoint.c never
//                              runs at all.
//   amfi=0xff,
//   cs_enforcement_disable=1,
//   amfi_get_out_of_my_way=1   entrypoint.c is our own unsigned binary, so
//                              code-signing enforcement has to be off for it
//                              to exec as PID 1.
//   -v                         verbose console. The only channel this project
//                              has for seeing a boot fail (see
//                              checkDeviceLeftRecoveryModeAfterBoot() above),
//                              and free.
//   pio-error=0                carried over from the original app's args.
// Notably absent: nand-enable-reformat=1, and the reason is NOT that it
// would reformat anything here. Researched rather than assumed: that arg
// only AUTHORIZES a reformat; the format itself is performed by asr under
// restored during a real restore. This ramdisk runs entrypoint.c as PID 1
// and never starts restored or asr, so nothing would invoke a format and
// the arg would simply be inert.
//
// It is left out because the closest real-world reference for this exact
// job says to leave it out. Legacy-iOS-Kit uses two different boot-arg sets
// on 32-bit devices: its SSH-ramdisk flow (boot a ramdisk, poke at the
// existing filesystem -- the same shape as what this does) uses
// "rd=md0 -v amfi=0xff amfi_get_out_of_my_way=1 cs_enforcement_disable=1
// pio-error=0", with no reformat arg, while its restore/downgrade flow adds
// nand-enable-reformat=1. The set above is that SSH-ramdisk string, modulo
// ordering.
//
// Worth knowing if flash access ever turns out to be the problem: the arg
// IS load-bearing on some chips, where the restore ramdisk otherwise fails
// to bring the flash stack up at all. SSHRD_Script appends
// "nand-enable-reformat=1 -restore" for exactly three CPIDs -- 0x8960 (A7),
// 0x7000 and 0x7001 (A8). This project's chip is 0x8947, which is not among
// them, and no A5-specific requirement is documented anywhere. So if
// entrypoint.c ever boots but cannot see or mount the data partition, this
// is a cheap thing to try before anything expensive -- it cannot format
// without asr, and the only evidence against it is that the 32-bit
// reference tooling does not use it here.
//
// Also absent: anything about a jailbroken userspace; the ramdisk boot is
// only a vehicle for the file writes, so there is nothing further to ask
// the kernel for.
static const char* const kRamdiskBootArgs =
    "setenv boot-args rd=md0 -v amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1 pio-error=0";
int DeviceManager::sendKernelCache(const std::string& KernelCache_Path, uint64_t ecid, bool skipBootCheck) {
    // get_tv_patient(): same reasoning as sendiBEC() above -- this reconnect
    // follows Ramdisk's own NOTIFY_FINISH-triggered reset.
    irecv_client_t client = get_tv_patient(ecid);
    // bReq=1: see sendFileThenCommand()'s own comment -- "bootx" is one of
    // idevicerestore's two bRequest=1 boot-triggering commands.
    //
    // The boot-args go in via extraCommandBeforeMain, which puts them after
    // the kernelcache upload and its zero-length DFU_DNLOAD (dnloadFinish)
    // and before 'bootx' -- byte-for-byte the order real idevicerestore uses,
    // and the same order sendStockRestoreTail() below already follows.
    fprintf(stderr, "sendKernelCache: %s\n", kRamdiskBootArgs);
    int result = sendFileThenCommand(client, "sendKernelCache", KernelCache_Path, "bootx", false, 1, true,
                                      kRamdiskBootArgs, /*extraCommandMustSucceed=*/true);
    if (result == 0 && skipBootCheck) {
        fprintf(stderr,
                "sendKernelCache: 'bootx' acknowledged. --no-shell-attach: not reading the recovery console; "
                "releasing USB so it can be inspected interactively (e.g. `irecovery -s`).\n");
        sleep(2);
        return 0;
    }
    if (result == 0 && !checkDeviceLeftRecoveryModeAfterBoot(ecid)) {
        result = -1;
    }
    sleep(2);
    return result;
}

int DeviceManager::sendDeviceTree(const std::string& DeviceTree_Path, uint64_t ecid) {
    // get_tv_patient(): same reasoning as sendiBEC() above -- this reconnect
    // follows iBEC's own NOTIFY_FINISH-triggered reset.
    irecv_client_t client = get_tv_patient(ecid);
    int result = sendFileThenCommand(client, "sendDeviceTree", DeviceTree_Path, "devicetree");
    sleep(2);
    return result;
}

int DeviceManager::sendRestoreLogo(const std::string& RestoreLogo_Path, uint64_t ecid) {
    // Same reconnect-per-step shape as sendDeviceTree()/sendRamdisk(): this
    // follows iBEC's own NOTIFY_FINISH-triggered reset. "setpicture 4" is the
    // exact command sendStockRestoreTail() uses for RestoreLogo (4 = the
    // recovery/restore image slot iBoot draws from).
    irecv_client_t client = get_tv_patient(ecid);
    int result = sendFileThenCommand(client, "sendRestoreLogo", RestoreLogo_Path, "setpicture 4");
    sleep(2);
    return result;
}

int DeviceManager::sendStockRestoreTail(uint64_t ecid, const PatchedComponents& components,
                                         const std::string& deviceModel, bool skipBootCheck) {
    irecv_client_t client = get_tv_patient(ecid);
    if (!client) {
        fprintf(stderr, "sendStockRestoreTail: device did not reconnect\n");
        return -1;
    }

    // Real idevicerestore (recovery.c) never trusts one connection across
    // this whole ticket->RestoreLogo->Ramdisk->DeviceTree->KernelCache
    // sequence the way this function does -- every one of its own send
    // steps defensively checks "if (client->recovery == NULL)" and
    // reopens before sending. Confirmed on real hardware: RestoreLogo (the
    // very first file send after a successful ApTicket+'ticket' exchange,
    // on a real non-checkm8 SecureROM run) failed outright with "Unable
    // to upload data to device" -- most consistent with this reused
    // handle having gone stale in between, not a wrong protocol/API (the
    // underlying irecv_send_buffer()/irecv_send_file() calls are
    // identical to idevicerestore's own). There's no cheap no-op
    // liveness ping in this USB protocol to check proactively, so instead
    // retry once against a freshly-reopened connection specifically when
    // a step's own send fails -- exactly the failure shape a stale handle
    // produces -- rather than giving up on the very first attempt.
    auto sendFileThenCommandWithReconnect = [&](const char* what, const std::string& path, const char* command,
                                                 uint8_t bReq = 0, const char* extraCommandBeforeMain = nullptr) -> bool {
        if (sendFileThenCommand(client, what, path, command, true, bReq, false, extraCommandBeforeMain) == 0) {
            return true;
        }
        fprintf(stderr, "%s: retrying once against a freshly-reopened connection...\n", what);
        client = get_tv_patient(ecid);
        if (!client) {
            fprintf(stderr, "%s: device did not reconnect for the retry\n", what);
            return false;
        }
        return sendFileThenCommand(client, what, path, command, true, bReq, false, extraCommandBeforeMain) == 0;
    };

    const struct irecv_device_info* info = irecv_get_device_info(client);
    auto ticket = fetchAPTicket(components.buildIdentity, ecid, info->ap_nonce, info->ap_nonce_size, deviceModel,
                                 components.buildID);
    if (!ticket) {
        irecv_close(client);
        return -1;
    }

    irecv_error_t err = irecv_send_buffer(client, ticket->data(), ticket->size(), 0);
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "sendStockRestoreTail: failed to send ApTicket (%zu bytes): %s\n", ticket->size(),
                irecv_strerror(err));
        irecv_close(client);
        return -1;
    }
    err = irecv_send_command(client, "ticket");
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "sendStockRestoreTail: failed to send 'ticket' command: %s\n", irecv_strerror(err));
        irecv_close(client);
        return -1;
    }
    fprintf(stderr, "sendStockRestoreTail: sent ApTicket (%zu bytes) and device acknowledged the 'ticket' command.\n",
            ticket->size());

    // Real idevicerestore's own recovery_enter_restore() (recovery.c) sends
    // `setenv auto-boot false` + `saveenv` right here, before RestoreLogo
    // -- blackb0x never did this at all. Without it, iBoot's own default
    // auto-boot behavior (whatever it was left at) stays in effect through
    // the rest of this sequence instead of being explicitly disabled for
    // the duration of the restore.
    err = irecv_send_command(client, "setenv auto-boot false");
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "sendStockRestoreTail: failed to send 'setenv auto-boot false' command: %s\n",
                irecv_strerror(err));
        irecv_close(client);
        return -1;
    }
    err = irecv_send_command(client, "saveenv");
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "sendStockRestoreTail: failed to send 'saveenv' command: %s\n", irecv_strerror(err));
        irecv_close(client);
        return -1;
    }

    // onlyBootComponents is gone with the tether-boot path -- this was its
    // only guard here, and it is now unconditionally taken.
    if (components.restoreLogo) {
        if (!sendFileThenCommandWithReconnect("sendStockRestoreTail(RestoreLogo)", *components.restoreLogo,
                                               "setpicture 4")) {
            if (client) irecv_close(client);
            return -1;
        }
        err = irecv_send_command(client, "bgcolor 0 0 0");
        if (err != IRECV_E_SUCCESS) {
            fprintf(stderr, "sendStockRestoreTail: failed to send 'bgcolor 0 0 0' command: %s\n",
                    irecv_strerror(err));
            irecv_close(client);
            return -1;
        }
    }

    for (const auto& [name, path] : components.loadedByIBoot) {
        if (!sendFileThenCommandWithReconnect(name.c_str(), path, "firmware")) {
            if (client) irecv_close(client);
            return -1;
        }
    }

    if (!components.ramdisk) {
        fprintf(stderr, "sendStockRestoreTail: no ramdisk to send\n");
        irecv_close(client);
        return -1;
    }
    // Round trip only, result discarded -- see sendRamdiskSizeGetenv() above.
    sendRamdiskSizeGetenv(client);
    if (!sendFileThenCommandWithReconnect("sendStockRestoreTail(Ramdisk)", *components.ramdisk, "ramdisk",
                                           /*bReq=*/0, "getenv ramdisk-delay")) {
        if (client) irecv_close(client);
        return -1;
    }

    if (!components.deviceTree) {
        fprintf(stderr, "sendStockRestoreTail: no devicetree to send\n");
        irecv_close(client);
        return -1;
    }
    if (!sendFileThenCommandWithReconnect("sendStockRestoreTail(DeviceTree)", *components.deviceTree,
                                           "devicetree")) {
        if (client) irecv_close(client);
        return -1;
    }

    if (!components.kernel) {
        fprintf(stderr, "sendStockRestoreTail: no kernelcache to send\n");
        irecv_close(client);
        return -1;
    }
    // Real idevicerestore's own recovery_send_kernelcache() (recovery.c)
    // does four things in this exact order, not just a file+command send
    // -- doesn't fit sendFileThenCommandWithReconnect()'s own shape, so
    // this step is inlined manually instead:
    //  1. upload the kernelcache file itself.
    //  2. a zero-length DFU_DNLOAD-class control transfer (bmRequestType
    //     0x21, bRequest 1) -- the same "that upload is finished, do
    //     something with it" signal boot_client()'s own checkm8 soft-DFU
    //     code elsewhere in this file already relies on. Confirmed on
    //     real hardware: sending 'bootx' with bRequest=1 alone (without
    //     this) still wasn't enough -- the device kept resetting back to
    //     iBoot's own command prompt every time ("Boot Failure Count"
    //     still climbing) until this was added too.
    //  3. `setenv boot-args rd=md0 nand-enable-reformat=1 -progress` --
    //     blackb0x's own patched iBEC route doesn't need this separately
    //     (patchiBEC()/Patcher.cpp already compiles an equivalent rd=md0
    //     string directly into the patched iBEC itself), but
    //     useStockIBEC()'s genuinely-unpatched stock iBEC has no such
    //     compiled-in args and was never being told this at all.
    //  4. `bootx` via bRequest=1 -- see sendFileThenCommand()'s own
    //     comment on why.
    {
        irecv_error_t fileErr =
            irecv_send_file(client, components.kernel->c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
        if (fileErr != IRECV_E_SUCCESS) {
            fprintf(stderr, "sendStockRestoreTail(KernelCache): failed to send %s: %s\n",
                    components.kernel->c_str(), irecv_strerror(fileErr));
            fprintf(stderr,
                    "sendStockRestoreTail(KernelCache): retrying once against a freshly-reopened connection...\n");
            client = get_tv_patient(ecid);
            if (!client) {
                fprintf(stderr, "sendStockRestoreTail(KernelCache): device did not reconnect for the retry\n");
                return -1;
            }
            fileErr = irecv_send_file(client, components.kernel->c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
            if (fileErr != IRECV_E_SUCCESS) {
                fprintf(stderr, "sendStockRestoreTail(KernelCache): failed to send %s: %s\n",
                        components.kernel->c_str(), irecv_strerror(fileErr));
                irecv_close(client);
                return -1;
            }
        }
    }

    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, nullptr, 0, 5000);

    err = irecv_send_command(client, "setenv boot-args rd=md0 nand-enable-reformat=1 -progress");
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "sendStockRestoreTail: failed to send 'setenv boot-args' command: %s\n",
                irecv_strerror(err));
        irecv_close(client);
        return -1;
    }
    err = irecv_send_command_breq(client, "bootx", 1);
    if (err != IRECV_E_SUCCESS) {
        fprintf(stderr, "sendStockRestoreTail: failed to send 'bootx' command: %s\n", irecv_strerror(err));
        irecv_close(client);
        return -1;
    }
    fprintf(stderr, "sendStockRestoreTail(KernelCache): sent %s and device acknowledged 'bootx'.\n",
            components.kernel->c_str());

    irecv_close(client);
    sleep(2);
    if (skipBootCheck) {
        fprintf(stderr,
                "sendStockRestoreTail: --no-shell-attach: not reading the recovery console; releasing USB so "
                "it can be inspected interactively (e.g. `irecovery -s`).\n");
        return 0;
    }
    return checkDeviceLeftRecoveryModeAfterBoot(ecid) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// AppleTV3,2 booting (soft DFU)
// ---------------------------------------------------------------------------

static int sendiBSS_ATV32(uint64_t ecid, const char* path, bool allowUnpwned) {
    irecv_client_t client = get_tv(ecid);
    if (!client) {
        return -1;
    }

    int handle = open(path, O_RDONLY);
    if (handle < 0) {
        fprintf(stderr, "Failed to open %s\n", path);
        return -1;
    }

    off_t buffer_size = lseek(handle, 0, SEEK_END);
    if (buffer_size <= 0) {
        fprintf(stderr, "iBSS file is empty: %s\n", path);
        close(handle);
        return -1;
    }

    unsigned char* buffer = (unsigned char*)malloc(buffer_size);
    if (!buffer) {
        close(handle);
        return -1;
    }

    if (pread(handle, buffer, buffer_size, 0) < 0) {
        fprintf(stderr, "Failed to read %s\n", path);
        free(buffer);
        close(handle);
        return -1;
    }

    // AppleTV3,2 boots iBSS via checkm8_bootkit's dfu_boot() (bootkit.c), NOT
    // boot_client(): the checkm8 payload only executes an image wrapped in the
    // "exec" usb_command_t + the CPID-0x8947 trampoline dfu_boot() builds. The
    // old port wrongly reused the AppleTV3,1 raw-upload boot_client() here, so
    // the iBSS was uploaded but never run (device fell back to installed
    // iBoot). dfu_boot() validates PWND:[checkm8] itself, so allowUnpwned no
    // longer applies on this path; it does not close `client`, so close here.
    (void)allowUnpwned;
    int ret = dfu_boot(client, buffer, (size_t)buffer_size);
    irecv_close(client);
    free(buffer);
    close(handle);
    return (ret == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// AppleTV3,1 booting
// ---------------------------------------------------------------------------

static int sendiBSS_ATV31(uint64_t ecid, const char* iBSSpath, bool allowUnpwned) {
    irecv_client_t client = get_tv(ecid);
    if (!client) return -1;

    FILE* iBSSfile = fopen(iBSSpath, "rb");
    if (!iBSSfile) {
        fprintf(stderr, "Failed to open %s\n", iBSSpath);
        return -1;
    }

    fseek(iBSSfile, 0, SEEK_END);
    long length = ftell(iBSSfile);
    fseek(iBSSfile, 0, SEEK_SET);

    void* buf = malloc(length);
    size_t nread = fread(buf, 1, length, iBSSfile);
    fclose(iBSSfile);
    (void)nread;

    int ret = boot_client(client, buf, length, allowUnpwned);
    free(buf);
    if (ret != 0) {
        return -1;
    }
    sleep(2);
    return 0;
}

static void send_progress(double progress) {
    if (progress < 0) return;
    if (progress > 100) progress = 100;
    if (DeviceManager::instance() && DeviceManager::instance()->sink().onProgress) {
        DeviceManager::instance()->sink().onProgress(progress);
    }
}

//** Thank You @dora2 for this below! **//

#define IMG3_HEADER     0x496d6733
#define ARMv7_VECTOR    0xEA00000E
#define IMG3_ILLB       0x696c6c62
#define IMG3_IBSS       0x69627373
#define IMG3_DATA       0x44415441
#define IMG3_KBAG       0x4B424147

typedef struct img3Tag {
    uint32_t magic;
    uint32_t totalLength;
    uint32_t dataLength;
} Img3RootHeader;

typedef struct Unparsed_KBAG_256 {
    uint32_t magic;
    uint32_t fullSize;
    uint32_t tagDataSize;
    uint32_t cryptState;
    uint32_t aesType;
    uint8_t encIV_start;
} UnparsedKbagAes256_t;

typedef struct img3File {
    uint32_t magic;
    uint32_t fullSize;
    uint32_t sizeNoPack;
    uint32_t sigCheckArea;
    uint32_t ident;
    struct img3Tag tags[];
} Img3Header;

static int send_data(irecv_client_t client, unsigned char* data, size_t size) {
    return irecv_usb_control_transfer(client, 0x21, 1, 0, 0, data, size, 100);
}

static int boot_client(irecv_client_t client, void* buf, size_t sz, bool allowUnpwned) {
    if (!client) {
        return -1;
    }

    const struct irecv_device_info* info = irecv_get_device_info(client);
    if (!isPwnedDFU(info)) {
        if (!allowUnpwned) {
            irecv_close(client);
            fprintf(stderr, "Device is not in pwned DFU mode.\n");
            return -1;
        }
        // --stock-recovery/--stock-securerom (Cli.hpp's CliOptions): the
        // caller deliberately wants this attempted against a device
        // without a "PWND:[" serial string. The soft-DFU sequence below is
        // built entirely around checkm8's own memory-corruption state
        // accepting a raw, unsigned payload over USB -- a genuinely
        // un-pwned device's real, intact SecureROM has no reason to honor
        // any of it, so this is expected to go on to fail below rather
        // than being a new bug (see the --stock-securerom-specific note on
        // that failure in Cli.cpp's sendComponentsToDevice()).
        fprintf(stderr,
                "Device does not report pwned DFU mode (no PWND:[ in its serial string) -- continuing anyway "
                "since --stock-recovery/--stock-securerom was passed.\n");
    }

    void* ibss;
    size_t ibss_sz;
    unsigned char blank[16];
    memset(blank, 0, 16);

    int ret = check_img3_file_format(client, buf, sz, &ibss, &ibss_sz);
    if (ret != 0) {
        irecv_close(client);
        return -1;
    }

    send_data(client, blank, 16);
    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, nullptr, 0, 100);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 100);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 100);

    size_t len = 0;
    unsigned char* ibss_bytes = (unsigned char*)ibss;
    while (len < ibss_sz) {
        size_t size = ((ibss_sz - len) > 0x800) ? 0x800 : (ibss_sz - len);
        size_t sent = irecv_usb_control_transfer(client, 0x21, 1, 0, 0, &ibss_bytes[len], size, 1000);
        if (sent != size) {
            free(ibss);
            return -1;
        }
        len += size;
        send_progress(((double)len / (double)ibss_sz) * 100);
    }
    free(ibss);

    // Per the USB DFU 1.1 spec (6.1.2): from dfuDNLOAD-IDLE, a DFU_DNLOAD
    // with wLength=0 is what actually tells the device "that was the last
    // block" and moves it into dfuMANIFEST-SYNC; the GETSTATUS reads that
    // follow drive it the rest of the way through dfuMANIFEST-SYNC ->
    // dfuMANIFEST. This exact triplet already exists a few lines up
    // (1223-1225) as a preamble clearing out any stale state left over from
    // a previous session -- but the real payload's own upload loop just
    // above never did the same thing at ITS end, so the device was left
    // sitting in dfuDNLOAD-IDLE (state 5) instead of ever manifesting/
    // jumping into the uploaded iBSS. Confirmed directly: the next
    // component sent afterwards (iBEC) reconnects fine, but its own
    // pre-upload GETSTATUS immediately observes that same leftover state 5.
    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, nullptr, 0, 100);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 100);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 100);

    // The line below (ported byte-for-byte from the original .m -- see git
    // history for DeviceManager.m, removed in 907b64b) is this function's
    // own trigger for "go run it."
    irecv_usb_control_transfer(client, 0xA1, 2, 0xFFFF, 0, (unsigned char*)buf, 0, 100);

    // Whether a real USB reset (irecv_reset()) belongs here turned out to
    // depend on the device's actual state, not be a flat yes/no:
    //  - Always resetting unconditionally (an earlier version of this
    //    function) worked at least once (reached Recovery Mode end to
    //    end), but was intermittently WORSE than not resetting at all: the
    //    device sometimes booted straight into the full, real OS instead
    //    of continuing the exploited chain. Adding a settle delay before
    //    that unconditional reset didn't fix it.
    //  - Never resetting at all (removing it outright) turned that
    //    intermittent failure into a DETERMINISTIC one: the device now
    //    reliably reports DFU state 8 (dfuMANIFEST-WAIT-RESET) when iBEC's
    //    own pre-upload GETSTATUS checks it next -- per the USB DFU spec,
    //    a device in that state (bitManifestationTolerant=0: it can't
    //    safely keep talking over USB while it reprograms itself) is
    //    waiting specifically for a host-issued reset and will never
    //    proceed without one.
    // Put together, these two results say the reset itself is genuinely
    // required when the device is actually in state 8 -- it's issuing one
    // to a device that ISN'T in state 8 (e.g. one whose
    // bitManifestationTolerant=1 already looped it back to dfuIDLE, or one
    // that's already off running the just-uploaded iBSS) that's the race:
    // landing a real bus reset on a device already mid-boot is exactly the
    // kind of timing-dependent hit that would show up as "usually fine,
    // sometimes falls through to the real trust chain." So: check the
    // real state first, and only reset if it's genuinely stuck in 8.
    unsigned char statusBuf[6] = {0};
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, statusBuf, sizeof(statusBuf), 100);
    unsigned int dfuState = statusBuf[4];
    if (dfuState == 8) {
        irecv_reset(client);
    }

    irecv_close(client);
    return 0;
}

static int check_img3_file_format(irecv_client_t client, void* file, size_t sz, void** out, size_t* outsz) {
    (void)client;
    (void)sz;
    unsigned char* base = (unsigned char*)file;
    uint32_t Img3header_magic = *(uint32_t*)(base + offsetof(struct img3File, magic));

    switch (Img3header_magic) {
        case ARMv7_VECTOR:
            *out = malloc(sz);
            *outsz = sz;
            memcpy(*out, file, *outsz);
            return 0;

        case IMG3_HEADER: {
            uint32_t ibss_data_start = 0;
            uint32_t tag_header = 0;

            uint32_t img3_ident = *(uint32_t*)(base + offsetof(struct img3File, ident));
            if (img3_ident != IMG3_ILLB && img3_ident != IMG3_IBSS) {
                fprintf(stderr, "Invalid iBSS image.\n");
                return -1;
            }

            uint32_t img3_fullSize = *(uint32_t*)(base + offsetof(struct img3File, fullSize));
            uint32_t img3_sizeNoPack = *(uint32_t*)(base + offsetof(struct img3File, sizeNoPack));

            uint32_t next = img3_fullSize - img3_sizeNoPack;

            for (uint32_t next_tag = next; next_tag < img3_fullSize;) {
                uint32_t img3_tag_magic = *(uint32_t*)(base + next_tag + offsetof(struct img3Tag, magic));
                uint32_t img3_tag_totalLength = *(uint32_t*)(base + next_tag + offsetof(struct img3Tag, totalLength));
                uint32_t img3_tag_dataLength = *(uint32_t*)(base + next_tag + offsetof(struct img3Tag, dataLength));

                if (img3_tag_magic == IMG3_DATA) {
                    tag_header = img3_tag_magic;
                    *outsz = img3_tag_dataLength;
                    ibss_data_start = next_tag + offsetof(struct img3Tag, dataLength) + 4;
                }

                (void)IMG3_KBAG;
                next_tag += img3_tag_totalLength;
            }

            if (tag_header != IMG3_DATA) {
                fprintf(stderr, "Invalid iBSS image.\n");
                return -1;
            }

            *out = malloc(*outsz);
            memcpy(*out, base + ibss_data_start, *outsz);
            return 0;
        }

        default:
            fprintf(stderr, "Invalid iBSS image.\n");
            return -1;
    }
}
