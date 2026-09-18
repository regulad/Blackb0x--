//
//  BakeRamdisk.cpp
//  Blackb0x
//
//  Implements bakeRamdisk() (see BakeRamdisk.hpp) — the one piece of this
//  tool that needs CAP_SYS_ADMIN/CAP_CHOWN, since it loop-mounts a real
//  HFS+ image. Deliberately not invoked by blackb0x itself. Touches exactly
//  two things on the pristine ramdisk: overwrites `/sbin/launchd`'s content
//  in place with the built entrypoint binary (spliceFileContentInPlace()) —
//  see docs/HISTORY.md's "Entrypoint injection point" entry for why this
//  isn't `/etc/rc.boot` (tried, and reverted: real on at least one firmware
//  generation, that file doesn't exist at all) —
//  and adds one brand new top-level directory, `/blackb0x` (stageBlackb0xTree()),
//  a flat mirror of the real device's final layout with every file/dir/
//  symlink already carrying its correct final owner/mode — entrypoint.c's
//  own merge_tree() just blindly replicates whatever's under there onto
//  the real device at boot, no branching left on-device at all. Which of
//  the three known per-firmware persistence payloads goes into /blackb0x
//  is decided here too, from this firmware's own ProductVersion — see
//  stageVersionBranch(). Fully static either way (no per-device secrets
//  get baked in — host key generation and authorized_keys delivery both
//  happen elsewhere), so the same patched output is valid for every device
//  on a given firmware build. bake-all-ramdisks (BakeAllRamdisks.cpp) calls
//  this once per known firmware and writes the results to dist/;
//  Patcher::patchRamdisk() just checks whether the expected dist/ output
//  already exists and tells the user to (re-)run bake-all-ramdisks if not.
//

#include "BakeRamdisk.hpp"
#include "ResourcePath.hpp"

// Already a linked dependency of this target either way (see
// CMakeLists.txt's bake-all-ramdisks target — same deps::plist IPSW.cpp
// already uses for its own plist parsing, same plist_* C API/calling
// convention followed here). Not wrapped in `extern "C" {}` below, matching
// IPSW.cpp's own include of the same header.
#include <plist/plist.h>

extern "C" {
#include <xpwntool.h>
}

extern "C" {
#include <abstractfile.h>
// third_party/xpwn's vendored hfs/hfsplus.h (pulled in transitively here,
// via dmg/dmg.h) uses the `register` storage-class specifier in one
// function prototype (FastUnicodeCompare) — legal C++14, removed from the
// C++17 grammar entirely. GCC only warns; Clang (macOS's default) hard-
// errors on it, which is why bake-all-ramdisks was Linux-only until this
// port (see CMakeLists.txt's own comment on its `if(NOT APPLE)` guard).
// Bracketing just this #include (rather than editing the vendored header
// itself) with an empty #define is harmless on GCC too, so this isn't
// __APPLE__-gated — a per-file CXX_STANDARD downgrade isn't an option
// instead, since this file uses std::filesystem throughout, a C++17
// *library* feature not reliably available under a downgraded language
// standard.
#define register
#include <dmg/dmglib.h>
#undef register
#include <hfs/hfslib.h>
#include <hfs/hfsplus.h>
#include <xpwn/libxpwn.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// HFS+ volume helpers (replace hdiutil attach/detach/resize and tar -C) —
// see the long comment on bakeRamdisk() below for why this went through two
// other designs (an in-memory xpwn Volume, then libhfsp) before landing on
// "loop-mount via the real Linux kernel driver, then use ordinary POSIX
// tools" as the one that actually survives real firmware data.
// ---------------------------------------------------------------------------

// Runs a command to completion and returns whether it exited 0. Uses
// fork()/execvp() (argv array, no shell) rather than system()/popen() so
// paths never pass through shell interpretation. `cwd`, when non-empty, is
// chdir()'d into in the child first — needed for `ar x`, which always
// extracts into the current directory with no destination-directory flag
// portable across ar implementations.
static bool runCommand(const std::vector<std::string>& argv, const std::string& cwd = "") {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Same argv/no-shell contract as runCommand(), but captures stdout instead
// of just a pass/fail exit code — needed for `du -sb`/`blkid` below, which
// this program needs the actual text output of, not just success/failure.
static std::string runCommandCapture(const std::vector<std::string>& argv) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    close(pipefd[1]);
    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) output.append(buf, (size_t)n);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) return "";
    return output;
}

// Real content size, measured as a portable std::filesystem walk summing
// fs::file_size() over every regular file. Used to replace an earlier
// `du -s --block-size=1` shellout, which was deliberately chosen over a
// plain byte-sum specifically to capture the *destination* filesystem's own
// allocation-block rounding (thousands of small files here — terminfo
// entries, dpkg info files, etc. — each consume a full allocation block on
// the destination HFS+ volume regardless of actual byte length; a real bake
// run with `-sb`/apparent-size instead ran out of space almost immediately
// despite a nominally oversized volume, confirming the undercount was real,
// not theoretical). `--block-size` is GNU-coreutils-only, though, with no
// macOS/BSD equivalent — it blocked this function from working on macOS at
// all. A plain byte-sum still undercounts real on-disk usage by roughly
// that same rounding gap, but bakeRamdisk()'s own newVolumeSize margin
// (10% of content, 4MB minimum, whichever is larger — see its own comment)
// already exists specifically to absorb slop between a size measurement and
// real device usage, and was proven, empirically, against a materially
// larger effect (extent fragmentation near a full volume) than block-
// rounding alone. Removing the `du` dependency entirely is worth that
// trade: this measurement now also works identically against whatever
// plain host directory the macOS build sequence measures (there's no live
// HFS+ mount to `du` against there at all — see bakeRamdisk()'s
// __APPLE__ branch).
static uint64_t directoryContentSize(const std::string& dir) {
    uint64_t total = 0;
    std::error_code walkEc;
    for (const auto& entry :
         fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, walkEc)) {
        if (walkEc) break;
        std::error_code typeEc;
        if (!fs::is_regular_file(entry.path(), typeEc) || typeEc) continue;
        std::error_code sizeEc;
        uint64_t sz = (uint64_t)fs::file_size(entry.path(), sizeEc);
        if (!sizeEc) total += sz;
    }
    return total;
}

// Reads the original ramdisk's volume label, so the freshly-mkfs'd
// replacement can be given the same name. mkfs.hfsplus can only set this
// at creation time — the volume name lives in the catalog (the root
// folder's own record), not in the fixed-size volume header, so there's
// no header-byte-copy shortcut for it the way there is for the fields in
// copyVolumeHeaderMetadata() below.
static std::string readVolumeLabel(const std::string& imagePath) {
    std::string label = runCommandCapture({"blkid", "-o", "value", "-s", "LABEL", imagePath});
    while (!label.empty() && (label.back() == '\n' || label.back() == '\r')) label.pop_back();
    return label.empty() ? "ramdisk" : label;
}

#if defined(__APPLE__)
// macOS equivalent of readVolumeLabel() above — there's no blkid on
// Darwin, and unlike blkid's read of the raw image file directly,
// `diskutil info` only works against an attached (mounted) device, not a
// bare image file, so bakeRamdisk()'s __APPLE__ branch calls this AFTER
// `hdiutil attach` rather than before mounting, the way the Linux path
// does. Parsed via libplist's C API — same plist_* calling convention
// IPSW.cpp's own plistDictString() already established project-wide.
static std::string readVolumeLabelMac(const std::string& mountpoint) {
    std::string plistXml = runCommandCapture({"diskutil", "info", "-plist", mountpoint});
    if (plistXml.empty()) return "ramdisk";

    plist_t root = nullptr;
    plist_from_xml(plistXml.c_str(), (uint32_t)plistXml.size(), &root);
    if (!root) return "ramdisk";

    std::string label;
    plist_t node = plist_dict_get_item(root, "VolumeName");
    if (node) {
        char* val = nullptr;
        plist_get_string_val(node, &val);
        if (val) {
            label = val;
            free(val);
        }
    }
    plist_free(root);
    return label.empty() ? "ramdisk" : label;
}
#endif

// Copies a deliberately narrow set of "identity" fields from the original
// volume's fixed-size HFS+ header into the freshly-mkfs'd replacement:
//   - finderInfo (32 bytes): encodes blessed-folder/boot-related info. This
//     is a bootable restore ramdisk, so this can plausibly affect boot
//     behavior — cheap to preserve, and risky to guess is safe to drop.
//   - createDate: the original firmware build's genuine volume-creation
//     timestamp — meaningful provenance, not something mkfs.hfsplus can
//     know to set correctly on its own.
//   - lastMountedVersion: a 4-byte tag identifying what tool last wrote the
//     volume — likewise provenance worth carrying over.
// Deliberately NOT copied:
//   - `attributes` (a state/journaling bitfield): copying it wholesale
//     risks importing a stale "unmounted"/journaled bit that doesn't match
//     the freshly-mkfs'd volume's actual, correct state.
//   - fileCount/folderCount/blockSize/totalBlocks/free space/clump sizes:
//     all content- or geometry-derived. mkfs.hfsplus already computed
//     these correctly for the new volume's real size; copying the old
//     volume's values would be actively wrong.
//
// This patches BOTH the primary header (fixed offset 1024, per the HFS+
// spec) and the backup/alternate header (the second-to-last 512-byte
// sector) so fsck.hfsplus doesn't flag a primary/alternate mismatch for the
// fields touched here. This is a plain, fixed-offset byte copy touching
// only the 512-byte header itself — no B-tree or catalog interaction at
// all, unlike the catalog-growth code that broke both xpwn and libhfsp
// (see the design-history comment on bakeRamdisk() below).
static void copyVolumeHeaderMetadata(const std::string& origPath, const std::string& newPath) {
    char origHeader[512] = {0};
    {
        std::ifstream in(origPath, std::ios::binary);
        if (!in) return;
        in.seekg(1024);
        in.read(origHeader, sizeof(origHeader));
        if (!in) return;
    }

    std::error_code ec;
    auto newSize = fs::file_size(newPath, ec);
    if (ec) return;

    struct FieldCopy {
        size_t offset;
        size_t size;
    };
    static const FieldCopy fields[] = {
        {offsetof(HFSPlusVolumeHeader, createDate), sizeof(uint32_t)},
        {offsetof(HFSPlusVolumeHeader, lastMountedVersion), sizeof(uint32_t)},
        {offsetof(HFSPlusVolumeHeader, finderInfo), sizeof(uint32_t) * 8},
    };

    std::fstream out(newPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) return;
    for (const auto& f : fields) {
        out.seekp((std::streamoff)1024 + (std::streamoff)f.offset);
        out.write(origHeader + f.offset, (std::streamsize)f.size);
        out.seekp((std::streamoff)newSize - 1024 + (std::streamoff)f.offset);
        out.write(origHeader + f.offset, (std::streamsize)f.size);
    }
}

#if defined(__APPLE__)
// Detects a UDIF ("koly" trailer) wrapper on `path` and, if present,
// rewrites `path` in place with the unwrapped raw partition bytes — the
// same probe-then-extractDmg() technique bakeRamdisk() already runs on the
// original decrypted firmware component near the top of this file,
// factored out so the macOS build sequence can apply it defensively to
// `hdiutil create`'s own output too. Real `hdiutil create -format UDRW
// -layout NONE`'s exact output byte layout could not be verified against
// actual hdiutil on this (Linux) build host — see bakeRamdisk()'s
// __APPLE__ branch for the full caveat. This is a no-op if that output
// turns out to already be flat raw bytes (no koly trailer found), and
// correctly unwraps it if it isn't, so it's safe to run unconditionally
// either way — copyVolumeHeaderMetadata() right after it, and the shared
// UDIF-rewrap step at the end of bakeRamdisk(), both need genuinely flat
// raw bytes to work correctly.
static bool unwrapUDIFIfPresent(const std::string& path) {
    bool wrapped = false;
    {
        std::ifstream probe(path, std::ios::binary);
        if (probe) {
            probe.seekg(0, std::ios::end);
            std::streamoff size = probe.tellg();
            if (size >= 512) {
                probe.seekg(size - 512);
                char magic[4] = {0};
                probe.read(magic, 4);
                wrapped = (memcmp(magic, "koly", 4) == 0);
            }
        }
    }
    if (!wrapped) return true;

    FILE* wrappedFile = fopen(path.c_str(), "rb");
    if (!wrappedFile) return false;
    void* rawBuffer = nullptr;
    size_t rawSize = 0;
    AbstractFile* wrappedAbs = createAbstractFileFromFile(wrappedFile);
    AbstractFile* rawOut = createAbstractFileFromMemoryFile(&rawBuffer, &rawSize);
    extractDmg(wrappedAbs, rawOut, -1);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        free(rawBuffer);
        return false;
    }
    out.write((const char*)rawBuffer, (std::streamsize)rawSize);
    free(rawBuffer);
    return true;
}
#endif

