//
//  IPSW.cpp
//  Blackb0x
//

#include "IPSW.hpp"
#include "ResourcePath.hpp"

#include <curl/curl.h>
#include <plist/plist.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

static const char* kBaseUrl = "https://api.ipsw.me/v2.1/";

static size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "blackb0x");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "httpGet(%s) failed: %s\n", url.c_str(), curl_easy_strerror(res));
        response.clear();
    }

    curl_easy_cleanup(curl);
    return response;
}

std::string IpswFetch::firmwareURLForDevice(const std::string& deviceModel, const std::string& buildID) {
    std::string url = kBaseUrl + deviceModel + "/" + (buildID.empty() ? "latest" : buildID) + "/url";
    return httpGet(url);
}

std::map<std::string, FirmwareKeyPair> IpswFetch::keysForDevice(const std::string& device,
                                                                  const std::string& buildID) {
    std::map<std::string, FirmwareKeyPair> result;

    std::string path = resolveImageKeyPath(device + "/" + device + "_" + buildID + ".keys");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fprintf(stderr, "keysForDevice: cannot open %s\n", path.c_str());
        return result;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string contents = ss.str();

    plist_t root = nullptr;
    plist_from_xml(contents.c_str(), (uint32_t)contents.size(), &root);
    if (!root) {
        fprintf(stderr, "keysForDevice: failed to parse %s\n", path.c_str());
        return result;
    }

    plist_dict_iter it = nullptr;
    plist_dict_new_iter(root, &it);

    char* key = nullptr;
    plist_t subnode = nullptr;
    plist_dict_next_item(root, it, &key, &subnode);

    while (subnode) {
        if (key && plist_get_node_type(subnode) == PLIST_ARRAY && plist_array_get_size(subnode) >= 2) {
            plist_t ivNode = plist_array_get_item(subnode, 0);
            plist_t keyNode = plist_array_get_item(subnode, 1);

            char* ivStr = nullptr;
            char* keyStr = nullptr;
            plist_get_string_val(ivNode, &ivStr);
            plist_get_string_val(keyNode, &keyStr);

            FirmwareKeyPair pair;
            pair.iv = ivStr ? ivStr : "";
            pair.key = keyStr ? keyStr : "";
            // "0"/"0" is the convention these key dumps use for "this
            // component isn't encrypted at all" (real IV/key values are
            // always full-length hex strings) — normalize to empty so
            // every consumer's existing "no key" handling (decrypt()
            // treats an empty key string as hasKey == FALSE) takes over,
            // instead of feeding "0" into AES/Img3 keying code that
            // assumes a real key is present.
            if (pair.iv == "0" && pair.key == "0") {
                pair.iv.clear();
                pair.key.clear();
            }
            result[key] = pair;

            free(ivStr);
            free(keyStr);
        }

        free(key);
        key = nullptr;
        plist_dict_next_item(root, it, &key, &subnode);
    }

    free(it);
    plist_free(root);

    return result;
}

static std::string plistDictString(plist_t dict, const char* key) {
    plist_t node = plist_dict_get_item(dict, key);
    if (!node) return "";
    char* val = nullptr;
    plist_get_string_val(node, &val);
    std::string result = val ? val : "";
    free(val);
    return result;
}

