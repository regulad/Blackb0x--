//
//  DeviceManager.hpp
//  Blackb0x
//
//  CLI port of DeviceManager.h. The checkm8/SHAtter exploit logic and
//  the DFU/iRecovery upload helpers are ported verbatim from the original
//  Objective-C (see DeviceManager.cpp) — this header only replaces the
//  Cocoa/AppKit-facing surface (AppleTVIcon, NSDictionary plist handling,
//  the implicit MainView/Blackb0x singleton reach-back) with plain
//  structs/callbacks a CLI front end can consume.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libirecovery.h>
#include <libimobiledevice/libimobiledevice.h>
}

// Patcher.hpp's own struct -- only referenced here (sendStockRestoreTail()'s
// own parameter, by const ref) to avoid pulling that whole header in;
// DeviceManager.cpp includes it for the real definition.
struct PatchedComponents;

// Declared here (with C linkage, matching their definitions in
// DeviceManager.cpp) so the friend declarations below grant access to these
// exact functions rather than implicitly declaring new C++-linkage ones.
extern "C" {
void blackb0x_irecv_device_event_cb(const irecv_device_event_t* event, void* user_data);
void blackb0x_idevice_event_cb(const idevice_event_t* event, void* user_data);
int blackb0x_irecv_progress_cb(irecv_client_t client, const irecv_event_t* event);
}

// Plain data replacement for the original AppleTVIcon (an NSImageView
// subclass). All rendering-only members (ATVImage, deviceField,
// versionField, checkingIndicator) are dropped — Cli.cpp owns all
// device-status text now, formatted directly from these fields, so there's
// no printDeviceInfo()/humanDeviceName() here to keep in sync with it.
struct AppleTVDevice {
    std::string deviceModel;
    std::string mode;
    std::string version;
    std::string buildID;
    std::string udid;
    uint64_t ecid = 0;

    int connected = 0;
    int pwnedDFU = -1;
    int jailbroken = 0;
    int jailbreakRunning = -1;
    int needsPostInstall = 1;
    int waitForRecovery = 0;
};

// Replaces every dispatch_async(dispatch_get_main_queue(), ^{ view.xxx = ... })
// call site in the original DeviceManager.m. The CLI front end (a later
// phase) wires these up; DeviceManager itself no longer knows about any UI.
struct DeviceEventSink {
    std::function<void(const AppleTVDevice&)> onDeviceAdded;
    std::function<void(uint64_t ecid, const std::string& udid)> onDeviceRemoved;
    std::function<void(const std::string&)> onStatus;
    std::function<void(double)> onProgress;
    // Called when a device believed jailbroken finishes being checked for a
    // running jailbreak (AFC2 reachable) or when jailbreak status changes.
    std::function<void(const AppleTVDevice&)> onDeviceUpdated;
};

// Only the fields Blackb0x's plist handling actually ever consumed (see the
// original dictionaryFromPlist:/addDeviceWithInfo:), extracted directly via
// libplist's C API instead of building a generic NSDictionary equivalent.
struct NormalModeInfo {
    bool valid = false;
    std::string productType;
    std::string productVersion;
    std::string buildVersion;
    std::string uniqueDeviceID;
    uint64_t uniqueChipID = 0;
};

NormalModeInfo plistInfoForDeviceUUID(const std::string& udid);
int isJailbroken(const std::string& udid);
int isJailbreakRunning(const std::string& udid);

class DeviceManager {
public:
    DeviceManager();

    void setEventSink(DeviceEventSink sink) { sink_ = std::move(sink); }
    DeviceEventSink& sink() { return sink_; }

