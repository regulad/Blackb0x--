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

// Shared by resolvePwnPath()/resolveBakeAllRamdisksPath()/
// resolveIBoot32PatcherPath() below: the directory blackb0x's own executable
// lives in, or empty if it can't be determined (falls back to a bare
// binaryName, resolved via PATH at exec time).
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

std::string resolvePackagePath(const std::string& relativePath) {
    if (const char* override_ = getenv("BLACKB0X_PACKAGE_DIR")) {
        return std::string(override_) + "/" + relativePath;
    }
    return "package/layout/" + relativePath;
}

std::string resolvePackageRoot() {
    if (const char* override_ = getenv("BLACKB0X_PACKAGE_ROOT")) {
        return std::string(override_);
    }
    return "package";
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
