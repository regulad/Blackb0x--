/*
 * entrypoint.c
 *
 * Blackb0x's own first-boot installer for the patched restore ramdisk. Not
 * real launchd, and no longer a replacement for it: on the LaunchDaemons
 * generation of ramdisk (AppleTV3,x 12H1006, the primary target) this is
 * installed as the NEW file /usr/sbin/blackb0x_entrypoint and started by
 * Apple's own real /sbin/launchd as an ordinary one-shot LaunchDaemon,
 * /System/Library/LaunchDaemons/xyz.regulad.blackb0x.entrypoint.plist. The
 * older rc.boot generation, which has no daemon-directory loader at all,
 * still gets the legacy splice over /sbin/launchd's content — see
 * BakeRamdisk.cpp's installEntrypoint(), which picks, and
 * entrypoint/README.md's "Run as a launchd unit, not as launchd" for the
 * measured evidence behind the change (short form: replacing PID 1 meant
 * restored_external never ran, so the display never came up AND the USB
 * device stack never came on-bus, because on this hardware nothing
 * enumerates until a userspace process calls
 * IOUSBDeviceControllerSetDescription).
 *
 * That buys observability and nothing else. It does NOT dodge AMFI — the
 * kernel's exec of PID 1 and launchd's posix_spawn land in the same
 * signature check, and if anything PID 1 is the less-checked position. The
 * baked boot-args remain the only thing that disables enforcement.
 *
 * Reverse-engineered from the precompiled ARMv6 Mach-O this project shipped
 * at misc/launchd (originally Blackb0x/Files/launchd). See misc/README.md
 * for the full reverse-engineering writeup this is built from (strings, the
 * original Patcher.mm, a full Ghidra decompilation, and raw disassembly of
 * every syscall trampoline).
 *
 * Originally a faithful reproduction of the original's exact behavior,
 * including two confirmed pre-existing bugs in its directory-creation
 * helper (a hardcoded-nonsense mode, and a missing-leading-zero octal
 * literal at its one call site) — since fixed, now that this entrypoint
 * does real first-time dpkg/apt bootstrap work where directory permissions
 * actually matter.
 *
 * This entrypoint deliberately consumes NO boot-args: the `blackb0x.*`
 * runtime-directive mechanism (and the raw kern.bootargs sysctl read behind
 * it) was removed once the inert-`setenv boot-args` root cause was found, and
 * is recoverable verbatim from git history if it is ever wanted again.
 *
 * ---------------------------------------------------------------------------
 * THIS IS A DYNAMICALLY LINKED armv7 BINARY. It used to be freestanding.
 * ---------------------------------------------------------------------------
 *
 * The original binary genuinely was freestanding — confirmed via its Mach-O
 * load commands (LC_UNIXTHREAD, zero LC_LOAD_DYLIB): no libSystem, no dyld,
 * every syscall made directly via `mov r12, #N; svc #128`, even
 * strlen/memcpy/memset hand-rolled. This file replicated that for a long
 * time, and no longer does.
 *
 * The reason for converting is convergence on a known-good configuration,
 * not novelty. Apple's own /sbin/launchd — the PID 1 this file's content
 * used to replace, and the one that demonstrably boots on this exact
 * hardware (`blackb0x --stock-ramdisk`) — is LC_MAIN + LC_LOAD_DYLINKER
 * against libSystem on BOTH target ramdisks. A dynamically linked binary is
 * the ordinary artifact here; the freestanding static one was the unusual
 * one. dyld is demonstrably live at PID 1 on this boot, and it is even less
 * in doubt now that we are spawned by launchd rather than by the kernel.
 *
 * The hand-rolled syscall layer was also where every ABI bug this project
 * has had to hunt down lived — all three of them:
 *
 *   - Darwin ARM signals syscall errors via the CARRY FLAG, not a negated
 *     return value, so every wrapper needed an `rsbcs r0, r0, #0` or a
 *     failed open()/stat() sailed past `fd < 0` as a valid result.
 *   - reboot(2) is really `reboot(int opt, char *msg)` (xnu syscalls.master
 *     #55), a TWO-argument syscall; a 1-arg raw call left r1 as garbage.
 *   - fork(2) returns the child pid in r0 for BOTH sides and flags the child
 *     in r1; a wrapper reading only r0 made every `if (pid == 0)` branch run
 *     in the parent.
 *
 * libc gets all three right by construction. Two more latent instances of
 * the same class died with the conversion: the STAT_*_OFFSET block, which
 * hard-coded the 96-byte pre-64-bit-inode layout the raw SYS_stat (188) trap
 * returns, and the per-byte console writer. Note on the first: libc's
 * `stat()` on these firmwares IS stat64 (`_stat` in libsystem_kernel
 * disassembles to syscall 338), so the real `struct stat` the SDK headers
 * describe — 108 bytes, st_mode@4, st_uid@16, st_gid@20 — is exactly what
 * comes back, and the offsets simply evaporated rather than being ported.
 *
 * Costs, recorded honestly: dynamic linking adds failure modes that happen
 * BEFORE the first instruction here runs, where even a working console shows
 * nothing. Those are closed at bake time rather than hoped away —
 * verifyEntrypointRuntimeClosure() (src/BakeRamdisk.cpp) proves, against the
 * real mounted ramdisk, that every LC_LOAD_DYLIB and the LC_LOAD_DYLINKER
 * target exist there and every undefined symbol resolves against that
 * volume's own dylibs. The dylib closure is ONE name,
 * /usr/lib/libSystem.B.dylib (verified: its re-export targets are 22/22 and
 * 32/32 present on the two ramdisks, zero missing).
 *
 * Build: armv7 (every dylib on both ramdisks is armv7 thin; there is no
 * armv6 slice to link against), -marm, -nostdlib, against the iPhoneOS SDK
 * MATCHED TO THE DEVICE — iPhoneOS 7.1 for AppleTV2,1 / 11D258, iPhoneOS 8.4
 * for AppleTV3,x / 12H1006. That pairing is not interchangeable and the
 * Makefile enforces it: matched is 0 phantom symbols on both branches, while
 * an 8.4 SDK against a 7.1.2 device advertises 787 symbols the device does
 * not export — the class that links clean and dies at dyld load, silently,
 * on hardware with no console. -nostdlib is required (an SDK root has no
 * crt1.o) and costs nothing: it still yields LC_MAIN with entryoff pointing
 * straight at _main, which is what dyld calls and what Apple's own launchd
 * uses.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MNT "/mnt1"

/* The on-NAND install record. /mnt1/var is a symlink to /mnt1/private/var,
 * which is where /dev/disk0s1s2 (the data partition) is mounted by main()
 * before do_install() ever runs — so this path only resolves after that
 * mount, which is exactly why the LaunchDaemon plist cannot point
 * StandardOutPath here. See "Where output goes" below. */
