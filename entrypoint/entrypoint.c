/*
 * entrypoint.c
 *
 * Replaces PID 1 on the patched restore ramdisk. Not real launchd — this is
 * Blackb0x's own first-boot installer, reverse-engineered from the
 * precompiled ARMv6 Mach-O this project shipped at misc/launchd
 * (originally Blackb0x/Files/launchd). Confirmed genuinely freestanding via
 * its Mach-O load commands: LC_UNIXTHREAD (not LC_MAIN), zero
 * LC_LOAD_DYLIB entries — no libSystem, no dyld. Every syscall is made
 * directly via `mov r12, #N; svc #128` (verified against the real
 * disassembly, not inferred), and even strlen/memcpy/memset are hand-rolled.
 * This is almost certainly load-bearing, not a style choice: this binary
 * *is* what execs as PID 1 at the earliest point of ramdisk boot, before
 * dyld/libSystem are guaranteed to be functional. Replicated here with the
 * same approach: -ffreestanding -nostdlib -static, no libc.
 *
 * Originally a faithful reproduction of the original's exact behavior,
 * including two confirmed pre-existing bugs in its directory-creation
 * helper (a hardcoded-nonsense mode, and a missing-leading-zero octal
 * literal at its one call site) — since fixed, now that this entrypoint
 * does real first-time dpkg/apt bootstrap work where directory permissions
 * actually matter. See misc/README.md for the full
 * reverse-engineering writeup this is built from (strings, the original
 * Patcher.mm, a full Ghidra decompilation, and raw disassembly of every
 * syscall trampoline).
 *
 * This entrypoint deliberately consumes NO boot-args: the `blackb0x.*`
 * runtime-directive mechanism (and the raw kern.bootargs sysctl read behind
 * it) was removed once the inert-`setenv boot-args` root cause was found, and
 * is recoverable verbatim from git history if it is ever wanted again.
 *
 * There are no #includes at all: -nostdlib means there are no system headers
 * to include, and nothing local is needed either.
 */

/* ---------------------------------------------------------------------- */
/* Raw syscalls — no libc. Numbers verified via llvm-objdump disassembly   */
/* of the original binary's syscall trampolines (mov r12,#N; svc #128).   */
/* ---------------------------------------------------------------------- */

#define SYS_exit    1
#define SYS_fork    2
#define SYS_read    3
#define SYS_write   4
#define SYS_open    5
#define SYS_close   6
#define SYS_wait4   7
#define SYS_unlink  10
#define SYS_chdir   12
#define SYS_chmod   15
#define SYS_chown   16
#define SYS_access  33
#define SYS_kill    37
#define SYS_sync    36
#define SYS_dup     41
#define SYS_reboot  55
#define SYS_symlink 57
#define SYS_readlink 58
#define SYS_chroot  61
#define SYS_dup2    90
#define SYS_mount     167
#define SYS_unmount   159
#define SYS_mkdir     136
#define SYS_rmdir     137
#define SYS_stat      188
#define SYS_fstat     189
#define SYS_getdirentries 196
#define SYS_execve  59

typedef unsigned int uint32;
typedef unsigned long size_t_;
typedef long ssize_t_;

/* Darwin's syscall ABI signals errors via the CARRY FLAG, not a negated
 * return value: after `svc #0x80`, carry clear = success (r0 is the result),
 * carry set = failure (r0 is a POSITIVE errno). This is unlike Linux, which
 * returns -errno in the result register. libSystem's stubs read that carry
 * and convert it to the C "-1 and set errno" convention; we have no
 * libSystem, so each wrapper must do it itself. Without this, a failed
 * open()/stat() returning e.g. ENOENT (2) as a positive value would sail
 * past every `fd < 0` / `!= 0` error check as if it were a valid fd/result.
 * `rsbcs r0, r0, #0` negates r0 in place ONLY when carry is set (reverse-
 * subtract from zero, predicated on CS), turning the positive errno into a
 * negative one so the C-side checks work like Linux's. It is a conditional
 * ARM instruction, valid because this file is built `-arch armv6` (ARM
 * encoding, not Thumb). "cc" is added to the clobber list because svc itself
 * writes the condition flags. Confirmed against Darwin's arm64 syscall path
 * (Go's asm_darwin_arm64.s BCC), same convention on armv7. */