    // The longest recovery COMMAND this bootloader will accept intact. Kept,
    // and still accurate, but note what it no longer applies to: boot-args
    // are compiled into iBEC now (Patcher.hpp's `bootargs` namespace), and a
    // baked string never passes through this buffer at all. Its ceiling is
    // bootargs::kMaxBakedBootArgsLength, which is a different number derived
    // from different hardware facts -- do not carry 127 over to it.
    //
    // What this DOES still bound is every `setenv`/`getenv`/`bootx`/`ramdisk`
    // command sent over the recovery protocol. The tightest limit is iBoot's
    // own, it is far below the obvious 256, and it TRUNCATES SILENTLY:
    //
    //  1. iBoot's recovery command parser allocates a 0x80-byte command
    //     buffer and hard-NULs it at [127] (command_parse_and_run, verified
    //     by disassembling iBoot-1537.9.55 for this exact device/build),
    //     with argc capped at 8 tokens. 127 bytes is all of any command that
    //     survives -- and nothing anywhere reports the loss. THE BINDING
    //     LIMIT.
    //  2. libirecovery refuses a recovery command of 0x100 bytes or more
    //     (irecv_send_command_breq() -> IRECV_E_INVALID_INPUT,
    //     third_party/libirecovery/src/libirecovery.c). That is the only
    //     limit on this path that fails LOUDLY -- and it sits at twice
    //     iBoot's, so anything between 128 and 255 bytes is accepted by
    //     libirecovery, crosses USB intact, and is then quietly chopped.
    //     Enforcing libirecovery's number would be exactly the
    //     silent-truncation failure worth avoiding.
    //
    // Two further limits bound the kernel command line rather than a
    // command, and they are the ones a BAKED string has to respect:
    //  3. XNU's ARM boot_args carries CommandLine[BOOT_LINE_LENGTH], a fixed
    //     256 bytes on 32-bit ARM (pexpert/pexpert/arm/boot.h; 608 on arm64,
    //     which is not this device). iBoot snprintf()s straight into it with
    //     size 0x100 -- confirmed as `MOV.W r1, #0x100` at file offsets
    //     0x1af7a and 0x1af9c of the decrypted iBEC -- so it truncates
    //     silently too.
    //  4. The kern.bootargs sysctl entrypoint.c reads the string back
    //     through is itself `char buf[256]` (sysctl_sysctl_bootargs(),
    //     bsd/kern/kern_sysctl.c).
    static const size_t kMaxRecoveryCommandLength = 127;

    // --- Exploits (ported verbatim from the original; see DeviceManager.cpp) ---
    int SHAtter(uint64_t ecid);
    // Shells out to blackb0x-pwn, this project's only pwntool. There used to
    // be a choice here (a vendored `gaster`, selected by --pwntool); gaster
    // never worked against an AppleTV3,2 on either Linux 7.1.x or macOS 26
    // and is gone — see docs/HISTORY.md.
    int checkm8(uint64_t ecid);

    irecv_client_t get_tv(uint64_t ecid);