#define INSTALL_LOG_PARENT "/mnt1/var/mobile"
#define INSTALL_LOG_DIR    INSTALL_LOG_PARENT "/Media"
#define INSTALL_LOG        INSTALL_LOG_DIR "/blackb0x_install.log"

/* uid/gid used throughout for installed files — 501:20, "mobile:staff",
 * standard iOS convention. */
#define UID_MOBILE 0x1f5
#define GID_STAFF  0x14

/* Historical note, no longer live: the original binary's directory-creation
 * helper never actually read its caller-supplied mode argument — it loaded
 * a literal from its own literal pool instead, which was, byte for byte,
 * the address of the binary's own chmod() syscall trampoline (0x63d4,
 * 25556 decimal — a nonsense permission value with no sane rwx/setuid
 * interpretation). Every one of the ~80 base Cydia directories was created
 * with that mode on every real device this jailbreak was ever installed
 * on. This entrypoint no longer creates directories from any such list
 * (see merge_tree() below and BakeRamdisk.cpp's stageBlackb0xTree(), which
 * bakes correct 0755 modes directly into /blackb0x at bake time) — kept
 * here purely as a record of the bug, not a pointer to live code. The two
 * remaining mkdir() call sites both pass a real, explicit mode:
 * merge_tree(), which copies the source entry's own, and
 * write_install_record(), whose 0755 is a last-resort fallback for a NAND
 * that somehow has no /var/mobile/Media. */

/* ---------------------------------------------------------------------- */
/* Where output goes                                                       */
/* ---------------------------------------------------------------------- */
/*                                                                         */
/* ONE STREAM, and this process does not configure it. Everything this      */
/* binary has to say goes to stdout (progress) or stderr (errors) with      */
/* plain printf/fprintf, and launchd connects both to /dev/console from     */
/* the unit's StandardOutPath/StandardErrorPath — exactly the way Apple's   */
/* own com.apple.restored_external.plist does it on this same ramdisk.      */
/*                                                                         */
/* WHAT WAS DELETED, and why it was not just a cleanup:                     */
/*                                                                         */
/*   - console_print(): opened /dev/console itself and dup2()'d it onto     */
/*     fds 1 and 2 from main(). That is launchd's job now, it is declared   */
/*     in the plist where it can be read without disassembling anything,    */
/*     and doing it here would fight whatever launchd already attached.     */
/*   - log_to_file(): a second, entirely independent writer that            */
/*     open/append/write/close/chown/chmod'd the on-NAND log on EVERY       */
/*     message. Two writers to two destinations meant the two halves of a   */
/*     run's story could interleave arbitrarily, and which sink a given     */
/*     message used was inherited verbatim from the original binary's own   */
/*     inconsistencies rather than from anything meaningful. With a single  */
/*     stream, MESSAGE ORDERING IS NOW GUARANTEED BY CONSTRUCTION.          */
/*                                                                         */
/* WHY THE PLIST CANNOT SIMPLY POINT AT THE ON-NAND LOG. launchd opens      */
/* StandardOutPath when it SPAWNS the job, long before this code has        */
/* mounted anything. INSTALL_LOG lives under /mnt1, i.e. on the NAND, which */
/* does not exist yet at that moment — and the failure would not even be    */
/* loud: launchd would create the file on the RAMDISK under the empty       */
/* /mnt1 mountpoint, we would then mount the real volume over the top, and  */
/* every byte would land in an invisible, shadowed file that dies with the  */
/* RAM disk.                                                                */
/*                                                                         */
/* THE ALTERNATIVE THAT WAS REJECTED, since it is the obvious one: point    */
/* StandardOutPath at a path on the ramdisk itself and copy the finished    */
/* file onto the NAND once /mnt1 is mounted. Two things kill it.            */
/*   1. It rests on the ramdisk root being mounted READ-WRITE at the moment */
/*      launchd spawns us, which is not established anywhere in this        */
/*      project. If it is read-only, launchd's open() fails, the job gets   */
/*      /dev/null for stdout, and we are totally blind with no diagnostic   */
/*      at all — the precise failure this whole redesign exists to stop      */
/*      producing. /dev/console is the one sink proven to work on this      */
/*      exact volume, because Apple's own daemon uses it there.             */
/*   2. It is in RAM until the copy happens, so every failure BEFORE the    */
/*      mount — the disk-wait loop spinning forever, a mount(2) refusal, a  */
/*      panic() — leaves nothing behind after a power cycle. Those are the  */
/*      failures currently under investigation.                             */
/*                                                                         */
/* SO THE ON-NAND LOG IS A SEPARATE, DELIBERATE ARTIFACT, not a transcript: */
/* there is no transcript to copy, since the live stream goes to a          */
/* character device nothing reads back. write_install_record() below is the */
/* whole of it — one call, at one point, after the volume is mounted.       */
/* ---------------------------------------------------------------------- */

