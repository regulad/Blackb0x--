//
//  FirmwarePatch.cpp
//  Blackb0x
//
//  See FirmwarePatch.hpp for what this is and why. Compiled into both
//  blackb0x (assembleSuite()) and make-patches (makeSuitePatches()).
//

#include "FirmwarePatch.hpp"

#include "BsdiffGlue.h"
#include "IPSW.hpp"
#include "IPSWDownloader.hpp"
#include "Img3Crypt.hpp"
#include "Patcher.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <bzlib.h>
#include <plist/plist.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;

namespace fwpatch {

namespace {

const char kPatchMagic[] = "ENDSLEY/BSDIFF43";  // 16 bytes, no terminator written
constexpr size_t kPatchMagicLen = 16;
constexpr size_t kPatchHeaderLen = kPatchMagicLen + 8;

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(in);
}

bool writeFile(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (size > 0) out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

bool nonEmpty(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec) && fs::file_size(path, ec) > 0 && !ec;
}

// bsdiff's own integer encoding: 8 bytes little-endian magnitude, sign in the
// top bit of the last byte.
void offtout(int64_t x, uint8_t* buf) {
    uint64_t y = x < 0 ? static_cast<uint64_t>(-x) : static_cast<uint64_t>(x);
    for (int i = 0; i < 8; i++) buf[i] = static_cast<uint8_t>(y >> (8 * i));
    if (x < 0) buf[7] |= 0x80;
}

int64_t offtin(const uint8_t* buf) {
    uint64_t y = 0;
    for (int i = 7; i >= 0; i--) y = (y << 8) | (i == 7 ? (buf[i] & 0x7F) : buf[i]);
    int64_t x = static_cast<int64_t>(y);
    return (buf[7] & 0x80) ? -x : x;
}

int bsdiffWrite(void* opaque, const void* buffer, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(opaque);
    const auto* p = static_cast<const uint8_t*>(buffer);
    out->insert(out->end(), p, p + size);
    return 0;
}

struct BzReader {
    bz_stream bz;
};

int bspatchRead(void* opaque, void* buffer, int length) {
    auto* r = static_cast<BzReader*>(opaque);
    r->bz.next_out = static_cast<char*>(buffer);
    r->bz.avail_out = static_cast<unsigned int>(length);
    while (r->bz.avail_out > 0) {
        int rc = BZ2_bzDecompress(&r->bz);
        if (rc == BZ_STREAM_END) break;
        if (rc != BZ_OK) return -1;
        if (r->bz.avail_in == 0 && r->bz.avail_out > 0) return -1;  // truncated
    }
    return r->bz.avail_out == 0 ? 0 : -1;
}

std::string hex(const unsigned char* bytes, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s.push_back(digits[bytes[i] >> 4]);
        s.push_back(digits[bytes[i] & 0xF]);
    }
    return s;
}

std::string tupleSuffix(const std::string& device, const std::string& build) {
    return "-" + device + "_" + build;
}

// The .keys entry each dist/ component is decrypted with. Both iBECs come
// from the one stock iBEC.
std::string keyNameFor(const std::string& component) {
    if (component == "iBSS") return "iBSS";
    if (component == "iBEC" || component == "iBECTether") return "iBEC";
    if (component == "KernelCache") return "Kernelcache";
    if (component == "RestoreRamDisk") return "RestoreRamdisk";
    return "";
}

// Where each dist/ component comes from inside the IPSW.
std::string sourceFor(const std::string& component, const ManifestInfo& m) {
    if (component == "iBSS") return m.iBSSPath;
    if (component == "iBEC" || component == "iBECTether") return m.iBECPath;
    if (component == "KernelCache") return m.kernelCachePath;
    if (component == "DeviceTree") return m.deviceTreePath;
    if (component == "RestoreLogo") return m.restoreLogoPath;
    if (component == "RestoreRamDisk") return m.restoreRamdiskPath;
    for (const auto& c : m.loadedByIBootComponents) {
        if (c.first == component) return c.second;
    }
    return "";
}