static inline long __syscall0(long n) {
    register long r12 __asm__("r12") = n;
    register long r0 __asm__("r0");
    __asm__ volatile("svc #128\n\trsbcs r0, r0, #0" : "=r"(r0) : "r"(r12) : "memory", "cc");
    return r0;
}
static inline long __syscall1(long n, long a0) {
    register long r12 __asm__("r12") = n;
    register long r0 __asm__("r0") = a0;
    __asm__ volatile("svc #128\n\trsbcs r0, r0, #0" : "+r"(r0) : "r"(r12) : "memory", "cc");
    return r0;
}
static inline long __syscall2(long n, long a0, long a1) {
    register long r12 __asm__("r12") = n;
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    __asm__ volatile("svc #128\n\trsbcs r0, r0, #0" : "+r"(r0) : "r"(r12), "r"(r1) : "memory", "cc");
    return r0;
}
static inline long __syscall3(long n, long a0, long a1, long a2) {
    register long r12 __asm__("r12") = n;
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    __asm__ volatile("svc #128\n\trsbcs r0, r0, #0" : "+r"(r0) : "r"(r12), "r"(r1), "r"(r2) : "memory", "cc");
    return r0;
}
static inline long __syscall4(long n, long a0, long a1, long a2, long a3) {
    register long r12 __asm__("r12") = n;
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    register long r2 __asm__("r2") = a2;
    register long r3 __asm__("r3") = a3;
    __asm__ volatile("svc #128\n\trsbcs r0, r0, #0" : "+r"(r0) : "r"(r12), "r"(r1), "r"(r2), "r"(r3) : "memory", "cc");
    return r0;
}

static int sys_open(const char *p, int flags, int mode) { return (int)__syscall3(SYS_open, (long)p, flags, mode); }
static int sys_close(int fd) { return (int)__syscall1(SYS_close, fd); }
static ssize_t_ sys_read(int fd, void *buf, size_t_ n) { return __syscall3(SYS_read, fd, (long)buf, (long)n); }
static ssize_t_ sys_write(int fd, const void *buf, size_t_ n) { return __syscall3(SYS_write, fd, (long)buf, (long)n); }
static int sys_unlink(const char *p) { return (int)__syscall1(SYS_unlink, (long)p); }
static int sys_chdir(const char *p) { return (int)__syscall1(SYS_chdir, (long)p); }
static int sys_chmod(const char *p, int mode) { return (int)__syscall2(SYS_chmod, (long)p, mode); }
static int sys_chown(const char *p, int uid, int gid) { return (int)__syscall3(SYS_chown, (long)p, uid, gid); }
static int sys_access(const char *p, int mode) { return (int)__syscall2(SYS_access, (long)p, mode); }
static int sys_sync(void) { return (int)__syscall0(SYS_sync); }
static int sys_dup2(int oldfd, int newfd) { return (int)__syscall2(SYS_dup2, oldfd, newfd); }
/* reboot(2) is really `reboot(int opt, char *msg)` (xnu syscalls.master #55),
 * a TWO-argument syscall -- libc's userspace reboot(int) is a 1-arg wrapper
 * over it. The msg pointer is only dereferenced when opt has RB_PANIC/command
 * bits set, which we never use, so an earlier 1-arg call left r1 as garbage
 * harmlessly -- but pass an explicit NULL to match the real ABI. */