/* The on-NAND fingerprint: /var/mobile/Media/blackb0x_install.log, the file
 * a later boot reads to learn whether this ramdisk ever ran. Same path and
 * same owner (501:20, mobile:staff) as every previous version of this
 * project.
 *
 * MODE CHANGED, DELIBERATELY: 0644, not the 0755 the old log_to_file()
 * chmod()'d on every single write. 0755 on a log file is not a permission
 * anyone wanted — it is what the original binary did, faithfully
 * reproduced, and an executable bit on a text file read by a later boot has
 * no meaning to grant.
 *
 * Both the directory check and every failure are reported on stderr. The
 * old code passed O_CREAT and then ignored the result, so a missing
 * /var/mobile/Media (a freshly-erased NAND) silently produced no artifact
 * and no complaint. That directory is created here if it is genuinely
 * absent — never touched if it already exists, so a real device keeps its
 * own ownership — and a failure to create it says so rather than
 * evaporating. */
static void write_install_record(const char *outcome) {
    struct stat st;
    const char *dirs[2] = {INSTALL_LOG_PARENT, INSTALL_LOG_DIR};
    for (int i = 0; i < 2; i++) {
        if (stat(dirs[i], &st) == 0) continue;
        fprintf(stderr, "entrypoint: %s is missing on the target volume — creating it\n", dirs[i]);
        if (mkdir(dirs[i], 0755) != 0) {
            fprintf(stderr, "entrypoint: cannot create %s (%s) — install record NOT written\n", dirs[i],
                    strerror(errno));
            return;
        }
        chown(dirs[i], UID_MOBILE, GID_STAFF);
    }

    int fd = open(INSTALL_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        fprintf(stderr, "entrypoint: cannot open %s (%s) — install record NOT written\n", INSTALL_LOG,
                strerror(errno));
        return;
    }
    (void)write(fd, outcome, strlen(outcome));
    close(fd);
    if (chown(INSTALL_LOG, UID_MOBILE, GID_STAFF) != 0 || chmod(INSTALL_LOG, 0644) != 0) {
        fprintf(stderr, "entrypoint: cannot set 501:20 0644 on %s (%s)\n", INSTALL_LOG, strerror(errno));
    }
}

/* ---------------------------------------------------------------------- */
/* File installation — matches FUN_00005c7c/FUN_00005d40 exactly: copy in */
/* 2048-byte chunks, then chown, then chmod (mode masked to 16 bits,       */
/* matching the original's `& 0xffff`).                                   */
/* ---------------------------------------------------------------------- */

/* Matches FUN_00005d40's exact structure: a stat() pre-check on the
 * source, with "Unable to find source file" tied ONLY to that stat()
 * failing — not to any later open() failure, which the original propagates
 * silently (no message) via a plain -1 return. Also matches the original's
 * destination open() exactly: mode 0 (not the flags value again — a real
 * bug caught by comparing this function's actual disassembly against the
 * original's, not just the source logic), relying entirely on the later
 * chmod() to set real permissions. */
static int install_file(const char *src, const char *dst, int uid, int gid, int mode) {
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "Unable to find source file: %s\n", src);
        return -1;
    }

    int in = open(src, O_RDONLY);
    if (in < 0) {
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT, 0);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[2048];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) < 0) { rc = -1; break; }
    }
    close(in);
    close(out);
    if (rc == 0) {
        chown(dst, (uid_t)uid, (gid_t)gid);
        chmod(dst, (mode_t)(mode & 0xffff));
    }
    return rc;
}