// The pieces both directions need for one tuple: the IPSW, its manifest, the
// keys, and a local cache directory to download stock components into.
struct Firmware {
    std::string workDir;
    std::unique_ptr<FragmentDownloader> downloader;
    ManifestInfo manifest;
    std::map<std::string, FirmwareKeyPair> keys;

    const FirmwareKeyPair* key(const std::string& name) const {
        if (name.empty()) return nullptr;
        auto it = keys.find(name);
        return it == keys.end() ? nullptr : &it->second;
    }

    // Download `remotePath` into the cache (once) and return its local path.
    std::string stock(const std::string& remotePath) {
        std::string local = workDir + "/" + fs::path(remotePath).filename().string();
        if (nonEmpty(local)) return local;
        if (!downloader->downloadComponent(remotePath, local, nullptr) || !nonEmpty(local)) {
            fprintf(stderr, "Failed to download %s from the IPSW\n", remotePath.c_str());
            return "";
        }
        return local;
    }
};

std::optional<Firmware> openFirmware(const std::string& device, const std::string& build) {
    IpswFetch fetcher;
    const std::string url = fetcher.firmwareURLForDevice(device, build);
    if (url.empty()) {
        fprintf(stderr, "No IPSW URL for %s %s\n", device.c_str(), build.c_str());
        return std::nullopt;
    }
    Firmware fw;
    fw.downloader = std::make_unique<FragmentDownloader>(url);
    if (!fw.downloader->open()) {
        fprintf(stderr, "Could not open the IPSW for %s %s\n", device.c_str(), build.c_str());
        return std::nullopt;
    }
    fw.workDir = ipswDataRoot() + "/" + device + "/" + build;
    std::error_code ec;
    fs::create_directories(fw.workDir, ec);
    const std::string manifestPath = fw.workDir + "/BuildManifest.plist";
    if (!nonEmpty(manifestPath) && !fw.downloader->downloadComponent("BuildManifest.plist", manifestPath, nullptr)) {
        fprintf(stderr, "Failed to download BuildManifest.plist for %s %s\n", device.c_str(), build.c_str());
        return std::nullopt;
    }
    auto manifest = parseManifest(manifestPath);
    if (!manifest) {
        fprintf(stderr, "Failed to parse BuildManifest.plist for %s %s\n", device.c_str(), build.c_str());
        return std::nullopt;
    }
    fw.manifest = *manifest;
    fw.keys = fetcher.keysForDevice(device, build);
    return std::optional<Firmware>(std::move(fw));
}

// The component names a bake's Manifest-<tuple>.txt lists, in order.
std::vector<std::string> readComponentList(const std::string& path) {
    std::vector<std::string> names;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        names.push_back(line);
    }
    return names;
}

std::string dictString(plist_t dict, const char* key) {
    plist_t node = plist_dict_get_item(dict, key);
    if (!node || plist_get_node_type(node) != PLIST_STRING) return "";
    char* val = nullptr;
    plist_get_string_val(node, &val);
    std::string s = val ? val : "";
    free(val);
    return s;
}

}  // namespace

std::string indexFileName(const std::string& device, const std::string& build) {
    return "Patches" + tupleSuffix(device, build) + ".plist";
}

std::string sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        if (got > 0) CC_SHA256_Update(&ctx, buf.data(), static_cast<CC_LONG>(got));
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    return hex(digest, sizeof(digest));
}

bool unwrap(const std::string& img3, const std::string& payloadOut, const FirmwareKeyPair* key) {
    const bool haveKey = key && !key->key.empty();
    // "FALSE" with no template is the plain unwrap: openAbstractFile2() peels
    // the IMG3 (and, for a kernelcache, the complzss layer) and decrypts when
    // given a key. Same call PatcherPatch.cpp's patch*() functions open with.
    decrypt(const_cast<char*>(img3.c_str()), const_cast<char*>(payloadOut.c_str()),
            haveKey ? const_cast<char*>(key->key.c_str()) : nullptr,
            haveKey ? const_cast<char*>(key->iv.c_str()) : nullptr, (char*)"FALSE", nullptr);
    if (!nonEmpty(payloadOut)) {
        fprintf(stderr, "Could not unwrap %s\n", img3.c_str());
        return false;
    }
    return true;
}

