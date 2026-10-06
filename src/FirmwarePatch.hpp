//
//  FirmwarePatch.hpp
//  Blackb0x
//
//  Ships the firmware suite as PATCHES rather than as Apple's bytes.
//
//  bake-firmware still writes a complete dist/ -- patched iBSS/iBEC/
//  KernelCache/RestoreRamDisk plus verbatim DeviceTree/RestoreLogo -- but
//  every one of those files is Apple firmware, so CI no longer publishes
//  dist/. It publishes what make-patches derives from it instead: one bsdiff
//  per modified component, taken between the stock and patched PAYLOADS (the
//  bytes inside the IMG3 container -- the decompressed kernel, the HFS+ DMG,
//  the raw bootloader), plus an index naming where each component comes from
//  and the SHA-256 it must have at every stage. Components blackb0x sends
//  unmodified carry no patch at all; the index just says "take Apple's".
//
//  blackb0x reverses it (assembleSuite()): fetch each stock component from
//  Apple's own IPSW, unwrap it, apply the patch, re-wrap it with the stock
//  image as the template exactly as the bake did, and check the result
//  against the hash of what the bake published. Re-wrapping is deterministic
//  (same payload, same template, same key/IV, same LZSS/AES code), so the
//  reconstructed dist/ is byte-identical to the one CI baked -- and the
//  output hash makes that a checked fact on every run rather than an
//  assumption.
//
//  Patch file format: mendsley/bsdiff's own, unchanged -- the 16-byte magic
//  "ENDSLEY/BSDIFF43", the new size as an 8-byte sign-magnitude integer, then
//  one bzip2 stream of bsdiff()'s control/diff/extra output. So a patch can be
//  checked by hand with that project's own bspatch CLI.
//

#pragma once

#include <optional>
#include <string>
#include <vector>

struct FirmwareKeyPair;

namespace fwpatch {

// How a component's dist/ file is rebuilt from Apple's.
//   "rewrap"   unwrap stock -> patch payload -> re-wrap with stock as template
//   "raw"      unwrap stock -> patch payload -> publish the payload itself
//              (the AppleTV3 iBSS, which checkm8 uploads as plaintext code)
//   "verbatim" stock bytes, untouched (DeviceTree, RestoreLogo, ...)
struct Entry {
    std::string name;           // dist/ component name, e.g. "KernelCache"
    std::string source;         // path of the stock image inside the IPSW
    std::string mode;           // "rewrap" | "raw" | "verbatim"
    std::string keyName;        // .keys entry to unwrap with; empty = none
    std::string sourceSHA256;   // the stock IMG3 as downloaded
    std::string payloadSHA256;  // its unwrapped payload ("" for verbatim)
    std::string outputSHA256;   // the finished dist/ file
    std::string patch;          // patch filename in the bundle ("" for verbatim)
};

struct Index {
    std::string device;
    std::string build;
    std::vector<Entry> entries;
};

// "Patches-<device>_<build>.plist" -- the bundle's index for one tuple.
std::string indexFileName(const std::string& device, const std::string& build);

// Lowercase hex SHA-256 of a file's contents, or "" if it can't be read.
std::string sha256File(const std::string& path);

// IMG3 container <-> payload, over Img3Crypt's decrypt(). `key` may be null
// or empty for an unencrypted image. Both return false (with the reason on
// stderr) if no non-empty output was produced.
bool unwrap(const std::string& img3, const std::string& payloadOut, const FirmwareKeyPair* key);
bool rewrap(const std::string& payload, const std::string& stockTemplate, const std::string& img3Out,
            const FirmwareKeyPair* key);

// bsdiff between two files / apply one. See the header comment for the format.
bool makePatch(const std::string& oldPath, const std::string& newPath, const std::string& patchOut);
bool applyPatch(const std::string& oldPath, const std::string& patchPath, const std::string& newOut);

bool writeIndex(const std::string& path, const Index& index);
std::optional<Index> readIndex(const std::string& path);

// make-patches: derive a patch bundle in `outDir` from a finished bake in
// `distDir` for one (device, build). Downloads the stock components it needs
// into the shared IPSW cache. Every patch is round-tripped (applied and, for
// "rewrap", re-wrapped) and must reproduce the baked file's exact hash before
// it is written to the index, so a bundle that exists is one that is known to
// rebuild the bake.
bool makeSuitePatches(const std::string& distDir, const std::string& outDir, const std::string& device,
                      const std::string& build);

// blackb0x: rebuild dist/ for one (device, build) from the patch bundle in
// `bundleDir` and Apple's own IPSW. Writes each file into `distDir` only once
// its output hash matches, and copies the bundle's Manifest-<tuple>.txt last,
// so a partial run never leaves a suite that looks complete.
bool assembleSuite(const std::string& bundleDir, const std::string& distDir, const std::string& device,
                   const std::string& build);

}  // namespace fwpatch