/* ---------------------------------------------------------------------- */
/* /blackb0x merge — replaces the old per-file                            */
/* install_file() call list. /blackb0x is a flat mirror of the real        */
/* device's final layout, staged at bake time by bakeRamdisk() with each   */
/* file/directory/symlink already carrying its correct final owner and     */
/* mode (see BakeRamdisk.cpp) — this just walks it and blindly replicates  */
/* it onto /mnt1, overwriting whatever's already there. No branching on    */
/* firmware version or install state happens here at all: which files      */
/* exist under /blackb0x for a given ramdisk was already decided at bake    */
/* time (bakeRamdisk() knows the target firmware's ProductVersion, so it    */
/* picks the right per-version payload once, up front, instead of shipping */
/* all three and branching at runtime). Not a disassembly reproduction —   */
/* the original binary never had this mechanism at all.                    */
/* ---------------------------------------------------------------------- */

/* Recursively merges `src` onto `dst`, both real directories, replacing
 * whatever's already at `dst`. Directories that already exist at the
 * destination are left with their own metadata untouched (only recursed
 * into) — matters for destinations like /System/Library/LaunchDaemons,
 * which already exist on the real device with Apple's own ownership,
 * implied as a parent of a staged file without needing (or wanting) this
 * to reset it. A directory that doesn't yet exist at the destination is
 * created fresh with `src`'s own owner/mode, read directly off the source
 * side via stat() — this is the mechanism that replaces the old, separate
 * create_cydia_directories() call entirely: those directories are just
 * empty entries under /blackb0x now, no separate list needed. Regular
 * files always get overwritten via install_file(), using the source
 * file's own real owner/mode (not a caller-supplied triple anymore).
 * Symlinks are unlinked first, then recreated pointing at the source
 * symlink's own target string verbatim — this is how /blackb0x/--early-boot
 * (a real symlink to /untether/expl.js, no different from any other file
 * under /blackb0x) becomes /mnt1/--early-boot.
 *
 * opendir/readdir rather than the raw getdirentries(2) this used to parse
 * by hand. readdir() reports end-of-directory and a real error the same
 * way (NULL), so errno is cleared immediately before each call and read
 * back immediately after — that is what preserves the old `n < 0` ->
 * "failed to get directories" path, which a bare NULL check would silently
 * turn into "directory ended early, all fine". */
static int merge_tree(const char *src, const char *dst) {
    DIR *dir = opendir(src);
    if (dir == NULL) {
        fprintf(stderr, "cannot open blackb0x source dir %s (%s)\n", src, strerror(errno));
        return -1;
    }

    int ok = 1;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (de == NULL) {
            if (errno != 0) {
                closedir(dir);
                fprintf(stderr, "failed to get directories under %s (%s)\n", src, strerror(errno));
                return -1;
            }
            break; /* genuine end of directory */
        }

        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
            continue; /* "." / ".." only — unlike the old clone_directory(),
                       * real dotfiles (.profile's parent, etc.) are staged
                       * content here, not something to skip. */
        }

        char srcPath[1024], dstPath[1024];
        srcPath[0] = '\0'; dstPath[0] = '\0';
        strcat(srcPath, src);
        strcat(srcPath, "/");
        strcat(srcPath, de->d_name);
        strcat(dstPath, dst);
        strcat(dstPath, "/");
        strcat(dstPath, de->d_name);

        if (de->d_type == DT_LNK) {
            char target[1024];
            ssize_t tn = readlink(srcPath, target, sizeof(target) - 1);
            if (tn < 0) { ok = 0; continue; }
            target[tn] = '\0';
            unlink(dstPath);
            if (symlink(target, dstPath) != 0) ok = 0;
        } else if (de->d_type == DT_DIR) {
            struct stat dstSt;
            if (stat(dstPath, &dstSt) != 0) {
                struct stat srcSt;
                if (stat(srcPath, &srcSt) == 0) {
                    mode_t mode = srcSt.st_mode & 07777;
                    mkdir(dstPath, mode);
                    chmod(dstPath, mode);
                    chown(dstPath, srcSt.st_uid, srcSt.st_gid);
                }
            }
            if (merge_tree(srcPath, dstPath) != 0) ok = 0;
        } else {
            struct stat srcSt;
            if (stat(srcPath, &srcSt) != 0) { ok = 0; continue; }
            int mode = (int)(srcSt.st_mode & 07777);
            if (install_file(srcPath, dstPath, (int)srcSt.st_uid, (int)srcSt.st_gid, mode) != 0) ok = 0;
        }
    }

    closedir(dir);
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------------- */
/* Main install sequence.                                                 */
/* ---------------------------------------------------------------------- */