std::optional<ManifestInfo> parseManifest(const std::string& manifestPath) {
    std::ifstream f(manifestPath, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string contents = ss.str();

    plist_t root = nullptr;
    if (contents.size() >= 6 && contents.compare(0, 6, "bplist") == 0) {
        plist_from_bin(contents.data(), (uint32_t)contents.size(), &root);
    } else {
        plist_from_xml(contents.data(), (uint32_t)contents.size(), &root);
    }
    if (!root) return std::nullopt;

    ManifestInfo info;
    info.realBuildID = plistDictString(root, "ProductBuildVersion");
    info.productVersion = plistDictString(root, "ProductVersion");

    plist_t identities = plist_dict_get_item(root, "BuildIdentities");
    uint32_t count = identities ? plist_array_get_size(identities) : 0;
    if (count == 0) {
        plist_free(root);
        return std::nullopt;
    }

    // Some manifests (confirmed on AppleTV2,1 8M89) carry more than one
    // BuildIdentity — e.g. an "Erase" (full restore/DFU) variant AND an
    // "Update" (in-place OTA, assumes an already-booted OS) variant — and
    // list them in an order where Update comes last. blackb0x always does
    // a full DFU restore, so it needs the Erase identity specifically; the
    // two variants reference different RestoreRamDisk files under
    // different (and differently keyed!) names, so picking the wrong one
    // silently decrypts the wrong file with the wrong key. Prefer the
    // identity whose own Info.RestoreBehavior is "Erase"; fall back to the
    // last identity (the original, pre-existing behavior) for any manifest
    // that doesn't carry this field at all.
    plist_t identity = plist_array_get_item(identities, count - 1);
    for (uint32_t i = 0; i < count; i++) {
        plist_t candidate = plist_array_get_item(identities, i);
        plist_t candidateInfo = plist_dict_get_item(candidate, "Info");
        if (candidateInfo && plistDictString(candidateInfo, "RestoreBehavior") == "Erase") {
            identity = candidate;
            break;
        }
    }
    plist_t manifest = plist_dict_get_item(identity, "Manifest");

    auto componentPath = [&](const char* component) -> std::string {
        plist_t comp = plist_dict_get_item(manifest, component);
        plist_t info_ = comp ? plist_dict_get_item(comp, "Info") : nullptr;
        return info_ ? plistDictString(info_, "Path") : "";
    };

    info.iBSSPath = componentPath("iBSS");
    info.iBECPath = componentPath("iBEC");
    info.kernelCachePath = componentPath("KernelCache");
    info.deviceTreePath = componentPath("DeviceTree");
    info.restoreRamdiskPath = componentPath("RestoreRamDisk");
    info.restoreLogoPath = componentPath("RestoreLogo");

    // Matches idevicerestore's own recovery_send_loaded_by_iboot(): walk
    // every entry in the manifest (not just the fixed set above) and
    // collect whichever ones are flagged Info.IsLoadedByiBoot (and not
    // Info.IsLoadedByiBootStage1) -- see ManifestInfo::loadedByIBootComponents'
    // own comment for why this stays generic.
    {
        plist_dict_iter iter = nullptr;
        plist_dict_new_iter(manifest, &iter);
        while (iter) {
            char* key = nullptr;
            plist_t node = nullptr;
            plist_dict_next_item(manifest, iter, &key, &node);
            if (!key) break;

            plist_t info_ = node ? plist_dict_get_item(node, "Info") : nullptr;
            if (info_) {
                plist_t loadedNode = plist_dict_get_item(info_, "IsLoadedByiBoot");
                plist_t stage1Node = plist_dict_get_item(info_, "IsLoadedByiBootStage1");
                uint8_t loaded = 0, stage1 = 0;
                if (loadedNode && plist_get_node_type(loadedNode) == PLIST_BOOLEAN) {
                    plist_get_bool_val(loadedNode, &loaded);
                }
                if (stage1Node && plist_get_node_type(stage1Node) == PLIST_BOOLEAN) {
                    plist_get_bool_val(stage1Node, &stage1);
                }
                if (loaded && !stage1) {
                    std::string path = plistDictString(info_, "Path");
                    if (!path.empty()) {
                        info.loadedByIBootComponents.emplace_back(key, path);
                    }
                }
            }
            free(key);
        }
        free(iter);
    }

    // plist_copy(): `identity` is a subtree of `root`, freed below --
    // detach an independent copy so it outlives this function.
    info.buildIdentity = std::shared_ptr<void>(plist_copy(identity), plist_free);

    plist_free(root);
    return info;
}

std::set<std::string> signedBuildsForDevice(const std::string& deviceModel) {
    std::set<std::string> result;
    std::string json = httpGet("https://api.ipsw.me/v4/device/" + deviceModel + "?type=ipsw");
    if (json.empty()) return result;

    // Each firmware entry is a flat object (no nested {}), e.g.:
    //   {"identifier":"AppleTV2,1", ..., "buildid":"11D258", ..., "signed":true}
    // so matching non-nested {...} spans is a safe, simple way to isolate
    // one entry at a time without a real JSON parser (see IPSW.hpp).
    std::regex entryRe(R"RE(\{[^{}]*\})RE");
    std::regex buildidRe(R"RE("buildid"\s*:\s*"([^"]*)")RE");
    std::regex signedRe(R"RE("signed"\s*:\s*true)RE");

    for (auto it = std::sregex_iterator(json.begin(), json.end(), entryRe); it != std::sregex_iterator(); ++it) {
        std::string entry = it->str();
        std::smatch buildidMatch;
        if (!std::regex_search(entry, buildidMatch, buildidRe)) continue;
        if (std::regex_search(entry, signedRe)) {
            result.insert(buildidMatch[1].str());
        }
    }
    return result;
}

std::string newestVersionForDevice(const std::string& deviceModel) {
    std::string json = httpGet("https://api.ipsw.me/v4/device/" + deviceModel + "?type=ipsw");
    if (json.empty()) return "";

    // Same flat non-nested-braces entry matching as signedBuildsForDevice()
    // above (see that function's own comment) — the "firmwares" array's
    // own entries have no nested {} of their own. Confirmed directly
    // (`curl -s 'https://api.ipsw.me/v4/device/AppleTV3,2?type=ipsw'`) that
    // this array is already ordered newest-release-first (by
    // "releasedate", descending) — so the first entry with a "version"
    // field found in document order is the newest, no sorting needed.
    // "buildid" is required in the match too only to distinguish a real
    // firmware entry from some other flat {} elsewhere in the response
    // (e.g. "boards") that happens to carry no "version"/"buildid" pair at
    // all.
    std::regex entryRe(R"RE(\{[^{}]*\})RE");
    std::regex versionRe(R"RE("version"\s*:\s*"([^"]*)")RE");
    std::regex buildidRe(R"RE("buildid"\s*:\s*"([^"]*)")RE");

    for (auto it = std::sregex_iterator(json.begin(), json.end(), entryRe); it != std::sregex_iterator(); ++it) {
        std::string entry = it->str();
        std::smatch versionMatch;
        if (!std::regex_search(entry, versionMatch, versionRe)) continue;
        if (!std::regex_search(entry, buildidRe)) continue;
        return versionMatch[1].str();
    }
    return "";
}
