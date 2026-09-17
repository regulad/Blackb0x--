//
//  IPSW.hpp
//  Blackb0x
//
//  C++/Linux port of IPSW.h/.mm. Replaces IPSW_Fetch's NSURLSession-based
//  networking with libcurl, and its NSDictionary-based .keys file parsing
//  with libplist's C API directly. The IPSW.me API call's response is a
//  plain-text URL string (confirmed by how the original consumed it — not
//  JSON), so this is a trivial GET-into-string, no JSON parser needed.
//

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// A firmware component's decryption [iv, key] pair, as stored in
// Blackb0x/ImageKeys/<device>/<device>_<buildID>.keys.
struct FirmwareKeyPair {
    std::string iv;
    std::string key;
};

// Everything downloadAndPatchComponents() (Cli.cpp) and bake-all-ramdisks
// (BakeAllRamdisks.cpp) both need out of a BuildManifest.plist — pulled out
// of Cli.cpp so both can share one parser instead of drifting copies.
struct ManifestInfo {
    std::string realBuildID;     // BuildManifest.plist's own ProductBuildVersion
    std::string productVersion;  // BuildManifest.plist's own ProductVersion (e.g. "6.1.3")
    std::string iBSSPath;
    std::string iBECPath;
    std::string kernelCachePath;
    std::string deviceTreePath;
    std::string restoreRamdiskPath;
    // Empty if this build's manifest doesn't list one (matches
    // idevicerestore's own recovery_send_applelogo(), which checks
    // build_identity_has_component() first and simply skips the whole
    // step if absent) -- --stock-recovery needs this sent (via
    // DeviceManager::sendStockRestoreTail()) before Ramdisk once a real
    // APTicket is on file; blackb0x's own patched-bootloader flow never
    // needed it because it was never ticket-gated in the first place.
    std::string restoreLogoPath;
    // Every OTHER manifest component (name -> Path) whose own
    // Info.IsLoadedByiBoot is true and Info.IsLoadedByiBootStage1 isn't
    // -- matches idevicerestore's own recovery_send_loaded_by_iboot(),
    // which iterates the whole manifest generically rather than a fixed
    // list, since which components (if any) carry this flag varies by
    // device/build. Empty for builds where nothing does (confirmed via a
    // real AppleTV3,2 manifest that this is currently the common case for
    // this project's own target hardware) -- kept generic anyway rather
    // than hardcoded to "always empty here", since a different build or
    // device this project targets later could genuinely have entries.
    std::vector<std::pair<std::string, std::string>> loadedByIBootComponents;

    // The matching BuildIdentity dict (a plist_t, type-erased as
    // shared_ptr<void> with plist_free as its deleter so this header
    // doesn't need <plist/plist.h> itself) -- only Personalize.cpp reads
    // this, for --stock-securerom's TSS personalization
    // (tss_parameters_add_from_manifest() needs the raw identity dict,
    // not just the handful of fields already pulled out above).
    std::shared_ptr<void> buildIdentity;
};

// Parses BuildManifest.plist (already downloaded to `manifestPath`) for the
// component paths and the manifest's own (possibly more specific) build ID
// string — matching the original's `[dict[@"BuildIdentities"] lastObject]`
// exactly (the LAST identity, not the first — BuildManifest.plist commonly
// lists multiple personalization variants).
std::optional<ManifestInfo> parseManifest(const std::string& manifestPath);

// Plain HTTP GET via libcurl; returns the response body, or "" on failure.
std::string httpGet(const std::string& url);

class IpswFetch {
public:
    // GET https://api.ipsw.me/v2.1/<deviceModel>/<buildID|"latest">/url —
    // returns the raw response body (a plain-text .ipsw URL), or "" on
    // failure. Passing an empty buildID fetches the latest firmware
    // (replaces firmwareForDevice:/latestFirmwareForDevice:).
    std::string firmwareURLForDevice(const std::string& deviceModel, const std::string& buildID);

    // Parses the local bundled .keys file (an XML plist: component name ->
    // [iv, key]) for the given device/build.
    std::map<std::string, FirmwareKeyPair> keysForDevice(const std::string& device, const std::string& buildID);
};

// GET https://api.ipsw.me/v4/device/<deviceModel>?type=ipsw — returns every
// build ID Apple is still actively signing for this device right now
// (per ipsw.me's own "signed" flag), for bake-all-ramdisks' --signed-only.
// Unlike firmwareURLForDevice()'s v2.1 endpoint, this response IS JSON —
// deliberately hand-extracted here (regex over the known-flat, no-nested-
// braces firmware-entry shape) rather than pulling in a real JSON parser:
// the only vendored libplist build with plist_from_json() (libplist-modern)
// is a decoy that satisfies libimobiledevice-glue's configure-time version
// probe and is never actually linked into any binary (see CMakeLists.txt)
// — linking it for real here would risk exactly the kind of duplicate-
// symbol ABI conflict this codebase has otherwise been careful to avoid.
std::set<std::string> signedBuildsForDevice(const std::string& deviceModel);

// GET https://api.ipsw.me/v4/device/<deviceModel>?type=ipsw — returns the
// "version" field of the newest firmware entry ipsw.me lists for this
// device, or "" on failure/empty response. Used as bake-all-ramdisks' own
// fallback when a real BuildManifest.plist (the normal, authoritative
// source of ProductVersion for a specific (device, buildID) tuple) somehow
// fails to yield a productVersion — confirmed directly (`curl -s
// 'https://api.ipsw.me/v4/device/AppleTV3,2?type=ipsw'`) that this
// endpoint's own "firmwares" array is already ordered newest-release-first
// (by "releasedate", descending), so the first entry with both "version"
// and "buildid" fields is simply the newest one — no sorting needed.
// Shares signedBuildsForDevice()'s same hand-rolled regex extraction (see
// that function's own comment for why a real JSON parser isn't used here).
std::string newestVersionForDevice(const std::string& deviceModel);