/* A genuine halt, not a graceful return: says so, records it on the NAND,
 * then hangs forever rather than letting main() reach its own
 * unmount/reboot path. An automatic reboot here would just re-run this same
 * ramdisk straight back into the same panic on every cycle, with nothing to
 * show a human debugging over console/serial that anything is wrong — a
 * dead stop forces attention instead of masking the problem as a boot loop.
 *
 * ONE CALL PER SINK, not the old four: the message goes out on stderr and
 * the outcome goes into the install record, and there is no longer an
 * arbitrary split deciding which fragments reach which writer.
 *
 * WHAT THIS MEANS NOW THAT WE ARE NOT PID 1 — it got strictly better.
 * Hanging as PID 1 froze the machine: launchd never ran, so nothing else on
 * the ramdisk did either and the device was dark and off the USB bus.
 * Hanging as an ordinary LaunchDaemon leaves Apple's launchd and
 * restored_external alive, so the display stays up and the device stays
 * enumerated while this process sits here — a panic is now something a host
 * can observe rather than something indistinguishable from a brick.
 * KeepAlive is false in the unit, so launchd will not respawn us either;
 * see BakeRamdisk.cpp's installEntrypointUnit(). */
static void panic(const char *msg) {
    char record[512];
    fprintf(stderr, "PANIC: %s", msg);
    snprintf(record, sizeof(record), "PANIC: %s", msg);
    write_install_record(record);
    sync();
    for (;;) sleep(60);
}

/* Whether /mnt1/private/var/lib/dpkg/status (already written onto the real
 * target volume by the merge_tree() call in do_install() below, staged at
 * bake time by BakeRamdisk.cpp's stageManualDpkgInstall()/
 * stageEtasonatv()) has a real `Package: <pkgName>` stanza — a plain
 * substring search on the stanza header line is enough here: this
 * project's own bake-time writer only ever appends a stanza for a package
 * name once it's decided that package is genuinely installed (see
 * BakeRamdisk.cpp's stageManualDpkgInstall() — every stanza it writes
 * always carries `Status: install ok installed`), so presence of the
 * header line alone is an unambiguous signal, no real stanza-boundary
 * parsing needed.
 *
 * Reads into a fixed, generously-sized static (BSS, not stack) buffer
 * rather than streaming, since a plain substring search across a
 * read-buffer boundary would need real overlap-handling logic this
 * one-shot check doesn't justify. If the real status file ever somehow
 * exceeds this buffer, this fails closed (reports "not found", so
 * fixup_etasonuntether_rtbuddyd() below just does nothing) rather than
 * searching a truncated/wrong window and risking a false answer. */
#define DPKG_STATUS_SCAN_BUF_SIZE (256 * 1024)
static char g_dpkgStatusScanBuf[DPKG_STATUS_SCAN_BUF_SIZE];

static int dpkg_status_has_installed_package(const char *statusPath, const char *pkgName) {
    int fd = open(statusPath, O_RDONLY);
    if (fd < 0) return 0;

    size_t total = 0;
    ssize_t n;
    while (total < DPKG_STATUS_SCAN_BUF_SIZE &&
           (n = read(fd, g_dpkgStatusScanBuf + total, DPKG_STATUS_SCAN_BUF_SIZE - total)) > 0) {
        total += (size_t)n;
    }
    close(fd);
    if (total >= DPKG_STATUS_SCAN_BUF_SIZE) {
        fprintf(stderr, "dpkg status file larger than expected — package-state check skipped\n");
        return 0;
    }

    char needle[192];
    needle[0] = '\0';
    strcat(needle, "Package: ");
    strcat(needle, pkgName);
    strcat(needle, "\n");
    return memmem(g_dpkgStatusScanBuf, total, needle, strlen(needle)) != NULL;
}

/* Copies `src` to `dst`, preserving `src`'s own real owner/mode (read via
 * stat()) rather than a caller-supplied triple — the same idiom
 * merge_tree() itself uses for regular files, reused here via the
 * existing install_file() primitive. */
static int copy_preserving(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    int mode = (int)(st.st_mode & 07777);
    int uid = (int)st.st_uid;
    int gid = (int)st.st_gid;
    return install_file(src, dst, uid, gid, mode);
}

/* net.tihmstar.etasonuntether's own real postinst (see misc/
 * README.md's "etasonATV / tihmstar-untether provenance" section, and the
 * postinst itself, extracted directly from the real .deb) swaps
 * /usr/libexec/rtbuddyd for a symlink to jsc, backing up any real rtbuddyd
 * it finds first: `if [ -e rtbuddyd ]; then [ ! -e rtbuddyd.orig ] && mv
 * rtbuddyd rtbuddyd.orig; fi; ln -s jsc rtbuddyd` (roughly). That postinst
 * never actually runs anywhere in this project — see BakeRamdisk.cpp's
 * stageEtasonatv() for why (its own live jsc self-test can't run against
 * an unbooted image) — so this reproduces just that specific swap here,
 * against the real target volume, once BakeRamdisk.cpp's bake-time dpkg
 * status stanza (stageManualDpkgInstall()) says the package is genuinely
 * installed. This intentionally runs from the entrypoint rather than being
 * baked directly into /blackb0x at bake time: bake time only ever sees a
 * pristine, not-yet-patched restore ramdisk with no access to this
 * specific device's actual /mnt1/usr/libexec/rtbuddyd content, so
 * "back up whatever's really there first" can only be decided here,
 * against the real mounted target volume, not baked as a static symlink
 * ahead of time.
 *
 * rtbuddyd.orig's own presence is the idempotency gate, matching the real
 * postinst's own `[ ! -e rtbuddyd.orig ]` check — this only ever needs to
 * run once (do_install() itself already refuses to run a second time at
 * all once install-done exists, but this check is what the real postinst
 * itself also relies on, kept here to mirror it exactly rather than lean
 * solely on the caller's own one-shot guarantee). */
