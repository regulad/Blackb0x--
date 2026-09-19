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
//  on a given firmware build. bake-firmware (BakeFirmware.cpp) calls
//  this once per known firmware and writes the results to dist/;
//  Patcher::patchRamdisk() just checks whether the expected dist/ output
//  already exists and tells the user to (re-)run bake-firmware if not.
//

#include "BakeRamdisk.hpp"
#include "ResourcePath.hpp"

// No <plist/plist.h> here any more: the only plist_* use in this file was
// readVolumeLabel(), parsing `diskutil info -plist` output to name a freshly
// created volume. That helper is gone along with the volume-rebuild path (see
// the note further down), and nothing else here parses a plist. deps::plist
// is still linked into bake-firmware for IPSW.cpp's sake; this translation
// unit just no longer needs the header.

extern "C" {
#include <xpwntool.h>
}

extern "C" {
#include <abstractfile.h>
// third_party/xpwn's vendored hfs/hfsplus.h (pulled in transitively here,
// via dmg/dmg.h) uses the `register` storage-class specifier in one
// function prototype (FastUnicodeCompare) — legal C++14, removed from the
// C++17 grammar entirely, and Clang (macOS's default compiler) hard-errors
// on it. Bracketing just this #include, rather than editing the vendored
// header itself, is what makes this file compile at all. A per-file
// CXX_STANDARD downgrade is not an option instead, since this file uses
// std::filesystem throughout — a C++17 *library* feature not reliably
// available under a downgraded language standard.
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
#include <cerrno>
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
// see the long comment on bakeRamdisk() below for the two designs that were
// tried and abandoned before this one (an in-memory xpwn Volume, then
// libhfsp), both of which corrupted real firmware data. That finding still
// binds: neither library's HFS+ write path is trustworthy here.
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
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            fprintf(stderr, "runCommand: cannot chdir to %s: %s\n", cwd.c_str(), strerror(errno));
            _exit(126);
        }
        execvp(cargv[0], cargv.data());
        // Diagnose rather than exiting mute. A bare `_exit(127)` here cost a
        // real debugging session: package/build.sh shipped mode 0644, execvp()
        // failed with EACCES, and the only thing the caller could report was
        // its own "package/build.sh failed -- see stderr above" pointing at an
        // empty stderr. The child is the only place that knows what went
        // wrong, so it has to say so before it dies.
        fprintf(stderr, "runCommand: cannot execute %s: %s\n", cargv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "runCommand: %s died on signal %d\n", cargv[0], WTERMSIG(status));
        return false;
    }
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
// trade: this measures a plain host directory, which is what the hdiutil
// build sequence has to work against — there is no live mount to `du`
// against.
static uint64_t directoryContentSize(const std::string& dir) {
    uint64_t total = 0;
    std::error_code walkEc;
    for (const auto& entry :
         fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, walkEc)) {
        if (walkEc) break;
        std::error_code typeEc;
        if (!fs::is_regular_file(entry.path(), typeEc) || typeEc) continue;
        // st_blocks (ALLOCATED 512-byte blocks), not fs::file_size (LOGICAL
        // length). The two diverge by design once the tree is decmpfs-
        // compressed: a compressed file still reports its full uncompressed
        // length through stat(), because that is what a reader will get, while
        // occupying a fraction of it on disk. Sizing the destination volume
        // from the logical length would size it for the uncompressed tree and
        // undo the entire point of compressing first.
        struct stat st;
        if (lstat(entry.path().c_str(), &st) == 0) total += (uint64_t)st.st_blocks * 512;
    }
    return total;
}

// NOTE: three helpers used to live here and are deleted -- readVolumeLabel()
// (diskutil/libplist volume-name reader), copyVolumeHeaderMetadata() (raw
// volume-header field copier) and unwrapUDIFIfPresent() (koly-trailer probe +
// extractDmg). All three existed only to make a FRESHLY SYNTHESIZED volume
// resemble the original: name it the same, carry its header fields over, and
// cope with hdiutil create possibly emitting UDIF structure. bakeRamdisk()
// grows and injects into the original volume now instead of rebuilding it, so
// the original's name, header and format are simply never lost and there is
// nothing to restore. Referenced by nothing after that change; see
// .claude/TODO.md item 15 and git history if the AppleTV2,1 4.x
// recreate-from-scratch case (still unimplemented) ever needs them back.

