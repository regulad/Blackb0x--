//
//  RamdiskOverlayTests.cpp
//  Blackb0x
//
//  patchRamdisk() itself (Patcher.cpp) needs a real loop mount (CAP_SYS_ADMIN)
//  against a real HFS+ image, so it isn't something this suite can exercise
//  directly. What IS testable without root, and where regressions in this
//  area have actually happened before (the dead top-level `dirhelper`
//  duplicate found during the Files/ -> ramdisk/ split), is: (1) path
//  resolution honoring its override env vars, and (2) the checked-in
//  ramdisk/ overlay tree actually containing what patchRamdisk() expects
//  and nothing it doesn't.
//

#include "../ResourcePath.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;

static void expect(bool condition, const std::string& description) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description.c_str());
        failures++;
    }
}

static void testResolveRamdiskPathDefault() {
    unsetenv("BLACKB0X_RAMDISK_DIR");
    expect(resolveRamdiskPath() == "Blackb0x/ramdisk", "resolveRamdiskPath() defaults to Blackb0x/ramdisk");
}

static void testResolveRamdiskPathOverride() {
    setenv("BLACKB0X_RAMDISK_DIR", "/tmp/some-override", 1);
    expect(resolveRamdiskPath() == "/tmp/some-override", "resolveRamdiskPath honors BLACKB0X_RAMDISK_DIR");
    unsetenv("BLACKB0X_RAMDISK_DIR");
}

static void testResolveImageKeyPathDefault() {
    unsetenv("BLACKB0X_IMAGEKEYS_DIR");
    expect(resolveImageKeyPath("AppleTV2,1/AppleTV2,1_10A406e.keys") ==
               "Blackb0x/ImageKeys/AppleTV2,1/AppleTV2,1_10A406e.keys",
           "resolveImageKeyPath defaults under Blackb0x/ImageKeys/");
}

static void testResolveImageKeyPathOverride() {
    setenv("BLACKB0X_IMAGEKEYS_DIR", "/tmp/some-keys-override", 1);
    expect(resolveImageKeyPath("x.keys") == "/tmp/some-keys-override/x.keys",
           "resolveImageKeyPath honors BLACKB0X_IMAGEKEYS_DIR");
    unsetenv("BLACKB0X_IMAGEKEYS_DIR");
}

static void testResolveDebsPathDefault() {
    unsetenv("BLACKB0X_DEBS_DIR");
    expect(resolveDebsPath() == "Blackb0x/Debs", "resolveDebsPath() defaults to Blackb0x/Debs");
}

static void testResolveDebsPathOverride() {
    setenv("BLACKB0X_DEBS_DIR", "/tmp/some-debs-override", 1);
    expect(resolveDebsPath() == "/tmp/some-debs-override", "resolveDebsPath honors BLACKB0X_DEBS_DIR");
    unsetenv("BLACKB0X_DEBS_DIR");
}

static void testDecryptedDMGFor() {
    expect(decryptedDMGFor("/tmp/RestoreRamdisk.dmg") == "/tmp/RestoreRamdisk-decrypted.dmg",
           "decryptedDMGFor produces the expected suffix");
}

// Blackb0x/ramdisk/ was deleted outright as the first step of the NEO_FLOW
// rewrite (see .claude/LEGACY_FLOW.md and .claude/NEO_FLOW.md) — the flat,
// pre-extracted Cydia tree this test used to check for doesn't exist
// anymore by design, and its replacement (debcache + sources list + dpkg +
// setup.sh + the persistence payload, all driven by a real dpkg/apt
// install) hasn't been built yet. Once entrypoint.c and setup.sh are
// rewritten to match NEO_FLOW.md, this should assert against whatever
// Blackb0x/ramdisk/ ends up containing then — asserting against the old,
// now-nonexistent file list in the meantime would just be checking that a
// deliberate deletion didn't happen.
static void testOverlayHasRequiredFiles() {
    unsetenv("BLACKB0X_RAMDISK_DIR");
    expect(!fs::exists(resolveRamdiskPath()),
           "Blackb0x/ramdisk/ should not exist yet — NEO_FLOW's replacement overlay hasn't been built");
}

// setup.sh's hardcoded `mv`/`dpkg -i` targets — if a deb ever gets renamed
// without updating setup.sh (or vice versa), this catches the drift
// immediately instead of failing silently on-device. Debs live in their
// own Blackb0x/Debs/ root (see ResourcePath::resolveDebsPath()), not inside
// the ramdisk/ overlay tree — bakeRamdisk() merges both into /files/.
static void testDebsHasRequiredFiles() {
    unsetenv("BLACKB0X_DEBS_DIR");
    fs::path root = resolveDebsPath();
    const std::vector<std::string> required = {
        "rtadvd_307.0.1-3_iphoneos-arm.deb",
        "sqlite3-dylib_3.5.9-2_iphoneos-arm.deb",
        "sqlite3-lib_3.5.9-3_iphoneos-arm.deb",
        "com.saurik.patcyh_1.2.0-1_iphoneos-arm.deb",
        "ldid_1.2.1_iphoneos-arm.deb",
        "uikittools_1.1.12-1_iphoneos-arm.deb",
        "beigelist_2.2.6-30_iphoneos-arm.deb",
        "com.nito.updatebegone_0.2-1_iphoneos-arm.deb",
    };
    for (const auto& rel : required) {
        expect(fs::exists(root / rel), "Blackb0x/Debs is missing " + rel);
    }
}