static void fixup_etasonuntether_rtbuddyd(void) {
    if (!dpkg_status_has_installed_package("/mnt1/private/var/lib/dpkg/status", "net.tihmstar.etasonuntether")) {
        return;
    }
    if (access("/mnt1/usr/libexec/rtbuddyd.orig", F_OK) == 0) {
        return; /* already backed up on a prior run */
    }
    if (access("/mnt1/usr/libexec/rtbuddyd", F_OK) == 0) {
        if (copy_preserving("/mnt1/usr/libexec/rtbuddyd", "/mnt1/usr/libexec/rtbuddyd.orig") != 0) {
            fprintf(stderr, "failed to back up rtbuddyd before etasonuntether symlink\n");
            return;
        }
        unlink("/mnt1/usr/libexec/rtbuddyd");
    }
    /* Either rtbuddyd was just backed up and removed above, or there was
     * never a real one to back up in the first place — the real postinst
     * symlinks unconditionally in that second case too (its own `else`
     * branch). */
    if (symlink("/System/Library/Frameworks/JavaScriptCore.framework/Resources/jsc",
                "/mnt1/usr/libexec/rtbuddyd") != 0) {
        fprintf(stderr, "failed to symlink rtbuddyd -> jsc for etasonuntether\n");
    }
}

/* No version/first-install branching left at all. Which
 * firmware this ramdisk targets was already resolved once, at bake time,
 * into exactly what's staged under /blackb0x (see BakeRamdisk.cpp's
 * stageBlackb0xTree()) — this function's only remaining job is a sanity
 * gate (don't touch a device that isn't actually an AppleTV) and one
 * unconditional, blind merge_tree().
 *
 * That merge is NOT safe to run twice against the same device, though:
 * /blackb0x ships a static `dpkg` binary at usr/bin/dpkg alongside apt's
 * own sources.list.d/trusted.gpg.d entries and a debcache — all real
 * dpkg-managed paths once postinstall.sh has actually run apt against them. Re-merging over an
 * already-provisioned device would blindly stomp whatever real state dpkg
 * itself has since written at those exact same paths (up to and including
 * dpkg's own binary, if apt ever replaces it) with this ramdisk's stale,
 * bake-time copies — silent, hard-to-diagnose corruption of a live package
 * database, not a merely-redundant no-op. /var/.blackb0x itself
 * isn't the right signal for that, though — this merge is what CREATES
 * that directory in the first place (postinstall.sh lands inside it), so
 * it already exists after the very first ramdisk run, before
 * postinstall.sh has done any real apt work at all against a repeat boot
 * of this same ramdisk. /var/.blackb0x/install-done is the real
 * signal: postinstall.sh itself only writes it, with a timestamp, at the
 * end of a genuinely successful apt-driven install (see Misc/postinstall.sh)
 * — its presence means real dpkg state now exists to protect, and the safe
 * response is to refuse outright, not press on. */
static int do_install(void) {
    if (access("/mnt1/Applications/AppleTV.app/AppleTV", F_OK) != 0) {
        fprintf(stderr, "Not an AppleTV — refusing to touch this volume\n");
        return 0;
    }

    if (access("/mnt1/var/.blackb0x/install-done", F_OK) == 0) {
        panic("/var/.blackb0x/install-done already exists — refusing to re-run (would clobber live dpkg state)\n");
    }

    printf("Merging blackb0x payload\n");
    int merged = merge_tree("/blackb0x", "/mnt1");
    fixup_etasonuntether_rtbuddyd();
    printf("Finished install\n");

    /* The one on-NAND write of a successful run. Deliberately AFTER the
     * merge, so a record on the NAND means the merge was actually attempted
     * against this volume, and deliberately carrying merge_tree()'s own
     * result — which nothing used to look at at all, so a partial merge was
     * indistinguishable from a clean one in every artifact this left
     * behind. The details of what failed are on the console stream, where
     * merge_tree() printed them as they happened. */
    write_install_record(merged == 0 ? "blackb0x: merged /blackb0x onto the device\n"
                                     : "blackb0x: merged /blackb0x onto the device WITH ERRORS "
                                       "(see the console stream for which entries failed)\n");

    return 0;
}