static int sys_reboot(int opt) { return (int)__syscall2(SYS_reboot, opt, 0); }
static int sys_symlink(const char *target, const char *linkpath) { return (int)__syscall2(SYS_symlink, (long)target, (long)linkpath); }
static long sys_readlink(const char *path, char *buf, size_t_ n) { return __syscall3(SYS_readlink, (long)path, (long)buf, (long)n); }
static int sys_chroot(const char *p) { return (int)__syscall1(SYS_chroot, (long)p); }
static int sys_mount(const char *type, const char *dir, int flags, void *data) { return (int)__syscall4(SYS_mount, (long)type, (long)dir, flags, (long)data); }
static int sys_unmount(const char *dir, int flags) { return (int)__syscall2(SYS_unmount, (long)dir, flags); }
static int sys_mkdir(const char *p, int mode) { return (int)__syscall2(SYS_mkdir, (long)p, mode); }
static int sys_rmdir(const char *p) { return (int)__syscall1(SYS_rmdir, (long)p); }
static int sys_stat(const char *p, void *buf) { return (int)__syscall2(SYS_stat, (long)p, (long)buf); }
static int sys_fstat(int fd, void *buf) { return (int)__syscall2(SYS_fstat, fd, (long)buf); }
static long sys_getdirentries(int fd, void *buf, size_t_ n, long *basep) { return __syscall4(SYS_getdirentries, fd, (long)buf, (long)n, (long)basep); }
static int sys_execve(const char *path, char *const argv[], char *const envp[]) { return (int)__syscall3(SYS_execve, (long)path, (long)argv, (long)envp); }
static int sys_wait4(int pid, int *status, int options, void *rusage) { return (int)__syscall4(SYS_wait4, pid, (long)status, options, (long)rusage); }
static void sys_exit(int code) { __syscall1(SYS_exit, code); }

/* fork(2) returns twice, and the plain __syscallN wrapper CANNOT express it:
 * the Darwin ABI puts the child pid in r0 for BOTH parent and child, and
 * flags the child in r1 (0 = parent, 1 = child) -- libSystem's fork stub is
 * what zeroes r0 in the child (see xnu libsyscall/custom/__fork.s). A wrapper
 * reading only r0 makes the child see its own pid, never 0, so every
 * `if (pid == 0)` child branch silently runs as the parent. This does that r1
 * check itself. Carry still means error (rsbcs negates to -errno; r1 is
 * meaningless then, so the child-zeroing is gated behind carry-clear).
 *
 * fork, not vfork, deliberately: with fork the child gets its own address
 * space, so it can safely return through this C wrapper into run-a-child code
 * and execve/_exit there. vfork shares the parent's stack and forbids the
 * child from returning from the calling frame at all -- correctness would then
 * depend on this wrapper being inlined, which is too fragile to rely on. The
 * original binary's own child-spawn helpers (FUN_00006074/FUN_00006134) used
 * fork too. See docs/HISTORY.md. */
static long sys_fork(void) {
    register long r12 __asm__("r12") = SYS_fork;
    register long r0 __asm__("r0");
    register long r1 __asm__("r1");
    __asm__ volatile("svc #128\n\t"
                     "bcs 1f\n\t"         /* carry set -> error path */
                     "cmp r1, #0\n\t"     /* r1: 0 = parent, 1 = child */
                     "beq 2f\n\t"         /* parent: r0 already holds pid */
                     "mov r0, #0\n\t"     /* child: return 0 */
                     "b 2f\n\t"
                     "1:\n\t"
                     "rsb r0, r0, #0\n\t" /* error: r0 = -errno */
                     "2:"
                     : "=r"(r0), "=r"(r1)
                     : "r"(r12)
                     : "memory", "cc");
    return r0;
}

/* O_* flags — BSD/XNU numeric values, not resolved from any header since
 * we have none (-nostdlib). */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x200
#define O_APPEND 0x8

#define F_OK 0

#define MNT "/mnt1"
#define INSTALL_LOG "/mnt1/var/mobile/Media/blackb0x_install.log"

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
 * on. This entrypoint no longer creates any directories itself at all
 * (see merge_tree() below and BakeRamdisk.cpp's stageBlackb0xTree(), which
 * bakes correct 0755 modes directly into /blackb0x at bake time) — kept
 * here purely as a record of the bug, not a pointer to live code. */

/* ---------------------------------------------------------------------- */
/* Hand-rolled string/memory primitives — no libc available.               */
/* ---------------------------------------------------------------------- */