// Manual PATH search for whether a bare command name resolves to an
// executable, so a missing external tool is reported up front instead of as an
// opaque subprocess failure minutes into a bake. Same no-shell convention (and
// same implementation) as DeviceManager.cpp's own copy; duplicated rather than
// shared because neither file has a natural header to put it in and the
// function is eight lines.
static bool commandExistsOnPath(const char* name) {
    const char* pathEnv = getenv("PATH");
    if (!pathEnv) return false;
    std::string path(pathEnv);
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

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
            // This mountpoint was attached via `hdiutil attach`, and
            // `hdiutil detach` is its own matching unmount command.
            runCommand({"hdiutil", "detach", mountpoint});
        }
        if (!mountpoint.empty()) {
            std::error_code ec;
            fs::remove(mountpoint, ec);
        }
    }
};

// Builds entrypoint/'s freestanding ARMv6 replacement for /sbin/launchd,
// rather than shipping a precompiled binary — see entrypoint/README.md for
// why. The only thing that has to be on $PATH beyond the Xcode Command Line
// Tools this project already requires is `ldid`, for ad-hoc signing.
//
// This used to run inside a podman container, which existed solely to give
// a LINUX host a port of Apple's own ld64/as (cctools-port). On macOS those
// are the native tools, so the container had nothing left to provide.
//
// It also used to require a cctools-port cross-compiler named
// arm-apple-darwin11-clang, bootstrapped against a real iPhoneOS 6.1 SDK.
// That is gone too, and for the same reason: Apple's own clang has the ARM
// backend and Apple's own ld still lists armv6 as a supported arch, so
// entrypoint/Makefile just passes -arch armv6 to the system compiler.
// Verified end to end on Apple clang 21 / ld-1267 — the resulting Mach-O is
// armv6, LC_UNIXTHREAD, zero LC_LOAD_DYLIB, signed com.apple.launchd.
//
// The binary is identical for every firmware target (no per-firmware
// customization at all), so BakeFirmware.cpp's main() calls this exactly
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

    bool ok = runCommand({"make", "clean", "all"}, entrypointDir);
    if (!ok) {
        fprintf(stderr,
                "bakeRamdisk: failed to build entrypoint/ — if this is the first run, see "
                "entrypoint/README.md's one-time setup step (`brew install ldid`; the compiler "
                "and linker come from the Xcode Command Line Tools)\n");
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
    // Darwin's struct stat spells these fields st_atimespec/st_mtimespec,
    // where Linux/glibc says st_atim/st_mtim. Same fields, different names.
    // Worth knowing if you ever try to syntax-check this file off a Mac:
    // -Dst_atimespec=st_atim -Dst_mtimespec=st_mtim makes it parse.
    struct timespec times[2] = {st.st_atimespec, st.st_mtimespec};
    utimensat(AT_FDCWD, targetPath.c_str(), times, 0);
    return true;
}

// ---------------------------------------------------------------------------
// /blackb0x staging — the current design's replacement for the old per-file
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

// Which .deb archives never get copied into the ramdisk's apt cache. The list
// itself lives in misc/never_stage_debs.txt so each entry can carry the
// reasoning for its own exclusion; see that file's header for the distinction
// between it and prebake_package_blacklist.txt, and for the size budget.
//
// The original audit behind these exclusions is worth keeping, since it is
// what established that dropping them cannot break dependency resolution.
// Built the real dependency graph from every vendored .deb's own
// Depends:/Pre-Depends: field and computed the transitive closure actually
// needed to bootstrap postinstall.sh (bash, dpkg, coreutils(-bin), apt7(-lib),
// and what THEY pull in -- berkeleydb, bzip2, diffutils, findutils, gnupg,
// grep, gzip, lzma, ncurses, readline, sed, tar; 20 packages total). None of
// the excluded packages is in that closure. odcctools (ld64/as/otool/nm/strip
// -- a native build toolchain, not something a media/jailbreak ramdisk needs
// at runtime) Depends only on openssl/uuid, and no other vendored .deb
// declares a dependency on it at all. gettext's only dependent among every
// vendored package is wget, itself outside the closure. curl's dependents
// (org.xbmc.kodi-atv2, the tihmstar exploit-tool packages, apt7-ssl) are all
// optional, already excluded, or not the apt7 actually staged -- checked
// directly rather than assumed: extracted apt7-lib's real data.tar and ran
// `strings` on its actual usr/lib/apt/methods/http binary, which has no
// libcurl reference at all (this old apt implements its own HTTP client), and
// usr/lib/apt/methods/https turned out to be a plain symlink to http (no TLS
// library linked either). openssl itself stays regardless: openssh genuinely
// Depends: on it, and SSH access is a real feature rather than a
// bootstrap-only tool.
static const std::set<std::string>& neverStageDebPrefixes() {
    static const std::set<std::string> prefixes = [] {
        std::set<std::string> out;
        std::ifstream in(resolveMiscPath("never_stage_debs.txt"));
        if (!in) {
            // Not fatal, and deliberately so: an empty list means everything
            // gets staged, which fails loudly and understandably at the size
            // check rather than silently dropping packages the operator
            // expected to be there.
            fprintf(stderr,
                    "bakeRamdisk: WARNING: cannot read misc/never_stage_debs.txt -- every resolved "
                    ".deb will be staged, which may push the ramdisk past the size limit\n");
            return out;
        }
        std::string line;
        while (std::getline(in, line)) {
            size_t begin = line.find_first_not_of(" \t");
            if (begin == std::string::npos || line[begin] == '#') continue;
            size_t last = line.find_last_not_of(" \t\r");
            out.insert(line.substr(begin, last - begin + 1));
        }
        return out;
    }();
    return prefixes;
}

