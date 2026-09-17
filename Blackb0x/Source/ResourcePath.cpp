//
//  ResourcePath.cpp
//  Blackb0x
//

#include "ResourcePath.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <vector>

#include <climits>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

std::string resolveRamdiskPath() {
    if (const char* override_ = getenv("BLACKB0X_RAMDISK_DIR")) {
        return std::string(override_);
    }
    return "Blackb0x/ramdisk";
}

std::string resolveImageKeyPath(const std::string& relativePath) {
    if (const char* override_ = getenv("BLACKB0X_IMAGEKEYS_DIR")) {
        return std::string(override_) + "/" + relativePath;
    }
    return "Blackb0x/ImageKeys/" + relativePath;
}

// Shared by resolveGasterPath()/resolvePwnPath() below: the directory
// blackb0x's own executable lives in, or empty if it can't be determined
// (falls back to a bare binaryName, resolved via PATH at exec time).
static std::string resolveOwnExecutableDir() {
    char exePath[PATH_MAX];
#if defined(__APPLE__)
    // No /proc on Darwin; _NSGetExecutablePath() may return a path
    // containing symlinks, so resolve it the same way readlink's result
    // already is on Linux.
    uint32_t size = sizeof(exePath);
    bool haveExePath = (_NSGetExecutablePath(exePath, &size) == 0);
    if (haveExePath) {
        char resolved[PATH_MAX];
        if (realpath(exePath, resolved) != nullptr) {
            strncpy(exePath, resolved, sizeof(exePath) - 1);
            exePath[sizeof(exePath) - 1] = '\0';
        }
    }
    ssize_t len = haveExePath ? static_cast<ssize_t>(strlen(exePath)) : -1;
#else
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) exePath[len] = '\0';
#endif
    if (len > 0) {
        std::string dir(exePath);
        size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos) {
            return dir.substr(0, slash);
        }
    }
    return "";
}

std::string resolveGasterPath() {
    if (const char* override_ = getenv("BLACKB0X_GASTER")) {
        return std::string(override_);
    }
    std::string dir = resolveOwnExecutableDir();
    if (!dir.empty()) return dir + "/gaster";
    return "gaster";
}

std::string resolvePwnPath() {
    if (const char* override_ = getenv("BLACKB0X_PWN")) {
        return std::string(override_);
    }
    std::string dir = resolveOwnExecutableDir();
    if (!dir.empty()) return dir + "/blackb0x-pwn";
    return "blackb0x-pwn";
}

std::string resolveBakeAllRamdisksPath() {
    if (const char* override_ = getenv("BLACKB0X_BAKE_ALL_RAMDISKS")) {
        return std::string(override_);
    }
    std::string dir = resolveOwnExecutableDir();
    if (!dir.empty()) return dir + "/bake-all-ramdisks";
    return "bake-all-ramdisks";
}

std::string resolveIBoot32PatcherPath() {
    if (const char* override_ = getenv("BLACKB0X_IBOOT32PATCHER")) {
        return std::string(override_);
    }
    std::string dir = resolveOwnExecutableDir();
    if (!dir.empty()) return dir + "/iBoot32Patcher";
    return "iBoot32Patcher";
}

std::string resolveDebsPath() {
    if (const char* override_ = getenv("BLACKB0X_DEBS_DIR")) {
        return std::string(override_);
    }
    return "Blackb0x/Debs";
}

std::string resolveMiscPath(const std::string& relativePath) {
    if (const char* override_ = getenv("BLACKB0X_MISC_DIR")) {
        return std::string(override_) + "/" + relativePath;
    }
    return "Blackb0x/Misc/" + relativePath;
}

std::string resolveEntrypointPath() {
    if (const char* override_ = getenv("BLACKB0X_ENTRYPOINT_DIR")) {
        return std::string(override_);
    }
    return "entrypoint";
}

std::string decryptedDMGFor(const std::string& path) {
    return fs::path(path).replace_extension("").string() + "-decrypted.dmg";
}