bool rewrap(const std::string& payload, const std::string& stockTemplate, const std::string& img3Out,
            const FirmwareKeyPair* key) {
    const bool haveKey = key && !key->key.empty();
    decrypt(const_cast<char*>(payload.c_str()), const_cast<char*>(img3Out.c_str()),
            haveKey ? const_cast<char*>(key->key.c_str()) : nullptr,
            haveKey ? const_cast<char*>(key->iv.c_str()) : nullptr, (char*)"FALSE",
            const_cast<char*>(stockTemplate.c_str()));
    // decrypt() validates its own IMG3 output and deletes it if malformed, so
    // existence is the whole check.
    if (!nonEmpty(img3Out)) {
        fprintf(stderr, "Could not re-wrap %s\n", payload.c_str());
        return false;
    }
    return true;
}

bool makePatch(const std::string& oldPath, const std::string& newPath, const std::string& patchOut) {
    std::vector<uint8_t> oldData, newData;
    if (!readFile(oldPath, oldData) || !readFile(newPath, newData)) {
        fprintf(stderr, "makePatch: cannot read %s or %s\n", oldPath.c_str(), newPath.c_str());
        return false;
    }

    std::vector<uint8_t> raw;
    if (blackb0x_bsdiff(oldData.data(), static_cast<int64_t>(oldData.size()), newData.data(),
                        static_cast<int64_t>(newData.size()), bsdiffWrite, &raw) != 0) {
        fprintf(stderr, "makePatch: bsdiff failed for %s\n", newPath.c_str());
        return false;
    }

    // bzip2's documented worst case: 1% larger plus 600 bytes.
    unsigned int compressedLen = static_cast<unsigned int>(raw.size() + raw.size() / 100 + 600);
    std::vector<uint8_t> out(kPatchHeaderLen + compressedLen);
    std::memcpy(out.data(), kPatchMagic, kPatchMagicLen);
    offtout(static_cast<int64_t>(newData.size()), out.data() + kPatchMagicLen);
    int rc = BZ2_bzBuffToBuffCompress(reinterpret_cast<char*>(out.data() + kPatchHeaderLen), &compressedLen,
                                      reinterpret_cast<char*>(raw.data()), static_cast<unsigned int>(raw.size()),
                                      9, 0, 0);
    if (rc != BZ_OK) {
        fprintf(stderr, "makePatch: bzip2 failed (%d)\n", rc);
        return false;
    }
    out.resize(kPatchHeaderLen + compressedLen);
    return writeFile(patchOut, out.data(), out.size());
}

bool applyPatch(const std::string& oldPath, const std::string& patchPath, const std::string& newOut) {
    std::vector<uint8_t> oldData, patch;
    if (!readFile(oldPath, oldData) || !readFile(patchPath, patch)) {
        fprintf(stderr, "applyPatch: cannot read %s or %s\n", oldPath.c_str(), patchPath.c_str());
        return false;
    }
    if (patch.size() < kPatchHeaderLen || std::memcmp(patch.data(), kPatchMagic, kPatchMagicLen) != 0) {
        fprintf(stderr, "applyPatch: %s is not a bsdiff patch\n", patchPath.c_str());
        return false;
    }
    const int64_t newSize = offtin(patch.data() + kPatchMagicLen);
    if (newSize < 0) {
        fprintf(stderr, "applyPatch: %s has a corrupt header\n", patchPath.c_str());
        return false;
    }

    BzReader reader;
    std::memset(&reader.bz, 0, sizeof(reader.bz));
    if (BZ2_bzDecompressInit(&reader.bz, 0, 0) != BZ_OK) return false;
    reader.bz.next_in = reinterpret_cast<char*>(patch.data() + kPatchHeaderLen);
    reader.bz.avail_in = static_cast<unsigned int>(patch.size() - kPatchHeaderLen);

    std::vector<uint8_t> newData(static_cast<size_t>(newSize));
    int rc = blackb0x_bspatch(oldData.data(), static_cast<int64_t>(oldData.size()), newData.data(), newSize,
                              bspatchRead, &reader);
    BZ2_bzDecompressEnd(&reader.bz);
    if (rc != 0) {
        fprintf(stderr, "applyPatch: %s does not apply to %s\n", patchPath.c_str(), oldPath.c_str());
        return false;
    }
    return writeFile(newOut, newData.data(), newData.size());
}