/* SecureROM/iBoot apparently clears the `auto-boot` NVRAM variable to 0
 * once ANY payload has been executed via DFU/USB boot — signed or not,
 * this isn't specific to unsigned/exploit payloads, just a general "a USB
 * boot session happened" signal — meaning a plain reboot(2) after this
 * ramdisk finishes would otherwise leave the device sitting at the
 * iBoot/DFU prompt instead of continuing
 * into the real, already-installed OS on NAND, needing a second manual
 * boot. Setting it back to 1 right before every reboot is exactly what the
 * ssh-rd-derived rc.boot this replaced also did (see misc/README.md's
 * `rc.boot` entry for the full recovered content) — /usr/sbin/nvram is a
 * real, pristine binary already present on every restore ramdisk (confirmed directly:
 * firmware-sbin's own preinst backs up this exact path before ever
 * touching it), so nothing needs to be staged for this, just invoked.
 * fork(), not vfork(): with fork the child gets its own address space, so
 * it can safely return through the call frame and execve/_exit there,
 * where vfork shares the parent's stack and forbids exactly that. The
 * original binary's own child-spawn helpers (FUN_00006074/FUN_00006134)
 * used fork too. The child execve()s /usr/sbin/nvram and, only if execve()
 * itself fails, _exit()s; the parent waits for it.
 *
 * UNCHANGED IN MEANING NOW THAT WE ARE NOT PID 1, but for a reason worth
 * stating rather than assuming: wait4(pid, ...) names this specific child,
 * so it cannot be confused by anything else. As PID 1 this process was also
 * the reaper of every orphan on the system and a bare wait() would have
 * been ambiguous; as an ordinary LaunchDaemon we have exactly the one child
 * we forked, and orphan reaping is Apple's launchd's problem again. The
 * explicitly empty envp is likewise still right — nvram reads none, and
 * inheriting launchd's environment would only widen what this exec depends
 * on. */
static void set_auto_boot(void) {
    char *argv[] = {"/usr/sbin/nvram", "auto-boot=1", 0};
    char *envp[] = {0};
    pid_t pid = fork();
    if (pid == 0) {
        execve("/usr/sbin/nvram", argv, envp);
        _exit(1); /* only reached if execve() itself failed */
    }
    /* pid > 0: parent, wait for the child. pid < 0: fork failed — nothing
     * to wait for, and auto-boot simply will not have been set. */
    if (pid > 0) {
        wait4(pid, NULL, 0, NULL);
    }
}

/* ---------------------------------------------------------------------- */
/* main() — disk wait, the two mount()s + devfs, do_install(), then        */
/* unmount everything and reboot. dyld calls this directly via LC_MAIN's   */
/* entryoff; there is no crt startup glue (-nostdlib), which is also why   */
/* it takes no argc/argv/envp — dyld passes them, and nothing here wants   */
/* them.                                                                   */
/*                                                                         */
/* The console-fd setup the original opened this function with is GONE.    */
/* fds 1 and 2 arrive already connected to /dev/console, from the          */
/* LaunchDaemon's StandardOutPath/StandardErrorPath — see "Where output    */
/* goes" above.                                                            */
/*                                                                         */
/* KNOWN CONSEQUENCE ON THE LEGACY rc.boot GENERATION, recorded rather     */
/* than silently accepted. Those ramdisks (AppleTV3,2 10B329a, AppleTV2,1  */
/* 11D258) cannot load a LaunchDaemon plist at all, so BakeRamdisk.cpp's   */
/* installEntrypoint() still splices this binary over /sbin/launchd there  */
/* and it runs as PID 1 — with nothing to set up its fds, because the      */
/* kernel does not open /dev/console for init. (That is precisely why the  */
/* original binary opened it by hand; the disassembly shows the open/dup2  */
/* as its first act.) So on that generation stdout/stderr are closed and   */
/* every printf here fails with EBADF. That path is ALREADY the one        */
/* documented as producing a dark, un-enumerated device with no            */
/* restored_external and no display — it is explicitly not endorsed, see   */
/* installEntrypoint()'s bake-time warning — so this costs a diagnostic    */
/* channel that was already of little use. If it is ever wanted back, the  */
/* honest shape is a guard that fires ONLY when launchd did not provide a  */
/* stream (`if (fcntl(1, F_GETFD) == -1) { ... open + dup2 ... }`), never  */
/* an unconditional re-open that would fight the plist on the primary      */
/* path.                                                                   */
/*                                                                         */
/* RETURNING FROM HERE MEANS SOMETHING COMPLETELY DIFFERENT NOW. As PID 1  */
/* an early `return -1` was a kernel-level catastrophe: init exiting takes */
/* the system with it, and in practice the device just died. As a          */
/* LaunchDaemon it is an ordinary job exit with an ordinary status —       */
/* Apple's launchd stays up, restored_external keeps the display lit and   */
/* the device on the USB bus, and the failure is something a host can      */
/* actually look at. The error paths below are therefore diagnostics now   */
/* rather than a second way to brick the boot, and they were left with     */
/* their original return values deliberately: nothing downstream reads     */
/* them, and inventing an exit-code scheme no reader exists for would be   */
/* noise.                                                                  */
/* ---------------------------------------------------------------------- */

