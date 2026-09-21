/*
 * entrypoint.c
 *
 * Blackb0x's own first-boot installer for the patched restore ramdisk. Not
 * real launchd, and not a replacement for it on ANY path any more:
 * /sbin/launchd is left byte for byte as Apple shipped it on every firmware
 * this project bakes. There are two restore-ramdisk generations and they
 * hand us two different jobs; BakeRamdisk.cpp's installEntrypoint() decides
 * WHERE this binary goes by probing the mounted image, and this file decides
 * WHAT to do once it is running by probing the system it woke up in:
 *
 *   * LaunchDaemons generation (AppleTV3,x 12H1006, the primary target) —
 *     installed as the NEW file /usr/sbin/blackb0x_entrypoint and started by
 *     Apple's own real launchd as an ordinary one-shot LaunchDaemon,
 *     /System/Library/LaunchDaemons/xyz.regulad.blackb0x.entrypoint.plist.
 *     launchd hands us live stdout/stderr, and launchd starts
 *     restored_external itself from com.apple.restored_external.plist.
 *
 *   * rc.boot generation (AppleTV2,1 11D258 today; all three devices if the
 *     10B329a fallback is ever taken) — installed AS /etc/rc.boot, replacing
 *     Apple's own 8,880-byte stub of that name. That generation's launchd
 *     has no daemon-directory loader at all: it spawns /bin/launchctl, whose
 *     system_specific_bootstrap() fwexec()s /etc/rc.boot and blocks in
 *     waitpid(). So rc.boot is the only hook that generation actually
 *     reaches, and standing in for it means inheriting its duties — see
 *     "Standing in for Apple's rc.boot" below.
 *
 * ONE BINARY, NO COMPILE-TIME ROLE. Everything generation-specific below is
 * a RUNTIME PROBE, deliberately. The generation is a property of the RAMDISK
 * IMAGE, not of the device, and the baker only learns which one it has after
 * it mounts the image — i.e. long after this binary was compiled. Plumbing
 * that backwards into a -D flag would encode, at build time, an assumption
 * about where the artifact will land, and when the assumption is wrong the
 * result is a device that boots and silently does the wrong thing. That is
 * the exact failure class this project has spent weeks deleting. A probe
 * asks the real question instead of a proxy for it, costs a few lines, and
 * is self-correcting if we ever retarget. It also means the artifact is
 * IDENTICAL on both generations: one build, one signature, one undefined-
 * symbol closure, and bakeRamdisk()'s verifyEntrypointRuntimeClosure()
 * covers both paths at once.
 *
 * None of this buys anything but observability. It does NOT dodge AMFI —
 * the kernel's exec of PID 1, launchctl's fwexec() of rc.boot and launchd's
 * posix_spawn of a LaunchDaemon all land in the same
 * mac_vnode_check_signature / AMFI execve hook, and nothing here is a
 * weaker position than any other. The baked boot-args (src/Patcher.hpp's
 * `bootargs`) remain the only thing that disables enforcement.
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The on-screen debug channel. screen.h owns the IOSurface lookup and the
 * "never make this a new way to fail" rules; fbtext.h owns the font and the
 * blitter and knows nothing about the device. Included here, before emit()
 * below, and deliberately NOT included anywhere else. */
#include "fbtext.h"
#include "screen.h"

/* ---------------------------------------------------------------------------
 * emit() / emit_err() — ONE call, TWO destinations.
 * ---------------------------------------------------------------------------
 *
 * Every status line, every error, every heartbeat in this file goes through
 * one of these two. They format ONCE into one buffer and then hand that same
 * buffer to stdout (or stderr) and to the screen console.
 *
 * WHY ONE WRAPPER RATHER THAN A SCREEN CALL PLACED BESIDE EACH printf:
 *
 *   1. One format, one buffer, so the two destinations cannot diverge. Two
 *      independently formatted strings are equal only by convention, and the
 *      first time someone edits one and not the other, what a human is
 *      reading on the TV stops matching what is in the log.
 *   2. A call site cannot forget one of them. The screen exists precisely
 *      because we could not see anything; a message that silently goes only
 *      to the dead channel is the exact failure being ended here.
 *   3. Ordering between the two is guaranteed by construction, not by
 *      discipline.
 *
 * ERRORS GO TO THE SCREEN TOO. emit_err() writes to stderr and to the same
 * console. Error output is the output that matters most on a device whose
 * only other failure signal is a black screen; leaving it invisible would
 * invert the priority.
 *
 * THE BUFFER AND TRUNCATION. EMIT_MAX is 512, comfortably more than twice
 * the longest message this file produces (the /mnt mount-failure explanation,
 * ~230 bytes with an errno string). vsnprintf always NUL-terminates and
 * returns the length it WOULD have written, so truncation is detectable — and
 * it is made VISIBLE rather than silently dropping a tail, because a
 * truncated diagnostic that looks complete is its own trap. The marker is
 * ASCII, since the 8x8 font has nothing else.
 *
 * NOT unbuffered-stdio-plus-fflush by accident: main() still sets both
 * streams to _IONBF, and these flush as well, because this process has paths
 * that never return (panic()'s sleep loop, reboot(2)) where a buffered tail
 * is a lost tail.
 *
 * screen.h's own diagnostics deliberately use plain printf, not emit(): they
 * are about the screen and belong on the console even when — especially
 * when — there is no screen. */
#define EMIT_MAX 512
#define EMIT_TRUNC_MARK "...[cut]\n"

static void emit_to(FILE *stream, const char *fmt, va_list ap)
{
    char buf[EMIT_MAX];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n >= (int)sizeof(buf)) {
        /* Overwrite the tail rather than append: the buffer is already full.
         * sizeof(buf) - 1 - strlen(mark) is where the marker starts. */
        memcpy(buf + sizeof(buf) - sizeof(EMIT_TRUNC_MARK), EMIT_TRUNC_MARK,
               sizeof(EMIT_TRUNC_MARK));
    }

    fputs(buf, stream);
    fflush(stream);
    screen_puts(buf);
}

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit_to(stdout, fmt, ap);
    va_end(ap);
}

static void emit_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void emit_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit_to(stderr, fmt, ap);
    va_end(ap);
}