bool writeIndex(const std::string& path, const Index& index) {
    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "Format", plist_new_uint(1));
    plist_dict_set_item(root, "Device", plist_new_string(index.device.c_str()));
    plist_dict_set_item(root, "Build", plist_new_string(index.build.c_str()));
    plist_t components = plist_new_array();
    for (const Entry& e : index.entries) {
        plist_t d = plist_new_dict();
        plist_dict_set_item(d, "Name", plist_new_string(e.name.c_str()));
        plist_dict_set_item(d, "Source", plist_new_string(e.source.c_str()));
        plist_dict_set_item(d, "Mode", plist_new_string(e.mode.c_str()));
        plist_dict_set_item(d, "KeyName", plist_new_string(e.keyName.c_str()));
        plist_dict_set_item(d, "SourceSHA256", plist_new_string(e.sourceSHA256.c_str()));
        plist_dict_set_item(d, "PayloadSHA256", plist_new_string(e.payloadSHA256.c_str()));
        plist_dict_set_item(d, "OutputSHA256", plist_new_string(e.outputSHA256.c_str()));
        plist_dict_set_item(d, "Patch", plist_new_string(e.patch.c_str()));
        plist_array_append_item(components, d);
    }
    plist_dict_set_item(root, "Components", components);

    char* xml = nullptr;
    uint32_t len = 0;
    plist_to_xml(root, &xml, &len);
    plist_free(root);
    if (!xml) return false;
    bool ok = writeFile(path, reinterpret_cast<const uint8_t*>(xml), len);
    free(xml);
    return ok;
}

std::optional<Index> readIndex(const std::string& path) {
    std::vector<uint8_t> data;
    if (!readFile(path, data)) return std::nullopt;
    plist_t root = nullptr;
    plist_from_xml(reinterpret_cast<const char*>(data.data()), static_cast<uint32_t>(data.size()), &root);
    if (!root) return std::nullopt;

    Index index;
    index.device = dictString(root, "Device");
    index.build = dictString(root, "Build");
    plist_t components = plist_dict_get_item(root, "Components");
    if (!components || plist_get_node_type(components) != PLIST_ARRAY) {
        plist_free(root);
        return std::nullopt;
    }
    for (uint32_t i = 0; i < plist_array_get_size(components); i++) {
        plist_t d = plist_array_get_item(components, i);
        Entry e;
        e.name = dictString(d, "Name");
        e.source = dictString(d, "Source");
        e.mode = dictString(d, "Mode");
        e.keyName = dictString(d, "KeyName");
        e.sourceSHA256 = dictString(d, "SourceSHA256");
        e.payloadSHA256 = dictString(d, "PayloadSHA256");
        e.outputSHA256 = dictString(d, "OutputSHA256");
        e.patch = dictString(d, "Patch");
        index.entries.push_back(e);
    }
    plist_free(root);
    return index;
}