// Regression guard: the ramdisk-stage sshd (and its host key/authorized_keys
// baking) was deliberately dropped — SSH access is now granted post-boot,
// over Cydia's own openssh package, by running scripts/push_authorized_keys.sh
// by hand once the jailbreak is confirmed running. None of it should quietly
// reappear here.
static void testOverlayHasNoSshdRemnants() {
    unsetenv("BLACKB0X_RAMDISK_DIR");
    fs::path root = resolveRamdiskPath();
    const std::vector<std::string> mustNotExist = {
        "sbin/sshd",
        "bin/bash",
        "bin/sh",
        "private/etc/ssh",
        "private/var/root",
        "usr/lib/libcrypto.0.9.8.dylib",
        "usr/libexec/sftp-server",
    };
    for (const auto& rel : mustNotExist) {
        expect(!fs::exists(root / rel), "ramdisk overlay has a ramdisk-stage sshd remnant: " + rel);
    }
}

// Regression guard for the dead standalone `dirhelper` found and removed
// during the Files/ -> ramdisk/ split: the only copy that should exist is
// the one actually referenced (usr/bin/dirhelper, extracted from the old
// RamdiskBins.tar) — never a loose top-level duplicate again. The copy that
// used to live inside the p0sixspwn payload was itself claimed by that
// package's dpkg .list and was removed along with every other deb-owned
// file (see testOverlayHasNoDebOwnedFiles).
static void testNoDeadDirhelperDuplicate() {
    unsetenv("BLACKB0X_RAMDISK_DIR");
    fs::path root = resolveRamdiskPath();
    expect(!fs::exists(root / "files" / "dirhelper"), "a dead top-level dirhelper duplicate has reappeared under files/");
    expect(!fs::exists(root / "dirhelper"), "a dead top-level dirhelper duplicate has reappeared at the ramdisk root");
}

// Regression guard: files/cydia and files/p0sixspwn used to be an unrolled
// snapshot of every file a dpkg .list claimed, including dpkg's own state
// directory. All of that is now supplied at bake time from the real .deb
// packages in Blackb0x/Debs/ (see bakeRamdisk()), so none of it belongs in
// the checked-in overlay tree.
static void testOverlayHasNoDebOwnedFiles() {
    unsetenv("BLACKB0X_RAMDISK_DIR");
    fs::path root = resolveRamdiskPath();
    const std::vector<std::string> mustNotExist = {
        "files/cydia/private/var/lib/dpkg",
        "files/cydia/bin/bash",
        "files/p0sixspwn/private/var/lib/dpkg",
        "files/p0sixspwn/usr/libexec/dirhelper",
    };
    for (const auto& rel : mustNotExist) {
        expect(!fs::exists(root / rel), "ramdisk overlay has a deb-owned file that should come from Blackb0x/Debs/ instead: " + rel);
    }
}

// Guard against macOS packaging cruft (AppleDouble ._* siblings, .DS_Store,
// the stale mktar.sh authoring script) creeping back into the overlay — all
// three were stripped out of the original .tar/.tgz sources when they were
// flattened into loose files.
static void testNoPackagingJunk() {
    unsetenv("BLACKB0X_RAMDISK_DIR");
    fs::path root = resolveRamdiskPath();
    if (!fs::exists(root)) return;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        expect(name.rfind("._", 0) != 0, "ramdisk overlay contains AppleDouble junk: " + entry.path().string());
        expect(name != ".DS_Store", "ramdisk overlay contains .DS_Store: " + entry.path().string());
        expect(name != "mktar.sh", "ramdisk overlay contains the stale ssh.tar authoring script");
    }
}

int main() {
    testResolveRamdiskPathDefault();
    testResolveRamdiskPathOverride();
    testResolveImageKeyPathDefault();
    testResolveImageKeyPathOverride();
    testResolveDebsPathDefault();
    testResolveDebsPathOverride();
    testDecryptedDMGFor();
    testOverlayHasRequiredFiles();
    testDebsHasRequiredFiles();
    testOverlayHasNoSshdRemnants();
    testNoDeadDirhelperDuplicate();
    testOverlayHasNoDebOwnedFiles();
    testNoPackagingJunk();

    if (failures == 0) {
        printf("All RamdiskOverlayTests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d RamdiskOverlayTests failure(s).\n", failures);
    return 1;
}