    // --- iRecovery upload helpers ---
    // stockRecovery/stockSecurerom: mirror Cli.hpp's CliOptions of the same
    // name.
    //   - stockRecovery alone: device was genuinely pwned by this run's own
    //     checkm8 call, just skip the hard "PWND:[" serial-string check
    //     that boot_client() (DeviceManager.cpp) would otherwise still
    //     enforce -- has no practical effect here since a real checkm8 run
    //     already leaves that string in place, kept for symmetry/defense in
    //     depth.
    //   - stockSecurerom: the device was never pwned at all, and for
    //     AppleTV3,1/3,2 the usual boot_client() soft-DFU path is skipped
    //     entirely in favor of the standard irecv_send_file() DFU-class
    //     protocol -- boot_client()'s raw control-transfer sequence is
    //     shaped around checkm8's own post-exploit memory-corruption state,
    //     not the real DFU protocol, so it has no reason to work against a
    //     device whose SecureROM was never exploited (see boot_client()'s
    //     own comment). Before sending, the file also gets personalized
    //     with a real, ECID/nonce-bound SHSH ticket fetched from Apple's
    //     TSS server (see Personalize.hpp's personalizeIMG3Component()) --
    //     a genuinely un-pwned SecureROM's signature check requires this
    //     regardless of how correct the delivery protocol is. buildIdentity
    //     is the matching BuildManifest.plist identity (IPSW.hpp's
    //     ManifestInfo::buildIdentity) that personalization needs;
    //     deviceModel/buildID (IPSW.hpp's ManifestInfo::realBuildID) let
    //     it check signedBuildsForDevice() before ever sending a real TSS
    //     request. All four ignored unless stockSecurerom is set.
    int sendiBSS(const std::string& path, uint64_t ecid, bool stockRecovery = false, bool stockSecurerom = false,
                 std::shared_ptr<void> buildIdentity = nullptr, const std::string& deviceModel = "",
                 const std::string& buildID = "");
    int sendiBEC(const std::string& path, uint64_t ecid);
    int sendRamdisk(const std::string& path, uint64_t ecid);
    // NO BOOT-ARGS ARE SENT FROM HERE. This used to say they were "set at
    // runtime with `setenv boot-args` rather than compiled into iBEC"; that
    // is the exact inversion of what is true, and believing it cost this
    // project weeks. iBoot never reads the boot-args environment variable on
    // its kernel-boot path, so the args are COMPILED INTO the iBEC that was
    // sent earlier in this same sequence (iBoot32Patcher -b, from
    // Patcher.hpp's `bootargs` namespace, which carries the disassembly).
    // skipBootCheck: when true, return right after 'bootx' is acknowledged
    // instead of reconnecting to poll/read the recovery console
    // (checkDeviceLeftRecoveryModeAfterBoot()). Frees USB immediately so the
    // console can be inspected interactively -- see CliOptions::noShellAttach.
    // ramdiskBoot: true (default) means the caller sent dist/iBEC-<tuple>,
    // whose baked args root off the uploaded ramdisk (rd=md0); false means it
    // sent dist/iBECTether-<tuple>, whose baked args root off the installed
    // OS on NAND (rd=disk0s1s1) with no ramdisk uploaded at all. rd= is NOT
    // omitted in the tether case -- `bootx` from a restore bootloader skips
    // fsboot's automatic NAND-root setup, so no rd= means no root device.
    // This flag therefore only selects logging/ordering here; the actual
    // difference lives in which iBEC image was uploaded. See
    // CliOptions::tetherBoot and Patcher.hpp's `bootargs` namespace -- there
    // are no boot-arg constants in the .cpp any more.
    int sendKernelCache(const std::string& path, uint64_t ecid, bool skipBootCheck = false,
                        bool ramdiskBoot = true);
    int sendDeviceTree(const std::string& path, uint64_t ecid);
    // RestoreLogo for the reconnect-per-step (non-stockRecovery) path, sent
    // between iBEC and Ramdisk with iBoot's "setpicture 4" command -- the
    // same file+command sendStockRestoreTail() sends on its single-connection
    // path. Only the stockRecovery path used to send it at all; the normal
    // reconnect-per-step flow skipped it despite the documented ordering.
    int sendRestoreLogo(const std::string& path, uint64_t ecid);

    // stockRecovery only (see sendComponentsToDevice()'s own comment for
    // why that's the gate, not stockSecurerom): whether useStockIBEC()'s
    // genuinely-unpatched iBEC is what's running, not whether checkm8 ran
    // to get there -- patch_ticket_check() (patchiBEC(), applied by
    // default) is what makes a ticket unnecessary, and it only ever runs
    // against blackb0x's own patched iBEC.
    //
    // Sends the combined APTicket that authorizes every component after
    // iBSS together (see Personalize.hpp's own fetchAPTicket() comment
    // for why it's separate from personalizing iBSS itself), then
    // RestoreLogo (if
    // PatchedComponents::restoreLogo is set -- not every build's
    // manifest has one) / loaded-by-iBoot components (almost always
    // empty) / Ramdisk / DeviceTree, then KernelCache ('bootx') --
    // matching real idevicerestore's own ordering (recovery.c's
    // recovery_enter_restore(): ticket -> AppleLogo -> loaded-by-iBoot ->
    // Ramdisk -> DeviceTree -> KernelCache). Includes sendKernelCache()'s
    // own post-'bootx' left-Recovery-mode check.
    //
    // Confirmed directly on real hardware that this whole sequence needs
    // to run on ONE persistent connection, not the usual reconnect-per-
    // step pattern the rest of this class uses: closing and reopening
    // right after the ticket specifically (not after any of the OTHER
    // resets in this chain -- iBSS->iBEC, iBEC->DeviceTree, etc., which
    // only ever need a Recovery-mode-preserving reconnect) drops the
    // device all the way back to DFU mode, even though the "ticket"
    // command itself gets acknowledged cleanly. Real idevicerestore never
    // closes its own connection across this same span either (one
    // irecv_client_t for the whole ticket-through-KernelCache sequence).
    //
    // deviceModel is AppleTVDevice::deviceModel (the signed-build warning
    // before the TSS request).
    int sendStockRestoreTail(uint64_t ecid, const PatchedComponents& components, const std::string& deviceModel,
                             bool skipBootCheck = false, bool skipRestoreLogo = false);