int main(void) {
    /* Unbuffered, both streams. This is what replaces the old hand-rolled
     * console_print()'s reason for existing: stdio would line-buffer on a
     * tty and FULLY buffer if /dev/console ever failed the isatty() test,
     * and this process has two paths that never return — panic()'s
     * sleep loop and reboot(2) — where a buffered tail is a lost tail. The
     * cost is one write(2) per printf, which for a few dozen status lines
     * is nothing. stderr is already unbuffered by C's own rules; it is set
     * here anyway so the guarantee is stated in one place rather than
     * inferred. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("Searching for disk...\n");
    /* Original waits on a stat() of /dev/disk0s1s1 succeeding — matches
     * FUN_00006434 (stat) usage at the call site exactly. The buffer's
     * contents are never read; only the success of the call matters. */
    struct stat st;
    while (stat("/dev/disk0s1s1", &st) != 0) {
        printf("Waiting for disk...\n");
        sleep(1);
    }

    printf("\n\n\n\n\n");
    printf("blackb0x Jailbreak - by @NSSpiral\n");
    printf("Mounting filesystem...\n");

    /* CRITICAL: matches FUN_00006028 exactly. Darwin's HFS mount doesn't
     * take the device path as a normal mount(2) argument — it goes in word
     * 0 of an 11-word args struct passed as the `data` (4th) argument,
     * with the rest of the struct left as uninitialized stack garbage
     * (matching the original exactly, garbage and all). Passing NULL here
     * (an earlier draft's mistake, caught by this exact disassembly
     * comparison) would leave the kernel with no device to mount at all —
     * this would have failed outright on real hardware. devfs below
     * genuinely does pass NULL, correctly, since it's a synthetic
     * filesystem with no backing block device. */
    long hfsArgs1[11];
    hfsArgs1[0] = (long)"/dev/disk0s1s1";
    if (mount("hfs", MNT, 0, hfsArgs1) != 0) {
        fprintf(stderr, "Failed to mount / r/w (%s)\n", strerror(errno));
        return -1;
    }
    printf("Main filesystem mounted\n");

    printf("Mounting user filesystem...\n");
    mkdir("/mnt1/private/var2", 0x1ed);
    long hfsArgs2[11];
    hfsArgs2[0] = (long)"/dev/disk0s1s2";
    if (mount("hfs", "/mnt1/private/var", 0, hfsArgs2) != 0) {
        fprintf(stderr, "Failed to mount /var r/w (%s)\n", strerror(errno));
        return -1;
    }
    printf("User Filesystem mounted\n");

    printf("Mounting devices...\n");
    if (mount("devfs", "/mnt1/dev", 0, NULL) != 0) {
        fprintf(stderr, "Unable to mount devices! (%s)\n", strerror(errno));
        unmount("/mnt1", 0);
        set_auto_boot();
        /* 0 is RB_AUTOBOOT. It is written as a bare 0 deliberately: Apple
         * strips <sys/reboot.h> from every iPhoneOS SDK, so the RB_*
         * constants are simply not available here, while `int reboot(int)`
         * IS declared in <unistd.h> on both the 7.1 and 8.4 SDKs and
         * `_reboot` is exported by both ramdisks. Defining RB_AUTOBOOT
         * locally would only re-spell the 0. */
        reboot(0);
        return -1;
    }
    printf("Devices mounted\n");

    do_install();

    unmount("/mnt1/dev", 0);
    unmount("/mnt1", 0);
    printf("Installation complete\n");
    sync();

    printf("Unmounting disks...\n");
    rmdir("/mnt1/private/var2");
    unmount("/mnt1/private/var", 0);
    unmount("/mnt1/dev", 0);
    unmount("/mnt1", 0);

    printf("Flushing buffers...\n");
    sync();

    printf("Rebooting device...\n");
    set_auto_boot();
    /* RB_AUTOBOOT (0), a normal reboot — see the bare-0 note above for why
     * the constant is not spelled out. The original binary passed 1
     * (RB_ASKNAME) here -- a deliberate deviation: RB_ASKNAME is a bootstrap-
     * prompt flag with nothing to act on under iOS, so it was an inert
     * wrong value, and 0 is the correct "reboot normally" request. libc's
     * reboot() is the one-argument wrapper over the two-argument syscall,
     * so the arity bug this project once had to fix by hand cannot recur.
     *
     * THIS IS THE ONE PLACE WHERE NOT BEING PID 1 MAKES US RUDER, NOT
     * SAFER, and it is deliberate. As PID 1 there was nothing else running
     * to disturb; now Apple's launchd and restored_external are alive, and
     * reboot(2) takes them down without a SIGTERM. That is acceptable —
     * restored_external is idle in an accept() loop at this point, it holds
     * no mount of its own and writes nothing (every destructive primitive
     * it has sits behind a host StartRestore message), and both NAND
     * filesystems have already been unmounted and sync()'d above.
     * `launchctl reboot`-style politeness would need /bin/launchctl and a
     * live bootstrap port, and would buy nothing over this. What is NOT
     * acceptable is a host-side restore client being connected while this
     * runs — see entrypoint/README.md; that is a usage rule, not something
     * this code can defend against. The close(consoleFd) that used to sit
     * here is gone with the console fd itself; stdio is unbuffered and the
     * sync() above has already flushed the filesystems. */
    reboot(0);

    return 0;
}