bool makeSuitePatches(const std::string& distDir, const std::string& outDir, const std::string& device,
                      const std::string& build) {
    const std::string suffix = tupleSuffix(device, build);
    const std::string componentList = distDir + "/Manifest" + suffix + ".txt";
    if (!nonEmpty(componentList)) {
        fprintf(stderr, "make-patches: no finished bake for %s %s (%s is missing)\n", device.c_str(), build.c_str(),
                componentList.c_str());
        return false;
    }
    std::vector<std::string> names = readComponentList(componentList);
    // Baked by the ramdisk half and deliberately absent from the list (see
    // BakeFirmware.cpp's index comment), but it is a required component.
    std::vector<std::pair<std::string, std::string>> files;  // component, dist filename
    for (const auto& n : names) files.emplace_back(n, n + suffix);
    files.emplace_back("RestoreRamDisk", "RestoreRamDisk" + suffix + ".dmg");

    auto fw = openFirmware(device, build);
    if (!fw) return false;

    std::error_code ec;
    fs::create_directories(outDir, ec);
    const fs::path scratch = fs::path(outDir) / ".work";
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);

    Index index;
    index.device = device;
    index.build = build;
    bool ok = true;
    for (const auto& f : files) {
        const std::string& name = f.first;
        const std::string baked = distDir + "/" + f.second;
        Entry e;
        e.name = name;
        e.source = sourceFor(name, fw->manifest);
        e.outputSHA256 = sha256File(baked);
        if (e.source.empty() || e.outputSHA256.empty()) {
            fprintf(stderr, "make-patches: %s: no stock source or no baked file\n", name.c_str());
            ok = false;
            continue;
        }
        const std::string stock = fw->stock(e.source);
        if (stock.empty()) {
            ok = false;
            continue;
        }
        e.sourceSHA256 = sha256File(stock);

        if (e.sourceSHA256 == e.outputSHA256) {
            e.mode = "verbatim";
            printf("%-14s verbatim\n", name.c_str());
            index.entries.push_back(e);
            continue;
        }

        e.keyName = keyNameFor(name);
        const FirmwareKeyPair* key = fw->key(e.keyName);
        const std::string stockPayload = (scratch / (name + ".stock")).string();
        const std::string bakedPayload = (scratch / (name + ".baked")).string();
        if (!unwrap(stock, stockPayload, key)) {
            ok = false;
            continue;
        }
        e.payloadSHA256 = sha256File(stockPayload);

        if (img3FileHasMagic(baked)) {
            e.mode = "rewrap";
            if (!unwrap(baked, bakedPayload, key)) {
                ok = false;
                continue;
            }
        } else {
            e.mode = "raw";
            fs::copy_file(baked, bakedPayload, fs::copy_options::overwrite_existing, ec);
        }

        e.patch = name + suffix + ".bsdiff";
        const std::string patchPath = outDir + "/" + e.patch;
        if (!makePatch(stockPayload, bakedPayload, patchPath)) {
            ok = false;
            continue;
        }

        // Round trip: the bundle is only worth publishing if it rebuilds the
        // bake byte for byte.
        const std::string rebuiltPayload = (scratch / (name + ".rebuilt")).string();
        const std::string rebuilt = (scratch / (name + ".out")).string();
        bool roundTrip = applyPatch(stockPayload, patchPath, rebuiltPayload);
        if (roundTrip && e.mode == "rewrap") {
            roundTrip = rewrap(rebuiltPayload, stock, rebuilt, key);
        } else if (roundTrip) {
            fs::rename(rebuiltPayload, rebuilt, ec);
        }
        if (!roundTrip || sha256File(rebuilt) != e.outputSHA256) {
            fprintf(stderr,
                    "make-patches: %s does not round-trip: re-applying its patch to Apple's image does not "
                    "reproduce the baked %s byte for byte\n",
                    name.c_str(), f.second.c_str());
            fs::remove(patchPath, ec);
            ok = false;
            continue;
        }
        printf("%-14s %-7s %llu-byte patch\n", name.c_str(), e.mode.c_str(),
               static_cast<unsigned long long>(fs::file_size(patchPath, ec)));
        index.entries.push_back(e);
    }
    fs::remove_all(scratch, ec);
    if (!ok) return false;

    fs::copy_file(componentList, outDir + "/Manifest" + suffix + ".txt", fs::copy_options::overwrite_existing, ec);
    if (ec || !writeIndex(outDir + "/" + indexFileName(device, build), index)) {
        fprintf(stderr, "make-patches: could not write the bundle index into %s\n", outDir.c_str());
        return false;
    }
    return true;
}