static std::string makeTempDir(const std::string& prefix) {
    std::string tmpl = (fs::temp_directory_path() / (prefix + "XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) return "";
    return std::string(buf.data());
}

// RAII guard so a failed bakeRamdisk() run never leaks a stale mount or temp
// mountpoint directory — `mounted` is only set true once the mount actually
// succeeds, and unmounting/removal here is best-effort (there's nothing
// more useful to do with a failure during cleanup after an error).
struct MountGuard {
    std::string mountpoint;
    bool mounted = false;
    ~MountGuard() {
        if (mounted) {
#if defined(__APPLE__)
            // hdiutil, not umount — this mountpoint was attached via
            // `hdiutil attach` (see bakeRamdisk()'s __APPLE__ branch), and
            // `hdiutil detach` is its own matching unmount command.
            runCommand({"hdiutil", "detach", mountpoint});
#else
            runCommand({"umount", mountpoint});
#endif
        }
        if (!mountpoint.empty()) {
            std::error_code ec;
            fs::remove(mountpoint, ec);
        }
    }
};

// Runs `argv`, transparently re-invoked as the real (non-root) invoking
// user via `runuser` when bakeRamdisk() itself is running under sudo. This
// needs to happen: bakeRamdisk() runs privileged (CAP_SYS_ADMIN, for the
// HFS+ mount below), but podman's own rootless image/volume storage —
// `blackb0x-entrypoint-toolchain`, `blackb0x-cctools-target`, set up once
// per entrypoint/README.md's own setup steps, and whatever
// scripts/build_deb_cache.py's own podman calls need underneath it — all
// belong to the real user, not root's own (separate) rootless podman
// storage.
static bool runAsInvokingUser(const std::vector<std::string>& argv) {
    std::vector<std::string> cmd;
    const char* sudoUser = getenv("SUDO_USER");
    if (sudoUser && *sudoUser) {
        cmd = {"runuser", "-u", sudoUser, "--"};
        // runuser does NOT reset XDG_RUNTIME_DIR — under `sudo`, that
        // variable is still whatever the original (root) shell had it as
        // (commonly /run/user/0), not the target user's own real runtime
        // directory, even though the command genuinely now runs as that
        // user. Confirmed directly against a real bake-all-ramdisks run:
        // without this, podman failed with "mkdir /run/user/0/libpod:
        // permission denied" — trying to use root's runtime dir while
        // running as uid 1000. `env VAR=value` here is a real, separate
        // argv entry (no shell involved), same "no shell interpretation"
        // guarantee as every other runCommand() call in this file.
        const char* sudoUid = getenv("SUDO_UID");
        if (sudoUid && *sudoUid) {
            cmd.push_back("env");
            cmd.push_back(std::string("XDG_RUNTIME_DIR=/run/user/") + sudoUid);
        }
        cmd.insert(cmd.end(), argv.begin(), argv.end());
    } else {
        cmd = argv;
    }
    return runCommand(cmd);
}

// bakeRamdisk() itself runs as real root (sudo), so any temp dir it
// creates via makeTempDir() comes out root-owned (mkdtemp() defaults to
// 0700, owner-only) — but anything invoked through runAsInvokingUser()
// runs as the non-root invoking user instead, and needs real write access
// if it's expected to write its own output files into that same
// directory. Confirmed directly against a real bake run: without this,
// scripts/build_deb_cache.py crashed with a plain PermissionError trying
// to write picklist.txt into a root-owned temp dir. Only the top-level
// directory needs chowning — files that user creates inside it afterward
// are already owned by them.
static void chownToInvokingUserIfSudo(const std::string& path) {
    const char* sudoUid = getenv("SUDO_UID");
    const char* sudoGid = getenv("SUDO_GID");
    if (sudoUid && *sudoUid && sudoGid && *sudoGid) {
        chown(path.c_str(), (uid_t)atoi(sudoUid), (gid_t)atoi(sudoGid));
    }
}

// Always the absolute /usr/bin/podman — a broken/shadowed `podman` earlier
// on some PATH is a real failure mode this project has already hit once
// (see entrypoint/README.md).
static bool runPodman(const std::vector<std::string>& podmanArgs) {
    std::vector<std::string> cmd = {"/usr/bin/podman"};
    cmd.insert(cmd.end(), podmanArgs.begin(), podmanArgs.end());
    return runAsInvokingUser(cmd);
}

// Builds entrypoint/'s freestanding ARMv6 replacement for /sbin/launchd,
// rather than shipping a precompiled binary — see entrypoint/README.md for
// why (needs cctools-port's real Apple ld64 port; a normal host toolchain
// can't produce this). Assumes the one-time toolchain setup documented
// there has already been done (the blackb0x-entrypoint-toolchain image
// built, blackb0x-cctools-target volume populated via cctools-port's
// SDK-gated build.sh) — this only runs the actual `make`, it doesn't
// bootstrap the whole cross-toolchain from scratch, since that needs an SDK
// that is deliberately not checked into this repo at all (Apple's
// copyrighted material — see entrypoint/assets/README.md).
//
// The binary is identical for every firmware target (no per-firmware
// customization at all), so BakeAllRamdisks.cpp's main() calls this exactly
// once, before its per-firmware loop, and hands the resulting path to every
// bakeRamdisk() call.
// Not this function's own job to guard against being called more than
// once; it just builds, every time it's asked to.
std::string buildEntrypointBinary() {
    std::string entrypointDir = fs::absolute(resolveEntrypointPath()).string();
    std::string outputPath = entrypointDir + "/entrypoint";

    // Remove any stale output first so a failed build can never be
    // mistaken for a fresh one below.
    std::error_code rmEc;
    fs::remove(outputPath, rmEc);

    bool ok = runPodman({
        "run", "--rm", "--security-opt", "label=disable",
        "-v", entrypointDir + ":/work",
        "-v", "blackb0x-cctools-target:/opt/cctools-port/usage_examples/ios_toolchain:ro",
        "-e", "PATH=/opt/cctools-port/usage_examples/ios_toolchain/target/bin:/usr/bin:/bin",
        "-w", "/work",
        "blackb0x-entrypoint-toolchain",
        "make", "clean", "all",
    });
    if (!ok) {
        fprintf(stderr,
                "bakeRamdisk: failed to build entrypoint/ via podman — if this is the first run, see "
                "entrypoint/README.md's one-time toolchain setup steps\n");
        return "";
    }
    if (!fs::exists(outputPath)) {
        fprintf(stderr, "bakeRamdisk: entrypoint/ build reported success but %s is missing\n", outputPath.c_str());
        return "";
    }
    return outputPath;
}

// Overwrites `targetPath`'s content with `newContentPath`'s bytes while
// preserving the target's existing mode/owner/group/mtime — for
// `/sbin/launchd`, which already exists for real on every known firmware's
// pristine ramdisk (unlike `/etc/rc.boot` — see docs/HISTORY.md's
// "Entrypoint injection point" entry for the real firmware where that
// assumption broke), where a blind overwrite-and-recreate would silently
// replace Apple's own permission bits with whatever this repo's checked-in
// replacement file happens to carry. Refuses to run if `targetPath` doesn't
// already exist — that would mean an assumption about the pristine
// ramdisk's layout is wrong, not something to paper over by creating the
// file fresh with guessed permissions.
static bool spliceFileContentInPlace(const std::string& targetPath, const std::string& newContentPath) {
    struct stat st;
    if (stat(targetPath.c_str(), &st) != 0) {
        fprintf(stderr,
                "bakeRamdisk: %s does not exist on the mounted volume — refusing to create it fresh with "
                "guessed permissions\n",
                targetPath.c_str());
        return false;
    }

    std::ifstream in(newContentPath, std::ios::binary);
    if (!in) {
        fprintf(stderr, "bakeRamdisk: cannot open %s\n", newContentPath.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    int fd = open(targetPath.c_str(), O_WRONLY | O_TRUNC);
    if (fd < 0) {
        fprintf(stderr, "bakeRamdisk: cannot open %s for writing\n", targetPath.c_str());
        return false;
    }
    ssize_t written = write(fd, content.data(), content.size());
    close(fd);
    if (written < 0 || (size_t)written != content.size()) {
        fprintf(stderr, "bakeRamdisk: short write to %s\n", targetPath.c_str());
        return false;
    }

    chmod(targetPath.c_str(), st.st_mode & 07777);
    chown(targetPath.c_str(), st.st_uid, st.st_gid);
#if defined(__APPLE__)
    // Darwin's struct stat spells these fields st_atimespec/st_mtimespec
    // instead of Linux/glibc's st_atim/st_mtim — same fields, different
    // names (see ResourcePath.cpp's own #if defined(__APPLE__) branches
    // for this project's established house style).
    struct timespec times[2] = {st.st_atimespec, st.st_mtimespec};
#else
    struct timespec times[2] = {st.st_atim, st.st_mtim};
#endif
    utimensat(AT_FDCWD, targetPath.c_str(), times, 0);
    return true;
}

// ---------------------------------------------------------------------------
// /blackb0x staging — NEO_FLOW's replacement for the old per-file
// install_file() call list inside entrypoint.c. entrypoint.c's own
// merge_tree() is now a completely blind, unconditional recursive copy: it
// has no idea what firmware it's running on or what any of these files are
// for, it just replicates whatever's staged under /blackb0x onto /mnt1
// using each entry's own real owner/mode (read back via stat() at
// runtime). Which files exist under /blackb0x, and with what final
// owner/mode, is entirely decided here, at bake time — including which one
// of the three known per-firmware persistence payloads applies, since this
// firmware's own ProductVersion is already known here and was previously
// only discovered by entrypoint.c on-device, at the one moment it's
// actually too late to ship a different ramdisk.
// ---------------------------------------------------------------------------

// 501:20, "mobile:staff" — the standard iOS convention, matching
// entrypoint.c's own UID_MOBILE/GID_STAFF.
static constexpr uid_t kUidMobile = 501;
static constexpr gid_t kGidStaff = 20;

// Creates every path component between `root` and `fullPath`'s parent that
// doesn't already exist, as root:wheel 0755 — the same convention every
// other top-level pristine-ramdisk directory already uses (confirmed
// against a real decrypted AppleTV2,1 RestoreRamdisk: /bin, /private,
// /sbin, /usr are all uid 0 gid 0). Never touches a directory that already
// exists, in either direction: a directory this creates fresh always gets
// this default metadata, but a directory some other, explicit stageDir()
// call already gave different metadata to (see below) is left alone
// regardless of which one runs first — order-independent by construction.
static void ensureParentDirs(const fs::path& root, const fs::path& fullPath) {
    std::error_code relEc;
    fs::path rel = fs::relative(fullPath.parent_path(), root, relEc);
    if (relEc) return;
    fs::path cur = root;
    for (const auto& part : rel) {
        if (part == ".") continue;
        cur /= part;
        std::error_code ec;
        if (fs::create_directory(cur, ec)) {
            chmod(cur.c_str(), 0755);
            chown(cur.c_str(), 0, 0);
        }
    }
}

// Stages `hostSrcPath` at `<blackb0xRoot>/<destRelPath>` with the exact
// owner/mode it should carry once entrypoint.c's merge_tree() copies it
// onto the real device — that's the metadata this actually sets here, not
// whatever `hostSrcPath` happens to already have on the build host. A
// missing source is a warning, not a fatal error: several of these are
// pre-existing, already-documented gaps (see entrypoint/README.md) rather
// than something a single bake run can fix, and the rest of a firmware's
// payload is still worth producing even if one loose end is missing.
static bool stageFile(const fs::path& blackb0xRoot, const std::string& destRelPath, const std::string& hostSrcPath,
                       uid_t uid, gid_t gid, mode_t mode) {
    std::error_code existsEc;
    if (!fs::exists(hostSrcPath, existsEc)) {
        fprintf(stderr, "bakeRamdisk: WARNING: missing source %s — not staging /blackb0x/%s\n", hostSrcPath.c_str(),
                destRelPath.c_str());
        return false;
    }
    fs::path destPath = blackb0xRoot / destRelPath;
    ensureParentDirs(blackb0xRoot, destPath);
    std::error_code ec;
    fs::copy_file(hostSrcPath, destPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fprintf(stderr, "bakeRamdisk: cannot stage %s -> %s (%s)\n", hostSrcPath.c_str(), destPath.c_str(),
                ec.message().c_str());
        return false;
    }
    chmod(destPath.c_str(), mode);
    chown(destPath.c_str(), uid, gid);
    return true;
}

// Stages an empty directory with specific final metadata — replaces the
// old kCydiaDirs[]/create_cydia_directories()/mkdir_owned() machinery that
// used to live in entrypoint.c entirely: these are just empty entries
// under /blackb0x now, merge_tree() creates them like anything else. Always
// unconditionally re-applies uid/gid/mode, regardless of whether
// ensureParentDirs() already auto-created this same path as a root:wheel
// 0755 implied parent of something else staged first — this call is always
// the authority on this path's final metadata, whichever order runs first.
static bool stageDir(const fs::path& blackb0xRoot, const std::string& destRelPath, uid_t uid, gid_t gid,
                      mode_t mode) {
    fs::path destPath = blackb0xRoot / destRelPath;
    ensureParentDirs(blackb0xRoot, destPath);
    std::error_code ec;
    fs::create_directory(destPath, ec);
    chmod(destPath.c_str(), mode);
    chown(destPath.c_str(), uid, gid);
    return true;
}

// Stages a real symlink under /blackb0x pointing at `target` verbatim —
// `target` need not exist relative to /blackb0x or even the build host at
// all (e.g. an absolute path into the real device's own /System, like
// rtbuddyd's jsc swap below); it only has to resolve once merge_tree()
// recreates this same symlink on the real /mnt1.
static bool stageSymlink(const fs::path& blackb0xRoot, const std::string& destRelPath, const std::string& target) {
    fs::path destPath = blackb0xRoot / destRelPath;
    ensureParentDirs(blackb0xRoot, destPath);
    std::error_code ec;
    fs::remove(destPath, ec);
    fs::create_symlink(target, destPath, ec);
    if (ec) {
        fprintf(stderr, "bakeRamdisk: cannot stage symlink %s -> %s (%s)\n", destPath.c_str(), target.c_str(),
                ec.message().c_str());
        return false;
    }
    return true;
}

// Recursively stages every regular file under `hostSrcDir` at
// `<blackb0xRoot>/<destRelDir>/<same relative path>`, all with the same
// owner/mode — used for the apt lists cache (stageAptListsCache() below),
// which is a whole directory tree of files apt itself produced, not a
// single named asset stageFile() already handles one at a time.
static bool stageDirectoryTree(const fs::path& blackb0xRoot, const std::string& destRelDir,
                                const fs::path& hostSrcDir, uid_t uid, gid_t gid, mode_t mode) {
    std::error_code walkEc;
    bool ok = true;
    for (const auto& entry :
         fs::recursive_directory_iterator(hostSrcDir, fs::directory_options::skip_permission_denied, walkEc)) {
        if (walkEc) {
            ok = false;
            break;
        }
        std::error_code typeEc;
        if (!fs::is_regular_file(entry.path(), typeEc) || typeEc) continue;
        fs::path rel = fs::relative(entry.path(), hostSrcDir);
        ok &= stageFile(blackb0xRoot, destRelDir + "/" + rel.string(), entry.path().string(), uid, gid, mode);
    }
    return ok;
}

// The base Cydia/dpkg/apt directory set every firmware needs regardless of
// which persistence payload applies — ported verbatim from entrypoint.c's
// old kCydiaDirs[] (see docs/HISTORY.md for FUN_00001b14's original order),
// just without the /mnt1 prefix (relative to /blackb0x now) and staged here
// instead of mkdir'd on-device. All mobile:staff 0755.
static const std::vector<std::string> kCydiaDirs = {
    "Library/LaunchDaemons",
    "private/etc/alternatives",
    "private/etc/apt",
    "private/etc/apt/apt.conf.d",
    "private/etc/apt/preferences.d",
    "private/etc/apt/sources.list.d",
    "private/etc/apt/trusted.gpg.d",
    "private/etc/default",
    "private/etc/dpkg",
    "private/etc/dpkg/origins",
    "private/etc/pam.d",
    "private/etc/profile.d",
    "private/etc/ssh",
    "private/etc/ssl",
    "private/etc/ssl/certs",
    "private/etc/ssl/private",
    "private/var/backups",
    "private/var/cache",
    "private/var/cache/apt",
    "private/var/cache/apt/archives",
    "private/var/cache/apt/archives/partial",
    "private/var/cache/findutils",
    "private/var/lib",
    "private/var/lib/apt",
    "private/var/lib/apt/lists",
    "private/var/lib/apt/lists/partial",
    "private/var/lib/apt/periodic",
    "private/var/lib/cydia",
    "private/var/lib/dpkg",
    "private/var/lib/dpkg/alternatives",
    "private/var/lib/dpkg/info",
    "private/var/lib/dpkg/parts",
    "private/var/lib/dpkg/updates",
    "private/var/lib/misc",
    "private/var/local",
    "private/var/lock",
    "private/var/log/apt",
    "private/var/root/Media",
    "private/var/run",
    "usr/etc",
    "usr/games",
    "usr/include",
    "usr/include/apt-pkg",
    "usr/include/curl",
    "usr/include/ncursesw",
    "usr/include/openssl",
    "usr/include/pam",
    "usr/include/readline",
    "usr/lib/_ncurses",
    "usr/lib/apt",
    "usr/lib/apt/methods",
    "usr/lib/dpkg",
    "usr/lib/dpkg/methods",
    "usr/lib/dpkg/methods/apt",
    "usr/lib/engines",
    "usr/lib/gettext",
    "usr/lib/pam",
    "usr/lib/pkgconfig",
    "usr/lib/ssl",
    "usr/lib/ssl/misc",
    "usr/libexec/cydia",
    "usr/libexec/gnupg",
    "usr/share/bigboss",
    "usr/share/bigboss/icons",
    "usr/share/bigboss/icons/.svn",
    "usr/share/bigboss/icons/.svn/prop-base",
    "usr/share/bigboss/icons/.svn/props",
    "usr/share/bigboss/icons/.svn/text-base",
    "usr/share/bigboss/icons/.svn/tmp",
    "usr/share/bigboss/icons/.svn/tmp/prop-base",
    "usr/share/bigboss/icons/.svn/tmp/props",
    "usr/share/bigboss/icons/.svn/tmp/text-base",
    "usr/share/dict",
    "usr/share/dpkg",
    "usr/share/dpkg/origins",
    "usr/share/gnupg",
    "usr/share/keyrings",
    "usr/share/tabset",
    "usr/share/terminfo",
    "usr/share/terminfo/a",
    "usr/share/terminfo/c",
    "usr/share/terminfo/d",
    "usr/share/terminfo/E",
    "usr/share/terminfo/l",
    "usr/share/terminfo/m",
    "usr/share/terminfo/p",
    "usr/share/terminfo/r",
    "usr/share/terminfo/s",
    "usr/share/terminfo/v",
    "usr/share/terminfo/x",
};

// Stages a verbatim copy of build_deb_cache.py's own sandboxed `apt-get
// update` cache (its real apt-lists/ output — see that script's own module
// docstring) at the real device's own /var/lib/apt/lists/ path. This is
// what lets apt on-device know about every package the configured
// regulad/saurik/awkwardtv/xbmc repos currently offer — including anything
// too big to also stage the .deb bytes for locally (kNeverStageDebs below)
// — without needing network at install time at all; network only becomes
// necessary for whatever wasn't also staged in private/var/cache/apt/archives/.
static bool stageAptListsCache(const fs::path& blackb0xRoot, const std::string& aptListsDir) {
    if (!fs::exists(aptListsDir)) {
        fprintf(stderr, "bakeRamdisk: build_deb_cache.py did not produce apt-lists/\n");
        return false;
    }
    return stageDirectoryTree(blackb0xRoot, "private/var/lib/apt/lists", aptListsDir, 0, 0, 0644);
}

// .deb filename prefixes that never get staged into the ramdisk's own apt
// cache, even when scripts/build_deb_cache.py's real dependency resolution
// says they're needed — real, but too big for this old A4-era ramdisk's
// 64MiB ceiling (kMaxRamdiskSize below) to absorb on top of everything else.
// org.xbmc.kodi-atv2 alone is ~40MB; com.nito.nitotv is ~1.65MB on its own
// (checked directly against the real vendored .deb — nowhere near
// kodi-atv2's size, but still excluded on request). odcctools (ld64/as/
// otool/nm/strip/etc — a native build toolchain, not something a media/
// jailbreak ramdisk needs at runtime) was confirmed via a real bake+ncdu
// pass to be the single largest package actually staged by default:
// ~7.7MB unpacked, versus single-digit-KB-to-low-MB for everything else
// (checked directly: `ar p odcctools_*.deb control.tar.gz | tar -xzO
// ./control` shows it Depends only on openssl/uuid, and grepping every
// other vendored .deb's own control file for "odcctools" turns up nothing
// — no other package here declares a dependency on it, so excluding it
// from bake-time staging can't break dependency resolution for anything
// else). gettext (3.2MB) and curl (0.7MB) followed the same audit: built
// the real dependency graph from every vendored .deb's own Depends:/
// Pre-Depends: field and computed the transitive closure actually needed
// to bootstrap postinstall.sh (bash, dpkg, coreutils(-bin), apt7(-lib),
// and what THEY pull in — berkeleydb, bzip2, diffutils, findutils, gnupg,
// grep, gzip, lzma, ncurses, readline, sed, tar; 20 packages total).
// Neither gettext nor curl is in that closure. gettext's only dependent
// among every vendored package is wget (itself outside the closure); curl's
// dependents (org.xbmc.kodi-atv2, the tihmstar exploit-tool packages,
// apt7-ssl) are all optional/already-excluded/not the apt7 actually
// staged. Checked directly, not assumed: extracted apt7-lib's real
// data.tar and ran `strings` on its actual usr/lib/apt/methods/http
// binary — no libcurl reference at all (this old apt implements its own
// HTTP client), and usr/lib/apt/methods/https turned out to be a plain
// symlink to http (no TLS lib linked either), confirming apt's own
// network fetch genuinely doesn't touch curl or, for that matter, openssl.
// openssl itself stays regardless: openssh (a real feature — SSH access is
// the whole point of the jailbreak, not a bootstrap-only tool) genuinely
// Depends: on it. Not staging the .deb bytes doesn't mean the package is
// unreachable, though: its real Packages metadata still gets staged via
// stageAptListsCache() below, and postinstall.sh's array (see
// package/packages.txt) still lists it — apt will fetch it over the
// network at install time if one is reachable, and just fail to install
// that one specific package (not the rest) if not, matching the
// "opportunistic network, not required" design this whole staging pass is
// built around.
static const std::vector<std::string> kNeverStageDebs = {
    "org.xbmc.kodi-atv2_",
    "com.nito.nitotv_",
    "odcctools_",
    "gettext_",
    "curl_",
};

static bool shouldSkipStagingDeb(const std::string& filename) {
    for (const auto& prefix : kNeverStageDebs) {
        if (filename.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Bake-time package preinstallation — for packages with no postinst (or a
// trivial one, see Blackb0x/Misc/prebake_package_blacklist.txt's own
// comment for the full audit) this unpacks the real .deb and writes real
// dpkg status/info state directly into /blackb0x at bake time, instead of
// just caching the .deb for postinstall.sh's own apt-get to install for
// real later. apt on-device then sees these packages as already installed
// and never touches their .deb at all — see stagePreinstalledPackages()
// below for the actual mechanism (real era-appropriate dpkg, in a
// container, not a hand-rolled reimplementation of dpkg's own status
// format).
// ---------------------------------------------------------------------------

struct DebControlInfo {
    std::string package;
    std::string depends;
    std::string preDepends;
    bool hasPostinst = false;
    bool hasPreinst = false;
};

// Extracts a .deb's control file via plain `ar`/`tar` (real, standard,
// already-vendored tools — this is reading one text field out of an
// archive, not reimplementing anything apt/dpkg itself does) and parses
// out Package:/Depends:/Pre-Depends:, including real RFC822 continuation-
// line folding (a field value can wrap onto following lines that start
// with whitespace).
static bool readDebControlInfo(const std::string& debPath, DebControlInfo& out) {
    std::string tempDir = makeTempDir("blackb0x-controlinfo-");
    if (tempDir.empty()) return false;
    bool ok = runCommand({"ar", "x", fs::absolute(debPath).string()}, tempDir);
    std::string controlTar;
    if (ok) {
        std::error_code dirEc;
        for (const auto& e : fs::directory_iterator(tempDir, dirEc)) {
            if (e.path().filename().string().rfind("control.tar", 0) == 0) {
                controlTar = e.path().string();
                break;
            }
        }
        ok = !controlTar.empty();
    }
    // The control archive's own member names aren't consistently
    // "./control"/"./postinst"/"./preinst" — confirmed directly against
    // real, already-vendored data: 14 org.tihmstar.* .debs in
    // Blackb0x/Debs/ store these as plain "control"/"postinst"/"preinst"
    // (no "./" prefix) instead, and which spelling a given .deb uses isn't
    // consistent even within that same set (whatever tool originally built
    // each one). Listing the archive first and matching whichever spelling
    // is actually present is more robust than assuming one fixed path for
    // any of the three — same fix already made and verified on the Python
    // side, see scripts/build_deb_cache_experimental_no_container.py's own
    // read_deb_control_info().
    std::vector<std::string> controlMembers;
    if (ok) {
        std::string listing = runCommandCapture({"tar", "--auto-compress", "-tf", controlTar});
        std::istringstream listingStream(listing);
        std::string member;
        while (std::getline(listingStream, member)) {
            while (!member.empty() && (member.back() == '\r' || member.back() == '\n')) member.pop_back();
            if (!member.empty()) controlMembers.push_back(member);
        }
    }
    auto findControlMember = [&](const std::string& name) -> std::string {
        std::string withDot = "./" + name;
        for (const auto& m : controlMembers) {
            if (m == name || m == withDot) return m;
        }
        return "";
    };

    out.hasPostinst = !findControlMember("postinst").empty();
    // Same audit-then-strip treatment as postinst, just enforced
    // differently: preinst runs unconditionally DURING --unpack
    // itself, before a plain post-hoc file delete could ever
    // intervene the way stripping postinst works, so a trivial
    // preinst instead gets removed from the .deb's own control
    // archive before dpkg ever sees it — see stripPreinstFromDeb().
    // Confirmed by real disassembly, not guessed: ncurses's preinst
    // (a real ARM Mach-O binary) turned out to be a harmless
    // migration symlink (`/usr/lib/_ncurses` -> `/usr/lib`, only on
    // install or upgrade-from-5.6-), identical on any platform — a
    // genuine false positive if treated the same as firmware-sbin/
    // rtadvd/pam/pam-modules's real, device-state-dependent preinst
    // scripts (see prebake_package_blacklist.txt's own note on each,
    // which — since preinst can't be stripped after the fact —
    // are the actual, enforced exclusion for those four, not just
    // documentation).
    out.hasPreinst = !findControlMember("preinst").empty();

    std::string controlMember = findControlMember("control");
    if (ok) ok = !controlMember.empty();
    if (ok) ok = runCommand({"tar", "--auto-compress", "-xf", controlTar, controlMember}, tempDir);
    std::ifstream in(tempDir + "/control");
    if (!ok || !in) {
        std::error_code rmEc;
        fs::remove_all(tempDir, rmEc);
        return false;
    }
    std::string line, field, value;
    auto flush = [&]() {
        if (field == "Package") out.package = value;
        else if (field == "Depends") out.depends = value;
        else if (field == "Pre-Depends") out.preDepends = value;
        field.clear();
        value.clear();
    };
    while (std::getline(in, line)) {
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (!field.empty()) value += " " + line.substr(1);
            continue;
        }
        if (!field.empty()) flush();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    }
    if (!field.empty()) flush();
    std::error_code rmEc;
    fs::remove_all(tempDir, rmEc);
    return !out.package.empty();
}

// Rebuilds `srcDebPath` at `destDebPath` with its control archive's
// `preinst` member removed — real `ar`/`tar`/`gzip`, not a hand-rolled .deb
// writer, matching this project's own established convention of using
// real tools for real archive formats. Needed because preinst runs
// unconditionally DURING dpkg --unpack itself: unlike postinst (which can
// just be deleted from an already-unpacked info/ directory before
// --configure runs), there's no way to intervene between "dpkg extracts
// preinst from the .deb" and "dpkg executes it" using plain dpkg flags —
// the only way to make a confirmed-trivial preinst a no-op is to remove it
// from the .deb's own control archive before dpkg ever sees the file at
// all. Preserves member order (debian-binary, control.tar.*, data.tar.*)
// and the original compression format of control.tar.* exactly.
static bool stripPreinstFromDeb(const std::string& srcDebPath, const std::string& destDebPath) {
    std::string tempDir = makeTempDir("blackb0x-stripdeb-");
    if (tempDir.empty()) return false;
    bool ok = runCommand({"ar", "x", fs::absolute(srcDebPath).string()}, tempDir);
    std::string debianBinary = tempDir + "/debian-binary";
    std::string controlTar, dataTar;
    if (ok) {
        std::error_code dirEc;
        for (const auto& e : fs::directory_iterator(tempDir, dirEc)) {
            std::string fn = e.path().filename().string();
            if (fn.rfind("control.tar", 0) == 0) controlTar = e.path().string();
            else if (fn.rfind("data.tar", 0) == 0) dataTar = e.path().string();
        }
        ok = fs::exists(debianBinary) && !controlTar.empty() && !dataTar.empty();
    }
    std::string extractDir = tempDir + "/control-extract";
    if (ok) {
        std::error_code mkEc;
        fs::create_directories(extractDir, mkEc);
        ok = runCommand({"tar", "--auto-compress", "-xf", controlTar, "-C", extractDir});
    }
    if (ok) {
        std::error_code rmEc2;
        fs::remove(fs::path(extractDir) / "preinst", rmEc2);
    }
    std::string newControlTar = tempDir + "/" + fs::path(controlTar).filename().string();
    if (ok) ok = runCommand({"tar", "--auto-compress", "-cf", newControlTar, "-C", extractDir, "."});
    if (ok) {
        std::error_code rmDestEc;
        fs::remove(destDebPath, rmDestEc);
        ok = runCommand({"ar", "rc", fs::absolute(destDebPath).string(), debianBinary, newControlTar, dataTar});
    }
    std::error_code rmEc;
    fs::remove_all(tempDir, rmEc);
    return ok;
}

// Splits a Depends:/Pre-Depends: field into its comma-separated groups,
// each itself a list of `|`-alternatives, with version constraints
// ("(>= 1.2)") and whitespace stripped down to bare package names.
static std::vector<std::vector<std::string>> parseDependencyGroups(const std::string& field) {
    std::vector<std::vector<std::string>> groups;
    std::stringstream ss(field);
    std::string group;
    while (std::getline(ss, group, ',')) {
        std::vector<std::string> alts;
        std::stringstream gs(group);
        std::string alt;
        while (std::getline(gs, alt, '|')) {
            size_t paren = alt.find('(');
            if (paren != std::string::npos) alt = alt.substr(0, paren);
            size_t start = alt.find_first_not_of(" \t");
            size_t end = alt.find_last_not_of(" \t");
            if (start == std::string::npos) continue;
            alts.push_back(alt.substr(start, end - start + 1));
        }
        if (!alts.empty()) groups.push_back(alts);
    }
    return groups;
}

static std::set<std::string> readPrebakeBlacklist() {
    std::set<std::string> blacklist;
    std::ifstream in(resolveMiscPath("prebake_package_blacklist.txt"));
    std::string line;
    while (std::getline(in, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        size_t end = line.find_last_not_of(" \t\r");
        blacklist.insert(line.substr(start, end - start + 1));
    }
    return blacklist;
}

// Computes which of `resolvedFilenames` (picklist.txt's full transitive
// closure) are safe to unpack + mark installed at bake time. A package is
// eligible only if it is NOT in
// Blackb0x/Misc/prebake_package_blacklist.txt AND every one of its real
// Depends:/Pre-Depends: (extracted directly from the actual .deb, not
// guessed) is ALSO eligible, transitively. This propagation is not
// optional: cydia is blacklisted (real, stateful first-run postinst — see
// the blacklist file's own comment), and plenty of otherwise-trivial
// packages in this ecosystem Depends: on it — without walking the real
// dependency graph, they'd end up marked "installed" in dpkg's own status
// file while a real dependency was never actually satisfied: a genuinely
// broken, inconsistent package database, not a merely-redundant one.
//
// Eligibility alone doesn't mean a package's postinst/preinst is safe to
// actually RUN, though — stagePreinstalledPackages() runs a genuine, real
// `dpkg --configure` over this whole set (needed to correctly resolve
// this ecosystem's Pre-Depends: cycle, see kPreinstallInnerScript's own
// comment), which executes any maintainer script that exists for real,
// inside a plain Linux container with no iOS-specific tools (`launchctl`
// etc.) available at all. `prebake_package_blacklist.txt` is for scripts
// that do real, stateful, runtime-dependent, or otherwise-uninspectable
// (a compiled binary, not a shell script) work and must run on the real
// device instead — anything eligible (i.e. not blacklisted, directly or
// transitively) that STILL happens to have a postinst or preinst is, by
// construction, one nobody has flagged as needing that: confirmed safe to
// run for real, just not inside this bootstrap container specifically.
// `outStripPostinstPackages`/`outStripPreinstFilenames` collect exactly
// those, by real inspection of each .deb's own control archive (not a
// maintained list) — stagePreinstalledPackages() deletes each such
// package's postinst between unpacking and configuring (safe: postinst
// only ever runs at --configure time, well after that), and rebuilds a
// preinst-stripped copy of each such package's own .deb before ever
// handing it to dpkg at all (not safe to do after the fact: preinst runs
// unconditionally DURING --unpack itself) — either way it configures as a
// genuine no-op instead of failing on a missing command/wrong
// architecture.
static std::set<std::string> computePreinstallEligibleFilenames(const std::vector<std::string>& resolvedFilenames,
                                                                  const std::string& debsRoot,
                                                                  std::set<std::string>& outStripPostinstPackages,
                                                                  std::set<std::string>& outStripPreinstFilenames) {
    std::map<std::string, DebControlInfo> infoByPackage;
    std::map<std::string, std::string> filenameByPackage;
    for (const auto& filename : resolvedFilenames) {
        DebControlInfo info;
        if (!readDebControlInfo(debsRoot + "/" + filename, info)) {
            fprintf(stderr,
                    "bakeRamdisk: WARNING: cannot read control info for %s — excluding from bake-time preinstall\n",
                    filename.c_str());
            continue;
        }
        infoByPackage[info.package] = info;
        filenameByPackage[info.package] = filename;
    }

    std::set<std::string> resolvedNames;
    for (const auto& [name, filename] : filenameByPackage) resolvedNames.insert(name);

    std::set<std::string> blacklist = readPrebakeBlacklist();
    std::set<std::string> excluded;
    for (const auto& name : resolvedNames) {
        if (blacklist.count(name)) excluded.insert(name);
    }

    // kNeverStageDebs (see its own comment, above shouldSkipStagingDeb())
    // means "never stage this .deb's bytes into the ramdisk, period" — that
    // has to hold here too, not just in stageDebcache()'s own apt-cache
    // loop. A real bake confirmed odcctools stayed in the finished ramdisk
    // anyway after being added to kNeverStageDebs: it has no postinst, so
    // it sailed straight into the bake-time preinstall set below (which
    // never consulted kNeverStageDebs at all) and got unpacked directly
    // into /blackb0x regardless. Folding it into `excluded` up front here,
    // same as blacklist entries, reuses the exact dependency-propagation
    // pass right below — anything that depends on a never-staged package
    // gets correctly excluded too, instead of being preinstalled against a
    // "dependency" whose files were never actually staged anywhere.
    std::set<std::string> neverStage;
    for (const auto& [name, filename] : filenameByPackage) {
        if (shouldSkipStagingDeb(filename)) neverStage.insert(name);
    }
    for (const auto& name : neverStage) excluded.insert(name);

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& name : resolvedNames) {
            if (excluded.count(name)) continue;
            const DebControlInfo& info = infoByPackage[name];
            bool unsatisfiable = false;
            for (const auto& field : {info.preDepends, info.depends}) {
                for (const auto& group : parseDependencyGroups(field)) {
                    bool groupHasResolvedAlt = false;
                    bool groupSatisfied = false;
                    for (const auto& alt : group) {
                        if (resolvedNames.count(alt)) {
                            groupHasResolvedAlt = true;
                            if (!excluded.count(alt)) groupSatisfied = true;
                        }
                    }
                    if (groupHasResolvedAlt && !groupSatisfied) {
                        unsatisfiable = true;
                        break;
                    }
                }
                if (unsatisfiable) break;
            }
            if (unsatisfiable) {
                excluded.insert(name);
                changed = true;
            }
        }
    }

    std::set<std::string> eligibleFilenames;
    int excludedByBlacklist = 0, excludedByNeverStage = 0, excludedByPropagation = 0;
    for (const auto& name : resolvedNames) {
        if (excluded.count(name)) {
            if (blacklist.count(name)) {
                excludedByBlacklist++;
            } else if (neverStage.count(name)) {
                excludedByNeverStage++;
            } else {
                excludedByPropagation++;
                fprintf(stderr,
                        "bakeRamdisk: %s not bake-time preinstalled — depends on a blacklisted or never-staged "
                        "package (transitively)\n",
                        name.c_str());
            }
        } else {
            const DebControlInfo& info = infoByPackage[name];
            const std::string& filename = filenameByPackage[name];
            eligibleFilenames.insert(filename);
            if (info.hasPostinst) {
                outStripPostinstPackages.insert(name);
                fprintf(stderr,
                        "bakeRamdisk: %s is bake-time eligible but has a real postinst — stripping it before "
                        "--configure (see computePreinstallEligibleFilenames()'s own comment)\n",
                        name.c_str());
            }
            if (info.hasPreinst) {
                outStripPreinstFilenames.insert(filename);
                fprintf(stderr,
                        "bakeRamdisk: %s is bake-time eligible but has a real preinst — rebuilding its .deb "
                        "without it before --unpack (see computePreinstallEligibleFilenames()'s own comment)\n",
                        name.c_str());
            }
        }
    }
    fprintf(stderr,
            "bakeRamdisk: bake-time preinstall: %zu eligible, %d blacklisted directly, %d never-staged directly, "
            "%d excluded via dependency propagation, out of %zu resolved packages\n",
            eligibleFilenames.size(), excludedByBlacklist, excludedByNeverStage, excludedByPropagation,
            resolvedNames.size());

    return eligibleFilenames;
}

// Recursively merges a real, ordinary filesystem directory (`hostSrcDir`)
// into `<blackb0xRoot>/<destRelDir>`, preserving each entry's own real
// owner/mode (or symlink target) read directly via lstat()/readlink() —
// unlike stageFile()/stageDir()'s single caller-supplied (uid, gid, mode)
// triple, this is for merging a tree whose correct metadata is already ON
// the files themselves (the real unpacked dpkg preinstall root below),
// not something this program's own call site knows ahead of time.
// Destination directories that already exist are left with their own
// metadata untouched (same "don't clobber" rule as ensureParentDirs()) —
// only freshly-created ones get this tree's metadata applied.
static bool mergeRealFilesystemTree(const fs::path& blackb0xRoot, const std::string& destRelDir,
                                     const fs::path& hostSrcDir) {
    bool ok = true;
    std::error_code dirEc;
    for (const auto& entry : fs::directory_iterator(hostSrcDir, dirEc)) {
        std::string name = entry.path().filename().string();
        std::string destRel = destRelDir.empty() ? name : destRelDir + "/" + name;
        struct stat st;
        if (lstat(entry.path().c_str(), &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            char buf[4096];
            ssize_t n = readlink(entry.path().c_str(), buf, sizeof(buf) - 1);
            if (n < 0) {
                ok = false;
                continue;
            }
            buf[n] = '\0';
            ok &= stageSymlink(blackb0xRoot, destRel, std::string(buf));
        } else if (S_ISDIR(st.st_mode)) {
            fs::path destPath = blackb0xRoot / destRel;
            ensureParentDirs(blackb0xRoot, destPath);
            std::error_code cdEc;
            if (fs::create_directory(destPath, cdEc)) {
                chmod(destPath.c_str(), st.st_mode & 07777);
                chown(destPath.c_str(), st.st_uid, st.st_gid);
            }
            ok &= mergeRealFilesystemTree(blackb0xRoot, destRel, entry.path());
        } else if (S_ISREG(st.st_mode)) {
            ok &= stageFile(blackb0xRoot, destRel, entry.path().string(), st.st_uid, st.st_gid,
                             st.st_mode & 07777);
        }
    }
    return ok;
}

// The container script that does the actual work: the standard real-
// debootstrap two-phase bootstrap pattern, not a per-package retry loop.
// Confirmed directly, the hard way, that a retry loop cannot work here:
// this ecosystem has a genuine, unbreakable CYCLE right at the root of
// its own bootstrap chain — dpkg Pre-Depends: on tar, tar Depends: on
// gzip/lzma, gzip/lzma Depend: on sed, and sed Pre-Depends: right back on
// dpkg. No sequential processing order, retried or not, can ever resolve
// that; something in the cycle has to go first with a knowingly-unmet
// dependency. Real debootstrap solves exactly this by (1) force-unpacking
// every package first, with all dependency checking disabled, so every
// file is physically on disk regardless of ordering, then (2) running one
// real, forced `--configure -a`, which uses dpkg's own internal
// processing order across the whole set and — since forcing is still in
// effect — pushes straight through the cycle instead of refusing. This is
// safe specifically because prebake_package_blacklist.txt's own
// transitive-closure computation already proved every package reaching
// this point has no postinst/prerm anywhere in its own dependency chain:
// there's no maintainer script for `--force-depends` to let run despite
// an unmet dependency, only real status/trigger bookkeeping, which is
// what actually satisfies downstream Pre-Depends: checks correctly.
// era-appropriate dpkg: debian:stretch ships 1.18.26, confirmed via
// `podman run debian:stretch dpkg --version`, right next to this
// project's own vendored on-device dpkg 1.18.10-12. Two REAL, independent
// consistency checks — dpkg --audit and apt-get check — run afterward,
// confirmed by direct testing to (a) both report clean/empty on a
// genuinely-consistent root and (b) both actually catch a real broken
// case (tested by unpacking a package whose Pre-Depends: was deliberately
// left unsatisfied). Either one failing aborts the whole step.
static const char* kPreinstallInnerScript = R"SCRIPT(#!/bin/sh
set -e
export DEBIAN_FRONTEND=noninteractive
mkdir -p /preinstall/var/lib/dpkg/info /preinstall/var/lib/dpkg/updates /preinstall/var/lib/dpkg/triggers
touch /preinstall/var/lib/dpkg/status /preinstall/var/lib/dpkg/available
mkdir -p /preinstall/etc/apt/preferences.d /preinstall/etc/apt/sources.list.d
touch /preinstall/etc/apt/sources.list
dpkg --root=/preinstall --add-architecture iphoneos-arm

# The same synthetic "firmware" package scripts/build_deb_cache.py's own
# apt sandbox declares (see that script's own comment for why) — several
# packages here have a plain Depends: firmware (>= X), and without this
# declared in THIS dpkg root too (not just the outer one that decided
# these packages were resolvable in the first place), the real dpkg
# --audit/apt-get check below would report every one of them as a false-
# positive "unmet dependency" and abort the whole step over nothing.
# __FIRMWARE_VERSION__ below is substituted with the real, per-tuple
# firmware version (the same one passed to build_deb_cache.py's own
# --firmware-version) right before this script is written to disk — see
# where kPreinstallInnerScript is instantiated below — so this dpkg root
# always agrees with whichever real (device, buildID) the outer apt
# resolution actually ran against, not a fixed pin.
cat >> /preinstall/var/lib/dpkg/status <<'EOF'
Package: firmware
Status: install ok installed
Priority: required
Section: base
Installed-Size: 0
Maintainer: Blackb0x BakeRamdisk.cpp <noreply@regulad.xyz>
Architecture: iphoneos-arm
Version: __FIRMWARE_VERSION__
Description: Synthetic package matching scripts/build_deb_cache.py's own
 declaration, so this bootstrap root's dpkg agrees with the outer apt
 resolution that already decided firmware-version-gated Depends: lines
 here are satisfied.

EOF
# dpkg --audit demands every installed package have a .list/.md5sums in
# dpkg/info — normally written by a real `dpkg --unpack`, which this
# synthetic package never goes through since it's hand-inserted straight
# into status. Empty files satisfy the audit (it only checks for their
# existence, not content) — firmware has no real files of its own to list.
touch /preinstall/var/lib/dpkg/info/firmware.list /preinstall/var/lib/dpkg/info/firmware.md5sums

: > /work/out/unpack.log
while IFS= read -r filename; do
    [ -z "$filename" ] && continue
    src="/debs-override/$filename"
    [ -f "$src" ] || src="/debs/$filename"
    dpkg --root=/preinstall --force-architecture --force-depends --unpack "$src" >>/work/out/unpack.log 2>&1 || true
done < /work/preinstall_filenames.txt

while IFS= read -r pkgname; do
    [ -z "$pkgname" ] && continue
    rm -f "/preinstall/var/lib/dpkg/info/$pkgname.postinst"
done < /work/strip_postinst_packages.txt

dpkg --root=/preinstall --force-depends --configure -a > /work/out/configure.log 2>&1 || true
if grep -q "^Status: install ok unpacked$\|^Status: install ok half-configured$" /preinstall/var/lib/dpkg/status; then
    echo "FATAL: some packages never reached 'installed' even after a forced --configure -a:" >&2
    grep -B2 "^Status: install ok unpacked$\|^Status: install ok half-configured$" /preinstall/var/lib/dpkg/status >&2
    cat /work/out/configure.log >&2
    exit 1
fi

dpkg --root=/preinstall --force-architecture --audit > /work/out/audit.txt 2>/work/out/audit-warnings.log
if [ -s /work/out/audit-warnings.log ]; then
    echo "dpkg --audit: non-fatal parse warnings (stderr, not actual audit findings):" >&2
    cat /work/out/audit-warnings.log >&2
fi
if [ -s /work/out/audit.txt ]; then
    echo "FATAL: dpkg --audit reported a problem:" >&2
    cat /work/out/audit.txt >&2
    exit 1
fi

apt-get -o Dir=/preinstall -o APT::Architecture=iphoneos-arm -o APT::Architectures::=iphoneos-arm check > /work/out/check.txt 2>&1
if grep -qiE "broken|unmet" /work/out/check.txt; then
    echo "FATAL: apt-get check reported broken/unmet dependencies:" >&2
    cat /work/out/check.txt >&2
    exit 1
fi

mkdir -p /out/dpkg-state/info
cp /preinstall/var/lib/dpkg/status /out/dpkg-state/status
cp -a /preinstall/var/lib/dpkg/info/. /out/dpkg-state/info/
rm -rf /preinstall/var/lib/dpkg /preinstall/etc/apt
)SCRIPT";

#if defined(__APPLE__)
// macOS has no container runtime at all (confirmed directly — not just
// podman, no viable alternative either), so the real, containerized dpkg
// bootstrap the #else branch below runs is off the table here. That real
// dpkg run only exists to correctly SEQUENCE maintainer-script execution
// around a genuine dpkg/tar/gzip/sed dependency cycle in this bootstrap-era
// package set (see kPreinstallInnerScript's own long comment for the full
// cycle) — force-unpack everything with checking off, then one real forced
// `--configure -a` pass to push scripts through in dpkg's own correct
// order. But every package that reaches this function has already been
// proven, by computePreinstallEligibleFilenames()'s own transitive
// closure over prebake_package_blacklist.txt, to need no real, stateful
// maintainer-script execution at all — any postinst/preinst an eligible
// package DOES happen to carry is either stripped (postinst, before a real
// --configure would run it) or never reaches dpkg in the first place
// (preinst, since --unpack never runs here either). With no script ever
// executed on this path (not "stripped-then-executed", literally never
// extracted-and-run), the whole reason for real, sequenced dpkg evaporates:
// "unpack every package" degenerates to "copy every package's real payload
// files onto disk," an operation with no meaningful ordering constraint
// left to get wrong — so `stripPostinstPackages`/`stripPreinstFilenames`
// are accepted for signature parity with the #else branch but genuinely
// unused here, not an oversight.
//
// Mechanism: reuse extractDebAndBuildStanza() (already proven by
// stageEtasonatv()/stageP0sixspwn() below) per eligible package to pull its
// real data.tar.* payload out with plain ar/tar and build a dpkg status
// stanza from its real control file via buildStatusStanzaFromControl();
// merge the payload into a preinstall root the same way mergeRealFilesystemTree()
// already merges any other real, ordinary filesystem tree (mirroring
// exactly what a real `dpkg --unpack` would have left on disk); concatenate
// every stanza into one status file and hand-write matching per-package
// dpkg/info/ state. The result lands in the exact same outPreinstallDir/
// outDpkgStateDir shape the #else branch produces, so mergePreinstalledPackages()
// (the shared, non-platform-specific caller) needs no changes at all.
//
// This intentionally skips the #else branch's `dpkg --audit`/`apt-get
// check` consistency pass — there is no real dpkg/apt state machine
// running here to audit in the first place, just files being copied. What
// substitutes for it: computePreinstallEligibleFilenames()'s own
// dependency-satisfaction propagation loop (the `while (changed) { ...
// unsatisfiable ... }` loop in that function) already proved, before this
// function is ever called, that every package in `eligibleFilenames` has
// every one of its real Depends:/Pre-Depends: also present in the eligible
// set — the actual condition a real audit would otherwise be checking for
// here. This is a real, deliberate, accepted simplification (no full
// apt-style audit), not an oversight.
static bool computePreinstalledPackages(const std::set<std::string>& eligibleFilenames,
                                         const std::set<std::string>& /*stripPostinstPackages*/,
                                         const std::set<std::string>& /*stripPreinstFilenames*/,
                                         const std::string& firmwareVersion,
                                         std::string& outPreinstallDir, std::string& outDpkgStateDir) {
    std::string preinstallDir = makeTempDir("blackb0x-preinstall-root-");
    std::string outDir = makeTempDir("blackb0x-preinstall-out-");
    if (preinstallDir.empty() || outDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create preinstall temp dirs\n");
        return false;
    }
    std::error_code mkEc;
    fs::create_directories(fs::path(outDir) / "dpkg-state" / "info", mkEc);
    outPreinstallDir = preinstallDir;
    outDpkgStateDir = outDir + "/dpkg-state";

    if (eligibleFilenames.empty()) {
        fprintf(stderr, "bakeRamdisk: no packages eligible for bake-time preinstall this run\n");
        return true;
    }

    // Loose members every .deb's own `ar x` + data.tar/control.tar
    // extraction (extractDebAndBuildStanza()) leaves sitting in its tempDir
    // alongside the real payload tree: the .deb's own top-level archive
    // members (debian-binary, control.tar.*, data.tar.* — never real
    // device paths), plus every possible flat control-archive member name
    // per the real Debian binary-package spec. None of these are ever
    // legitimate top-level payload paths with no directory prefix, so
    // they're removed before merging — otherwise this hand-rolled unpack
    // would stage a maintainer script (or the .deb's own archive bytes) as
    // if it were real content belonging on the device.
    static const std::vector<std::string> kDebControlArtifactNames = {
        "control", "preinst", "postinst", "prerm", "postrm",
        "conffiles", "md5sums", "triggers", "shlibs", "templates", "config",
    };

    std::string debsRoot = resolveDebsPath();
    std::string statusOut;
    for (const auto& filename : eligibleFilenames) {
        std::string debPath = fs::absolute(debsRoot + "/" + filename).string();
        // dropRelationshipFields=false: unlike stageManualDpkgInstall()'s
        // two firmware-version-gated callers, these are ordinary bake-time
        // preinstalls with nothing to route around — keep Depends:/
        // Pre-Depends: verbatim so this stanza matches what a real dpkg
        // --unpack/--configure would actually have produced.
        ExtractedDeb extracted = extractDebAndBuildStanza(debPath, "blackb0x-preinstall-pkg-",
                                                            "bake-time preinstall payload for " + filename,
                                                            /*hold=*/false, /*dropRelationshipFields=*/false);
        if (!extracted.ok || extracted.stanza.empty()) {
            fprintf(stderr,
                    "bakeRamdisk: FATAL: cannot extract eligible package %s for bake-time preinstall (no "
                    "container fallback available on macOS)\n",
                    filename.c_str());
            if (!extracted.tempDir.empty()) {
                std::error_code rmEc;
                fs::remove_all(extracted.tempDir, rmEc);
            }
            return false;
        }

        std::string pkgName;
        {
            std::istringstream stanzaLines(extracted.stanza);
            std::string line;
            while (std::getline(stanzaLines, line)) {
                if (line.rfind("Package:", 0) != 0) continue;
                std::string val = line.substr(8);
                size_t start = val.find_first_not_of(" \t");
                size_t end = val.find_last_not_of(" \t\r");
                if (start != std::string::npos) pkgName = val.substr(start, end - start + 1);
                break;
            }
        }
        if (pkgName.empty()) {
            fprintf(stderr, "bakeRamdisk: FATAL: %s's stanza has no parseable Package: name\n", filename.c_str());
            std::error_code rmEc;
            fs::remove_all(extracted.tempDir, rmEc);
            return false;
        }

        for (const auto& name : kDebControlArtifactNames) {
            std::error_code rmEc;
            fs::remove(fs::path(extracted.tempDir) / name, rmEc);
        }
        {
            std::error_code dirEc;
            for (const auto& e : fs::directory_iterator(extracted.tempDir, dirEc)) {
                std::string fn = e.path().filename().string();
                if (fn == "debian-binary" || fn.rfind("control.tar", 0) == 0 || fn.rfind("data.tar", 0) == 0) {
                    std::error_code rmEc2;
                    fs::remove(e.path(), rmEc2);
                }
            }
        }

        // Real dpkg .list convention: one absolute path per line, every
        // directory the package owns as well as every file/symlink,
        // starting with "/." for the root itself. Captured now, before
        // mergeRealFilesystemTree() below consumes this tempDir.
        std::vector<std::string> ownedPaths = {"/."};
        {
            std::error_code walkEc;
            for (const auto& e : fs::recursive_directory_iterator(extracted.tempDir, walkEc)) {
                if (walkEc) break;
                std::error_code relEc;
                fs::path rel = fs::relative(e.path(), extracted.tempDir, relEc);
                if (relEc) continue;
                ownedPaths.push_back("/" + rel.string());
            }
        }

        bool mergeOk = mergeRealFilesystemTree(fs::path(preinstallDir), "", extracted.tempDir);
        std::error_code rmEc;
        fs::remove_all(extracted.tempDir, rmEc);
        if (!mergeOk) {
            fprintf(stderr, "bakeRamdisk: FATAL: failed to merge %s's payload into the preinstall root\n",
                    filename.c_str());
            return false;
        }

        statusOut += extracted.stanza + "\n";

        // Per-package .list/.md5sums, same convention stageManualDpkgInstall()
        // already established for the hand-rolled etasonatv/p0sixspwn case:
        // real (owned-path) .list content, but an intentionally-empty
        // .md5sums — nothing in this codebase computes real md5 sums, and
        // (as noted above) nothing here runs `dpkg --audit` to care either
        // way; see stageManualDpkgInstall()'s own comment for the same
        // precedent.
        std::string listPath = outDpkgStateDir + "/info/" + pkgName + ".list";
        std::string md5Path = outDpkgStateDir + "/info/" + pkgName + ".md5sums";
        {
            std::ofstream listFile(listPath, std::ios::trunc);
            for (const auto& p : ownedPaths) listFile << p << "\n";
        }
        { std::ofstream md5File(md5Path, std::ios::trunc); }
        chmod(listPath.c_str(), 0644);
        chown(listPath.c_str(), 0, 0);
        chmod(md5Path.c_str(), 0644);
        chown(md5Path.c_str(), 0, 0);
    }

    // The synthetic "firmware" package — the exact same entry
    // kPreinstallInnerScript's own copy declares in the #else branch (see
    // its own comment), and the one scripts/build_deb_cache.py /
    // scripts/build_deb_cache_experimental_no_container.py's own outer apt
    // resolution already declares too. Genuinely needed here, not just for
    // symmetry: dropRelationshipFields=false above means every preinstalled
    // package's real Depends:/Pre-Depends: — including any genuine
    // `Depends: firmware (>= X)` line (e.g. rtadvd's) — is preserved
    // verbatim in the status file this produces, so without this entry a
    // real device's own dpkg/apt would see an unmet dependency the #else
    // branch never has.
    statusOut +=
        "Package: firmware\n"
        "Status: install ok installed\n"
        "Priority: required\n"
        "Section: base\n"
        "Installed-Size: 0\n"
        "Maintainer: Blackb0x BakeRamdisk.cpp <noreply@regulad.xyz>\n"
        "Architecture: iphoneos-arm\n"
        "Version: " + firmwareVersion + "\n"
        "Description: Synthetic package matching scripts/build_deb_cache.py's own\n"
        " declaration, so this ramdisk's dpkg agrees with the outer apt resolution\n"
        " that already decided firmware-version-gated Depends: lines here are\n"
        " satisfied.\n"
        "\n";
    {
        std::string firmwareListPath = outDpkgStateDir + "/info/firmware.list";
        std::string firmwareMd5Path = outDpkgStateDir + "/info/firmware.md5sums";
        { std::ofstream f(firmwareListPath, std::ios::trunc); }
        { std::ofstream f(firmwareMd5Path, std::ios::trunc); }
        chmod(firmwareListPath.c_str(), 0644);
        chown(firmwareListPath.c_str(), 0, 0);
        chmod(firmwareMd5Path.c_str(), 0644);
        chown(firmwareMd5Path.c_str(), 0, 0);
    }

    std::string statusPath = outDpkgStateDir + "/status";
    {
        std::ofstream statusFile(statusPath, std::ios::trunc);
        statusFile << statusOut;
    }
    chmod(statusPath.c_str(), 0644);
    chown(statusPath.c_str(), 0, 0);

    return true;
}
#else
// Runs the real dpkg preinstall for `eligibleFilenames` into fresh temp
// directories and leaves them in place (caller owns cleanup — or, in
// practice, never cleans them up at all: see computeGlobalDebcacheOnce(),
// which keeps these alive for the whole process so every firmware this
// run bakes can merge from the same result). Deliberately invoked via
// plain runCommand(), NOT runAsInvokingUser() like every other podman call
// in this file: bakeRamdisk() itself already runs as real root (needed
// for the HFS+ mount anyway), and this specific step needs that — rootless
// podman running AS the invoking user maps container-root to that user's
// own host uid, not real root (confirmed directly: an unpack test through
// that path came back host-side owned by the invoking user, not root),
// which would make every ownership value this function reads back from
// the bind-mounted output wrong. Root's own podman storage pulling
// debian:stretch fresh on first use is a one-time cost, same category as
// every other one-time podman setup step this project already has.
static bool computePreinstalledPackages(const std::set<std::string>& eligibleFilenames,
                                         const std::set<std::string>& stripPostinstPackages,
                                         const std::set<std::string>& stripPreinstFilenames,
                                         const std::string& firmwareVersion,
                                         std::string& outPreinstallDir, std::string& outDpkgStateDir) {
    std::string preinstallDir = makeTempDir("blackb0x-preinstall-root-");
    std::string outDir = makeTempDir("blackb0x-preinstall-out-");
    if (preinstallDir.empty() || outDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create preinstall temp dirs\n");
        return false;
    }
    std::error_code mkEc;
    fs::create_directories(fs::path(outDir) / "dpkg-state" / "info", mkEc);
    outPreinstallDir = preinstallDir;
    outDpkgStateDir = outDir + "/dpkg-state";

    if (eligibleFilenames.empty()) {
        fprintf(stderr, "bakeRamdisk: no packages eligible for bake-time preinstall this run\n");
        return true;
    }

    std::string workDir = makeTempDir("blackb0x-preinstall-work-");
    std::string debsOverrideDir = makeTempDir("blackb0x-preinstall-debs-override-");
    if (workDir.empty() || debsOverrideDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create preinstall work dir\n");
        return false;
    }
    fs::create_directories(fs::path(workDir) / "out", mkEc);
    {
        std::ofstream f(workDir + "/preinstall_filenames.txt");
        for (const auto& fn : eligibleFilenames) f << fn << "\n";
    }
    {
        std::ofstream f(workDir + "/strip_postinst_packages.txt");
        for (const auto& name : stripPostinstPackages) f << name << "\n";
    }
    {
        // kPreinstallInnerScript is a plain shell script template using its
        // own real `$`/`{}` syntax throughout, so this is a targeted literal
        // substitution rather than treating it as an f-string-style
        // template (see this constant's own comment) — matches
        // scripts/build_deb_cache.py's identical INNER_SCRIPT.replace()
        // technique for the same placeholder. Replace EVERY occurrence, not
        // just the first: the constant's own explanatory comment right
        // above the real "Version:" line also mentions the placeholder by
        // name (to explain what it is), so a find()-once/replace-once pass
        // hit that comment instead of the real line, leaving the literal
        // token in the dpkg status stanza and making dpkg reject it
        // ("version number does not start with digit") — confirmed on a
        // real run, not hypothetical. Python's str.replace() already
        // replaces every occurrence by default, so build_deb_cache.py's
        // own identical substitution never had this bug.
        std::string innerScript = kPreinstallInnerScript;
        const std::string placeholder = "__FIRMWARE_VERSION__";
        for (size_t pos = innerScript.find(placeholder); pos != std::string::npos;
             pos = innerScript.find(placeholder, pos + firmwareVersion.size())) {
            innerScript.replace(pos, placeholder.size(), firmwareVersion);
        }
        std::ofstream f(workDir + "/inner.sh");
        f << innerScript;
    }
    std::string debsRoot = resolveDebsPath();
    bool stripOk = true;
    for (const auto& filename : stripPreinstFilenames) {
        stripOk &= stripPreinstFromDeb(debsRoot + "/" + filename, debsOverrideDir + "/" + filename);
    }
    if (!stripOk) {
        fprintf(stderr, "bakeRamdisk: failed to rebuild a preinst-stripped .deb\n");
        return false;
    }

    bool ok = runCommand({
        "/usr/bin/podman", "run", "--rm", "--security-opt", "label=disable",
        "-v", workDir + ":/work",
        "-v", fs::absolute(resolveDebsPath()).string() + ":/debs:ro",
        "-v", debsOverrideDir + ":/debs-override:ro",
        "-v", preinstallDir + ":/preinstall",
        "-v", outDir + ":/out",
        "debian:stretch", "sh", "/work/inner.sh",
    });
    if (!ok) {
        fprintf(stderr, "bakeRamdisk: bake-time dpkg preinstall failed (see podman output above)\n");
        return false;
    }
    return true;
}
#endif

// The fast, per-bake half of the mechanism above: merges an already-
// computed preinstall payload + dpkg state into `blackb0xRoot`. No
// network, no container, no dpkg invocation — just local file copies —
// which is exactly why the expensive computation above is worth caching
// across every firmware a single bake-all-ramdisks run bakes (see
// computeGlobalDebcacheOnce()) while this part still runs once per
// firmware, into that firmware's own /blackb0x.
static bool mergePreinstalledPackages(const fs::path& blackb0xRoot, const std::string& preinstallDir,
                                       const std::string& dpkgStateDir) {
    bool ok = true;
    ok &= mergeRealFilesystemTree(blackb0xRoot, "", preinstallDir);
    if (fs::exists(dpkgStateDir + "/status")) {
        ok &= stageFile(blackb0xRoot, "private/var/lib/dpkg/status", dpkgStateDir + "/status", 0, 0, 0644);
    }
    if (fs::exists(dpkgStateDir + "/info")) {
        ok &= mergeRealFilesystemTree(blackb0xRoot, "private/var/lib/dpkg/info", dpkgStateDir + "/info");
    }
    return ok;
}

// Everything scripts/build_deb_cache.py resolves, plus the bake-time
// preinstall computed from it, cached per real firmware version for the
// lifetime of this process (see computeGlobalDebcacheOnce()).
struct GlobalDebcacheResult {
    bool ok = false;
    std::vector<std::string> allFilenames;
    std::set<std::string> preinstallFilenames;
    std::vector<std::string> resolvedPackages;
    std::string aptListsDir;
    std::string preinstallPayloadDir;
    std::string dpkgStateDir;
    // build_deb_cache.py's local_only_debs.txt output (a real
    // dpkg-scanpackages Packages index + the loose .debs it describes) —
    // empty string if that run had no local-only entries to build one
    // for. See Blackb0x/Misc/apt/local.list's own comment for why this
    // needs a real generated index, not just cached .deb bytes.
    std::string localRepoDir;
};

// Runs scripts/build_deb_cache.py and the bake-time dpkg preinstall
// mechanism at most ONCE per distinct real firmware version, no matter how
// many (device, buildID) tuples this run bakes. Blackb0x/Misc/packages.txt's
// own resolution (and the real dpkg unpack/audit it feeds) is entirely
// firmware-independent EXCEPT for the synthetic "firmware" package's own
// declared version (see kPreinstallInnerScript's/INNER_SCRIPT's own
// comments) — the same apt repos, the same package set, but a package
// gated on `Depends: firmware (>= X)` can genuinely resolve differently
// depending on which real firmware is being declared. So this can no
// longer be a single process-wide result: it's cached per firmware-version
// string instead, keyed by the real, per-tuple ProductVersion the caller
// passes in. Two tuples that happen to share the same real firmware
// version (e.g. two different device models both actually running 6.1.3)
// still correctly reuse one resolution — re-running the whole
// apt-resolution + real-dpkg-container pipeline for each would just repeat
// identical network fetches, GPG verification, and container work for no
// reason, and risks a genuinely different result on different bakes in the
// same run if an upstream repo happens to change mid-run — but two tuples
// with genuinely different firmware versions each get their own real,
// independent resolution, which is the whole point of this fix. A
// function-local `static` map is exactly the right lifetime here: entries
// persist for as long as this one process runs (one bake-all-ramdisks
// invocation), and a fresh process (the next run) correctly recomputes
// from scratch. stageDebcache() calls this once per firmware and merges
// the (per-version) cached result into each bake's own /blackb0x.
static bool computeGlobalDebcacheOnce(const std::string& firmwareVersion, GlobalDebcacheResult& outResult) {
    static std::map<std::string, GlobalDebcacheResult> cache;
    auto existing = cache.find(firmwareVersion);
    if (existing != cache.end()) {
        outResult = existing->second;
        return existing->second.ok;
    }
    // Default-constructed (ok=false) until filled in below — every early
    // return on failure below leaves this cached as a real, negative result
    // for this exact firmware version, matching the previous single-result
    // cache's own failure-caching behavior.
    GlobalDebcacheResult& cached = cache[firmwareVersion];

    std::string tempDir = makeTempDir("blackb0x-debcache-");
    if (tempDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create debcache temp dir\n");
        outResult = cached;
        return false;
    }
    chownToInvokingUserIfSudo(tempDir);
#if defined(__APPLE__)
    // No container runtime on macOS (see computePreinstalledPackages()'s
    // own macOS branch above for the full story) — build_deb_cache.py
    // itself shells out to real apt-get inside a podman sandbox, so it
    // can't run here either. scripts/build_deb_cache_experimental_no_container.py
    // is the portable, no-container stand-in: same --output-dir/picklist.txt/
    // resolved_packages.txt contract (verified directly against its own
    // main()), just a plain local transitive-closure walk over Blackb0x/Debs/
    // instead of a real apt dependency solve — see that script's own module
    // docstring for its real, accepted gaps versus build_deb_cache.py. It
    // has no notion of firmware-version-gated Depends: at all (see its own
    // module docstring — parse_dependency_groups() strips version
    // constraints outright), so it takes no --firmware-version flag.
    bool ok = runAsInvokingUser(
        {"python3", "scripts/build_deb_cache_experimental_no_container.py", "--output-dir", tempDir});
    if (!ok) {
        fprintf(stderr, "bakeRamdisk: scripts/build_deb_cache_experimental_no_container.py failed\n");
        outResult = cached;
#else
    bool ok = runAsInvokingUser(
        {"python3", "scripts/build_deb_cache.py", "--output-dir", tempDir, "--firmware-version", firmwareVersion});
    if (!ok) {
        fprintf(stderr, "bakeRamdisk: scripts/build_deb_cache.py failed\n");
        outResult = cached;
#endif
        return false;
    }
    std::ifstream picklist(tempDir + "/picklist.txt");
    if (!picklist) {
        fprintf(stderr, "bakeRamdisk: build_deb_cache.py did not produce picklist.txt\n");
        outResult = cached;
        return false;
    }
    std::string debsRoot = resolveDebsPath();
    std::string line;
    while (std::getline(picklist, line)) {
        if (line.empty() || line[0] == '#') continue;
        cached.allFilenames.push_back(line);
    }

    // Which of these get unpacked + marked installed at bake time (real
    // dpkg state) vs. left as a plain cached .deb for postinstall.sh's own
    // apt-get to install for real — see computePreinstallEligibleFilenames()
    // and computePreinstalledPackages()'s own comments for the full
    // mechanism (prebake_package_blacklist.txt + its real dependency-graph
    // closure, then real era-appropriate dpkg + a dpkg/apt consistency
    // audit).
    std::set<std::string> stripPostinstPackages;
    std::set<std::string> stripPreinstFilenames;
    cached.preinstallFilenames = computePreinstallEligibleFilenames(cached.allFilenames, debsRoot,
                                                                      stripPostinstPackages, stripPreinstFilenames);

    if (!computePreinstalledPackages(cached.preinstallFilenames, stripPostinstPackages, stripPreinstFilenames,
                                      firmwareVersion, cached.preinstallPayloadDir, cached.dpkgStateDir)) {
        outResult = cached;
        return false;
    }

    std::ifstream resolvedPackages(tempDir + "/resolved_packages.txt");
    if (!resolvedPackages) {
        fprintf(stderr, "bakeRamdisk: build_deb_cache.py did not produce resolved_packages.txt\n");
        outResult = cached;
        return false;
    }
    while (std::getline(resolvedPackages, line)) {
        if (line.empty() || line[0] == '#') continue;
        cached.resolvedPackages.push_back(line);
    }

    cached.aptListsDir = tempDir + "/apt-lists";
    // Only present if local_only_debs.txt had entries this run — see that
    // file and Blackb0x/Misc/apt/local.list's own comments.
    if (fs::exists(tempDir + "/local-repo")) {
        cached.localRepoDir = tempDir + "/local-repo";
    }
    cached.ok = true;
    outResult = cached;
    return true;
}

// Stages this firmware's share of the (per-firmware-version cached)
// debcache result into `blackb0xRoot`: the non-preinstalled .deb set (minus
// kNeverStageDebs) into the real apt cache directory
// (private/var/cache/apt/archives/) — apt finds these itself via its
// normal cache-before-download check, no local file:// source or
// synthetic Packages index needed at all anymore (see
// scripts/build_deb_cache.py's own module docstring for why that whole
// mechanism is gone) — plus the preinstalled-package payload/dpkg-state
// and the apt lists cache. Returns the real, apt-resolved package NAMES
// (not .deb filenames) via `outResolvedPackages` for
// package/build.sh to template into postinstall.sh's install array.
// `firmwareVersion` is this bake's real, per-tuple ProductVersion (see
// stageBlackb0xTree()'s own caller) — threaded straight into
// computeGlobalDebcacheOnce() as both the cache key and the synthetic
// "firmware" package's own declared version.
// Real failure (the underlying computation failing, or producing no
// picklist) is treated as fatal for the whole bake, unlike a single
// missing loose asset elsewhere: a ramdisk with no packages to install
// can't actually finish the jailbreak.
static bool stageDebcache(const fs::path& blackb0xRoot, const std::string& firmwareVersion,
                           std::vector<std::string>& outResolvedPackages) {
    GlobalDebcacheResult result;
    if (!computeGlobalDebcacheOnce(firmwareVersion, result)) {
        return false;
    }

    std::string debsRoot = resolveDebsPath();
    bool allOk = true;
    for (const auto& filename : result.allFilenames) {
        if (result.preinstallFilenames.count(filename)) continue;  // handled by mergePreinstalledPackages() instead
        if (shouldSkipStagingDeb(filename)) {
            fprintf(stderr, "bakeRamdisk: not staging %s locally (too big for this ramdisk — network-only)\n",
                    filename.c_str());
            continue;
        }
        allOk &= stageFile(blackb0xRoot, "private/var/cache/apt/archives/" + filename, debsRoot + "/" + filename, 0,
                            0, 0644);
    }

    allOk &= mergePreinstalledPackages(blackb0xRoot, result.preinstallPayloadDir, result.dpkgStateDir);
    allOk &= stageAptListsCache(blackb0xRoot, result.aptListsDir);

    // The bundled local apt repo is NOT staged here -- it is
    // xyz.regulad.blackb0x package content, and package/build.sh builds it
    // itself from package/local_only_debs.txt (see its own comment for why no
    // picklist is involved: every local-only filename is in the picklist
    // unconditionally, so filtering against it would be a no-op).
    //
    // The debcache staged above (private/var/cache/apt/archives + apt-lists)
    // is a different thing entirely: the native apt cache, filled by the
    // baker with whatever could not usefully be pre-baked. Both happen to be
    // .debs, which is the only reason they were ever conflated.

    outResolvedPackages = result.resolvedPackages;
    return allOk;
}

// Hand-writes a real dpkg `status` stanza for a package whose persistence
// files stageEtasonatv()/stageP0sixspwn() below extract directly from its
// real .deb instead of running a genuine `dpkg --unpack`/`--configure` —
// merged (appended) into the exact same private/var/lib/dpkg/status
// stageDebcache()'s mergePreinstalledPackages() already staged earlier in
// stageBlackb0xTree() (see that function's own call ordering), not a
// separate/competing status file. Both callers' packages are firmware-
// version-gated (see scripts/build_deb_cache.py's
// KNOWN_EXPECTED_UNRESOLVABLE) and never reach computePreinstallEligibleFilenames()'s
// real `dpkg --unpack`+`--configure` pass at all, so without this dpkg has
// zero record either package exists — this is the "install it for real
// rather than a dpkg stub" fix for that gap.
//
// Deliberately does NOT carry over the real .deb's own Depends:/Conflicts:/
// Provides: fields. This project's own synthetic "firmware" version (see
// kPreinstallInnerScript's own comment) is exactly what makes these two
// packages' real Depends: firmware lines unsatisfiable — copying that field
// into a stanza that otherwise unconditionally claims "installed" would
// make the on-device dpkg/apt genuinely believe it has a broken package.
// postinstall.sh's own `apt-get install -f`/`upgrade`/`autoremove` run for
// real against this exact status file on a real device (under `set -e`):
// apt trying to "fix" a broken dependency by removing the package would hit
// each package's own prerm, which explicitly refuses removal (`exit 1`) —
// turning a cosmetic dependency mismatch into a hard postinstall.sh
// failure. Omitting Depends:/Conflicts:/Provides: avoids ever triggering
// that path; nothing in packages.txt's own closure references either
// package by name anyway, so nothing downstream needed those relationship
// fields to resolve correctly in the first place.
//
// `stanza` itself is expected to come from buildStatusStanzaFromControl()
// below — the real .deb's own control fields (Version, Architecture,
// Maintainer, Section, Installed-Size if the real control has one,
// Description, ...) copied verbatim, not hand-typed here, so this can't
// silently drift from whatever .deb actually ships. The empty `.md5sums`
// matches the "firmware" synthetic package's own precedent in
// kPreinstallInnerScript (dpkg --audit only checks a `.md5sums` file
// exists, never its content) — but `.list` here is real and non-empty
// (unlike firmware's, which has no files of its own), so `dpkg -S`/`-L`
// correctly attributes these real on-disk paths to their real package name
// once merge_tree() has written them onto the actual device (entrypoint.c,
// not this bake-time staging, creates usr/libexec/rtbuddyd for
// net.tihmstar.etasonuntether specifically — see that file's
// fixup_etasonuntether_rtbuddyd() — so it's listed here as an owned path
// even though it isn't one of the files this function itself stages).
static bool stageManualDpkgInstall(const fs::path& blackb0xRoot, const std::string& stanza,
                                    const std::string& pkgName, const std::vector<std::string>& ownedPaths) {
    fs::path statusPath = blackb0xRoot / "private/var/lib/dpkg/status";
    std::ofstream status(statusPath, std::ios::app);
    if (!status) {
        fprintf(stderr, "bakeRamdisk: cannot append dpkg status for %s at %s\n", pkgName.c_str(),
                statusPath.c_str());
        return false;
    }
    status << stanza << "\n";
    status.close();

    fs::path listPath = blackb0xRoot / ("private/var/lib/dpkg/info/" + pkgName + ".list");
    ensureParentDirs(blackb0xRoot, listPath);
    std::ofstream list(listPath, std::ios::trunc);
    if (!list) {
        fprintf(stderr, "bakeRamdisk: cannot write %s\n", listPath.c_str());
        return false;
    }
    for (const auto& p : ownedPaths) list << p << "\n";
    list.close();
    chmod(listPath.c_str(), 0644);
    chown(listPath.c_str(), 0, 0);

    fs::path md5Path = blackb0xRoot / ("private/var/lib/dpkg/info/" + pkgName + ".md5sums");
    std::ofstream md5(md5Path, std::ios::trunc);
    if (!md5) {
        fprintf(stderr, "bakeRamdisk: cannot write %s\n", md5Path.c_str());
        return false;
    }
    md5.close();
    chmod(md5Path.c_str(), 0644);
    chown(md5Path.c_str(), 0, 0);
    return true;
}

// Turns a real .deb's own extracted `control` file into a dpkg status
// stanza for stageManualDpkgInstall() above: injects `Status: install ok
// installed` (or, with `hold` set, `Status: hold ok installed`) right after
// the `Package:` line, and — when `dropRelationshipFields` is true (the
// default) — drops Depends:/Conflicts:/Provides: (plus their RFC822
// continuation lines — anything starting with a space/tab immediately
// following a dropped field) per stageManualDpkgInstall()'s own comment on
// why those three specifically aren't safe to carry over verbatim for its
// two firmware-version-gated callers. Every other real field (Version,
// Architecture, Maintainer, Section, Description, ...) is copied exactly as
// the real .deb's own control file has it, so this stanza can't silently
// drift from whatever .deb is actually sitting in Blackb0x/Debs/ the way a
// hand-typed literal duplicating those same fields could.
//
// `dropRelationshipFields=false` is for computePreinstalledPackages()'s
// macOS path (see that function's own comment): those packages are real,
// ordinary bake-time preinstalls with no firmware-version-gating weirdness
// to route around, so the faithful thing — matching what a real `dpkg
// --unpack`/`--configure` would actually have left in status — is to keep
// their real Depends:/Pre-Depends: verbatim, not strip them.
//
// `hold`'s "hold ok installed" is exactly the on-disk effect a real
// `apt-mark hold`/`dpkg --set-selections` run would produce (only the
// status file's `want` field — the first of its three space-separated
// words — changes; `apt-mark hold` doesn't touch anything else) — dpkg
// doesn't care how that word got there, so precomputing it here at bake
// time is equivalent to running the real tool, without needing a live
// device to actually run it against (apt-mark is a real ARM binary, see
// Blackb0x/Debs/apt7_*.deb — nothing on this Linux build host can execute
// it). stageEtasonatv() passes true: this project's own untether.bin
// deliberately differs from the real .deb's own payload (see that
// function's own comment), so letting postinstall.sh's later `apt-get
// upgrade`/`autoremove` ever silently "fix" this package back to a real
// resolved install would overwrite it with the wrong one.
static std::string buildStatusStanzaFromControl(const std::string& controlPath, bool hold = false,
                                                  bool dropRelationshipFields = true) {
    std::ifstream in(controlPath, std::ios::binary);
    if (!in) return std::string();

    static const std::vector<std::string> kDropFields = {"Depends:", "Conflicts:", "Provides:"};

    std::string out;
    std::string line;
    bool statusInjected = false;
    bool dropping = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF control files, just in case
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (dropping) continue;  // continuation of a dropped field
            out += line + "\n";
            continue;
        }
        dropping = false;
        if (dropRelationshipFields) {
            for (const auto& field : kDropFields) {
                if (line.rfind(field, 0) == 0) {
                    dropping = true;
                    break;
                }
            }
        }
        if (dropping) continue;
        out += line + "\n";
        if (!statusInjected && line.rfind("Package:", 0) == 0) {
            out += hold ? "Status: hold ok installed\n" : "Status: install ok installed\n";
            statusInjected = true;
        }
    }
    if (!statusInjected) return std::string();  // no Package: field — not a real control file
    return out;
}

// Result of extractDebAndBuildStanza() below.
struct ExtractedDeb {
    // Caller-owned either way — remove_all() this when done, whether `ok`
    // came back true or false (mirrors makeTempDir()'s own contract: only
    // empty if temp-dir creation itself failed, nothing to clean up then).
    std::string tempDir;
    // The dpkg status stanza built from the .deb's own control file — see
    // buildStatusStanzaFromControl(). Empty if control.tar.* couldn't be
    // found/extracted, or had no usable control file; this does NOT make
    // `ok` false (see below), since the payload files under `tempDir` are
    // still usable either way.
    std::string stanza;
    // True once data.tar.* has been located and extracted into `tempDir` —
    // i.e. whether the caller has anything left to stage at all. False
    // means the caller should give up immediately (no payload extracted);
    // true does NOT mean `stanza` is non-empty — that's a separate,
    // non-fatal-to-extraction failure the caller checks on its own, same as
    // both callers already did before this was factored out.
    bool ok = false;
};

// Shared by stageEtasonatv()/stageP0sixspwn() below — both need the exact
// same three-step dance against their own .deb: `ar x` it into a fresh temp
// dir, find+extract its data.tar.* (the actual persistence payload files,
// left sitting in `tempDir` for the caller to stageFile() individually
// afterward — every payload file lives at a different relative path per
// package, so this helper can't know which ones to stage on the caller's
// behalf), then find+extract its control.tar.* and turn its `control` file
// into a dpkg status stanza via buildStatusStanzaFromControl().
//
// Why this is hand-rolled at all, instead of the normal dpkg-based install
// pipeline every other .deb in Blackb0x/Debs/ goes through
// (stageDebcache() -> computePreinstallEligibleFilenames() -> a real `dpkg
// --unpack`/`--configure` in a container, or else a plain apt-cache entry
// for postinstall.sh's own apt-get to install on-device): both
// net.tihmstar.etasonuntether and com.ih8sn0w-squiffy-winocm.p0sixspwn are
// firmware-version-gated packages (Depends: firmware = <exact point
// release>) that can never resolve against this project's own synthetic
// "firmware" package (see kPreinstallInnerScript's own comment), so they
// never reach that normal pipeline's dependency resolution at all — apt/
// dpkg would consider them uninstallable, not merely un-preinstalled. On
// top of that, each needs bake-time treatment a plain install can't express
// either way: etasonuntether's stanza has to be written `hold`, because
// this project deliberately overrides the real .deb's own untether.bin with
// a different one (see stageEtasonatv()'s own comment below) and a later
// `apt-get upgrade` resolving the real package for real would silently
// clobber that override; and neither package's real postinst is safe to
// run at bake time at all (see stageManualDpkgInstall()'s own comment
// above) — it does live, on-device work (testing a real exploit against
// real hardware, or writing /etc/launchd.conf) that a mounted-but-not-
// booted image can't meaningfully execute. So rather than force either
// package through a pipeline built for unconditional, dependency-resolved
// installs, stageEtasonatv()/stageP0sixspwn() extract each .deb's real
// payload+control fields directly and hand-write the dpkg state
// stageManualDpkgInstall() produces — a special-cased, conditional install
// for the two packages that are exploit-critical enough to need one, not a
// hand-rolled reimplementation of dpkg for its own sake.
//
// Also reused, on macOS, by computePreinstalledPackages()'s own no-
// container path (see that function's own comment) — the exact same
// ar/tar extraction dance generalizes cleanly to every bake-time-eligible
// package, not just these two hand-picked ones. `dropRelationshipFields`
// is threaded straight through to buildStatusStanzaFromControl() — see
// its own comment for why that caller needs it false.
static ExtractedDeb extractDebAndBuildStanza(const std::string& debPath, const std::string& tempDirPrefix,
                                              const std::string& labelForLogging, bool hold = false,
                                              bool dropRelationshipFields = true) {
    ExtractedDeb result;
    result.tempDir = makeTempDir(tempDirPrefix);
    if (result.tempDir.empty()) return result;

    if (!fs::exists(debPath) || !runCommand({"ar", "x", debPath}, result.tempDir)) {
        fprintf(stderr, "bakeRamdisk: WARNING: failed to extract %s — %s not staged\n", debPath.c_str(),
                labelForLogging.c_str());
        return result;
    }

    std::string dataTar;
    std::error_code dirEc;
    for (const auto& e : fs::directory_iterator(result.tempDir, dirEc)) {
        if (e.path().filename().string().rfind("data.tar", 0) == 0) {
            dataTar = e.path().string();
            break;
        }
    }
    if (dataTar.empty() || !runCommand({"tar", "--auto-compress", "-xf", dataTar}, result.tempDir)) {
        fprintf(stderr, "bakeRamdisk: WARNING: failed to extract %s's data.tar.* — %s not staged\n", debPath.c_str(),
                labelForLogging.c_str());
        return result;
    }

    std::string controlTar;
    for (const auto& e : fs::directory_iterator(result.tempDir, dirEc)) {
        if (e.path().filename().string().rfind("control.tar", 0) == 0) {
            controlTar = e.path().string();
            break;
        }
    }
    if (controlTar.empty() || !runCommand({"tar", "--auto-compress", "-xf", controlTar}, result.tempDir)) {
        fprintf(stderr, "bakeRamdisk: WARNING: failed to extract %s's control.tar.* — dpkg state not staged\n",
                debPath.c_str());
    } else {
        result.stanza = buildStatusStanzaFromControl(result.tempDir + "/control", hold, dropRelationshipFields);
        if (result.stanza.empty()) {
            fprintf(stderr,
                    "bakeRamdisk: WARNING: %s's control.tar.* has no usable control file — dpkg state not staged\n",
                    debPath.c_str());
        }
    }

    result.ok = true;
    return result;
}

// iOS 8.4 branch — tihmstar's EtasonATV untether (see Blackb0x/Misc/README.md's
// "etasonATV / tihmstar-untether provenance" for the jsc/rtbuddyd/--early-boot
// mechanism this stages), extracted directly from the real
// net.tihmstar.etasonuntether .deb (Depends: firmware = 8.4.1, and its one
// real repo — repo.tihmstar.net — turned out to be an unreliable live apt
// dependency besides; see Blackb0x/Misc/apt/net.tihmstar.list.disabled and
// Blackb0x/Misc/local_only_debs.txt) rather than the old, hand-assembled
// tihmstar-untether.tar. Its real postinst never runs either way — see
// stageManualDpkgInstall() below. Only the four loose payload
// files plus the real dpkg state (stageManualDpkgInstall() above) are
// staged here — the package's own postinst does real, live work (testing
// the jsc stage1 exploit against a real device, then swapping
// usr/libexec/rtbuddyd for a jsc symlink) that can't run at bake time
// against a mounted-but-not-booted image, so it's never run through real
// apt/dpkg (see prebake_package_blacklist.txt). The `--early-boot` symlink
// below is the one piece of that postinst safe to replicate directly at
// bake time (no live device state involved, unconditional either way in
// the real postinst too). The rtbuddyd swap is NOT done here anymore —
// see entrypoint.c's fixup_etasonuntether_rtbuddyd(), which needs to run
// against the real target volume at install time, after this stanza's
// dpkg state has already told it this package is installed, not against
// this bake-time staging tree. uid 1000 / gid 985 on the /untether
// directory itself is ported verbatim from the original disassembly
// (FUN_00003b44) — an intentional, specific non-mobile/non-root ownership,
// not the same 755-decimal-literal bug the directory *mode* values
// elsewhere in this branch had (fixed to real 0755 here rather than
// propagated).
static bool stageEtasonatv(const fs::path& blackb0xRoot) {
    std::string debPath = fs::absolute(resolveDebsPath() + "/net.tihmstar.etasonuntether-1.3.1.deb").string();
    ExtractedDeb extracted = extractDebAndBuildStanza(debPath, "blackb0x-etasonatv-", "8.4 untether payload",
                                                        /*hold=*/true);
    if (!extracted.ok) {
        if (!extracted.tempDir.empty()) {
            std::error_code rmEc;
            fs::remove_all(extracted.tempDir, rmEc);
        }
        return false;
    }
    const std::string& tempDir = extracted.tempDir;
    const std::string& stanza = extracted.stanza;
    bool ok = true;
    ok &= stageFile(blackb0xRoot, "private/etc/rc.d/daemonload", tempDir + "/etc/rc.d/daemonload", 0, 0, 0755);
    ok &= stageFile(blackb0xRoot, "usr/bin/orphan_commander", tempDir + "/usr/bin/orphan_commander", 0, 0, 0755);
    stageDir(blackb0xRoot, "untether", 1000, 985, 0755);
    // The real .deb's own untether.bin is NOT staged here — see
    // Blackb0x/Misc/README.md's "etasonATV / tihmstar-untether provenance"
    // section: this project's own untether.bin (kept standalone at
    // Blackb0x/Misc/untether.bin once the original tarball that bundled it
    // was retired) checks against real AppleTV3 (S5L8947X) kernel banners
    // across several tvOS 8.4.x point releases, while the .deb's own build
    // never references that SoC at all — it's a generic multi-device
    // (iPhone4S/iPad2/iPad3/iPod5/iPhone5/iPad4) payload pinned to exactly
    // firmware 8.4.1. Using ours instead is deliberate, not a bug.
    ok &= stageFile(blackb0xRoot, "untether/untether.bin", resolveMiscPath("untether.bin"), 0, 0, 0644);
    ok &= stageFile(blackb0xRoot, "untether/expl.js", tempDir + "/untether/expl.js", 0, 0, 0644);
    stageSymlink(blackb0xRoot, "--early-boot", "/untether/expl.js");
    ok &= stageFile(blackb0xRoot, "Library/LaunchDaemons/xyz.regulad.blackb0x.postinstall.plist",
                     resolvePackagePath("System/Library/LaunchDaemons/xyz.regulad.blackb0x.postinstall.plist"), 0, 0, 0644);
    if (!stanza.empty()) {
        ok &= stageManualDpkgInstall(blackb0xRoot, stanza, "net.tihmstar.etasonuntether",
                                      {"/etc/rc.d/daemonload", "/usr/bin/orphan_commander", "/untether/untether.bin",
                                       "/untether/expl.js", "/--early-boot", "/usr/libexec/rtbuddyd"});
    } else {
        ok = false;
    }
    std::error_code rmEc;
    fs::remove_all(tempDir, rmEc);
    return ok;
}

// iOS 7.x/8.x (non-8.4) branch — swaps a custom replacement binary into
// /usr/libexec/dirhelper, the persistence exploit itself: real Apple/Cydia
// code never touches this path at all (see Misc/README.md's own
// "dirhelper here IS the file..." note for why this specific file, not
// p0sixspwn's differently-sourced same-named binary below, and Misc/
// dirhelper's own provenance writeup for what it actually is).
static bool stageIos7Tether(const fs::path& blackb0xRoot) {
    bool ok = stageFile(blackb0xRoot, "usr/libexec/dirhelper", resolveMiscPath("dirhelper"), kUidMobile,
                         kGidStaff, 0755);
    ok &= stageFile(blackb0xRoot, "System/Library/LaunchDaemons/xyz.regulad.blackb0x.postinstall.plist",
                     resolvePackagePath("System/Library/LaunchDaemons/xyz.regulad.blackb0x.postinstall.plist"), 0, 0, 0644);
    return ok;
}

// 6.1.4 branch — p0sixspwn's own untether payload, extracted directly from
// the real .deb rather than loose files (superseding the old, deleted
// p0sixspwn.tgz — see Misc/README.md's "Dropped entirely" section). The
// three payload files p0sixspwn's persistence exploit itself needs in
// place before reboot are staged here, plus the real dpkg state
// (stageManualDpkgInstall() above) — this package never actually resolves
// through real apt (see scripts/build_deb_cache.py's
// KNOWN_EXPECTED_UNRESOLVABLE), so without that call dpkg would have zero
// record it's installed at all, same gap net.tihmstar.etasonuntether had.
//
// Its own /etc/launchd.conf — the real, load-bearing mechanism (`unload
// MobileFileIntegrity` -> remount rw -> `DYLD_INSERT_LIBRARIES=_.dylib` ->
// `bsexec untether` -> reload) that actually makes these staged files run
// at every boot — is still NOT staged anywhere in this codebase. An
// earlier version of this comment claimed the real .deb "registers all of
// that properly once postinstall.sh actually installs it through apt,"
// but that's not what happens: this package is firmware-gated (see
// scripts/build_deb_cache.py's KNOWN_EXPECTED_UNRESOLVABLE), so it's never
// even in postinstall.sh's own install array, and its postinst never runs
// as a result — a different mechanism than net.tihmstar.etasonuntether
// above (which DOES reach that array now, via local_only_debs.txt, but
// whose postinst still never runs either, since apt finds it already at
// the exact held Status/Version stageManualDpkgInstall() wrote and treats
// the real install as a no-op). Reimplementing that
// /etc/launchd.conf write is tracked as an open TODO (see .claude/TODO.md)
// rather than attempted blind here — nobody on this project currently has
// the 6.1.3/6.1.4-era AppleTV2,1 hardware this branch targets to verify
// against.
static bool stageP0sixspwn(const fs::path& blackb0xRoot) {
    std::string debPath = fs::absolute(resolveDebsPath() + "/com.ih8sn0w-squiffy-winocm.p0sixspwn_1.4-1_iphoneos-arm.deb").string();
    ExtractedDeb extracted = extractDebAndBuildStanza(debPath, "blackb0x-p0sixspwn-", "6.1.4 untether payload");
    if (!extracted.ok) {
        if (!extracted.tempDir.empty()) {
            std::error_code rmEc;
            fs::remove_all(extracted.tempDir, rmEc);
        }
        return false;
    }
    const std::string& tempDir = extracted.tempDir;
    const std::string& stanza = extracted.stanza;
    bool ok = true;
    ok &= stageFile(blackb0xRoot, "usr/libexec/dirhelper", tempDir + "/usr/libexec/dirhelper", 0, 0, 0755);
    stageDir(blackb0xRoot, "private/var/untether", 0, 0, 0755);
    ok &= stageFile(blackb0xRoot, "private/var/untether/_.dylib", tempDir + "/var/untether/_.dylib", 0, 0, 0644);
    ok &= stageFile(blackb0xRoot, "private/var/untether/untether", tempDir + "/var/untether/untether", 0, 0, 0755);
    ok &= stageFile(blackb0xRoot, "System/Library/LaunchDaemons/xyz.regulad.blackb0x.postinstall.plist",
                     resolvePackagePath("System/Library/LaunchDaemons/xyz.regulad.blackb0x.postinstall.plist"), 0, 0, 0644);
    if (!stanza.empty()) {
        ok &= stageManualDpkgInstall(blackb0xRoot, stanza, "com.ih8sn0w-squiffy-winocm.p0sixspwn",
                                      {"/usr/libexec/dirhelper", "/var/untether/_.dylib", "/var/untether/untether"});
    } else {
        ok = false;
    }
    std::error_code rmEc;
    fs::remove_all(tempDir, rmEc);
    return ok;
}

// Picks exactly one of the three known persistence payloads based on this
// firmware's own ProductVersion — replicates entrypoint.c's old runtime
// version[0]/version[2]/version[4] checks exactly (see docs/HISTORY.md),
// just evaluated once here against a real ProductVersion string instead of
// on-device against a hand-rolled plist scan. A version matching none of
// the three is not fatal — the common content (Cydia dirs, apt sources,
// debcache) staged by stageBlackb0xTree() below is still produced, just
// with no device-specific persistence payload on top, so this only warns.
static bool stageVersionBranch(const fs::path& blackb0xRoot, const std::string& productVersion) {
    if (productVersion.rfind("8.4", 0) == 0) {
        return stageEtasonatv(blackb0xRoot);
    }
    if (!productVersion.empty() && (productVersion[0] == '8' || productVersion[0] == '7')) {
        return stageIos7Tether(blackb0xRoot);
    }
    if (productVersion.rfind("6.1.4", 0) == 0) {
        return stageP0sixspwn(blackb0xRoot);
    }
    fprintf(stderr,
            "bakeRamdisk: WARNING: ProductVersion '%s' matches no known persistence payload — staging common "
            "content only\n",
            productVersion.c_str());
    return true;
}

// Builds the real xyz.regulad.blackb0x .deb and installs it into the staged
// /blackb0x tree, which is what most of this file used to do by hand with a
// dozen individual stageFile() calls (.claude/TODO.md item 11).
//
// The package needs no state beyond its own package/ tree and the bundled
// local repo: package/build.sh templates postinstall.sh's install list from
// package/packages.txt itself, not from this bake's resolved closure, so this
// is a plain package build that happens to run during a bake rather than
// something entangled with it.
//
// Installing it is the same mechanism stageEtasonatv()/stageP0sixspwn()
// already use for packages that never go through real apt: extract the .deb,
// merge its payload into /blackb0x, and append a real dpkg status stanza so
// the on-device dpkg has a genuine record of it. Every file in the .deb is
// uid 0 / gid 0 (dm.pl records the container-side root), which is exactly
// what the LaunchDaemon plist needs -- launchd refuses to load a plist that
// is not root-owned -- and mergeRealFilesystemTree() preserves that.
//
// Top-level etc/ and var/ are remapped to private/etc/ and private/var/. The
// .deb ships them unprefixed, which is right for dpkg on-device (iOS's /etc
// and /var are symlinks into /private), but /blackb0x is a flat mirror that
// entrypoint.c replicates literally, so the real paths are used here.
static bool stageBlackb0xPackage(const fs::path& blackb0xRoot, const std::string& productVersion) {
    std::string stagingDir = makeTempDir("blackb0x-package-stage-");
    std::string outDir = makeTempDir("blackb0x-package-out-");
    if (stagingDir.empty() || outDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create package build temp dirs\n");
        return false;
    }

    bool ok = true;
    std::error_code ec;
    fs::copy(resolvePackagePath(""), stagingDir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                 fs::copy_options::copy_symlinks,
             ec);
    if (ec) {
        fprintf(stderr, "bakeRamdisk: cannot copy %s into the package staging tree: %s\n",
                resolvePackagePath("").c_str(), ec.message().c_str());
        fs::remove_all(stagingDir, ec);
        fs::remove_all(outDir, ec);
        return false;
    }

    // build.sh builds the bundled local repo itself, straight from
    // package/local_only_debs.txt, and generates its Packages index with the
    // real dpkg-scanpackages inside the container. Only the .deb source
    // directory has to be pointed at, since it is not under package/.
    setenv("BLACKB0X_DEBS_DIR", fs::absolute(resolveDebsPath()).c_str(), 1);

    std::string debPath = outDir + "/xyz.regulad.blackb0x.deb";
    if (!runCommand({resolvePackageRoot() + "/build.sh", stagingDir, debPath, productVersion}, ".")) {
        fprintf(stderr, "bakeRamdisk: package/build.sh failed — see stderr above\n");
        fs::remove_all(stagingDir, ec);
        fs::remove_all(outDir, ec);
        return false;
    }

    ExtractedDeb extracted = extractDebAndBuildStanza(debPath, "blackb0x-package-install-",
                                                       "xyz.regulad.blackb0x");
    if (!extracted.ok || extracted.stanza.empty()) {
        fprintf(stderr, "bakeRamdisk: could not extract/describe the built xyz.regulad.blackb0x .deb\n");
        if (!extracted.tempDir.empty()) fs::remove_all(extracted.tempDir, ec);
        fs::remove_all(stagingDir, ec);
        fs::remove_all(outDir, ec);
        return false;
    }

    std::vector<std::string> ownedPaths;
    for (const auto& entry : fs::directory_iterator(extracted.tempDir, ec)) {
        std::string name = entry.path().filename().string();
        if (name == "control" || name == "debian-binary" || name.rfind("control.tar", 0) == 0 ||
            name.rfind("data.tar", 0) == 0) {
            continue;
        }
        std::string destRel = name;
        if (name == "etc" || name == "var") destRel = "private/" + name;
        ok &= mergeRealFilesystemTree(blackb0xRoot, destRel, entry.path());
    }

    // Record the real on-device paths, which are the .deb's own (unprefixed)
    // ones -- not the private/-prefixed spellings used inside /blackb0x.
    for (const char* p : {"/etc/apt/sources.list.d/regulad.list", "/etc/apt/sources.list.d/saurik.list",
                          "/etc/apt/sources.list.d/awkwardtv.list", "/etc/apt/sources.list.d/bigboss.list",
                          "/etc/apt/sources.list.d/xbmc.list", "/etc/apt/sources.list.d/local.list",
                          "/etc/apt/trusted.gpg.d/regulad.gpg", "/etc/apt/trusted.gpg.d/saurik.gpg",
                          "/etc/apt/trusted.gpg.d/awkwardtv.gpg", "/etc/apt/trusted.gpg.d/bigboss.gpg",
                          "/System/Library/LaunchDaemons/xyz.regulad.blackb0x.postinstall.plist",
                          "/var/.blackb0x/postinstall.sh", "/var/root/.profile"}) {
        ownedPaths.push_back(p);
    }
    ok &= stageManualDpkgInstall(blackb0xRoot, extracted.stanza, "xyz.regulad.blackb0x", ownedPaths);

    fs::remove_all(extracted.tempDir, ec);
    fs::remove_all(stagingDir, ec);
    fs::remove_all(outDir, ec);
    return ok;
}

// Builds /blackb0x under `parentDir` (a plain host directory — this no
// longer has to be a mounted HFS+ volume at all; bakeRamdisk() stages this
// into a host temp dir first specifically so its real size is known
// before the destination volume gets created, then `cp -a`s the result
// into place) — everything entrypoint.c's merge_tree() will blindly
// replicate onto /mnt1 at boot, with every entry already carrying its
// correct final owner/mode. /blackb0x itself is root:wheel 0755, the same
// convention every other top-level pristine-ramdisk directory uses.
static bool stageBlackb0xTree(const std::string& parentDir, const std::string& productVersion) {
    fs::path blackb0xRoot = fs::path(parentDir) / "blackb0x";
    std::error_code ec;
    fs::create_directory(blackb0xRoot, ec);
    chmod(blackb0xRoot.c_str(), 0755);
    chown(blackb0xRoot.c_str(), 0, 0);

    bool ok = true;

    // Bare directories the device needs to exist with specific ownership,
    // which a .deb payload does not express well (they hold no files of ours).
    stageDir(blackb0xRoot, "private/etc/ssh", kUidMobile, kGidStaff, 0700);
    for (const auto& dir : kCydiaDirs) {
        stageDir(blackb0xRoot, dir, kUidMobile, kGidStaff, 0755);
    }

    // net.tihmstar's keyring is staged loose, alone among the repo keys: it
    // is the one with no matching source list (only
    // Blackb0x/Misc/apt/net.tihmstar.list.disabled exists), so it is not
    // package content. Kept so the .deb stays verifiable if the live repo is
    // ever re-enabled by restoring that list.
    ok &= stageFile(blackb0xRoot, "private/etc/apt/trusted.gpg.d/net.tihmstar.gpg",
                     resolveMiscPath("apt/net.tihmstar.gpg"), 0, 0, 0644);

    // Everything else blackb0x installs -- the apt sources and keyrings,
    // postinstall.sh, the first-boot LaunchDaemon, /var/root/.profile and the
    // bundled local repo -- arrives as one real .deb instead of a dozen
    // stageFile() calls dpkg had no record of.
    std::vector<std::string> resolvedPackages;
    if (!stageDebcache(blackb0xRoot, productVersion, resolvedPackages)) ok = false;
    if (!stageBlackb0xPackage(blackb0xRoot, productVersion)) ok = false;
    if (!stageVersionBranch(blackb0xRoot, productVersion)) ok = false;

    return ok;
}

// patchRamdisk() (as it was, before this file existed) went through three
// designs before this one. Recording why, since the investigation was
// expensive and the wrong lesson ("just patch the bug") would be easy for a
// future reader to draw:
//
// 1. In-memory xpwn Volume (add_hfs()/grow_hfs(), no mount at all — this
//    matched the original port plan's intent exactly, avoiding any
//    OS-level mount/loopback device). Real-data testing against a
//    downloaded AppleTV2,1 11D258 RestoreRamdisk got most of the way
//    through — the full baseline+overlay content set injected correctly,
//    after separately fixing a UF_COMPRESSED-overwrite crash — before
//    hitting hfs_panic("BTree inconsistent!") partway through the debs.
//    Two gdb backtraces confirmed this is a genuine, pre-existing bug in
//    xpwn's own catalog-B-tree *growth* code: grow_hfs() only resizes the
//    overall volume bitmap, not the catalog file's own B-tree extents, and
//    the organic-growth path that kicks in once the catalog needs to grow
//    past its initial capacity has never been exercised enough by xpwn's
//    own, much smaller, reference tools to catch this. Not a bug in the
//    ported code.
// 2. libhfsp (Debian's `hfsplus`/`libhfsp-dev` package — a from-scratch
//    userspace HFS+ reader/writer, again no mount). Ruled out even faster:
//    a single hpmkdir() call, no file copy at all, on a freshly-
//    mkfs.hfsplus'd volume already corrupted an extent entry into the
//    reserved alternate-volume-header block, confirmed via fsck.hfsplus and
//    root-caused in libhfsp's own source (libhfsp/src/volume.c) to a bounds
//    check against exactly that reserved region existing in the source only
//    as commented-out dead code, in all three block-allocation functions.
//    libhfsp's own ChangeLog calls its 1.0.1 release (2000) "a stable,
//    readonly implementation" and never claims the write path reached that
//    bar through its final 1.0.4 release (2002).
// 3. The Linux kernel's own, actively-maintained, in-tree `hfsplus` driver
//    (fs/hfsplus/) — this DOES require a real loop mount, which the
//    original port plan explicitly wanted to avoid, but real functional
//    testing (loop-mounting a blank mkfs.hfsplus volume and copying the
//    *entire* real payload set onto it) confirmed it handles exactly the
//    catalog growth that broke both designs above, with a clean
//    fsck.hfsplus verdict and a byte-for-byte content match against the
//    source. This is what's actually used below — and is exactly why this
//    logic lives in its own binary instead of the main blackb0x one: it's
//    the only piece of this tool that needs CAP_SYS_ADMIN/CAP_CHOWN.
//
// GUTTED, then partially rebuilt (see git history for both the original,
// much larger version and the fully-gutted one in between): this used to
// grow the volume and merge a whole ramdisk/ overlay + debcache directly
// onto the final destination paths. That's gone for good — the on-device
// job stays "just copy files and then die" — but the baking job does two
// things to the pristine ramdisk: overwrite ONE existing file's content in
// place (`/sbin/launchd`, real PID-1 on every known firmware — see
// docs/HISTORY.md's "Entrypoint injection point" entry for why this isn't
// `/etc/rc.boot`) via spliceFileContentInPlace(), and add one brand new
// top-level directory, `/blackb0x`, via stageBlackb0xTree() above. Every
// file the pristine ramdisk already shipped, including launchd's own
// permissions/ownership/timestamps, is either left completely untouched or
// has only its content overwritten in place. Since /blackb0x's real size
// isn't known ahead of time and generally doesn't fit in the pristine
// volume's own free space, the destination is actually a freshly created,
// correctly-sized volume (not the original, grown in place — see the
// "growing HFS+ in place is fundamentally broken on Linux" note further
// below), populated with the original content via `cp -a` before either of
// the two changes above are made.
bool bakeRamdisk(const std::string& path, const std::string& key, const std::string& iv,
                  const std::string& productVersion, const std::string& outputPath,
                  const std::string& entrypointBinaryPath, bool& outSizeWarning) {
    outSizeWarning = false;

    // Resolved here, at the very top, even though it is not used until the
    // finished image is measured at the end of this function. A bake takes
    // minutes; discovering a typo'd DEBUG_RAMDISK_LIMIT_MIB only after paying
    // for all of it would be its own small cruelty. See the size check at the
    // end of this function for what the values mean.
    int64_t limitMiB = 64;
    if (const char* limitOverride = getenv("DEBUG_RAMDISK_LIMIT_MIB")) {
        char* end = nullptr;
        long long parsed = strtoll(limitOverride, &end, 10);
        // Rejected rather than guessed: a typo that silently fell back to the
        // default would be indistinguishable from the knob working, and a 0
        // limit would fail every bake for no stated reason.
        if (end == limitOverride || *end != '\0' || parsed < -1 || parsed == 0) {
            fprintf(stderr,
                    "bakeRamdisk: DEBUG_RAMDISK_LIMIT_MIB=\"%s\" is not a valid limit (expected -1 to disable, "
                    "or a positive number of MiB)\n",
                    limitOverride);
            return false;
        }
        limitMiB = (int64_t)parsed;
    }

    // Real root is required on BOTH platforms, for different reasons — and
    // this needs to fail loudly up front rather than incidentally. On
    // Linux, the loop-mount below needs CAP_SYS_ADMIN and fails with its
    // own clear error if it isn't root (see the "are we running as root?"
    // hint further down). On macOS there's no mount to fail on — hdiutil
    // attach/create work fine unprivileged — but every chown() this file
    // calls while staging content (root:wheel for most of the tree,
    // mobile:staff for a few paths, and preserving each original file's
    // real owner while copying the pristine ramdisk's own content) targets
    // an arbitrary uid, which chown(2) silently refuses for a non-root
    // caller on any POSIX system, Darwin included, without the syscall
    // itself failing loudly. Left unchecked, that would silently bake a
    // ramdisk with the wrong ownership instead of failing — worse than
    // just requiring root outright.
    if (geteuid() != 0) {
#if defined(__APPLE__)
        fprintf(stderr,
                "bakeRamdisk: must run as root — chown() to root:wheel/mobile:staff (and to preserve the "
                "original ramdisk's own file ownership) requires real root on macOS too, even though hdiutil "
                "itself doesn't\n");
#else
        fprintf(stderr, "bakeRamdisk: must run as root — loop-mounting a real HFS+ image needs CAP_SYS_ADMIN\n");
#endif
        return false;
    }

    std::string decDMG = decryptedDMGFor(path);
    const std::string& patchedDMG = outputPath;

    fprintf(stderr, "Patching ramdisk...\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decDMG.c_str()), const_cast<char*>(key.c_str()),
            const_cast<char*>(iv.c_str()), (char*)"FALSE", nullptr);

    // NOTE: the original's "AppleTV2,1_4." branch rebuilds the ramdisk from
    // scratch via `hdiutil create ... -format UDRW` for the oldest ATV2 4.x
    // firmware — a materially different problem (no pre-existing content at
    // all) from splicing into an existing one. Flag and bail rather than
    // silently produce a broken ramdisk.
    if (path.find("AppleTV2,1_4.") != std::string::npos) {
        fprintf(stderr, "bakeRamdisk: AppleTV2,1 4.x ramdisk recreation is not implemented in this port\n");
        return false;
    }

    // Not every decrypted restore component is UDIF-wrapped: on this old
    // (A4-era) Apple TV 2/3 hardware, decrypting the RestoreRamDisk yields a
    // RAW HFS+ image directly (confirmed empirically — "H+" signature right
    // at the standard offset 0x400, no "koly" UDIF trailer at all), unlike
    // the UDIF-wrapped root-filesystem images third_party/xpwn's own
    // ipsw-patch/main.c reference code assumes. Detect which one this is
    // rather than assuming, and only extractDmg()/buildDmg() when genuinely
    // needed — the Linux kernel's hfsplus driver only understands raw
    // partition images, not Apple's UDIF/DMG wrapper, so a genuinely
    // UDIF-wrapped image still needs unwrapping before it can be mounted.
    bool isUDIF = false;
    {
        std::ifstream probe(decDMG, std::ios::binary);
        if (probe) {
            probe.seekg(0, std::ios::end);
            std::streamoff size = probe.tellg();
            if (size >= 512) {
                probe.seekg(size - 512);
                char magic[4] = {0};
                probe.read(magic, 4);
                isUDIF = (memcmp(magic, "koly", 4) == 0);
            }
        }
    }

    std::string rawImgPath = decDMG + ".raw.hfs";
    if (isUDIF) {
        FILE* decFile = fopen(decDMG.c_str(), "rb");
        if (!decFile) {
            fprintf(stderr, "bakeRamdisk: cannot open %s\n", decDMG.c_str());
            return false;
        }
        void* rawBuffer = nullptr;
        size_t rawSize = 0;
        AbstractFile* decryptedAbs = createAbstractFileFromFile(decFile);
        AbstractFile* rawOut = createAbstractFileFromMemoryFile(&rawBuffer, &rawSize);
        extractDmg(decryptedAbs, rawOut, -1);

        std::ofstream out(rawImgPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            fprintf(stderr, "bakeRamdisk: cannot write %s\n", rawImgPath.c_str());
            free(rawBuffer);
            return false;
        }
        out.write((const char*)rawBuffer, (std::streamsize)rawSize);
        free(rawBuffer);
    } else {
        std::error_code ec;
        fs::copy_file(decDMG, rawImgPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fprintf(stderr, "bakeRamdisk: cannot copy %s (%s)\n", decDMG.c_str(), ec.message().c_str());
            return false;
        }
    }

    // /blackb0x is a brand new top-level entry the pristine ramdisk never
    // had — potentially tens of MB once the debcache/apt-lists/bake-time
    // preinstall payload are all in it — and this old volume was never
    // sized to hold anything beyond what Apple originally shipped. Growing
    // an existing HFS+ volume in place isn't viable on Linux (every tool
    // tried — in-memory xpwn grow_hfs(), libhfsp, libparted-fs-resize —
    // turned out broken/unsupported for exactly this operation; see the
    // design-history comment below patchRamdisk() used to carry, and
    // docs/HISTORY.md), so instead: build a brand new volume sized to
    // actually fit.
    //
    // "Actually fit" needs a real number, and there isn't a cheap way to
    // get one ahead of time — `du` on a plain host directory (ext4, tmpfs,
    // whatever) reports that filesystem's own block-rounded usage, not
    // HFS+'s, and the gap between the two (per-file allocation-block
    // rounding across a tree with thousands of small files — terminfo
    // entries, dpkg's per-package .list/.md5sums, etc. — plus HFS+'s own
    // catalog/allocation-file overhead) turned out to be large enough in
    // practice to make that estimate genuinely unreliable: real bakes with
    // it undercounted by several MB, not a rounding error. So instead of
    // estimating, this assembles the real final content — original ramdisk
    // + spliced launchd + /blackb0x — onto a generously oversized scratch
    // HFS+ volume first, and asks that mounted HFS+ filesystem itself how
    // much space its own content actually used (`du` against a live HFS+
    // mount reports real HFS+ block counts via stat(), same as any other
    // real, already-existing file on it — no guessing involved). That real
    // number is what actually sizes the final volume; the scratch one is
    // discarded immediately after.
#if defined(__APPLE__)
    // macOS build sequence — see this function's own header comment above
    // for the full "three designs tried" history behind Linux's three-
    // mount dance. That history doesn't apply here: those were all real,
    // confirmed bugs in Linux-side HFS+ *write* tooling (xpwn's own
    // grow_hfs(), libhfsp), not anything intrinsic to HFS+ itself, so this
    // instead generalizes the original, pre-port Objective-C
    // implementation's one legacy special case (AppleTV2,1 4.x only:
    // `hdiutil create -srcfolder <mounted-original> -format UDRW -layout
    // NONE`, synthesizing a whole new volume from a folder tree in one
    // step — see origin/main:Blackb0x/Source/Patcher.mm's
    // patchRamdisk:ssh:, lines 335-516) into the GENERAL mechanism here,
    // combined with this file's own real improvement over that original:
    // computing the actual needed volume size dynamically
    // (directoryContentSize() below) instead of a hardcoded 60MB/40MB
    // guess. Only ONE real mount is needed (the original, read-only) —
    // Linux's second "scratch" HFS+ mount exists only to let a real,
    // mounted HFS+ filesystem's own `du` measure real block-rounded usage;
    // directoryContentSize() is now a portable std::filesystem walk that
    // works directly against a plain host directory, so the assembled
    // original+launchd+/blackb0x content is staged straight onto one
    // instead of a second mounted volume.
    MountGuard origMount;
    origMount.mountpoint = makeTempDir("blackb0x-origmnt-");
    if (origMount.mountpoint.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create a temp mountpoint\n");
        return false;
    }
    if (!runCommand(
            {"hdiutil", "attach", "-mountpoint", origMount.mountpoint, "-nobrowse", "-readonly", rawImgPath})) {
        fprintf(stderr, "bakeRamdisk: failed to attach original ramdisk via hdiutil\n");
        return false;
    }
    origMount.mounted = true;

    // No blkid equivalent on Darwin — read back via `diskutil info -plist`
    // instead, which only works against an attached device, so (unlike
    // Linux's readVolumeLabel(), which reads the raw image file directly
    // and so can run before mounting) this has to run after the attach
    // above.
    std::string label = readVolumeLabelMac(origMount.mountpoint);

    // A plain host directory — no second mounted HFS+ volume needed here,
    // see this branch's own header comment above. Reuses this file's own
    // makeTempDir() helper, same as every other scratch directory
    // bakeRamdisk() creates.
    std::string stagingDir = makeTempDir("blackb0x-stage-");
    if (stagingDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create staging dir\n");
        return false;
    }
    if (!runCommand({"cp", "-a", origMount.mountpoint + "/.", stagingDir + "/"})) {
        fprintf(stderr, "bakeRamdisk: failed to copy original ramdisk contents\n");
        return false;
    }
    if (!runCommand({"hdiutil", "detach", origMount.mountpoint})) {
        fprintf(stderr, "bakeRamdisk: failed to detach original ramdisk\n");
        return false;
    }
    origMount.mounted = false;

    std::string blackb0xStagingDir = makeTempDir("blackb0x-payload-");
    if (blackb0xStagingDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create /blackb0x staging dir\n");
        return false;
    }
    if (!stageBlackb0xTree(blackb0xStagingDir, productVersion)) {
        fprintf(stderr, "bakeRamdisk: failed to stage /blackb0x\n");
        return false;
    }

    // The one and only content change to anything the pristine ramdisk
    // already shipped — see spliceFileContentInPlace()'s own comment for
    // why a blind overwrite isn't good enough. The `cp -a` above already
    // carried over launchd's real permissions onto this staged copy, so
    // splicing against a plain directory here works exactly the same as
    // splicing against a mounted volume does on Linux.
    if (!spliceFileContentInPlace(stagingDir + "/sbin/launchd", entrypointBinaryPath)) {
        return false;
    }
    if (!runCommand({"cp", "-a", blackb0xStagingDir + "/blackb0x", stagingDir + "/blackb0x"})) {
        fprintf(stderr, "bakeRamdisk: failed to move staged /blackb0x into place\n");
        return false;
    }
    std::error_code stagingRmEc;
    fs::remove_all(blackb0xStagingDir, stagingRmEc);

    // The real number: how much space the assembled content actually
    // needs — see directoryContentSize()'s own comment. Same margin
    // computation Linux uses below, just pointed at this plain staging
    // directory instead of a live HFS+ mount.
    uint64_t realContentSize = directoryContentSize(stagingDir);
    constexpr uint64_t kFlatSizeMargin = 4ull * 1024 * 1024;
    uint64_t percentSizeMargin = realContentSize / 10;
    uint64_t newVolumeSize = realContentSize + std::max(kFlatSizeMargin, percentSizeMargin);

    std::string newRawImgPath = decDMG + ".new-raw.hfs";
    std::error_code newRawRmEc;
    fs::remove(newRawImgPath, newRawRmEc);
    // UNVERIFIED against real hdiutil — this file could only be written
    // and read over, never actually run, on this (Linux) build host, with
    // no macOS available at all. Two specific things here to check against
    // real hardware before trusting this blindly:
    //   1. The exact -fs argument string for case-sensitive HFS+.
    //      "Case-sensitive HFS+" is believed correct (it's the display
    //      name macOS's own Disk Utility/hdiutil use for this format on
    //      modern macOS), but if a real bake on real macOS hardware
    //      rejects it, check `hdiutil create -help`'s own -fs listing on
    //      that machine and correct it here. Case sensitivity itself is
    //      NOT optional, though — see the Linux mkfs.hfsplus -s call's own
    //      comment further down (real ncurses terminfo e/E collisions
    //      under case-insensitive lookup).
    //   2. Whether `-format UDRW -layout NONE`'s real output is genuinely
    //      flat raw bytes (what copyVolumeHeaderMetadata() below, and the
    //      shared UDIF-rewrap step further down, both assume) or itself
    //      carries additional UDIF structure beyond a trailing "koly"
    //      block. unwrapUDIFIfPresent() right below is a defensive,
    //      no-op-if-already-raw safety net for exactly that uncertainty,
    //      reusing the same probe+extractDmg() technique this function
    //      already runs on the original decrypted firmware component
    //      above.
    if (!runCommand({"hdiutil", "create", "-srcfolder", stagingDir, "-volname", label, "-fs",
                      "Case-sensitive HFS+", "-format", "UDRW", "-layout", "NONE", "-size",
                      std::to_string(newVolumeSize) + "b", newRawImgPath})) {
        fprintf(stderr, "bakeRamdisk: hdiutil create failed on %s\n", newRawImgPath.c_str());
        return false;
    }
    if (!unwrapUDIFIfPresent(newRawImgPath)) {
        fprintf(stderr, "bakeRamdisk: failed to unwrap hdiutil's own output at %s\n", newRawImgPath.c_str());
        return false;
    }
    copyVolumeHeaderMetadata(rawImgPath, newRawImgPath);

    std::error_code stagingDirRmEc;
    fs::remove_all(stagingDir, stagingDirRmEc);
#else
    MountGuard origMount;
    origMount.mountpoint = makeTempDir("blackb0x-origmnt-");
    if (origMount.mountpoint.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create a temp mountpoint\n");
        return false;
    }
    if (!runCommand({"mount", "-t", "hfsplus", "-o", "loop,ro", rawImgPath, origMount.mountpoint})) {
        fprintf(stderr, "bakeRamdisk: failed to mount original ramdisk (are we running as root?)\n");
        return false;
    }
    origMount.mounted = true;

    std::string label = readVolumeLabel(rawImgPath);

    std::string blackb0xStagingDir = makeTempDir("blackb0x-payload-");
    if (blackb0xStagingDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create /blackb0x staging dir\n");
        return false;
    }
    if (!stageBlackb0xTree(blackb0xStagingDir, productVersion)) {
        fprintf(stderr, "bakeRamdisk: failed to stage /blackb0x\n");
        return false;
    }

    // Comfortably larger than kMaxRamdiskSize below on purpose — this
    // volume is never shipped, just measured and thrown away, so it only
    // needs enough headroom that real content never fails to fit here
    // regardless of how big /blackb0x turns out to be. If it doesn't fit
    // even in this, the `cp -a` calls below fail loudly on their own.
    constexpr uint64_t kScratchWorkingSize = 256ull * 1024 * 1024;
    std::string scratchRawImgPath = decDMG + ".scratch-raw.hfs";
    {
        std::ofstream create(scratchRawImgPath, std::ios::binary | std::ios::trunc);
        if (!create) {
            fprintf(stderr, "bakeRamdisk: cannot create %s\n", scratchRawImgPath.c_str());
            return false;
        }
    }
    std::error_code scratchSizeEc;
    fs::resize_file(scratchRawImgPath, kScratchWorkingSize, scratchSizeEc);
    if (scratchSizeEc) {
        fprintf(stderr, "bakeRamdisk: cannot size scratch volume (%s)\n", scratchSizeEc.message().c_str());
        return false;
    }
    // -s: case-sensitive filenames, matching the real iOS/tvOS root
    // filesystem (HFSX, not plain case-insensitive HFS+) — without it,
    // mkfs.hfsplus defaults to case-insensitive, which broke a real cp -a
    // here: ncurses' real terminfo database ships sibling first-letter
    // buckets like `e`/`E` and `a`/`A` (genuinely distinct terminal names
    // differing only in case), which collide under case-insensitive
    // lookup ("cannot create directory: File exists") but are exactly what
    // a case-sensitive volume is required to keep apart.
    if (!runCommand({"mkfs.hfsplus", "-s", "-v", label, scratchRawImgPath})) {
        fprintf(stderr, "bakeRamdisk: mkfs.hfsplus failed on %s\n", scratchRawImgPath.c_str());
        return false;
    }
    copyVolumeHeaderMetadata(rawImgPath, scratchRawImgPath);

    MountGuard scratchMount;
    scratchMount.mountpoint = makeTempDir("blackb0x-scratchmnt-");
    if (scratchMount.mountpoint.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create a temp mountpoint\n");
        return false;
    }
    if (!runCommand({"mount", "-t", "hfsplus", "-o", "loop", scratchRawImgPath, scratchMount.mountpoint})) {
        fprintf(stderr, "bakeRamdisk: failed to mount scratch ramdisk\n");
        return false;
    }
    scratchMount.mounted = true;

    // -a preserves permissions/ownership (including setuid bits — this is
    // a real Unix root filesystem, not just data files) and symlinks-as-
    // symlinks rather than following them.
    if (!runCommand({"cp", "-a", origMount.mountpoint + "/.", scratchMount.mountpoint + "/"})) {
        fprintf(stderr, "bakeRamdisk: failed to copy original ramdisk contents\n");
        return false;
    }
    if (!runCommand({"umount", origMount.mountpoint})) {
        fprintf(stderr, "bakeRamdisk: failed to unmount original ramdisk\n");
        return false;
    }
    origMount.mounted = false;

    // The one and only content change to anything the pristine ramdisk
    // already shipped: /sbin/launchd's bytes become the built entrypoint
    // binary, everything else about that catalog entry (mode/owner/group/
    // mtime) preserved as-is by spliceFileContentInPlace() — see its own
    // comment for why a blind overwrite isn't good enough. The `cp -a`
    // above already carried over launchd's real permissions onto this
    // scratch volume's copy, so splicing here works exactly the same as
    // splicing in place on the original would have.
    if (!spliceFileContentInPlace(scratchMount.mountpoint + "/sbin/launchd", entrypointBinaryPath)) {
        return false;
    }
    if (!runCommand({"cp", "-a", blackb0xStagingDir + "/blackb0x", scratchMount.mountpoint + "/blackb0x"})) {
        fprintf(stderr, "bakeRamdisk: failed to move staged /blackb0x into place\n");
        return false;
    }
    std::error_code stagingRmEc;
    fs::remove_all(blackb0xStagingDir, stagingRmEc);

    sync();
    // The real number: how much space the assembled content actually uses
    // on an actual HFS+ filesystem, not an estimate.
    uint64_t realContentSize = directoryContentSize(scratchMount.mountpoint);

    // A flat 2MB margin here (covering just fixed per-volume overhead —
    // volume header, alternate header, boot blocks, allocation bitmap) was
    // tried and measured short in practice, real bakes still hit "No space
    // left on device" partway through the final `cp -a` below. The likely
    // reason: `realContentSize` is measured on a spacious 256MB scratch
    // volume with plenty of contiguous free space to allocate into, but
    // the same files packed onto a destination volume with very little
    // slack left have much less room to lay out contiguously, so they
    // fragment into more extents — and each extra extent costs additional
    // catalog/extents-overflow B-tree records that don't show up in any
    // per-file byte count. A percentage-of-content margin (not just a
    // flat one) leaves proportionally more breathing room for that as
    // content grows. kMaxRamdiskSize below is only a warning, not a hard
    // ceiling, so there's no reason to cut this margin close.
    constexpr uint64_t kFlatSizeMargin = 4ull * 1024 * 1024;
    uint64_t percentSizeMargin = realContentSize / 10;
    uint64_t newVolumeSize = realContentSize + std::max(kFlatSizeMargin, percentSizeMargin);

    std::string newRawImgPath = decDMG + ".new-raw.hfs";
    {
        std::ofstream create(newRawImgPath, std::ios::binary | std::ios::trunc);
        if (!create) {
            fprintf(stderr, "bakeRamdisk: cannot create %s\n", newRawImgPath.c_str());
            return false;
        }
    }
    std::error_code volSizeEc;
    fs::resize_file(newRawImgPath, newVolumeSize, volSizeEc);
    if (volSizeEc) {
        fprintf(stderr, "bakeRamdisk: cannot size new volume (%s)\n", volSizeEc.message().c_str());
        return false;
    }
    // -s: see the scratch volume's mkfs.hfsplus call above for why.
    if (!runCommand({"mkfs.hfsplus", "-s", "-v", label, newRawImgPath})) {
        fprintf(stderr, "bakeRamdisk: mkfs.hfsplus failed on %s\n", newRawImgPath.c_str());
        return false;
    }
    copyVolumeHeaderMetadata(rawImgPath, newRawImgPath);

    MountGuard newMount;
    newMount.mountpoint = makeTempDir("blackb0x-newmnt-");
    if (newMount.mountpoint.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create a temp mountpoint\n");
        return false;
    }
    if (!runCommand({"mount", "-t", "hfsplus", "-o", "loop", newRawImgPath, newMount.mountpoint})) {
        fprintf(stderr, "bakeRamdisk: failed to mount new ramdisk\n");
        return false;
    }
    newMount.mounted = true;

    // Single copy of the already-fully-assembled scratch content (original
    // + spliced launchd + /blackb0x, all in one tree) onto the correctly-
    // sized final volume — nothing left to splice or merge separately here.
    if (!runCommand({"cp", "-a", scratchMount.mountpoint + "/.", newMount.mountpoint + "/"})) {
        fprintf(stderr, "bakeRamdisk: failed to copy assembled content onto the final volume\n");
        return false;
    }
    if (!runCommand({"umount", scratchMount.mountpoint})) {
        fprintf(stderr, "bakeRamdisk: failed to unmount scratch ramdisk\n");
        return false;
    }
    scratchMount.mounted = false;
    std::error_code scratchRmEc;
    fs::remove(scratchRawImgPath, scratchRmEc);

    sync();
    if (!runCommand({"umount", newMount.mountpoint})) {
        fprintf(stderr, "bakeRamdisk: failed to unmount ramdisk\n");
        return false;
    }
    newMount.mounted = false;
#endif

    std::error_code oldRawRmEc;
    fs::remove(rawImgPath, oldRawRmEc);
    rawImgPath = newRawImgPath;

    // Write the modified image back out, overwriting the decrypted file in
    // place (replaces `hdiutil detach`) — rewrapped into UDIF only if the
    // source was genuinely UDIF-wrapped to begin with; otherwise the raw
    // HFS+ bytes are used directly, matching what decrypt() actually handed
    // us.
    std::error_code rmEc;
    if (isUDIF) {
        std::ifstream rawFile(rawImgPath, std::ios::binary);
        if (!rawFile) {
            fprintf(stderr, "bakeRamdisk: cannot open %s\n", rawImgPath.c_str());
            return false;
        }
        std::ostringstream ss;
        ss << rawFile.rdbuf();
        std::string contents = ss.str();
        size_t rawSize = contents.size();
        void* rawBuffer = malloc(rawSize);
        memcpy(rawBuffer, contents.data(), rawSize);

        FILE* rebuiltFile = fopen(decDMG.c_str(), "wb");
        if (!rebuiltFile) {
            fprintf(stderr, "bakeRamdisk: cannot write %s\n", decDMG.c_str());
            free(rawBuffer);
            return false;
        }
        AbstractFile* rebuiltOut = createAbstractFileFromFile(rebuiltFile);
        buildDmg(createAbstractFileFromMemoryFile(&rawBuffer, &rawSize), rebuiltOut, 2048);
        free(rawBuffer);
        fs::remove(rawImgPath, rmEc);
    } else {
        fs::remove(decDMG, rmEc);
        fs::rename(rawImgPath, decDMG, rmEc);
        if (rmEc) {
            fprintf(stderr, "bakeRamdisk: failed to move the patched image into place (%s)\n", rmEc.message().c_str());
            return false;
        }
    }

    std::error_code outDirEc;
    fs::create_directories(fs::path(patchedDMG).parent_path(), outDirEc);

    decrypt(const_cast<char*>(decDMG.c_str()), const_cast<char*>(patchedDMG.c_str()), const_cast<char*>(key.c_str()),
            const_cast<char*>(iv.c_str()), (char*)"FALSE", const_cast<char*>(path.c_str()));

    // Hard size limit on the finished, baked ramdisk.
    //
    // 64MiB is not a rule of thumb, and it is not an extrapolation from one
    // failure either: a real run against an AppleTV3,2 failed mid-upload with
    // a USB bulk short-write at exactly byte offset 0x4000000 on a 68.9MiB
    // ramdisk, and the ceiling has since been confirmed against that same live
    // device as either exactly 64MiB or extremely close to it.
    //
    // There is NO runtime check backing this up, which is precisely why the
    // bake-time one has to be real. The device cannot be asked: `ramdisk-size`
    // does not exist on 32-bit iBoot at all (measured -- zero occurrences in
    // the decrypted iBECs of both AppleTV2,1/iBoot-1537.9.55 and
    // AppleTV3,2/iBoot-1458.2), and neither carries the older 32-bit
    // "Ramdisk too large" / kRamdiskMaxSize mechanism either. See
    // DeviceManager.cpp's sendRamdiskSizeGetenv() for the full evidence.
    //
    // No evidence the A4 ceiling differs from A5's: the two iBECs have
    // byte-identical ramdisk and size-check string sets, and A4's iBoot is
    // actually the NEWER of the two (1537.9.55 vs 1458.2) -- these product
    // lines version independently, so "older SoC, smaller cap" does not hold.
    //
    // This used to warn and return success. It FAILS the bake now. A ramdisk
    // over the limit cannot be uploaded, so "succeeding" here only moved the
    // failure to real hardware, where it costs a DFU cycle to discover and
    // presents as an exploit problem rather than a size problem. Writing a
    // dist/ entry that is known-unusable is worse than writing none.
    //
    // DEBUG_RAMDISK_LIMIT_MIB overrides the limit, following the DEBUG_
    // convention the pwn binaries already use for investigation knobs (see
    // Checkm8Pwn.c): unset means the real, hardware-confirmed default.
    //   unset  -> 64 MiB, hard fail past it.
    //   -1     -> no limit; warn only, exactly the old behaviour. For
    //             deliberately baking an oversized ramdisk to measure it --
    //             .claude/TODO.md item 10's size-shedding work needs real
    //             per-tuple sizes, including for the tuples that are over.
    //   N      -> N MiB, hard fail past it. For a device whose real
    //             ramdisk-size is known to differ.
    //
    // `limitMiB` is resolved at the top of this function, not here, so a
    // malformed value fails before the bake rather than after it.
    std::error_code finalSizeEc;
    uint64_t finalSize = fs::file_size(patchedDMG, finalSizeEc);
    if (finalSizeEc) {
        fprintf(stderr, "bakeRamdisk: cannot stat finished ramdisk %s (%s)\n", patchedDMG.c_str(),
                finalSizeEc.message().c_str());
        return false;
    }

    if (limitMiB < 0) {
        fprintf(stderr,
                "bakeRamdisk: finished ramdisk is %llu bytes (%.1f MiB); limit disabled via "
                "DEBUG_RAMDISK_LIMIT_MIB=-1\n",
                (unsigned long long)finalSize, (double)finalSize / (1024.0 * 1024.0));
        outSizeWarning = true;
        return true;
    }

    uint64_t limitBytes = (uint64_t)limitMiB * 1024 * 1024;
    if (finalSize > limitBytes) {
        fprintf(stderr,
                "bakeRamdisk: FAILED: finished ramdisk is %llu bytes (%.1f MiB), past the %lld MiB limit.\n"
                "  A ramdisk this size cannot be uploaded (a real AppleTV3,2 short-writes at exactly 64MiB), so\n"
                "  this would fail on hardware instead of here. Shed content (kNeverStageDebs in this file, and\n"
                "  .claude/TODO.md item 10), or set DEBUG_RAMDISK_LIMIT_MIB=-1 to bake it anyway for measurement.\n",
                (unsigned long long)finalSize, (double)finalSize / (1024.0 * 1024.0), (long long)limitMiB);
        // Remove it rather than leaving a known-unusable dist/ entry behind:
        // bake-all-ramdisks skips targets whose output already exists, so a
        // leftover oversized file would be silently reused on the next run.
        std::error_code rmOversizeEc;
        fs::remove(patchedDMG, rmOversizeEc);
        return false;
    }

    return true;
}