    // --- Jailbreak status polling (was checkJailbreak/checkJailbreakRunning) ---
    // Blocks for up to a few seconds while it handshakes with the device
    // over lockdownd/AFC, then updates that device's jailbroken/
    // jailbreakRunning fields and fires onDeviceUpdated. Call it from the
    // main flow at the point the answer is needed -- it is no longer
    // dispatched in the background (see DeviceManager.cpp's newDevice()).
    // Only meaningful for a device in Normal mode; a no-op after the first
    // call for a given UDID.
    void checkJailbreak(const std::string& udid);

    // --- Device bookkeeping (replaces MainView.AppleTVs) ---
    std::vector<AppleTVDevice> devicesSnapshot() const;
    AppleTVDevice* deviceWithUDID(const std::string& udid, uint64_t ecid = 0);

    static DeviceManager* instance() { return instance_; }

private:
    static DeviceManager* instance_;

    // (An orphaned comment fragment used to sit here, describing an
    // --extra-boot-args member "appended to the boot-args sent at 'bootx'".
    // There is no such member and no such channel: extra boot-args are baked
    // in by `bake-iboot --extra-boot-args`, and `blackb0x --extra-boot-args`
    // hard-fails pointing there. Removed rather than left half-written.)

    // RAII window during which this process stays completely off USB and
    // says nothing about what the device is doing -- held while an external
    // pwntool owns the device. See its definition in DeviceManager.cpp for
    // why ignoring the events is not enough on its own.
    class UsbQuietWindow {
    public:
        explicit UsbQuietWindow(DeviceManager& deviceManager);
        ~UsbQuietWindow();
        UsbQuietWindow(const UsbQuietWindow&) = delete;
        UsbQuietWindow& operator=(const UsbQuietWindow&) = delete;

    private:
        DeviceManager& deviceManager_;
    };

    DeviceEventSink sink_;
    mutable std::mutex devicesMutex_;

    // Kept so UsbQuietWindow can actually unsubscribe: libirecovery tears
    // its event-handler thread (and with it all of its own USB polling)
    // down only once the last listener is gone.
    irecv_device_event_context_t irecvEventCtx_ = nullptr;
    std::atomic<bool> deviceEventsSuspended_{false};

    // UDIDs checkJailbreak() has already run a full AFC handshake for --
    // see its own comment. Needs no mutex: nothing calls it off the main
    // flow any more.
    std::set<std::string> jailbreakChecked_;
    // deque, not vector: push_back must not invalidate AppleTVDevice*
    // pointers a background thread (e.g. checkJailbreak's poll loop) may
    // still be holding onto while a second device connects concurrently.
    std::deque<AppleTVDevice> devices_;

    void newDevice(const std::string& productType, const std::string& modeStr,
                   const std::string& version, const std::string& buildID,
                   uint64_t ecid, const std::string& udid, int pwnedDFU);
    void disconnectDevice(uint64_t ecid, const std::string& udid);
    void checkJailbreakRunning(const std::string& udid);

    // One full pass through the checkm8 exploit sequence — shells out to
    // `blackb0x-pwn` rather than driving the low-level USB request sequence
    // itself (see docs/HISTORY.md for why); checkm8() (public) retries this
    // a bounded number of times on failure — see its own comment for why.
    bool checkm8Attempt(uint64_t ecid);

    friend void ::blackb0x_irecv_device_event_cb(const irecv_device_event_t* event, void* user_data);
    friend void ::blackb0x_idevice_event_cb(const idevice_event_t* event, void* user_data);
    friend int ::blackb0x_irecv_progress_cb(irecv_client_t client, const irecv_event_t* event);
};