bool assembleSuite(const std::string& bundleDir, const std::string& distDir, const std::string& device,
                   const std::string& build) {
    const std::string suffix = tupleSuffix(device, build);
    auto index = readIndex(bundleDir + "/" + indexFileName(device, build));
    if (!index) {
        fprintf(stderr, "The patch bundle has no index for %s %s\n", device.c_str(), build.c_str());
        return false;
    }
    auto fw = openFirmware(device, build);
    if (!fw) return false;

    // Every image below is hash-checked, which subsumes the per-image summary.
    img3SetReportSuccess(false);
    struct RestoreReport {
        ~RestoreReport() { img3SetReportSuccess(true); }
    } restoreReport;

    std::error_code ec;
    fs::create_directories(distDir, ec);
    const fs::path scratch = fs::path(distDir) / ".assemble-tmp";
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);

    bool ok = true;
    for (const Entry& e : index->entries) {
        const std::string distName = e.name + suffix + (e.name == "RestoreRamDisk" ? ".dmg" : "");
        const std::string dest = distDir + "/" + distName;
        if (sha256File(dest) == e.outputSHA256) continue;  // already rebuilt

        printf("Rebuilding %s from Apple's %s...\n", e.name.c_str(), fs::path(e.source).filename().string().c_str());
        fflush(stdout);
        const std::string stock = fw->stock(e.source);
        if (stock.empty()) {
            ok = false;
            break;
        }
        if (sha256File(stock) != e.sourceSHA256) {
            fprintf(stderr, "%s from the IPSW is not the image the patch was made against\n", e.source.c_str());
            ok = false;
            break;
        }

        const std::string out = (scratch / distName).string();
        if (e.mode == "verbatim") {
            fs::copy_file(stock, out, fs::copy_options::overwrite_existing, ec);
        } else if (e.mode == "rewrap" || e.mode == "raw") {
            const FirmwareKeyPair* key = fw->key(e.keyName);
            const std::string payload = (scratch / (e.name + ".stock")).string();
            const std::string patched = (scratch / (e.name + ".patched")).string();
            if (!unwrap(stock, payload, key)) {
                ok = false;
                break;
            }
            if (sha256File(payload) != e.payloadSHA256) {
                fprintf(stderr, "%s unwrapped to the wrong payload; check keys/ for %s %s (%s)\n", e.name.c_str(),
                        device.c_str(), build.c_str(), e.keyName.c_str());
                ok = false;
                break;
            }
            if (!applyPatch(payload, bundleDir + "/" + e.patch, patched)) {
                ok = false;
                break;
            }
            if (e.mode == "rewrap") {
                if (!rewrap(patched, stock, out, key)) {
                    ok = false;
                    break;
                }
            } else {
                fs::rename(patched, out, ec);
            }
        } else {
            fprintf(stderr, "Unknown mode \"%s\" for %s in the patch bundle\n", e.mode.c_str(), e.name.c_str());
            ok = false;
            break;
        }

        if (sha256File(out) != e.outputSHA256) {
            fprintf(stderr, "Rebuilt %s does not match the baked image's hash; refusing to use it\n", e.name.c_str());
            ok = false;
            break;
        }
        fs::rename(out, dest, ec);
        if (ec) {
            fprintf(stderr, "Could not place %s into %s: %s\n", distName.c_str(), distDir.c_str(),
                    ec.message().c_str());
            ok = false;
            break;
        }
    }
    fs::remove_all(scratch, ec);
    if (!ok) return false;

    // Last, so its presence still means "this tuple is complete".
    fs::copy_file(bundleDir + "/Manifest" + suffix + ".txt", distDir + "/Manifest" + suffix + ".txt",
                  fs::copy_options::overwrite_existing, ec);
    return !ec;
}

}  // namespace fwpatch