/* WHERE WE MOUNT THE NAND: /mnt, OUR OWN DIRECTORY, created at BAKE time by
 * BakeRamdisk.cpp (createBlackb0xMountpoint()) with the same owner and mode
 * the pristine ramdisk's own mountpoints carry. There is deliberately no
 * mkdir() and no fallback here — if /mnt is missing, the mount(2) below
 * fails loudly and says so, which is the correct response to a ramdisk that
 * was not baked by us.
 *
 * WHY WE STOPPED BORROWING APPLE'S. Three separate problems dissolve at
 * once, and the potted history is worth keeping because each step was a real
 * finding:
 *
 *   * It was /mnt1 for this project's whole life. /usr/local/bin/
 *     restored_external is the ONLY process on either ramdisk that contains
 *     mount code at all, and /mnt1 turned out to be its own system-partition
 *     mountpoint: it carries /sbin/mount, /sbin/mount_hfs,
 *     create_partition_mountpoints, "libpartition, mounting '%s' at '%s'",
 *     and hardcoded /mnt1/private/var and /mnt1/usr/sbin/lsof. It only
 *     mounts on a host StartRestore — already forbidden while we run — so
 *     the collision was conditional, but sharing that name bought nothing.
 *   * /mnt2 was the next answer, and /mnt4 (the intuitive "furthest away"
 *     pick) was rejected because THE MOUNTPOINT SET IS NOT THE SAME ON BOTH
 *     GENERATIONS: 12H1006 ships /mnt1 /mnt2 /mnt3 /mnt4, 10B329a ships only
 *     /mnt1 and /mnt2 (checked directly on both mounted volumes, all empty).
 *     But restored_external names /mnt1 and /mnt2 on 10B329a and /mnt1../mnt4
 *     on 12H1006, so NO borrowed mountpoint is un-referenced by it anywhere.
 *   * So: our own. A directory we create cannot be the one restored_external
 *     reaches for; it exists on both generations by construction, so there
 *     is no per-firmware branch and no "which mountpoints does this image
 *     happen to have" question to get wrong later.
 *
 * AND IT IS CREATED AT BAKE TIME, NOT HERE, which is the subtle part. A
 * runtime mkdir() would depend on the ramdisk root being writable at that
 * moment — exactly the thing ensure_root_writable() exists because we cannot
 * assume. The baker has no such problem: it already has the image attached
 * read-write to stage /blackb0x and the entrypoint itself.
 *
 * The minimal-footprint principle this used to be justified by is NOT
 * abandoned, just paid for honestly: one empty directory is a trivial
 * addition beside the overlay, and BakeRamdisk.cpp's header states the full
 * per-generation footprint rather than leaving a reader to wonder. */
#define MNT "/mnt"

/* The on-NAND install record. MNT/var is a symlink to MNT/private/var,
 * which is where /dev/disk0s1s2 (the data partition) is mounted by main()
 * before do_install() ever runs — so this path only resolves after that
 * mount, which is exactly why the LaunchDaemon plist cannot point
 * StandardOutPath here. See "Where output goes" below.
 *
 * IT IS ON THE SYSTEM PARTITION, AND IT TOOK TWO FAILURES TO GET THERE.
 *
 * It was /var/mobile/Media/blackb0x_install.log, this project's path since
 * the original. The first run where the screen console could show an error
 * said:
 *
 *     entrypoint: cannot open /mnt/var/mobile/Media/blackb0x_install.log (o
 *
 * The obvious reading was that /var/mobile/Media is a data-protected
 * location, so it moved to /var/.blackb0x — beside install-done and the
 * postinstall logs, directly under /var, which carries no protection class.
 * SAME EPERM, as root. What settled it was that the mkdir of that directory
 * SUCCEEDED on the same volume in the same run: a directory needs no
 * per-file content key, a regular file does, and nothing on a restore
 * ramdisk loads a keybag to supply one. The whole data partition refuses new
 * regular files; the volume never cared which directory we picked.
 *
 * So the record lives on the SYSTEM partition, which accepts writes — 130
 * merge entries failed on that hardware run and every one of them was bound
 * for /var, while everything bound for the system partition landed. It sits
 * in /usr/share/blackb0x, the directory this project now owns there, beside
 * postinstall.sh and the staged /var payload.
 *
 * NOT under /usr/share/blackb0x/var: that subtree is the staging area the
 * first-boot daemon moves into /var and then deletes. This record is about
 * what the RAMDISK did, so it belongs somewhere permanent, not somewhere
 * designed to be consumed and removed.
 *
 * The directory is created if absent, because panic() can call this before
 * the merge that would otherwise have supplied it. */
#define BLACKB0X_STATE_DIR MNT "/usr/share/blackb0x"
#define INSTALL_LOG        BLACKB0X_STATE_DIR "/install.log"

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
/* ONE STREAM, and this process configures it only when nobody else did.    */
/* Everything this binary has to say goes to stdout (progress) or stderr    */
/* (errors) with plain printf/fprintf. On the LaunchDaemons generation      */
/* launchd connects both to /dev/console from the unit's StandardOutPath/   */
/* StandardErrorPath — exactly the way Apple's own                          */
/* com.apple.restored_external.plist does it on that same ramdisk — and     */
/* ensure_console_fds() below then does nothing at all.                     */
/*                                                                         */
/* ON THE rc.boot GENERATION NOBODY SETS THEM UP. The kernel does not open  */
/* /dev/console for init there, launchd inherits nothing, launchctl         */
/* inherits nothing, and the /etc/rc.boot it fwexec()s inherits nothing —   */
/* which is precisely why the ORIGINAL binary opened /dev/console by hand   */
/* as its very first act (the disassembly shows the open/dup2). Without a   */
/* guard every printf here would fail with EBADF on that path and we would  */
/* be blind on exactly the generation with the least other observability.   */
/* ensure_console_fds() restores it, and does so by TESTING each descriptor */
/* rather than by asking which generation this is: `fcntl(fd, F_GETFD) ==   */
/* -1` is a direct question about the thing that actually matters.          */
/*                                                                         */
/* WHAT WAS DELETED, and why it was not just a cleanup:                     */
/*                                                                         */
/*   - console_print(): opened /dev/console itself, dup2()'d it onto fds 1  */
/*     and 2 UNCONDITIONALLY from main(), and then wrote to it one          */
/*     write(2) per byte with a sync() after each. The per-byte writer is   */
/*     gone for good and is not coming back; the open/dup2 is back, but     */
/*     only behind the fd test above, because an unconditional re-open      */
/*     would fight whatever launchd already attached on the primary path.   */
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
/* mounted anything. INSTALL_LOG lives under /mnt, i.e. on the NAND, which */
/* does not exist yet at that moment — and the failure would not even be    */
/* loud: launchd would create the file on the RAMDISK under the empty       */
/* /mnt mountpoint, we would then mount the real volume over the top, and   */
/* every byte would land in an invisible, shadowed file that dies with the  */
/* RAM disk.                                                                */
/*                                                                         */
/* THE ALTERNATIVE THAT WAS REJECTED, since it is the obvious one: point    */
/* StandardOutPath at a path on the ramdisk itself and copy the finished    */
/* file onto the NAND once /mnt is mounted. Two things kill it.             */
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

    if (stat(BLACKB0X_STATE_DIR, &st) != 0) {
        emit("entrypoint: %s absent on the target volume — creating it\n", BLACKB0X_STATE_DIR);
        if (mkdir(BLACKB0X_STATE_DIR, 0755) != 0) {
            emit_err("entrypoint: cannot create %s (errno %d, %s) — install record NOT written\n",
                     BLACKB0X_STATE_DIR, errno, strerror(errno));
            return;
        }
        chown(BLACKB0X_STATE_DIR, UID_MOBILE, GID_STAFF);
    }

    /* The numeric errno goes out alongside strerror's text deliberately:
     * "Operation not permitted" and "Operation not supported" read almost
     * identically at a glance on a TV and mean very different things. */
    int fd = open(INSTALL_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        emit_err("entrypoint: cannot open %s (errno %d, %s) — install record NOT written\n",
                 INSTALL_LOG, errno, strerror(errno));
        return;
    }
    (void)write(fd, outcome, strlen(outcome));
    close(fd);
    if (chown(INSTALL_LOG, UID_MOBILE, GID_STAFF) != 0 || chmod(INSTALL_LOG, 0644) != 0) {
        emit_err("entrypoint: cannot set 501:20 0644 on %s (errno %d, %s)\n",
                 INSTALL_LOG, errno, strerror(errno));
    }
    emit("entrypoint: install record written to %s\n", INSTALL_LOG);
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
        emit_err("Unable to find source file: %s\n", src);
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
/* it onto /mnt, overwriting whatever's already there. No branching on     */
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
 * under /blackb0x) becomes /mnt/--early-boot.
 *
 * opendir/readdir rather than the raw getdirentries(2) this used to parse
 * by hand. readdir() reports end-of-directory and a real error the same
 * way (NULL), so errno is cleared immediately before each call and read
 * back immediately after — that is what preserves the old `n < 0` ->
 * "failed to get directories" path, which a bare NULL check would silently
 * turn into "directory ended early, all fine". */