static bool shouldSkipStagingDeb(const std::string& filename) {
    for (const auto& prefix : neverStageDebPrefixes()) {
        if (filename.rfind(prefix, 0) == 0) return true;
    }
    return false;
}


// ---------------------------------------------------------------------------
// Bake-time package preinstallation — for packages with no postinst (or a
// trivial one, see misc/prebake_package_blacklist.txt's own
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
    // debcache/ store these as plain "control"/"postinst"/"preinst"
    // (no "./" prefix) instead, and which spelling a given .deb uses isn't
    // consistent even within that same set (whatever tool originally built
    // each one). Listing the archive first and matching whichever spelling
    // is actually present is more robust than assuming one fixed path for
    // any of the three — same fix already made and verified on the Python
    // side, see scripts/build_deb_cache_experimental_no_container.py's own
    // read_deb_control_info().
    std::vector<std::string> controlMembers;
    if (ok) {
        std::string listing = runCommandCapture({"tar", "-tf", controlTar});
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
    if (ok) ok = runCommand({"tar", "-xf", controlTar, controlMember}, tempDir);
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
        ok = runCommand({"tar", "-xf", controlTar, "-C", extractDir});
    }
    if (ok) {
        std::error_code rmEc2;
        fs::remove(fs::path(extractDir) / "preinst", rmEc2);
    }
    std::string newControlTar = tempDir + "/" + fs::path(controlTar).filename().string();
    // --auto-compress survives here, and ONLY here: on a create it picks the
    // compressor from newControlTar's extension, which is what preserves the
    // original control.tar.*'s format. macOS's tar is bsdtar, where -a is
    // create-only -- it hard-errors under -t ("Option -a is not permitted in
    // mode -t") and warns-and-ignores under -x. Every read-mode call above
    // dropped it for that reason; none of them needed it, since tar detects
    // compression from the stream on read.
    if (ok) ok = runCommand({"tar", "--auto-compress", "-cf", newControlTar, "-C", extractDir, "."});
    if (ok) {
        std::error_code rmDestEc;
        fs::remove(destDebPath, rmDestEc);
        // `rcS`, not `rc`: macOS's `ar` is Apple's cctools ar, which runs
        // ranlib on every archive it creates and prepends a
        // `__.SYMDEF SORTED` member. A .deb whose first member is not
        // `debian-binary` is not a .deb -- confirmed directly, dpkg-deb
        // rejects an `ar rc`-built one outright ("is not a Debian binary
        // archive"), and this file's own readDebControlInfo() cannot find
        // control.tar.* in it either. `S` suppresses the symbol table, which
        // is meaningless for an archive of tarballs anyway. GNU ar happened
        // to do the right thing here without being asked, which is why this
        // only ever surfaced on macOS.
        ok = runCommand({"ar", "rcS", fs::absolute(destDebPath).string(), debianBinary, newControlTar, dataTar});
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
// misc/prebake_package_blacklist.txt AND every one of its real
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
// NOT EXECUTED ANY MORE. This is the shell script the containerized dpkg
// bootstrap used to run inside a debian:stretch sandbox, kept verbatim
// because several comments below reason about what it did and why the
// current no-container path is a safe simplification of it. It references
// container-only paths (/debs, /preinstall, /work) and a real era-matched
// dpkg, neither of which exists on the host. Delete it only together with
// the comments that cite it.
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

// Defined further down, next to its other callers. Declared here because
// computePreinstalledPackages() below is the earliest caller.
//
// NOTE: the default arguments deliberately do NOT appear here -- they are on
// the definition, and C++ forbids repeating a default argument in a second
// declaration of the same function.
static ExtractedDeb extractDebAndBuildStanza(const std::string& debPath, const std::string& tempDirPrefix,
                                              const std::string& labelForLogging, bool hold,
                                              bool dropRelationshipFields);

// macOS has no container runtime at all (confirmed directly — not just
// podman, no viable alternative either), so the real, containerized dpkg
// bootstrap this used to do on Linux is off the table. That real dpkg run
// (kPreinstallInnerScript, kept below purely as the record of what it did —
// nothing executes it any more) only existed to correctly SEQUENCE
// maintainer-script execution
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
// are accepted for signature parity with that older, containerized path
// but genuinely unused here, not an oversight.
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
// outDpkgStateDir shape the containerized path produced, so
// mergePreinstalledPackages() (the shared caller) needs no changes at all.
//
// This intentionally skips the containerized path's `dpkg --audit`/`apt-get
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

    std::string debsRoot = resolveDebcachePath();
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
    // kPreinstallInnerScript's own copy declares (see
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

// The fast, per-bake half of the mechanism above: merges an already-
// computed preinstall payload + dpkg state into `blackb0xRoot`. No
// network, no container, no dpkg invocation — just local file copies —
// which is exactly why the expensive computation above is worth caching
// across every firmware a single bake-firmware run bakes (see
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
    // for. See misc/apt/local.list's own comment for why this
    // needs a real generated index, not just cached .deb bytes.
    std::string localRepoDir;
};

