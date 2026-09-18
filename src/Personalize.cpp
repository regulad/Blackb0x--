//
//  Personalize.cpp
//  Blackb0x
//

#include "IPSW.hpp"
#include "Personalize.hpp"

extern "C" {
#include <libtatsu/tss.h>
#include <plist/plist.h>

#include "idevicerestore_img3.h"
}

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>

namespace {
constexpr const char* kTSSServerURL = "https://gs.apple.com/TSS/controller?action=2";
// idevicerestore's own img3_stitch_component() callers (personalize_component()
// in idevicerestore.c) always pass this exact constant as blob_size, not the
// TSS response blob's true total length -- img3_replace_signature() actually
// walks the ECID/SHSH/CERT elements sequentially using each element's own
// embedded full_size field, so this is only a sanity check against the
// first (ECID) element's own fixed, known size. Matched exactly rather than
// re-derived, per this file's own "don't reimplement, adapt exactly" intent.
constexpr size_t kTssBlobSizeCheck = 64;

// Shared by personalizeIMG3Component()/fetchAPTicket() below -- one real
// TSS request/response covers every Trusted component in the manifest at
// once (tss_parameters_add_from_manifest()'s own "include_manifest=true"
// copies the whole thing in), including the combined ApTicket that
// authorizes all of them together, so both callers just need their own
// extraction step afterward, not a separate request each. Caller owns
// the returned plist_t (plist_free() it) -- nullptr on failure, with the
// reason already printed to stderr.
plist_t requestTSS(std::shared_ptr<void> buildIdentity, uint64_t ecid, const unsigned char* apNonce,
                    unsigned int apNonceSize, const std::string& deviceModel, const std::string& buildID) {
    plist_t identity = static_cast<plist_t>(buildIdentity.get());
    if (!identity) {
        fprintf(stderr, "requestTSS: no BuildIdentity available\n");
        return nullptr;
    }

    // Best-effort, cheap check before ever bothering Apple's real TSS
    // server: ipsw.me's own crowd-sourced signing-status snapshot (the
    // same one bake-all-ramdisks' --signed-only already relies on) can
    // at least warn upfront that this is very likely a wasted request --
    // it's not authoritative (can lag Apple's own signing-window changes
    // in either direction), so this only warns, never blocks.
    if (!deviceModel.empty() && !buildID.empty()) {
        std::set<std::string> signedBuilds = signedBuildsForDevice(deviceModel);
        if (!signedBuilds.count(buildID)) {
            fprintf(stderr,
                    "requestTSS: ipsw.me does not currently list %s %s as signed by Apple -- the TSS request "
                    "below is very likely to be refused. Trying anyway, since ipsw.me's own signing-status "
                    "snapshot isn't authoritative.\n",
                    deviceModel.c_str(), buildID.c_str());
        }
    }

    // Level 1 only unlocks tss.c's own error()-level messages
    // ("ERROR: Unable to find X node", etc.) -- its actual request/
    // response plist dumps (debug_plist(), the only way to see exactly
    // which manifest components got included and what a real TSS
    // response actually contains) are gated on level >= 2 specifically
    // (confirmed directly against libtatsu's own tss.c: both debug() and
    // debug_plist() check `debug_level < 2`). Real hardware runs of the
    // combined-ApTicket flow have hit a wall that's impossible to
    // diagnose further without seeing the actual wire content.
    tss_set_debug_level(2);

    plist_t parameters = plist_new_dict();
    plist_dict_set_item(parameters, "ApECID", plist_new_uint(ecid));
    if (apNonce && apNonceSize > 0) {
        plist_dict_set_item(parameters, "ApNonce",
                             plist_new_data(reinterpret_cast<const char*>(apNonce), apNonceSize));
    }
    plist_dict_set_item(parameters, "ApProductionMode", plist_new_bool(1));
    // This device generation (AppleTV3,2, A5) predates img4/SEP entirely --
    // no ApSepNonce, no ApSecurityMode, no ApImg4 tags at all, matching
    // idevicerestore's own `if (client->image4supported) {...} else { ... }`
    // img3 branch.
    plist_dict_set_item(parameters, "ApSupportsImg4", plist_new_bool(0));

    tss_parameters_add_from_manifest(parameters, identity, true);

    plist_t request = tss_request_new(nullptr);
    if (!request) {
        fprintf(stderr, "requestTSS: tss_request_new failed\n");
        plist_free(parameters);
        return nullptr;
    }

    if (tss_request_add_common_tags(request, parameters, nullptr) < 0 ||
        tss_request_add_ap_tags(request, parameters, nullptr) < 0 ||
        tss_request_add_ap_img3_tags(request, parameters) < 0) {
        fprintf(stderr, "requestTSS: failed to build TSS request\n");
        plist_free(request);
        plist_free(parameters);
        return nullptr;
    }

    fprintf(stderr, "requestTSS: requesting a real, ECID-personalized ticket from Apple's signing server...\n");
    plist_t response = tss_request_send(request, kTSSServerURL);
    plist_free(request);
    plist_free(parameters);

    if (!response) {
        fprintf(stderr,
                "requestTSS: Apple's TSS server would not sign this request -- most likely this build has left "
                "Apple's current signing window (a SHSH-less downgrade to firmware Apple no longer signs isn't "
                "possible without a blob saved while it was still signed). This is a real, expected answer, not "
                "a bug in this tool.\n");
        return nullptr;
    }

    return response;
}
}  // namespace

