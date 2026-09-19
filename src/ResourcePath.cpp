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

#include <mach-o/dyld.h>
#include <unistd.h>

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
    return "keys/" + relativePath;
}

// Shared by resolvePwnPath()/resolveBakeFirmwarePath()/
// resolveIBoot32PatcherPath() below: the directory blackb0x's own executable
// lives in, or empty if it can't be determined (falls back to a bare
// binaryName, resolved via PATH at exec time).
static std::string resolveOwnExecutableDir() {
    char exePath[PATH_MAX];
    // _NSGetExecutablePath() may hand back a path containing symlinks,
    // hence the realpath() pass.
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

std::string resolveBakeFirmwarePath() {
    if (const char* override_ = getenv("BLACKB0X_BAKE_FIRMWARE")) {
        return std::string(override_);
    }
    std::string dir = resolveOwnExecutableDir();
    if (!dir.empty()) return dir + "/bake-firmware";
    return "bake-firmware";
}

std::string resolveIBoot32PatcherPath() {
    if (const char* override_ = getenv("BLACKB0X_IBOOT32PATCHER")) {
        return std::string(override_);
    }
    std::string dir = resolveOwnExecutableDir();
    if (!dir.empty()) return dir + "/iBoot32Patcher";
    return "iBoot32Patcher";
}

std::string resolveCBPatcherPath() {
    if (const char* override_ = getenv("BLACKB0X_CBPATCHER")) {
        return std::string(override_);
    }
    std::string dir = resolveOwnExecutableDir();
    if (!dir.empty()) return dir + "/CBPatcher";
    return "CBPatcher";
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

void chownToSudoCaller(const std::string& path) {
    const char* uidStr = getenv("SUDO_UID");
    const char* gidStr = getenv("SUDO_GID");
    if (!uidStr || !gidStr || !*uidStr || !*gidStr) return;
    char* uidEnd = nullptr;
    char* gidEnd = nullptr;
    unsigned long uid = strtoul(uidStr, &uidEnd, 10);
    unsigned long gid = strtoul(gidStr, &gidEnd, 10);
    if (uidEnd == uidStr || *uidEnd != '\0' || gidEnd == gidStr || *gidEnd != '\0') return;
    // Return value ignored deliberately -- see the header's own comment.
    (void)chown(path.c_str(), (uid_t)uid, (gid_t)gid);
}

std::string resolveAptToolsDir() {
    if (const char* override_ = getenv("BLACKB0X_APT_TOOLS_DIR")) {
        return std::string(override_);
    }
    std::string dir = resolveOwnExecutableDir();
    if (!dir.empty()) return dir + "/apt-tools";
    return "apt-tools";
}

std::string resolveDebcachePath() {
    if (const char* override_ = getenv("BLACKB0X_DEBCACHE_DIR")) {
        return std::string(override_);
    }
    return "debcache";
}

std::string resolveMiscPath(const std::string& relativePath) {
    if (const char* override_ = getenv("BLACKB0X_MISC_DIR")) {
        return std::string(override_) + "/" + relativePath;
    }
    return "misc/" + relativePath;
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