static int my_strlen(const char *s) {
    int n = 0;
    while (s[n] != '\0') n++;
    return n;
}
static void *my_memcpy(void *dst, const void *src, int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static void *my_memset(void *dst, int c, int n) {
    unsigned char *d = (unsigned char *)dst;
    for (int i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}
static int my_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}
static char *my_strcat(char *dst, const char *src) {
    int dl = my_strlen(dst);
    my_memcpy(dst + dl, src, my_strlen(src) + 1);
    return dst;
}

/* ---------------------------------------------------------------------- */
/* Console + file-log output — two entirely separate, non-overlapping      */
/* sinks in the original (verified: FUN_00005ef4 never touches the log     */
/* file; FUN_00001a88 never touches the console). Which messages go to     */
/* which sink is preserved exactly as originally split, including its own  */
/* inconsistencies (e.g. "Installing etasonATV" logs to file only, while   */
/* "Installing p0sixspwn"/"Installing blackb0x tether" print to console    */
/* only) — not something to "fix" or unify in this pass.                  */
/* ---------------------------------------------------------------------- */

static void console_print(const char *s) {
    for (int i = 0; s[i] != '\0'; i++) {
        sys_write(1, s + i, 1);
    }
    sys_sync();
}

/* Matches FUN_000019b0/FUN_00001a88 exactly: append if the log file
 * already exists (O_WRONLY|O_APPEND), else create it (O_WRONLY|O_CREAT,
 * mode 0600); write the message; close; then unconditionally chown+chmod
 * the file to 501:20 / 0755 — done on every single call, not just the
 * first, matching the original's redundant-but-harmless behavior. */
static void log_to_file(const char *msg) {
    int fd;
    if (sys_access(INSTALL_LOG, F_OK) == 0) {
        fd = sys_open(INSTALL_LOG, O_WRONLY | O_APPEND, 0);
    } else {
        fd = sys_open(INSTALL_LOG, O_WRONLY | O_CREAT, 0600);
    }
    sys_write(fd, msg, my_strlen(msg));
    sys_close(fd);
    sys_chown(INSTALL_LOG, UID_MOBILE, GID_STAFF);
    sys_chmod(INSTALL_LOG, 0755);
}

/* Matches FUN_00005e9c exactly: a pure CPU busy-spin, not a real sleep
 * syscall — there is no sleep-family syscall anywhere in this binary. */
static void busy_wait(int seconds) {
    volatile long counter = (long)seconds * 10000000L;
    while (counter > 0) counter--;
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
    struct { char pad[96]; } st;
    if (sys_stat(src, &st) != 0) {
        console_print("Unable to find source file\n");
        return -1;
    }

    int in = sys_open(src, O_RDONLY, 0);
    if (in < 0) {
        return -1;
    }
    int out = sys_open(dst, O_WRONLY | O_CREAT, 0);
    if (out < 0) {
        sys_close(in);
        return -1;
    }
    char buf[2048];
    ssize_t_ n;
    int rc = 0;
    while ((n = sys_read(in, buf, sizeof(buf))) > 0) {
        if (sys_write(out, buf, (size_t_)n) < 0) { rc = -1; break; }
    }
    sys_close(in);
    sys_close(out);
    if (rc == 0) {
        sys_chown(dst, uid, gid);
        sys_chmod(dst, mode & 0xffff);
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

/* Minimal BSD dirent layout for getdirentries(2) on this XNU vintage:
 * d_fileno(4) d_reclen(2) d_type(1) d_namlen(1) d_name[...] */
struct bsd_dirent {
    uint32 d_fileno;
    unsigned short d_reclen;
    unsigned char d_type;
    unsigned char d_namlen;
    char d_name[256];
};
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

/* Field offsets into the raw 96-byte stat(2) buffer used throughout this
 * file. This is the pre-64-bit-inode `struct stat` layout (dev_t st_dev;
 * ino_t st_ino; mode_t st_mode; nlink_t st_nlink; uid_t st_uid; gid_t
 * st_gid; dev_t st_rdev; 3x struct timespec; off_t st_size; blkcnt_t
 * st_blocks; blksize_t st_blksize; 3x uint32/int32; int64[2]) — the one the
 * raw SYS_stat (188) trap itself returns, not libc's $INODE64-suffixed
 * stat() (which iOS's real headers always redirect plain `stat()` calls to
 * — see the SDK's sys/cdefs.h __DARWIN_INODE64 macro — but has no bearing
 * on what this file's own direct `svc #128` calls get back). Verified
 * against the real iPhoneOS6.1 SDK's sys/stat.h and sys/_types.h field
 * typedefs: summing every field's real size lands at exactly 96 bytes with
 * natural alignment and no hidden padding, matching this buffer size
 * exactly, which is itself confirming evidence this is the right layout. */
#define STAT_MODE_OFFSET 8  /* mode_t, 2 bytes */
#define STAT_UID_OFFSET  12 /* uid_t, 4 bytes */
#define STAT_GID_OFFSET  16 /* gid_t, 4 bytes */

static unsigned short stat_mode(const void *st) { return *(const unsigned short *)((const char *)st + STAT_MODE_OFFSET); }
static unsigned int stat_uid(const void *st) { return *(const unsigned int *)((const char *)st + STAT_UID_OFFSET); }
static unsigned int stat_gid(const void *st) { return *(const unsigned int *)((const char *)st + STAT_GID_OFFSET); }

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
 * under /blackb0x) becomes /mnt1/--early-boot. */
static int merge_tree(const char *src, const char *dst) {
    int fd = sys_open(src, O_RDONLY, 0);
    if (fd < 0) {
        log_to_file("cannot open blackb0x source dir\n");
        return -1;
    }

    char buf[4096];
    long basep = 0;
    long n;
    int ok = 1;
    while ((n = sys_getdirentries(fd, buf, sizeof(buf), &basep)) > 0) {
        char *p = buf;
        char *end = buf + n;
        while (p < end) {
            struct bsd_dirent *de = (struct bsd_dirent *)p;
            p += de->d_reclen;
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
                continue; /* "." / ".." only — unlike the old clone_directory(),
                           * real dotfiles (.profile's parent, etc.) are staged
                           * content here, not something to skip. */
            }

            char srcPath[1024], dstPath[1024];
            srcPath[0] = '\0'; dstPath[0] = '\0';
            my_strcat(srcPath, src);
            my_strcat(srcPath, "/");
            my_strcat(srcPath, de->d_name);
            my_strcat(dstPath, dst);
            my_strcat(dstPath, "/");
            my_strcat(dstPath, de->d_name);

            if (de->d_type == DT_LNK) {
                char target[1024];
                long tn = sys_readlink(srcPath, target, sizeof(target) - 1);
                if (tn < 0) { ok = 0; continue; }
                target[tn] = '\0';
                sys_unlink(dstPath);
                if (sys_symlink(target, dstPath) != 0) ok = 0;
            } else if (de->d_type == DT_DIR) {
                struct { char pad[96]; } dstSt;
                if (sys_stat(dstPath, &dstSt) != 0) {
                    struct { char pad[96]; } srcSt;
                    if (sys_stat(srcPath, &srcSt) == 0) {
                        unsigned short mode = stat_mode(&srcSt) & 07777;
                        sys_mkdir(dstPath, mode);
                        sys_chmod(dstPath, mode);
                        sys_chown(dstPath, (int)stat_uid(&srcSt), (int)stat_gid(&srcSt));
                    }
                }
                if (merge_tree(srcPath, dstPath) != 0) ok = 0;
            } else {
                struct { char pad[96]; } srcSt;
                if (sys_stat(srcPath, &srcSt) != 0) { ok = 0; continue; }
                int mode = stat_mode(&srcSt) & 07777;
                if (install_file(srcPath, dstPath, (int)stat_uid(&srcSt), (int)stat_gid(&srcSt), mode) != 0) ok = 0;
            }
        }
    }
    sys_close(fd);
    if (n < 0) {
        log_to_file("failed to get directories\n");
        return -1;
    }
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------------- */
/* Main install sequence.                                                 */
/* ---------------------------------------------------------------------- */

/* A genuine halt, not a graceful return: prints to both sinks, then hangs
 * forever rather than letting entry() reach its own unmount/reboot path.
 * An automatic reboot here would just re-run this same ramdisk straight
 * back into the same panic on every cycle, with nothing to show a human
 * debugging over console/serial that anything is wrong — a dead stop
 * forces attention instead of masking the problem as a boot loop. */
static void panic(const char *msg) {
    console_print("PANIC: ");
    console_print(msg);
    log_to_file("PANIC: ");
    log_to_file(msg);
    for (;;) busy_wait(60);
}

/* Hand-rolled memmem() — no libc here. Linear substring search; fine for
 * the one-shot, small-needle use below (dpkg status files this project's
 * own package set produces are not large). */
static const char *my_memmem(const char *hay, int haylen, const char *needle, int needlelen) {
    if (needlelen <= 0 || haylen < needlelen) return 0;
    for (int i = 0; i <= haylen - needlelen; i++) {
        int j = 0;
        while (j < needlelen && hay[i + j] == needle[j]) j++;
        if (j == needlelen) return hay + i;
    }
    return 0;
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
 * Reads into a fixed, generously-sized static (BSS, not stack — this
 * entrypoint runs with a small, freestanding stack) buffer rather than
 * streaming, since a plain substring search across a read-buffer boundary
 * would need real overlap-handling logic this one-shot check doesn't
 * justify. If the real status file ever somehow exceeds this buffer, this
 * fails closed (reports "not found", so fixup_etasonuntether_rtbuddyd()
 * below just does nothing) rather than searching a truncated/wrong window
 * and risking a false answer. */
#define DPKG_STATUS_SCAN_BUF_SIZE (256 * 1024)
static char g_dpkgStatusScanBuf[DPKG_STATUS_SCAN_BUF_SIZE];

static int dpkg_status_has_installed_package(const char *statusPath, const char *pkgName) {
    int fd = sys_open(statusPath, O_RDONLY, 0);
    if (fd < 0) return 0;

    int total = 0;
    ssize_t_ n;
    while (total < DPKG_STATUS_SCAN_BUF_SIZE &&
           (n = sys_read(fd, g_dpkgStatusScanBuf + total, DPKG_STATUS_SCAN_BUF_SIZE - total)) > 0) {
        total += (int)n;
    }
    sys_close(fd);
    if (total >= DPKG_STATUS_SCAN_BUF_SIZE) {
        log_to_file("dpkg status file larger than expected — package-state check skipped\n");
        return 0;
    }

    char needle[192];
    needle[0] = '\0';
    my_strcat(needle, "Package: ");
    my_strcat(needle, pkgName);
    my_strcat(needle, "\n");
    return my_memmem(g_dpkgStatusScanBuf, total, needle, my_strlen(needle)) != 0;
}

/* Copies `src` to `dst`, preserving `src`'s own real owner/mode (read via
 * stat()) rather than a caller-supplied triple — the same idiom
 * merge_tree() itself uses for regular files, reused here via the
 * existing install_file() primitive. */
static int copy_preserving(const char *src, const char *dst) {
    struct { char pad[96]; } st;
    if (sys_stat(src, &st) != 0) return -1;
    int mode = stat_mode(&st) & 07777;
    int uid = (int)stat_uid(&st);
    int gid = (int)stat_gid(&st);
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
    if (sys_access("/mnt1/usr/libexec/rtbuddyd.orig", F_OK) == 0) {
        return; /* already backed up on a prior run */
    }
    if (sys_access("/mnt1/usr/libexec/rtbuddyd", F_OK) == 0) {
        if (copy_preserving("/mnt1/usr/libexec/rtbuddyd", "/mnt1/usr/libexec/rtbuddyd.orig") != 0) {
            log_to_file("failed to back up rtbuddyd before etasonuntether symlink\n");
            return;
        }
        sys_unlink("/mnt1/usr/libexec/rtbuddyd");
    }
    /* Either rtbuddyd was just backed up and removed above, or there was
     * never a real one to back up in the first place — the real postinst
     * symlinks unconditionally in that second case too (its own `else`
     * branch). */
    if (sys_symlink("/System/Library/Frameworks/JavaScriptCore.framework/Resources/jsc",
                     "/mnt1/usr/libexec/rtbuddyd") != 0) {
        log_to_file("failed to symlink rtbuddyd -> jsc for etasonuntether\n");
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
    if (sys_access("/mnt1/Applications/AppleTV.app/AppleTV", F_OK) != 0) {
        console_print("Not an AppleTV...\n");
        return 0;
    }

    if (sys_access("/mnt1/var/.blackb0x/install-done", F_OK) == 0) {
        panic("/var/.blackb0x/install-done already exists — refusing to re-run (would clobber live dpkg state)\n");
    }

    log_to_file("Merging blackb0x payload\n");
    merge_tree("/blackb0x", "/mnt1");
    fixup_etasonuntether_rtbuddyd();
    log_to_file("Finished install\n");

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
 * fork(), not vfork(): see sys_fork()'s own comment for why the shared-stack
 * hazard of vfork made a raw-syscall C wrapper unsafe. The child execve()s
 * /usr/sbin/nvram and, only if execve() itself fails, _exit()s via
 * sys_exit(); the parent waits for it. */
static void set_auto_boot(void) {
    char *argv[] = {"/usr/sbin/nvram", "auto-boot=1", 0};
    char *envp[] = {0};
    long pid = sys_fork();
    if (pid == 0) {
        sys_execve("/usr/sbin/nvram", argv, envp);
        sys_exit(1); /* only reached if execve() itself failed */
    }
    /* pid > 0: parent, wait for the child. pid < 0: fork failed (-errno) --
     * nothing to wait for, and auto-boot simply will not have been set. */
    if (pid > 0) {
        sys_wait4((int)pid, 0, 0, 0);
    }
}

/* ---------------------------------------------------------------------- */
/* entry() — matches the original's Mach-O entry point exactly: console   */
/* fd setup, disk wait, the two mount()s + devfs, do_install(), then       */
/* unmount everything and reboot. LC_UNIXTHREAD jumps straight here — no   */
/* argc/argv/envp convention applies (no crt, no dyld).                   */
/* ---------------------------------------------------------------------- */

int entry(void) {
    int consoleFd = sys_open("/dev/console", O_WRONLY, 0);
    sys_dup2(consoleFd, 1);
    sys_dup2(consoleFd, 2);

    console_print("Searching for disk...\n");
    /* Original waits on a stat() of /dev/disk0s1s1 succeeding — matches
     * FUN_00006434 (stat) usage at the call site exactly, including its
     * exact 96-byte stack buffer size (auStack_80 in the decompiled
     * entry()) even though we never read the buffer's contents. */
    struct { char pad[96]; } st;
    while (sys_stat("/dev/disk0s1s1", &st) != 0) {
        console_print("Waiting for disk...\n");
        busy_wait(1);
    }

    console_print("\n\n\n\n\n");
    console_print("blackb0x Jailbreak - by @NSSpiral\n");
    console_print("Mounting filesystem...\n");

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
    if (sys_mount("hfs", MNT, 0, hfsArgs1) != 0) {
        console_print("Failed to mount / r/w\n");
        return -1;
    }
    console_print("Main filesystem mounted\n");

    console_print("Mounting user filesystem...\n");
    sys_mkdir("/mnt1/private/var2", 0x1ed);
    long hfsArgs2[11];
    hfsArgs2[0] = (long)"/dev/disk0s1s2";
    if (sys_mount("hfs", "/mnt1/private/var", 0, hfsArgs2) != 0) {
        console_print("Failed to mount /var r/w\n");
        return -1;
    }
    console_print("User Filesystem mounted\n");

    console_print("Mounting devices...\n");
    if (sys_mount("devfs", "/mnt1/dev", 0, 0) != 0) {
        console_print("Unable to mount devices!\n");
        sys_unmount("/mnt1", 0);
        set_auto_boot();
        sys_reboot(0); /* RB_AUTOBOOT */
        return -1;
    }
    console_print("Devices mounted\n");

    do_install();

    sys_unmount("/mnt1/dev", 0);
    sys_unmount("/mnt1", 0);
    console_print("Installation complete\n");
    sys_sync();

    console_print("Unmounting disks...\n");
    sys_rmdir("/mnt1/private/var2");
    sys_unmount("/mnt1/private/var", 0);
    sys_unmount("/mnt1/dev", 0);
    sys_unmount("/mnt1", 0);

    console_print("Flushing buffers...\n");
    sys_sync();

    console_print("Rebooting device...\n");
    set_auto_boot();
    sys_close(consoleFd);
    /* RB_AUTOBOOT (0), a normal reboot. The original binary passed 1
     * (RB_ASKNAME) here -- a deliberate deviation: RB_ASKNAME is a bootstrap-
     * prompt flag with nothing to act on under iOS, so it was an inert
     * wrong value, and 0 is the correct "reboot normally" request. */
    sys_reboot(0);

    return 0;
}