/* MERGE FAILURES WERE SILENT, AND THAT IS HOW THIS WENT UNNOTICED.
 *
 * merge_tree() recorded a failure by setting `ok = 0` and moving on, so a
 * run in which every file on one volume was refused looked, from the
 * outside, exactly like a run in which one file was. The only survivor was a
 * single summary line at the end saying errors had happened "see the console
 * stream" — a stream that, until the screen console existed, nothing could
 * read.
 *
 * Counting rather than printing each one is deliberate. If a whole volume is
 * refusing writes there will be thousands of failures, and thousands of
 * lines would push the useful part of the log off a screen that holds about
 * ninety. So: a total, and the FIRST failure in full, with its errno. One
 * example plus a count is what distinguishes "a file was busy" from "the
 * volume said no", which is the only distinction that matters here. */
static unsigned g_mergeFailures;
static char     g_mergeFirstPath[512];
static int      g_mergeFirstErrno;

static void note_merge_failure(const char *path) {
    if (g_mergeFailures == 0) {
        g_mergeFirstErrno = errno;
        snprintf(g_mergeFirstPath, sizeof(g_mergeFirstPath), "%s", path);
    }
    g_mergeFailures++;
}

static void report_merge_failures(void) {
    if (g_mergeFailures == 0) return;
    emit_err("  %u entr%s failed; first was %s (errno %d, %s)\n",
             g_mergeFailures, g_mergeFailures == 1 ? "y" : "ies",
             g_mergeFirstPath, g_mergeFirstErrno, strerror(g_mergeFirstErrno));
}