std::optional<std::vector<uint8_t>> personalizeIMG3Component(const std::string& componentName,
                                                               const std::string& rawImg3Path,
                                                               std::shared_ptr<void> buildIdentity, uint64_t ecid,
                                                               const unsigned char* apNonce, unsigned int apNonceSize,
                                                               const std::string& deviceModel,
                                                               const std::string& buildID) {
    plist_t response = requestTSS(buildIdentity, ecid, apNonce, apNonceSize, deviceModel, buildID);
    if (!response) return std::nullopt;

    unsigned char* blob = nullptr;
    if (tss_response_get_blob_by_entry(response, componentName.c_str(), &blob) < 0 || !blob) {
        fprintf(stderr, "personalizeIMG3Component: TSS response has no signed ticket for %s\n",
                componentName.c_str());
        plist_free(response);
        return std::nullopt;
    }
    plist_free(response);

    std::ifstream in(rawImg3Path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "personalizeIMG3Component: failed to open %s\n", rawImg3Path.c_str());
        free(blob);
        return std::nullopt;
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    void* stitched = nullptr;
    size_t stitchedSize = 0;
    int ret = img3_stitch_component(componentName.c_str(), raw.data(), raw.size(), blob, kTssBlobSizeCheck,
                                     &stitched, &stitchedSize);
    free(blob);
    if (ret != 0 || !stitched) {
        fprintf(stderr, "personalizeIMG3Component: failed to stitch the signed ticket into %s\n",
                componentName.c_str());
        return std::nullopt;
    }

    std::vector<uint8_t> result(static_cast<uint8_t*>(stitched), static_cast<uint8_t*>(stitched) + stitchedSize);
    free(stitched);
    return result;
}

std::optional<std::vector<uint8_t>> fetchAPTicket(std::shared_ptr<void> buildIdentity, uint64_t ecid,
                                                    const unsigned char* apNonce, unsigned int apNonceSize,
                                                    const std::string& deviceModel, const std::string& buildID) {
    plist_t response = requestTSS(buildIdentity, ecid, apNonce, apNonceSize, deviceModel, buildID);
    if (!response) return std::nullopt;

    unsigned char* ticket = nullptr;
    unsigned int ticketSize = 0;
    if (tss_response_get_ap_ticket(response, &ticket, &ticketSize) < 0 || !ticket) {
        fprintf(stderr, "fetchAPTicket: TSS response has no ApTicket\n");
        plist_free(response);
        return std::nullopt;
    }
    plist_free(response);

    std::vector<uint8_t> result(ticket, ticket + ticketSize);
    free(ticket);
    return result;
}