// Runs scripts/build_deb_cache.py and the bake-time dpkg preinstall
// mechanism at most ONCE per distinct real firmware version, no matter how
// many (device, buildID) tuples this run bakes. package/packages.txt's
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
// persist for as long as this one process runs (one bake-firmware
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
    // REAL apt does the solve now. No container is involved and none is
    // available on macOS -- this is Debian's own solver, built for the host
    // from third_party/apt (see CMakeLists.txt's apt_ext target), running
    // against a synthetic root over the locally-vendored debcache/.
    //
    // It replaced scripts/build_deb_cache_experimental_no_container.py, whose
    // documented gaps were not academic. On this project's own package list,
    // for a 6.1.3 target, the two disagree in exactly the ways that resolver
    // warned about:
    //
    //   * it put BOTH apt7-lib and apt7-ssl in the picklist, which genuinely
    //     Conflict with each other, because it had no Conflicts: handling;
    //   * it picked NEITHER persistence payload, while apt correctly selects
    //     p0sixspwn for 6.1.3 and rejects net.tihmstar.etasonuntether, which
    //     is pinned to `firmware (= 8.4.1)`.
    //
    // That second one is only possible because the firmware version is passed
    // through: the script declares a synthetic `firmware` package at this
    // tuple's real ProductVersion, the same synthetic package
    // build_deb_cache.py and kPreinstallInnerScript already rely on. The old
    // resolver stripped version constraints outright and so could not have
    // asked the question, which is precisely why this cache is keyed by
    // firmware version (see this function's own comment above).
    //
    // --allow-drops: packages.txt is one flat, firmware-independent list, but
    // a few of its entries are not (the apt7-lib/apt7-ssl conflict, and the
    // two era-specific persistence payloads). Real apt refuses the whole solve
    // over them rather than picking; the script drops those top-level entries
    // one at a time, reporting each to stderr. See .claude/TODO.md item 18 for
    // fixing packages.txt properly.
    std::string aptToolsDir = resolveAptToolsDir();
    if (!fs::exists(aptToolsDir + "/apt-get")) {
        fprintf(stderr,
                "bakeRamdisk: no apt at %s -- build it once with:\n"
                "    cmake --build build --target apt\n"
                "  (it is deliberately not part of a default build; see CMakeLists.txt)\n",
                aptToolsDir.c_str());
        outResult = cached;
        return false;
    }
    bool ok = runCommand({"python3", "scripts/build_deb_cache_apt.py", "--output-dir", tempDir,
                           "--apt-tools", aptToolsDir, "--firmware-version", firmwareVersion,
                           "--allow-drops"});
    if (!ok) {
        fprintf(stderr, "bakeRamdisk: scripts/build_deb_cache_apt.py failed\n");
        outResult = cached;
        return false;
    }
    std::ifstream picklist(tempDir + "/picklist.txt");
    if (!picklist) {
        fprintf(stderr, "bakeRamdisk: build_deb_cache.py did not produce picklist.txt\n");
        outResult = cached;
        return false;
    }
    std::string debsRoot = resolveDebcachePath();
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
    // file and misc/apt/local.list's own comments.
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

    std::string debsRoot = resolveDebcachePath();
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
// drift from whatever .deb is actually sitting in debcache/ the way a
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
// debcache/apt7_*.deb — the build host cannot execute it). stageEtasonatv() passes true: this project's own untether.bin
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
// pipeline every other .deb in debcache/ goes through
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
    if (dataTar.empty() || !runCommand({"tar", "-xf", dataTar}, result.tempDir)) {
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
    if (controlTar.empty() || !runCommand({"tar", "-xf", controlTar}, result.tempDir)) {
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

// iOS 8.4 branch — tihmstar's EtasonATV untether (see misc/README.md's
// "etasonATV / tihmstar-untether provenance" for the jsc/rtbuddyd/--early-boot
// mechanism this stages), extracted directly from the real
// net.tihmstar.etasonuntether .deb (Depends: firmware = 8.4.1, and its one
// real repo — repo.tihmstar.net — turned out to be an unreliable live apt
// dependency besides; see misc/apt/net.tihmstar.list.disabled and
// package/local_only_debs.txt) rather than the old, hand-assembled
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
    std::string debPath = fs::absolute(resolveDebcachePath() + "/net.tihmstar.etasonuntether-1.3.1.deb").string();
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
    // misc/README.md's "etasonATV / tihmstar-untether provenance"
    // section: this project's own untether.bin (kept standalone at
    // misc/untether.bin once the original tarball that bundled it
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
    std::string debPath = fs::absolute(resolveDebcachePath() + "/com.ih8sn0w-squiffy-winocm.p0sixspwn_1.4-1_iphoneos-arm.deb").string();
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
// uid 0 / gid 0 (a non-root dm.pl stamps root:wheel by construction -- see
// package/README.md's ownership section), which is exactly
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
    // real dpkg-scanpackages. Only the .deb source directory has to be
    // pointed at, since it is not under package/.
    setenv("BLACKB0X_DEBCACHE_DIR", fs::absolute(resolveDebcachePath()).c_str(), 1);

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
    // misc/apt/net.tihmstar.list.disabled exists), so it is not
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
// has only its content overwritten in place. /blackb0x does not fit in the
// pristine volume's own free space, so the image is GROWN first: measure the
// staged payload, `hdiutil resize` the decrypted image to fit it, then mount
// that and write into it. The original volume is never rebuilt and its
// content is never copied out, which is what keeps Apple's ownership, decmpfs
// compression, hard links and volume identity intact for free. The "growing
// HFS+ in place is fundamentally broken" note further below is about
// Linux-side write tooling (xpwn's grow_hfs(), libhfsp, the kernel driver)
// and does not apply to Apple's own hdiutil -- which is a large part of why
// this project is macOS-only.
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

    // Real root is required, and this needs to fail loudly up front rather
    // than incidentally. There is no mount to fail on — hdiutil
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
        fprintf(stderr,
                "bakeRamdisk: must run as root — chown() to root:wheel/mobile:staff (and to preserve the "
                "original ramdisk's own file ownership) requires real root on macOS too, even though hdiutil "
                "itself doesn't\n");
        return false;
    }

    std::string decDMG = decryptedDMGFor(path);
    const std::string& patchedDMG = outputPath;

    fprintf(stderr, "Patching ramdisk...\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decDMG.c_str()), const_cast<char*>(key.c_str()),
            const_cast<char*>(iv.c_str()), (char*)"FALSE", nullptr);

    // WHY THE PRISTINE APPLE CONTENT IS KEPT, RATHER THAN DELETED FOR SPACE
    //
    // Tempting idea, measured and rejected: entrypoint.c is freestanding, so
    // almost nothing Apple ships on this ramdisk is referenced at boot. Its
    // entire dependency on the pristine tree is /dev (an empty directory the
    // kernel mounts devfs onto itself), /mnt1 as a mountpoint, and
    // /usr/sbin/nvram, which set_auto_boot() execs. Both filesystem mounts go
    // through sys_mount() directly, so mount_hfs and friends are unused, and
    // nothing ever reads /bin, /System/Library/LaunchDaemons,
    // restored_external, asr or sed.
    //
    // Deleting "everything except what entrypoint needs" nonetheless frees
    // almost nothing, because nvram is not a cheap dependency. Measured
    // against a real AppleTV3,1 12H606 RestoreRamdisk, its transitive dylib
    // closure is 43 files totalling 14.22 MiB of a 15 MiB volume:
    // CoreFoundation (4.5 MB), libobjc (2.1 MB), libicucore (2.1 MB), IOKit,
    // dyld and the whole of /usr/lib/system. Keeping a 36 KB binary means
    // keeping essentially the entire ramdisk, so the saving is under a
    // megabyte and not worth the risk of deleting something load-bearing that
    // only shows up as a device that will not boot.
    //
    // The version of this that WOULD pay is dropping nvram itself, since it
    // exists only for set_auto_boot(). The host can do that job instead --
    // DeviceManager.cpp already sends `setenv auto-boot false` + `saveenv` to
    // iBEC in sendStockRestoreTail(), so issuing the auto-boot=1 equivalent
    // before bootx would move it off the device entirely and make the whole
    // 14.22 MiB closure deletable. Deliberately not done here: it is a real
    // behavioural change to the boot chain, unverifiable without hardware,
    // and a mistake in it presents as an exploit failure rather than a size
    // problem. Recorded so the measurement does not have to be redone.
    //
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
    // needed — a genuinely UDIF-wrapped image still needs unwrapping before
    // the volume can be worked with.
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
    // an existing HFS+ volume in place was never made to work: every tool
    // tried — in-memory xpwn grow_hfs(), libhfsp, libparted-fs-resize —
    // turned out broken or unsupported for exactly this operation (see the
    // design history further down, and docs/HISTORY.md). So instead: build a
    // brand new volume sized to actually fit.
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
    // guess. Only ONE real mount is needed (the original, read-only). There
    // used to be a second "scratch" HFS+ mount whose only purpose was to let
    // a real mounted filesystem's own `du` measure block-rounded usage;
    // directoryContentSize() is a plain std::filesystem walk that works
    // against an ordinary host directory, so the assembled
    // original+launchd+/blackb0x content is staged straight onto one
    // instead of a second mounted volume.
    // ---------------------------------------------------------------------
    // Grow the original volume and inject into it. Do NOT rebuild it.
    //
    // This used to attach the original read-only, `cp -a` its whole content
    // out to a plain host directory, stage there, and synthesize a brand new
    // volume with `hdiutil create -srcfolder`. That was a porting mistake,
    // and an expensive one. The pre-port Objective-C original
    // (origin/main:Blackb0x/Source/Patcher.mm, patchRamdisk:ssh:) did it the
    // way this does now: `hdiutil resize` the decrypted image, `hdiutil
    // attach` it, write new content straight into the mounted original,
    // detach. Its one `hdiutil create -srcfolder` call lived inside
    // `if([path containsString:@"AppleTV2,1_4."])` -- the legacy case where
    // there is genuinely no content to preserve. This port generalized that
    // special case into the main mechanism, and separately refuses the 4.x
    // case, which is the one place it was correct.
    //
    // What the round trip cost, all measured against real decrypted ramdisks:
    //
    //   * Ownership, silently. The read-only attach passed no `-owners on`,
    //     so Darwin maps every file to the invoking user. Same image, two
    //     attaches: without the flag /sbin/launchd reads as uid 501; with it,
    //     root:wheel. This function requires root so its chown()s work, then
    //     copied the mapped uid anyway -- baking Apple's entire tree owned by
    //     the build user.
    //   * HFS+ compression, worth ~19 MiB. Apple ships this content
    //     decmpfs-compressed (ZLIB types 3/4 on every firmware, plus LZVN
    //     type 8 on the 8.4.x ones). `du` on the mounted original: 15 MB.
    //     After `cp -a` to a plain directory: 34 MB, with the `compressed`
    //     flag gone. Against the 64 MiB ceiling enforced at the end of this
    //     function, that was the single largest waste in the bake.
    //   * Hard links. /sbin/halt and /sbin/reboot are one inode; BSD `cp -R`
    //     writes two copies.
    //   * Volume identity. A new volume means new CNIDs, a new UUID and a new
    //     header -- which is the only reason copyVolumeHeaderMetadata() had to
    //     exist. Growing in place keeps the original's, so that call and
    //     unwrapUDIFIfPresent() are both gone from this path.
    //
    // `-imagekey diskimage-class=CRawDiskImage` is REQUIRED on both hdiutil
    // calls: by this point rawImgPath is a bare HFS+ volume with no partition
    // map, and without the hint `attach` fails outright with "image not
    // recognized". Verified directly.
    // ---------------------------------------------------------------------

    // Staged before the resize, because its real size is what decides how far
    // to grow. Unlike the old flow this is the ONLY thing measured -- the
    // original content is already on the volume and already accounted for by
    // the image's existing size, so there is nothing to re-measure.
    std::string blackb0xStagingDir = makeTempDir("blackb0x-payload-");
    if (blackb0xStagingDir.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create /blackb0x staging dir\n");
        return false;
    }
    if (!stageBlackb0xTree(blackb0xStagingDir, productVersion)) {
        fprintf(stderr, "bakeRamdisk: failed to stage /blackb0x\n");
        return false;
    }

    // Compress BEFORE measuring, so the volume is grown to fit the compressed
    // tree rather than the uncompressed one. Order matters more than it looks:
    // compressing after the copy (the obvious arrangement, and the first one
    // tried) reclaims almost nothing, because HFS+ can only shrink down to its
    // highest allocated block. Measured on a real bake-shaped tree: usage fell
    // from 62 MiB to 31 MiB, and `hdiutil resize -size min` still could not go
    // below 73 MiB, since in-place compression frees blocks scattered through
    // the volume without relocating anything. Growing the right amount once is
    // the only arrangement that actually pays.
    //
    // -T ZLIB is NOT optional, and must never be relaxed to whatever the host
    // prefers. decmpfs codecs are per-kernel, and the oldest firmware this
    // project targets cannot read the modern default:
    //
    //   * The 10B329a (6.1.3) kernelcache ships AppleFSCompressionTypeZlib.kext
    //     declaring `providesType3` and `providesType4`, and carries NO LZVN or
    //     LZFSE compressor at all. Confirmed by reading the prelinked kext
    //     Info.plists out of the decrypted kernelcache, not inferred.
    //   * Its RestoreRamdisk uses only types 3 and 4. The 8.4.x ramdisks also
    //     carry type 8 (LZVN), so support genuinely varies across the range.
    //   * `ditto --hfsCompression` on a current macOS produces type 8. Using it
    //     here would bake something that boots on 8.4.x and fails on 6.1.x --
    //     silently, and only on hardware.
    //
    // afsctool already defaults to ZLIB; passing -T makes that a stated
    // requirement rather than a default this project happens to rely on. It
    // skips files that do not compress (the staged .deb bytes, already gzip/
    // lzma) rather than inflating them.
    if (!commandExistsOnPath("afsctool")) {
        fprintf(stderr,
                "bakeRamdisk: afsctool not found on PATH -- required to HFS+-compress the staged\n"
                "  payload (brew install afsctool). Without it the finished ramdisk is far larger\n"
                "  and will not fit under the 64 MiB limit.\n");
        return false;
    }
    if (!runCommand({"afsctool", "-c", "-T", "ZLIB", blackb0xStagingDir})) {
        fprintf(stderr, "bakeRamdisk: afsctool failed to compress the staged /blackb0x\n");
        return false;
    }

    uint64_t payloadSize = directoryContentSize(blackb0xStagingDir);
    uint64_t currentImageSize = 0;
    {
        std::error_code szEc;
        currentImageSize = (uint64_t)fs::file_size(rawImgPath, szEc);
        if (szEc) {
            fprintf(stderr, "bakeRamdisk: cannot stat %s to size the resize\n", rawImgPath.c_str());
            return false;
        }
    }
    // Margin covers HFS+'s own metadata growth (catalog/extents B-trees,
    // allocation bitmap) for the files about to be added, which is not part of
    // payloadSize. Same shape of margin the old create path used.
    constexpr uint64_t kFlatSizeMargin = 4ull * 1024 * 1024;
    uint64_t margin = std::max(kFlatSizeMargin, payloadSize / 10);
    uint64_t targetSize = currentImageSize + payloadSize + margin;
    // hdiutil resize takes 512-byte sectors; round up so the request is never
    // short of the computed target.
    uint64_t targetSectors = (targetSize + 511) / 512;

    if (!runCommand({"hdiutil", "resize", "-sectors", std::to_string(targetSectors), "-imagekey",
                      "diskimage-class=CRawDiskImage", rawImgPath})) {
        fprintf(stderr, "bakeRamdisk: hdiutil resize to %llu sectors failed on %s\n",
                (unsigned long long)targetSectors, rawImgPath.c_str());
        return false;
    }

    // `-owners on` is load-bearing, not decoration: without it Darwin mounts
    // the volume ignoring on-disk ownership, every file reads back as the
    // invoking user, and anything this function writes would be recorded with
    // the wrong uid. See this block's header comment.
    MountGuard mount;
    mount.mountpoint = makeTempDir("blackb0x-rdmnt-");
    if (mount.mountpoint.empty()) {
        fprintf(stderr, "bakeRamdisk: cannot create a temp mountpoint\n");
        return false;
    }
    if (!runCommand({"hdiutil", "attach", "-mountpoint", mount.mountpoint, "-nobrowse", "-owners", "on",
                      "-imagekey", "diskimage-class=CRawDiskImage", rawImgPath})) {
        fprintf(stderr, "bakeRamdisk: failed to attach the grown ramdisk via hdiutil\n");
        return false;
    }
    mount.mounted = true;

    // The one and only content change to anything the pristine ramdisk already
    // shipped -- see spliceFileContentInPlace()'s own comment for why a blind
    // overwrite is not good enough. Now genuinely in place, against the real
    // volume: the target keeps its inode, mode, owner and timestamps.
    //
    // Note the pristine /sbin/launchd is itself decmpfs-compressed on every
    // firmware checked, and is mode 0555 (no write bit at all) on the 6.1-era
    // ones. Both are fine and were verified directly: opening it O_WRONLY|
    // O_TRUNC transparently drops the compression (the `compressed` flag and
    // the com.apple.decmpfs xattr both go away, and the file reads back
    // byte-identical to what was written), and root bypasses the missing write
    // bit. A non-root writer gets EACCES here, which is one of the reasons
    // this function checks geteuid() up front.
    if (!spliceFileContentInPlace(mount.mountpoint + "/sbin/launchd", entrypointBinaryPath)) {
        return false;
    }

    // `ditto`, NOT `cp -a`, and that is load-bearing rather than stylistic:
    // ditto carries decmpfs compression across the copy, cp does not. Measured
    // on the same tree, APFS staging dir -> HFS+ volume: ditto lands 3.3 MB and
    // the destination still reports `compressed`; `cp -a` lands 10 MB with the
    // flag gone, because cp reads through the VFS and gets decompressed bytes.
    // (A `tar` pipe preserves it too, but ditto is one process and is Apple's
    // own tool for exactly this.) ditto also preserves mode, owner, group,
    // extended attributes and ACLs by default, which is what `cp -a` was here
    // for in the first place.
    //
    // Only the new tree moves. Apple's own content is never read or rewritten,
    // which is what preserves ITS compression, hard links and ownership for
    // free.
    if (!runCommand({"ditto", blackb0xStagingDir + "/blackb0x", mount.mountpoint + "/blackb0x"})) {
        fprintf(stderr, "bakeRamdisk: failed to copy staged /blackb0x onto the ramdisk\n");
        return false;
    }
    std::error_code stagingRmEc;
    fs::remove_all(blackb0xStagingDir, stagingRmEc);

    if (!runCommand({"hdiutil", "detach", mount.mountpoint})) {
        fprintf(stderr, "bakeRamdisk: failed to detach the ramdisk\n");
        return false;
    }
    mount.mounted = false;

    // Final tidy: give back whatever trailing slack the margin left over.
    // `-size min` is the correct spelling and does the arithmetic itself.
    //
    // Do NOT reach for `hdiutil resize -limits` here. For a bare raw HFS+ image
    // its first field is not a usable minimum -- it reports the image's
    // ORIGINAL size (19440 sectors on a real AppleTV3,2 ramdisk) regardless of
    // how much is now allocated, and resizing to it fails with EINVAL. That
    // exact mistake silently defeated an entire bake: the shrink failed, the
    // warning scrolled past, and the finished ramdisk carried the full grown
    // image. `-alllimits` reports the real per-image minimum if a number is
    // ever needed; `-size min` is simpler and is what this wants.
    //
    // This only reclaims the margin now, not the compression saving. The tree
    // is already compressed before the volume is grown, so there is no large
    // gap left to close -- which is the point.
    if (!runCommand({"hdiutil", "resize", "-size", "min", "-imagekey", "diskimage-class=CRawDiskImage",
                      rawImgPath})) {
        // Not fatal: an unshrunk image is oversized, not corrupt, and the size
        // check at the end of this function reports that clearly rather than
        // this producing a duplicate failure.
        fprintf(stderr, "bakeRamdisk: WARNING: could not shrink the finished image to its minimum\n");
    }

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

    // Same reason as the bootchain half's publish(): this function requires
    // root, but its output is a build artifact the invoking user owns in
    // practice. Done before the size check below, so an oversized ramdisk that
    // gets deleted is deleted by a process that could have handed it over --
    // no ordering subtlety either way, just no reason to wait.
    chownToSudoCaller(fs::path(patchedDMG).parent_path().string());
    chownToSudoCaller(patchedDMG);

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
        // bake-firmware skips targets whose output already exists, so a
        // leftover oversized file would be silently reused on the next run.
        std::error_code rmOversizeEc;
        fs::remove(patchedDMG, rmOversizeEc);
        return false;
    }

    return true;
}