// FNV-1a 64-bit — see ramdiskOverlayContentHash()'s doc comment for why a
// non-cryptographic hash is the right choice here.
static void fnv1aUpdate(uint64_t& h, const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
}

static void fnv1aUpdateStr(uint64_t& h, const std::string& s) {
    fnv1aUpdate(h, s.data(), s.size());
    fnv1aUpdate(h, "\0", 1);
}

static void hashFileInto(uint64_t& h, const fs::path& path) {
    fnv1aUpdateStr(h, "FILE");
    std::ifstream f(path, std::ios::binary);
    char buf[65536];
    while (f) {
        f.read(buf, sizeof(buf));
        std::streamsize n = f.gcount();
        if (n > 0) fnv1aUpdate(h, buf, (size_t)n);
    }
}

static void hashDirectoryTreeInto(uint64_t& h, const fs::path& root) {
    std::error_code existsEc;
    if (!fs::exists(root, existsEc)) return;

    std::vector<fs::path> entries;
    std::error_code walkEc;
    for (const auto& e :
         fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, walkEc)) {
        entries.push_back(e.path());
    }
    std::sort(entries.begin(), entries.end());

    for (const auto& p : entries) {
        fnv1aUpdateStr(h, fs::relative(p, root).string());

        std::error_code typeEc;
        if (fs::is_symlink(p, typeEc)) {
            fnv1aUpdateStr(h, "SYMLINK");
            std::error_code linkEc;
            fnv1aUpdateStr(h, fs::read_symlink(p, linkEc).string());
        } else if (fs::is_regular_file(p, typeEc) && !typeEc) {
            fnv1aUpdateStr(h, "FILE");
            std::ifstream f(p, std::ios::binary);
            char buf[65536];
            while (f) {
                f.read(buf, sizeof(buf));
                std::streamsize n = f.gcount();
                if (n > 0) fnv1aUpdate(h, buf, (size_t)n);
            }
        } else {
            fnv1aUpdateStr(h, "DIR");
        }
    }
}

std::string ramdiskOverlayContentHash() {
    uint64_t h = 0xcbf29ce484222325ULL;
    hashDirectoryTreeInto(h, resolveRamdiskPath());
    hashDirectoryTreeInto(h, resolveDebsPath());
    // Misc/ (rc.boot content spliced into /etc/rc.boot) and entrypoint/'s
    // own source (rebuilt fresh via podman into /sbin/launchd on every
    // bake — see buildEntrypointBinary() in BakeRamdisk.cpp) both now
    // affect the baked output just as much as ramdisk/ and Debs/ do.
    // Hashing entrypoint/'s whole directory would also pick up its own
    // obj/ build output and churn the hash on every bake for no reason;
    // hash only the inputs that actually change what gets compiled.
    hashDirectoryTreeInto(h, resolveMiscPath(""));
    hashFileInto(h, resolveEntrypointPath() + "/entrypoint.c");
    hashFileInto(h, resolveEntrypointPath() + "/Makefile");

    // bakeRamdisk() (BakeRamdisk.cpp) doesn't just splice ramdisk/+Debs/
    // bytes in unmodified — it decides WHICH Debs/ .debs get staged at all
    // (kNeverStageDebs), how big is too big (kMaxRamdiskSize), and every
    // other detail of what actually ends up in the finished image. A real
    // run hit this directly: changing kNeverStageDebs to exclude odcctools
    // changed the baked output with zero change to ramdisk/, Debs/, or
    // Misc/'s own file content, so the hash above didn't move and
    // bake-all-ramdisks kept reporting "already baked, overlay unchanged,
    // skipping" against the stale, oversized dist/ entry. Hashing the
    // baker's own source closes that gap the same way entrypoint.c/
    // Makefile above already do for buildEntrypointBinary().
    hashFileInto(h, "Blackb0x/Source/BakeRamdisk.cpp");

    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}

std::string sumFileFor(const std::string& outputPath) { return outputPath + ".sum"; }