static int merge_tree(const char *src, const char *dst) {
    DIR *dir = opendir(src);
    if (dir == NULL) {
        emit_err("cannot open blackb0x source dir %s (%s)\n", src, strerror(errno));
        return -1;
    }

    int ok = 1;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (de == NULL) {
            if (errno != 0) {
                closedir(dir);
                emit_err("failed to get directories under %s (%s)\n", src, strerror(errno));
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
            if (tn < 0) { ok = 0; note_merge_failure(srcPath); continue; }
            target[tn] = '\0';
            unlink(dstPath);
            if (symlink(target, dstPath) != 0) { ok = 0; note_merge_failure(dstPath); }
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
            if (stat(srcPath, &srcSt) != 0) { ok = 0; note_merge_failure(srcPath); continue; }
            int mode = (int)(srcSt.st_mode & 07777);
            if (install_file(srcPath, dstPath, (int)srcSt.st_uid, (int)srcSt.st_gid, mode) != 0) {
                ok = 0;
                note_merge_failure(dstPath);
            }
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
    /* Red from here on, and a blank-line-free banner, so a panic is
     * distinguishable at TV distance from the ordinary progress lines above
     * it. screen_alert() is a no-op when no surface was ever found, which is
     * the only way it can fail. */
    screen_alert();
    emit_err("PANIC: %s", msg);
    snprintf(record, sizeof(record), "PANIC: %s", msg);
    write_install_record(record);
    sync();
    for (;;) sleep(60);
}

/* Whether /mnt/private/var/lib/dpkg/status (already written onto the real
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
        emit_err("dpkg status file larger than expected — package-state check skipped\n");
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
 * specific device's actual /mnt/usr/libexec/rtbuddyd content, so
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
    if (!dpkg_status_has_installed_package(MNT "/private/var/lib/dpkg/status", "net.tihmstar.etasonuntether")) {
        return;
    }
    if (access(MNT "/usr/libexec/rtbuddyd.orig", F_OK) == 0) {
        return; /* already backed up on a prior run */
    }
    if (access(MNT "/usr/libexec/rtbuddyd", F_OK) == 0) {
        if (copy_preserving(MNT "/usr/libexec/rtbuddyd", MNT "/usr/libexec/rtbuddyd.orig") != 0) {
            emit_err("failed to back up rtbuddyd before etasonuntether symlink\n");
            return;
        }
        unlink(MNT "/usr/libexec/rtbuddyd");
    }
    /* Either rtbuddyd was just backed up and removed above, or there was
     * never a real one to back up in the first place — the real postinst
     * symlinks unconditionally in that second case too (its own `else`
     * branch). */
    if (symlink("/System/Library/Frameworks/JavaScriptCore.framework/Resources/jsc",
                MNT "/usr/libexec/rtbuddyd") != 0) {
        emit_err("failed to symlink rtbuddyd -> jsc for etasonuntether\n");
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
 * isn't the right signal for that: it is not even ours to create any more —
 * this ramdisk cannot create anything on the data partition, so that
 * directory now appears only when the first-boot daemon moves the staged
 * payload into place. /var/.blackb0x/install-done is the real
 * signal: postinstall.sh itself only writes it, with a timestamp, at the
 * end of a genuinely successful apt-driven install — its presence means real
 * dpkg state now exists to protect, and the safe response is to refuse
 * outright, not press on.
 *
 * READING it still works, which is the only reason this check survived the
 * move. The data partition refuses new regular files; it serves existing
 * ones normally, so an access(F_OK) against a file the booted OS wrote is
 * exactly as reliable as it ever was. */
static int do_install(void) {
    if (access(MNT "/Applications/AppleTV.app/AppleTV", F_OK) != 0) {
        emit_err("Not an AppleTV — refusing to touch this volume\n");
        return 0;
    }

    if (access(MNT "/var/.blackb0x/install-done", F_OK) == 0) {
        panic("/var/.blackb0x/install-done already exists — refusing to re-run (would clobber live dpkg state)\n");
    }

    emit("Merging blackb0x payload\n");
    int merged = merge_tree("/blackb0x", MNT);
    report_merge_failures();
    fixup_etasonuntether_rtbuddyd();
    emit("Finished install\n");

    /* The one on-NAND write of a successful run. Deliberately AFTER the
     * merge, so a record on the NAND means the merge was actually attempted
     * against this volume, and deliberately carrying merge_tree()'s own
     * result — which nothing used to look at at all, so a partial merge was
     * indistinguishable from a clean one in every artifact this left
     * behind. The failure count and the first failing entry are on the
     * console just above, via report_merge_failures(); the count goes into
     * the record too, so the artifact says how bad it was rather than only
     * that it was bad. */
    char record[256];
    if (merged == 0) {
        snprintf(record, sizeof(record), "blackb0x: merged /blackb0x onto the device\n");
    } else {
        snprintf(record, sizeof(record),
                 "blackb0x: merged /blackb0x onto the device WITH ERRORS "
                 "(%u entries failed, first %s: errno %d)\n",
                 g_mergeFailures, g_mergeFirstPath, g_mergeFirstErrno);
    }
    write_install_record(record);

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

/* ====================================================================== */
/* Standing in for Apple's rc.boot                                        */
/* ====================================================================== */
/*                                                                        */
/* On the rc.boot generation this binary IS /etc/rc.boot: bakeRamdisk()    */
/* replaces Apple's own 8,880-byte stub of that name with ours. That is    */
/* the only hook that generation reaches (its launchd has no daemon-       */
/* directory loader; launchctl's system_specific_bootstrap() fwexec()s     */
/* /etc/rc.boot and blocks in waitpid()), and standing in for a file means */
/* INHERITING WHAT IT DID. Apple's stub is tiny and fully disassembled —   */
/* five imports, _getfsfile _mount _umask _execl _reboot, and four         */
/* strings:                                                               */
/*                                                                        */
/*     f = getfsfile("/");                                                */
/*     if (!f) reboot(0);                                                 */
/*     if (mount(f->fs_vfstype, "/", 0x10001, &args{f->fs_spec}))         */
/*         reboot(0);                                                     */
/*     umask(0);                                                          */
/*     for (p in {restored_external, restored_update, restored, ramrod})  */
/*         execl(p, p, NULL);                                             */
/*     reboot(0);                                                         */
/*                                                                        */
/* Three corrections to how that has been described in this repo, all read */
/* off the real binary rather than inferred:                              */
/*                                                                        */
/*   1. **0x10001 is MNT_UPDATE|MNT_RDONLY, not "read-write".** Apple's    */
/*      rc.boot DEMOTES the ramdisk root to read-only; it does not promote */
/*      it. So the root arrives from the kernel writable and Apple gives   */
/*      that up before handing off to restored_external. We deliberately   */
/*      do NOT replicate the demotion (see ensure_root_writable()).        */
/*   2. **getfsfile() here never reads /etc/fstab.** There is no           */
/*      /etc/fstab on either ramdisk, and the legacy ramdisk's own         */
/*      libsystem_c contains the string "fstab" nowhere at all — it        */
/*      imports _getfsstat/_statfs/_fstatfs and synthesises the entry from */
/*      the LIVE MOUNT TABLE. Apple's rc.boot is therefore asking the      */
/*      kernel what / actually is, with getfsfile() as the middleman.      */
/*      statfs("/") asks the same question directly, so that is what       */
/*      ensure_root_writable() uses: same answer, one fewer moving part,   */
/*      no dependence on a libc fallback that is invisible in the headers. */
/*   3. The execl() list always lands on the first entry. /usr/local/bin   */
/*      holds exactly restored_external and ioflashstoragetool on BOTH     */
/*      generations; restored_update, restored and ramrod do not exist on  */
/*      either. The list is still walked in Apple's order below, because   */
/*      being a faithful stand-in costs three array entries.               */

/* Give this process a console if, and ONLY if, nobody already gave it    */
/* one. See "Where output goes" above for the whole argument; the short   */
/* form is that launchd attaches /dev/console from the unit's Standard*   */
/* Path keys on the LaunchDaemons generation, and NOTHING attaches        */
/* anything on the rc.boot generation, where the kernel does not even     */
/* open /dev/console for init.                                           */
/*                                                                        */
/* fcntl(fd, F_GETFD) is the cheapest possible "is this descriptor open?" */
/* — it touches no file, allocates nothing, and returns -1/EBADF exactly  */
/* when the descriptor is closed. The two descriptors are tested          */
/* INDEPENDENTLY rather than assuming they share a state: there is no     */
/* rule that says a process handed a stdout was also handed a stderr, and */
/* clobbering a live fd 2 to fix a dead fd 1 would be its own bug.        */
/*                                                                        */
/* O_WRONLY, not O_RDWR: this process never reads the console, and asking */
/* for read access on a tty we are not the session leader of is a         */
/* needless way to fail. The extra descriptor is closed once duplicated   */
/* unless it already landed on 0/1/2. Failure is silent-but-reported and  */
/* never fatal — a missing console is a lost diagnostic, not a reason to  */
/* abandon an install. */
static void ensure_console_fds(void) {
    int needOut = (fcntl(1, F_GETFD) == -1);
    int needErr = (fcntl(2, F_GETFD) == -1);
    if (!needOut && !needErr) {
        return; /* launchd already attached both — leave them alone */
    }

    int fd = open("/dev/console", O_WRONLY);
    if (fd < 0) {
        return; /* nothing to report it ON; this is the blind case */
    }
    if (needOut) dup2(fd, 1);
    if (needErr) dup2(fd, 2);
    if (fd > 2) close(fd);
}

/* Make sure the ramdisk root is writable, by ASKING rather than by        */
/* inferring it from the generation.                                      */
/*                                                                        */
/* statfs("/") reports the live mount's own device (f_mntfromname), its    */
/* own filesystem type (f_fstypename) and its own flags, so the whole      */
/* decision and every argument to the fixup come from the kernel. If       */
/* MNT_RDONLY is clear we are already writable and this is a no-op — which */
/* is what it is expected to be on BOTH generations, since the kernel      */
/* mounts an md0 ramdisk root read-write and Apple's rc.boot only ever     */
/* took that away again.                                                  */
/*                                                                        */
/* WHY PROMOTE RATHER THAN DEMOTE, when Apple demotes: we are not the last */
/* thing to run on this ramdisk the way Apple's rc.boot is. Apple's stub   */
/* execs restored_external and is gone; we keep running, we fork           */
/* restored_external alongside ourselves, and a read-only root would turn  */
/* any future need to write there (a scratch file, a socket, a mountpoint  */
/* we did not anticipate) into a silent failure on the generation with the */
/* least observability. Nothing here writes to the ramdisk root TODAY —    */
/* /blackb0x is only read, and every write goes to MNT, i.e. the NAND — so */
/* this is a safety net rather than a dependency, and it is written to     */
/* announce itself either way.                                            */
/*                                                                        */
/* The 11-word args block is the same shape main()'s HFS mounts use and    */
/* the same shape Apple's rc.boot builds (44 bytes, fully zeroed, word 0 = */
/* the device path): Darwin's HFS mount takes its device out of word 0 of  */
/* the mount data rather than as a normal mount(2) argument. Zeroed in     */
/* full here, unlike main()'s two call sites, which deliberately reproduce */
/* the original binary's uninitialised stack garbage. */
static void ensure_root_writable(void) {
    struct statfs fs;
    if (statfs("/", &fs) != 0) {
        emit_err("entrypoint: statfs(\"/\") failed (%s) — cannot tell if the ramdisk root is writable\n",
                strerror(errno));
        return;
    }
    if ((fs.f_flags & MNT_RDONLY) == 0) {
        return; /* already read-write; nothing to do */
    }

    long args[11];
    memset(args, 0, sizeof(args));
    args[0] = (long)fs.f_mntfromname;
    if (mount(fs.f_fstypename, "/", MNT_UPDATE, args) != 0) {
        emit_err("entrypoint: could not remount / read-write from %s (%s)\n", fs.f_mntfromname,
                strerror(errno));
        return;
    }
    emit("Remounted the ramdisk root read-write\n");
}

/* Apple's rc.boot order, preserved: restored_external first, and the three
 * it falls back to after. Only the first exists on either ramdisk
 * (/usr/local/bin is exactly restored_external + ioflashstoragetool on both
 * 10B329a and 12H1006), so the rest are faithfulness rather than function. */
static const char *const kRestoredCandidates[] = {
    "/usr/local/bin/restored_external",
    "/usr/local/bin/restored_update",
    "/usr/local/bin/restored",
    "/usr/libexec/ramrod/ramrod",
};

/* Bring up restored_external if nothing else is going to.                 */
/*                                                                         */
/* THIS IS THE WHOLE REASON THE rc.boot BRANCH EXISTS. restored_external is */
/* the only binary on either ramdisk that calls                            */
/* IOUSBDeviceControllerCreate / IOUSBDeviceDescriptionCreateFromDefaults / */
/* IOUSBDeviceControllerSetDescription, and on this hardware the USB device */
/* stack stays OFF THE BUS until a userspace process does exactly that      */
/* (the DeviceTree's usb0-device says configuration-string =               */
/* "standardMuxOnly" while the in-kernel auto-configurator personality      */
/* matches only "standardBringup", so it never fires). It is also what      */
/* draws the display: it links IOMobileFramebuffer and IOSurface and owns   */
/* /usr/share/progressui. Without it a perfect run is indistinguishable     */
/* from a dead device.                                                      */
/*                                                                         */
/* THE PROBE. The question that matters is "will something else start it?", */
/* and the direct, causal, observable answer is whether a LaunchDaemon      */
/* exists that does. com.apple.restored_external.plist is present on the    */
/* LaunchDaemons generation and absent (with the whole directory) on the    */
/* rc.boot one, so this one stat() distinguishes the two environments by    */
/* asking about the exact mechanism in question rather than about a proxy   */
/* for it — and it stays correct if that plist is ever removed from a       */
/* ramdisk that still has the directory. The alternatives were weighed and  */
/* rejected: a process scan needs a userland this ramdisk does not have,    */
/* and asking IOKit whether the device controller is already configured     */
/* would mean linking IOKit — a second LC_LOAD_DYLIB dragging in            */
/* CoreFoundation, libobjc and libicucore, for a probe, against a binary    */
/* whose one-dylib closure is the thing that makes the bake-time closure    */
/* check tractable.                                                         */
/*                                                                         */
/* FORK, NOT EXEC, and this is the load-bearing decision. Apple's rc.boot   */
/* execl()s: it REPLACES ITS OWN PROCESS IMAGE, which for us would mean the */
/* install never happens. Exec'ing at the END instead is worse than it      */
/* sounds — the display and USB would come up only after the merge, i.e.    */
/* seconds before reboot(2) tears them down, so the entire window we are    */
/* trying to make observable would already be over. Forking first puts      */
/* restored_external up DURING our run: the device enumerates as 05ac:12a7  */
/* while the merge is in flight, and AppleUSBDeviceMux (prelinked into both */
/* kernelcaches) makes anything listening reachable from the host.          */
/*                                                                         */
/* WE DO NOT WAIT FOR IT, deliberately. It never exits — it blocks in an    */
/* accept() loop — so a wait4() here would hang the install forever, and    */
/* widening the window in which the NAND is mounted and half-written is     */
/* precisely the wrong direction (see main()'s teardown). It is reparented  */
/* to launchd when we exit, which is launchd's ordinary business.           */
/*                                                                         */
/* KNOWN TAIL RISK, recorded rather than defended against. On BOTH          */
/* generations (12H1006 as well as 10B329a — an earlier note in this repo   */
/* called it 10B329a-specific and that was wrong) restored_external's       */
/* main() with no arguments forks and execs ITSELF with -server, and the    */
/* parent waitpid()s; when that child exits for any reason the parent logs  */
/* "restored exited ... - rebooting" and reboots via /sbin/reboot. If that  */
/* happens mid-merge the device reboots with our mount dirty. With no host  */
/* attached the child blocks in accept() forever, so this is a tail risk    */
/* rather than a likely one, and the mitigation is to keep the dirty        */
/* window short (main() syncs and unmounts the instant the merge returns)   */
/* rather than to add a lock or a wait — nothing else on the ramdisk        */
/* touches block devices at all (launchd, launchctl, xpcproxy, syslogd and  */
/* ReportCrash contain zero references to /mnt, disk0, rdisk or any mount   */
/* call), so there is nothing to lock against.                              */
/*                                                                         */
/* Its own startup is non-destructive and that was checked, not assumed: it */
/* sets an IOPMUBootStage property, starts a gas-gauge thread, creates a    */
/* listen socket, disables the watchdog, calls enable_usb_connections() and */
/* blocks. Every destructive primitive it has (WipeStorageDevice,           */
/* clean_NAND, FormatForLwVM, partition_nand_device, asr) sits behind a     */
/* host StartRestore message — which is why pointing a restore client at    */
/* the device while this runs is forbidden. A bonus for our own output: its */
/* log primitive ends at fputs(msg, __stdoutp) and it drains syslogd's ASL  */
/* store on the way, re-printing every record as "SYSLOG: %s", so once it   */
/* is up anything logged through ASL reaches /dev/console too.              */
static void start_restored_external(void) {
    struct stat st;
    if (stat("/System/Library/LaunchDaemons/com.apple.restored_external.plist", &st) == 0) {
        emit("restored_external has its own LaunchDaemon here — leaving it to launchd\n");
        return;
    }

    const char *prog = NULL;
    for (unsigned i = 0; i < sizeof(kRestoredCandidates) / sizeof(kRestoredCandidates[0]); i++) {
        if (access(kRestoredCandidates[i], X_OK) == 0) {
            prog = kRestoredCandidates[i];
            break;
        }
    }
    if (prog == NULL) {
        emit_err("entrypoint: no restored binary on this ramdisk — the display and USB will stay down, "
                 "and this install will be unobservable\n");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {(char *)prog, 0};
        char *envp[] = {0};
        execve(prog, argv, envp);
        /* Only reached if the exec itself failed. Say so on the console we
         * just inherited from the parent, then leave — the parent is the
         * one doing the install and must not be disturbed. */
        emit_err("entrypoint: cannot exec %s (%s)\n", prog, strerror(errno));
        _exit(127);
    }
    if (pid < 0) {
        emit_err("entrypoint: fork() for %s failed (%s) — continuing without display or USB\n", prog,
                strerror(errno));
        return;
    }
    emit("Started %s (pid %d) for display and USB bring-up\n", prog, (int)pid);
}

/* ---------------------------------------------------------------------- */
/* Saying what we found, not just that we got there.                       */
/*                                                                          */
/* Until the screen console worked, extra logging was writing into a hole,  */
/* so this binary said only "Main filesystem mounted" and moved on. Now     */
/* that a human can watch a run, the interesting facts are the ones that    */
/* distinguish a mount that worked from a mount that succeeded onto the     */
/* wrong thing: which device backs it, how big it is, how much room is left */
/* (the merge needs room), and whether the directories the next step is     */
/* about to use are actually there. Every one of these is a question this   */
/* project has had to answer by guessing at least once.                     */
/*                                                                          */
/* Both are pure reporting: they never fail the run, and a probe that       */
/* cannot answer says so and returns.                                       */
/* ---------------------------------------------------------------------- */

static void report_volume(const char *label, const char *path) {
    struct statfs fs;
    if (statfs(path, &fs) != 0) {
        emit_err("  %s: statfs(%s) failed (errno %d, %s)\n",
                 label, path, errno, strerror(errno));
        return;
    }
    emit("  %s: %s on %s, %llu MiB total, %llu MiB free\n", label,
         fs.f_mntfromname, fs.f_mntonname,
         (unsigned long long)fs.f_blocks * fs.f_bsize >> 20,
         (unsigned long long)fs.f_bavail * fs.f_bsize >> 20);
}

/* WHICH VOLUME WILL ACCEPT A NEW FILE, AND WHICH WILL NOT.
 *
 * The install record failed with EPERM at /var/mobile/Media, so it was moved
 * to /var/.blackb0x — and failed there too, with the same errno, as root.
 * That rules out the explanation that was obvious at /var/mobile/Media
 * (Media is a data-protected location) and replaces it with a much more
 * serious possibility: that NOTHING can create a file on the data partition
 * from this ramdisk.
 *
 * That would matter far beyond one log. /blackb0x stages a large part of its
 * payload into /var and /private/var — dpkg's database, apt's lists, this
 * project's own /private/var/.blackb0x — all of which live on disk0s1s2.
 * If creates are refused there, the install has been failing quietly for
 * every one of those files, and the log was simply the first to say so out
 * loud now that there is a screen to say it on.
 *
 * TWO EXPLANATIONS FIT, and they call for opposite responses, which is
 * exactly why this measures instead of guessing.
 *
 *   1. The VOLUME refuses. iOS content protection: on a CP-enabled volume a
 *      new file needs a per-file key wrapped by a class key from the keybag.
 *      Nothing here loads one — /usr/libexec/keybagd is referenced by
 *      launchd's embedded bootstrap but is NOT PRESENT on this ramdisk, and
 *      the only MobileKeyBag symbols restored_external imports are
 *      MKBKeyBagCreateSystem and MKBDeviceObliterateClassDKey, i.e. the
 *      restore-time destructive ones, not "load the existing bag". Reads of
 *      existing class-D files keep working, which matches what we see. This
 *      device has no passcode, so the class keys need no user input; the bag
 *      would just need loading, and loading it is setup this project does
 *      not currently do.
 *
 *   2. The PROCESS is refused. EPERM is also the canonical sandbox denial,
 *      and an ad-hoc-signed binary that the kernel does not treat as a
 *      platform binary can end up under a default profile regardless of the
 *      AMFI boot-args. If that is what is happening, the data partition is
 *      innocent and no amount of keybag work would help.
 *
 * The probes below separate them. A create on the RAMDISK's own root is the
 * control: it is local memory, not content-protected, and already writable
 * (ensure_root_writable ran). If even that is refused, the restriction is on
 * us and hypothesis 2 is the answer. If the ramdisk and the system partition
 * accept a file and only the data partition refuses, it is the volume, and
 * mkdir-vs-create then says whether it is specifically per-file keys —
 * directories need none, so mkdir succeeding where create fails is content
 * protection all but confirmed.
 *
 * Reading the data partition is the fourth control: a volume that lists its
 * own entries is genuinely mounted, so a create failure on it is a refusal
 * rather than a consequence of a mount that silently did nothing.
 *
 * Every probe cleans up after itself. Nothing here fails the run. */
static void probe_writability(void) {
    struct { const char *what; const char *path; int isDir; } probes[] = {
        { "create on ramdisk root",     "/.blackb0x-probe",                      0 },
        { "create on system partition", MNT "/.blackb0x-probe",                  0 },
        { "mkdir  on data partition",   MNT "/private/var/.blackb0x-probe.d",    1 },
        { "create on data partition",   MNT "/private/var/.blackb0x-probe",      0 },
        { "create in existing data dir", MNT "/private/var/mobile/.blackb0x-probe", 0 },
    };

    emit("Probing what each volume will accept...\n");
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (probes[i].isDir) {
            if (mkdir(probes[i].path, 0755) != 0) {
                emit("  %s: REFUSED (errno %d, %s)\n", probes[i].what, errno, strerror(errno));
            } else {
                emit("  %s: ok\n", probes[i].what);
                rmdir(probes[i].path);
            }
            continue;
        }
        int fd = open(probes[i].path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            emit("  %s: REFUSED (errno %d, %s)\n", probes[i].what, errno, strerror(errno));
        } else {
            close(fd);
            emit("  %s: ok\n", probes[i].what);
            unlink(probes[i].path);
        }
    }

    /* Reading the data partition, to prove the mount itself is sound. A
     * volume that lists its own entries is mounted; a volume that cannot is
     * a different bug entirely and would make every create failure above a
     * consequence rather than a cause. */
    DIR *d = opendir(MNT "/private/var");
    if (!d) {
        emit_err("  read   on data partition: REFUSED (errno %d, %s)\n", errno, strerror(errno));
    } else {
        int n = 0;
        while (readdir(d) != NULL) n++;
        closedir(d);
        emit("  read   on data partition: ok, %d entries\n", n);
    }
}

static void report_path(const char *path) {
    struct stat st;
    const char *kind;
    if (lstat(path, &st) != 0) {
        emit("  %s: ABSENT (errno %d, %s)\n", path, errno, strerror(errno));
        return;
    }
    kind = S_ISDIR(st.st_mode)  ? "dir"  :
           S_ISREG(st.st_mode)  ? "file" :
           S_ISLNK(st.st_mode)  ? "link" : "other";
    emit("  %s: %s mode 0%o uid %u gid %u size %llu\n", path, kind,
         (unsigned)(st.st_mode & 07777), (unsigned)st.st_uid,
         (unsigned)st.st_gid, (unsigned long long)st.st_size);
}

/* THE ONE PLACE THIS BINARY WAITS TO BE READ.
 *
 * Both endings of a run now go through here, and they did not used to. The
 * no-overlay diagnostic ending had its own ten-second heartbeat loop, and the
 * normal ending had no wait at all: it printed "Installation complete" and
 * called reboot(2) on the next line, so the last thing a human most wants to
 * see was on screen for whatever fraction of a second the kernel took to tear
 * the display down. Two different endings meant two different amounts of time
 * to read them, which is exactly backwards -- the interesting ending is the
 * one that goes past fastest.
 *
 * So: one function, one interval, announced. Thirty seconds is long enough to
 * read a screenful and short enough that an unattended run is not wedged, and
 * saying so on the way in means a still screen is legible as "waiting" rather
 * than as "hung", which on a device with no other output channel is the whole
 * difference between a measurement and a mystery.
 *
 * THE LINE GOES TO BOTH DESTINATIONS BUT NOT THE SAME WAY, and that is
 * arithmetic rather than inconsistency. The console has free scrollback, so
 * it gets an ordinary line. The screen console is about forty rows; a caller
 * that loops here would wrap it in twenty minutes and erase the boot lines
 * the run exists to show, so the screen gets the same text on its
 * non-scrolling status row, rewritten in place. One buffer, formatted once,
 * two destinations.
 *
 * SAFE AT BOTH CALL SITES because both call it with the NAND already
 * unmounted and sync()'d. Waiting while mounted would widen the window in
 * which restored_external can decide to reboot under us (see the teardown
 * comment in main()); waiting after the teardown cannot. */
#define HOLD_SECONDS 30

static void wait_to_be_read(void)
{
    char line[SCREEN_LINE_MAX];
    snprintf(line, sizeof(line), "waiting %d seconds...", HOLD_SECONDS);
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    screen_status(line);
    sleep(HOLD_SECONDS);
}

/* ---------------------------------------------------------------------- */
/* main() — disk wait, the two mount()s + devfs, do_install(), then        */
/* unmount everything and reboot. dyld calls this directly via LC_MAIN's   */
/* entryoff; there is no crt startup glue (-nostdlib), which is also why   */
/* it takes no argc/argv/envp — dyld passes them, and nothing here wants   */
/* them.                                                                   */
/*                                                                         */
/* THE FIRST THREE CALLS ARE THE RUNTIME PROBES, and they run before       */
/* anything else because everything else is better with them and none of   */
/* them can hurt. ensure_console_fds() gives us output if nobody else did; */
/* ensure_root_writable() promotes the ramdisk root if it arrived          */
/* read-only; start_restored_external() puts the display and USB up if no  */
/* LaunchDaemon is going to. Each one asks a direct question about the     */
/* system it is standing in rather than about which generation it is, so   */
/* the same instructions are correct in both environments — see each       */
/* function's own comment, and the header's "ONE BINARY, NO COMPILE-TIME   */
/* ROLE".                                                                  */
/*                                                                         */
/* Putting them ahead of the disk-wait loop is deliberate: that loop is    */
/* unbounded, so anything sequenced after it is hostage to a disk that     */
/* never appears — which is exactly the failure we most want a lit display */
/* and a USB-enumerated device to be able to report.                       */
/*                                                                         */
/* RETURNING FROM HERE IS NO LONGER CATASTROPHIC ON EITHER PATH. As PID 1  */
/* an early `return -1` took the system with it. We are never PID 1 now:   */
/* on the LaunchDaemons generation this is an ordinary job exit (launchd    */
/* stays up, restored_external keeps the display lit and the device on the */
/* USB bus), and on the rc.boot generation we are a child of launchctl,    */
/* which is blocked in waitpid() and simply resumes its bootstrap. Either  */
/* way the failure is something a host can look at. The error paths below  */
/* were left with their original return values deliberately: nothing       */
/* downstream reads them, and inventing an exit-code scheme no reader      */
/* exists for would be noise.                                              */
/* ---------------------------------------------------------------------- */

int main(void) {
    /* Before stdio is touched: if we were handed no descriptors, attach
     * /dev/console to them ourselves. setvbuf() on a closed fd 1 would be a
     * silent no-op followed by a run with no output at all. */
    ensure_console_fds();

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

    /* Open the bounded window in which the on-screen console may attach.
     * This does NOT wait for the display: it tries once, returns, and every
     * emit() below retries at most once a second until the window closes.
     * Everything said in the meantime is held and replayed the moment a
     * surface appears, so nothing is lost by refusing to block — and the
     * install is not delayed by a diagnostic. See screen.h.
     *
     * Deliberately BEFORE start_restored_external(): on the rc.boot
     * generation we are the one who forks restored_external, so its
     * surfaces cannot exist yet and the first attempt is expected to fail.
     * That is the normal case, not an error, and it is exactly what the
     * backlog is for. */
    screen_init();

    /* Apple's /etc/rc.boot duties, in Apple's order (root, umask, then hand
     * off to restored), minus the parts that only make sense for a stub
     * that is about to exec itself out of existence. Each is a no-op when
     * something else has already taken care of it.
     *
     * umask(0) is Apple's, verbatim, and it is also what our LaunchDaemon
     * plist declares (Umask = 0, copied from com.apple.restored_external
     * .plist) — so setting it here makes the two paths behave identically
     * instead of leaving the rc.boot path with whatever launchctl handed
     * down. merge_tree() chmod()s everything it creates explicitly, so a
     * nonzero umask would not survive anyway; what this removes is the
     * window between create and chmod. It is also inherited by the
     * restored_external we are about to fork, exactly as under Apple's
     * rc.boot. */
    ensure_root_writable();
    umask(0);
    start_restored_external();

    emit("Searching for disk...\n");
    /* Original waits on a stat() of /dev/disk0s1s1 succeeding — matches
     * FUN_00006434 (stat) usage at the call site exactly. The buffer's
     * contents are never read; only the success of the call matters. */
    struct stat st;
    while (stat("/dev/disk0s1s1", &st) != 0) {
        emit("Waiting for disk...\n");
        sleep(1);
    }

    emit("\n\n\n\n\n");
    emit("Blackb0x-- by regulad (orig. Blackb0x by NSSpiral)\n");
    emit("Mounting filesystem...\n");

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
        emit_err("Failed to mount / r/w at " MNT " (%s)\n", strerror(errno));
        emit_err("  (" MNT " is created at bake time by BakeRamdisk.cpp; ENOENT here means\n"
                 "   this ramdisk was not baked by blackb0x)\n");
        return -1;
    }
    emit("Main filesystem mounted\n");
    report_volume("system", MNT);
    report_path(MNT "/private/var");
    report_path(MNT "/Applications");

    emit("Mounting user filesystem...\n");
    mkdir(MNT "/private/var2", 0x1ed);
    long hfsArgs2[11];
    hfsArgs2[0] = (long)"/dev/disk0s1s2";
    if (mount("hfs", MNT "/private/var", 0, hfsArgs2) != 0) {
        emit_err("Failed to mount /var r/w (%s)\n", strerror(errno));
        return -1;
    }
    emit("User Filesystem mounted\n");
    report_volume("data", MNT "/private/var");
    report_path(MNT "/var/.blackb0x/install-done");
    report_path(MNT "/var/mobile");
    /* The system-partition side: where the record goes and where the staged
     * /var payload will land. Both are on the volume that accepts writes. */
    report_path(BLACKB0X_STATE_DIR);
    report_path(BLACKB0X_STATE_DIR "/var");

    emit("Mounting devices...\n");
    if (mount("devfs", MNT "/dev", 0, NULL) != 0) {
        emit_err("Unable to mount devices! (%s)\n", strerror(errno));
        /* Innermost first, and var2 removed while the volume carrying it is
         * still mounted — the same ordering fix as the teardown below. The
         * old code unmounted MNT only, leaving the data partition mounted
         * across a reboot. */
        sync();
        unmount(MNT "/private/var", 0);
        rmdir(MNT "/private/var2");
        unmount(MNT, 0);
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
    emit("Devices mounted\n");
    report_path(MNT "/dev/disk0s1s1");

    /* What we are about to install, before we try. A missing or empty
     * overlay is the difference between the install path and the diagnostic
     * one, and it has been diagnosed from the outside twice. */
    report_path("/blackb0x");
    report_volume("ramdisk", "/");

    /* Before the merge, not after: if a volume will not take a new file, the
     * merge is going to fail thousands of times and the reason is worth
     * knowing in one line rather than inferring from the wreckage. */
    probe_writability();

    /* NO OVERLAY => SAY SO AND EXIT CLEANLY, WITHOUT REBOOTING.
     *
     * This is the DiagBinary ramdisk's whole purpose. That image carries this
     * binary, its plist and /mnt, and deliberately no /blackb0x at all, so
     * there is nothing to install. Left to itself the normal path would run
     * merge_tree() against a directory that does not exist, log the failure,
     * and then fall through to reboot(0) anyway -- which is exactly what the
     * first hardware run of it did: the device rebooted so fast that nothing
     * ever reached the screen.
     *
     * That reboot destroys the only thing we are trying to measure. Staying
     * alive keeps Apple's launchd and restored_external running, which means
     * the display stays lit and the device stays enumerated on USB, so
     * whatever we print here has somewhere to land and a human has time to
     * read it.
     *
     * WHAT THIS TESTED, and the answer. The open question was whether console
     * output survives restored_external pointing the display pipe at its own
     * IOSurfaces -- the kernel console draws into the boot framebuffer iBoot
     * set up, and once that swap lands the boot framebuffer is plausibly
     * off-screen. It does not survive: on hardware the logo came up and this
     * line never did. /dev/console is wired up but not observable here, which
     * is why screen.h exists and why every line worth reading goes to both.
     *
     * exit code 0, not a reboot and not a failure: KeepAlive is absent from
     * the plist, so launchd reaps us and does not respawn. Nothing else on
     * the ramdisk touches block devices, and the NAND is already unmounted
     * below before we return. */
    if (access("/blackb0x", F_OK) != 0) {
        emit("no overlay to copy, dying peacefully\n");
        fflush(stdout);
        sync();
        rmdir(MNT "/private/var2");
        unmount(MNT "/dev", 0);
        unmount(MNT "/private/var", 0);
        unmount(MNT, 0);
        sync();

        /* WAIT, THEN REBOOT — the same ending as a successful install, and
         * deliberately no longer a special one.
         *
         * Three revisions of this path, each answering the previous one's
         * question. It first returned 0, and on hardware the device rebooted
         * into iBoot Recovery even though the shipped binary provably called
         * no reboot(2) -- something else reboots when our job exits, and the
         * DiagRepack image (identical but carrying no job at all) does not.
         * It then slept forever to test that, and the device did sit there
         * indefinitely, which confirmed it: on this ramdisk a job EXITING is
         * itself a reason for the system to go down, and a restore ramdisk
         * whose bootstrap carries Boot tasks is a coherent place for that to
         * be true.
         *
         * With that answered, sleeping forever has nothing left to measure
         * and costs a power-cycle to get out of. So it now reads its screen
         * for thirty seconds and reboots the way the install path does,
         * through set_auto_boot() so the device comes up on the NAND OS
         * rather than back in Recovery. The reboot is OURS and explicit,
         * which is the point: the thing we learned is that leaving the
         * decision to the environment produces a reboot we do not control. */
        wait_to_be_read();
        set_auto_boot();
        reboot(0);
        return 0;
    }

    do_install();

    /* CLOSE THE DIRTY WINDOW IMMEDIATELY. Nothing goes between the merge
     * returning and the NAND being flushed and unmounted — not a status
     * line, not a second thought. The reason is restored_external, which is
     * alive alongside us on both generations now: its no-argument main()
     * forks a -server child and waitpid()s, and when that child exits for
     * ANY reason the parent logs "restored exited ... - rebooting" and
     * reboots the device via /sbin/reboot. With no host attached the child
     * blocks in accept() forever, so this is a tail risk rather than a
     * likely one — but the cheap, correct mitigation is to be unmounted
     * before it can fire, not to add a lock (there is nothing to lock
     * against: launchd, launchctl, xpcproxy, syslogd and ReportCrash
     * contain zero references to /mnt, disk0, rdisk or any mount call, and
     * restored_external only mounts on a host StartRestore) and not to wait
     * for anything (waiting is the one thing that provably widens it).
     *
     * ORDER FIXED while this was being rewritten, and the old order was
     * genuinely wrong: it unmounted MNT before MNT/private/var, which is a
     * submount of it, and then rmdir()'d MNT/private/var2 after MNT was
     * already gone — i.e. against the pristine ramdisk's own empty
     * mountpoint rather than the volume the directory was created on.
     * Innermost first, and var2 removed while the volume that carries it is
     * still mounted. */
    sync();
    rmdir(MNT "/private/var2");
    unmount(MNT "/dev", 0);
    unmount(MNT "/private/var", 0);
    unmount(MNT, 0);
    sync();

    emit("Installation complete — rebooting device...\n");
    /* Held open so the line above can actually be read. Everything is
     * unmounted and sync()'d at this point, so the wait costs nothing but the
     * wait itself — see wait_to_be_read(). */
    wait_to_be_read();
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
