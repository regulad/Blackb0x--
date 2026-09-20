/*
 * bootargs.h
 *
 * Parses blackb0x's own runtime directives out of the kernel boot-args
 * string. Header-only, and deliberately touches nothing but the characters
 * it is handed — no syscalls, no allocation, no libc — for two reasons:
 *
 *  1. entrypoint.c includes it freestanding (-ffreestanding -nostdlib,
 *     -arch armv6) and runs it as PID 1 with no crash handler, no console
 *     on this hardware, and nothing downstream to catch a fault. Every loop
 *     below is bounded by an explicit length as well as by NUL.
 *  2. src/tests/BootArgsTests.cpp compiles this EXACT source on the host and
 *     runs it over a table of adversarial inputs (see that file). A parser
 *     that only exists inside a cross-compiled freestanding binary cannot be
 *     tested at all, and this one is only ever exercised for real on a
 *     device whose sole observable is whether it power-cycles.
 *
 * ---------------------------------------------------------------------------
 * Why boot-args rather than a compile-time switch
 * ---------------------------------------------------------------------------
 *
 * The ramdisk this binary lives inside is baked AHEAD OF TIME — by CI, one
 * `firmware-<device>` artifact per model, consumed verbatim by the `blackb0x`
 * jailbreak binary, which links no ramdisk-baking code at all (see AGENTS.md).
 * So a compile-time `-D` diagnostic costs a full ~30-minute ramdisk re-bake
 * under `sudo` with Theos and `afsctool` installed — exactly the work CI
 * exists to spare users — just to flip one bit. These directives exist for
 * failures that reproduce only on hardware; needing a re-bake to arm one
 * makes every hardware cycle more expensive at precisely the moment cycles
 * are the scarce resource.
 *
 * Boot-args have none of that problem: they are per-boot data the kernel
 * hands us, so arming a directive costs nothing on this side at all. This is
 * the same trick systemd plays with the Linux kernel command line, for the
 * same reason: PID 1 needs per-boot configuration and has nowhere else to
 * get it that early.
 *
 * HOW THEY GET SET does not affect a line of this file, but the answer used
 * to be recorded here wrongly and is worth stating correctly: they are
 * COMPILED INTO the iBEC (iBoot32Patcher's -b, from Patcher.hpp's `bootargs`
 * namespace). `setenv boot-args` over the recovery protocol is INERT on
 * AppleTV3,2's iBoot-1537.9.55 — it never reads that variable back on its
 * kernel-boot path — so there is no runtime channel and never was one that
 * worked. Arming a directive therefore means
 * `bake-iboot --extra-boot-args "blackb0x.<name>"`, which is rootless and
 * takes seconds; `blackb0x --extra-boot-args` hard-fails and points there.
 * Either way the kernel ends up holding the string and this parser reads it
 * back out of kern.bootargs. What matters is that neither costs the
 * ~30-minute rooted ramdisk re-bake a compile-time -D demanded.
 *
 * ---------------------------------------------------------------------------
 * The vocabulary
 * ---------------------------------------------------------------------------
 *
 * Every directive is `blackb0x.<name>` with an optional `=<value>`. The
 * `blackb0x.` prefix is the whole point of the naming: XNU's own
 * PE_parse_boot_argn() matches boot-arg names literally and ignores anything
 * it does not know, and no Apple boot-arg has ever contained a dot-qualified
 * vendor prefix, so a directive can never be mistaken for a real kernel
 * argument in either direction — the kernel skips ours, we skip the kernel's.
 * It also reads unambiguously in a log line next to `rd=md0` and `amfi=0xff`,
 * which matters when the boot-args string is the only artifact of a failed
 * run anyone ever sees. The cost is 9 bytes per directive against a tight
 * budget (see BLACKB0X_BOOT_ARGS_MAX below and DeviceManager.cpp's own
 * accounting); that is worth paying once for names that cannot collide.
 *
 * Adding a directive is: one field here, one case in the dispatch below, one
 * row in the test table, and one line of documentation in Cli.hpp. Nothing
 * in the bake, the Makefile or the build system needs to know about it.
 *
 * REMOVED DIRECTIVE, recorded so it is not re-invented from scratch:
 * `blackb0x.beacon=<seconds>` was a proof-of-life reboot beacon — reboot
 * immediately as PID 1 so the host sees a USB re-enumeration, answering "did
 * entrypoint reach PID 1 at all" with no hardware instrumentation. It was
 * deleted deliberately once the inert-`setenv boot-args` root cause was
 * found, because that finding is a complete mechanism-level answer to the
 * question the beacon was built to bisect. Note the honest hedge: the
 * boot-args fix is NOT yet confirmed on hardware. If a hardware run shows it
 * did not resolve the failure, the beacon (parser case, struct field, and
 * entry()'s busy_wait/sys_reboot block) is recoverable verbatim from git
 * history rather than needing a redesign.
 */

#ifndef BLACKB0X_ENTRYPOINT_BOOTARGS_H
#define BLACKB0X_ENTRYPOINT_BOOTARGS_H

/* The prefix every directive carries, and its length. Kept as a literal
 * rather than a strlen() call so this stays usable from a freestanding TU
 * with no libc at all. */
#define BLACKB0X_DIRECTIVE_PREFIX "blackb0x."
#define BLACKB0X_DIRECTIVE_PREFIX_LEN 9

/* Hard ceiling on the boot-args string this parser will look at, and the
 * size entrypoint.c's read buffer is declared with.
 *
 * XNU's ARM boot_args carries the command line in a fixed
 * CommandLine[BOOT_LINE_LENGTH] array (pexpert/pexpert/arm/boot.h), 256 on
 * 32-bit ARM, so the kernel itself cannot hold a longer string than that no
 * matter what iBoot is handed. 512 here is that ceiling plus slack, so a
 * kernel that ever grew the field still gets read rather than silently
 * truncated mid-directive. Everything below is bounded by the caller's
 * length argument anyway; this constant only sizes the buffer. */
#define BLACKB0X_BOOT_ARGS_MAX 512

/* Parsed directives. Zero is "not requested" for every field, so a missing,
 * empty or unreadable boot-args string leaves this struct at exactly the
 * shipped default behaviour. */
struct blackb0x_directives {
    /* Non-zero once at least one syntactically valid blackb0x.* directive
     * was seen. Purely informational — entrypoint.c logs it — but it is the
     * one signal that distinguishes "boot-args were read and carried no
     * directives" from "boot-args could not be read at all", which are very
     * different diagnoses if a directive appears not to take effect. */
    int any;

    /* blackb0x.skip-install=<0|1>, or bare `blackb0x.skip-install`.
     * Runs the whole boot path — console, disk wait, both HFS mounts, devfs
     * — but not do_install(), then unmounts, sets auto-boot and reboots
     * normally. See entry(). */
    int skip_install;

    /* How many `blackb0x.`-prefixed tokens were rejected: an unknown name, a
     * value that is not a decimal number, or an `=` with nothing after it.
     * entrypoint.c reports this; a typo'd directive that silently did
     * nothing would be indistinguishable from the bug being hunted. */
    int rejected;
};

/* --- internal helpers; all bounded, none of them touch anything but the
 * characters between `s` and `s + len`. --------------------------------- */

/* Whitespace as iBoot/XNU treat it when splitting the command line. */
static int blackb0x_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Compares exactly `n` bytes of `a` against `b`, where `a` is known to have
 * at least `n` readable bytes. Returns 1 on equality. */
static int blackb0x_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Decimal parse of [p, p+n). Returns 1 and writes *out on success; returns
 * 0 — leaving *out untouched — on an empty span, any non-digit (a sign
 * included: negative directives are meaningless here and a `-` is far more
 * likely a typo than an intent), or a value that would run away. Saturates
 * at 1000000 rather than overflowing, so a 40-digit value is rejected by the
 * caller's own range clamp instead of wrapping into something plausible. */
static int blackb0x_parse_uint(const char *p, int n, int *out) {
    if (n <= 0) return 0;
    long value = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        value = value * 10 + (p[i] - '0');
        if (value > 1000000L) value = 1000000L;
    }
    *out = (int)value;
    return 1;
}

/* Parses `s` (the raw boot-args string, `len` bytes, NOT required to be NUL
 * terminated) into `out`.
 *
 * Contract, and the reason each clause exists:
 *  - `out` is fully zeroed first, so a NULL/empty/garbage input yields the
 *    shipped default behaviour rather than whatever was on the stack.
 *  - `s == 0` or `len <= 0` is a normal, expected case (no boot-args, or the
 *    sysctl read failed), not an error to report.
 *  - Tokens are split on whitespace; a token that does not start with
 *    `blackb0x.` is skipped without being examined further. rd=md0, -v and
 *    amfi=0xff are not this parser's business.
 *  - A directive with no `=` is the flag form and means 1. A directive with
 *    an `=` and nothing after it is REJECTED rather than treated as either 0
 *    or 1: `blackb0x.skip-install=` is a mistake, and guessing which mistake
 *    would either arm a directive nobody asked for or silently disarm one
 *    they did.
 *  - An unknown `blackb0x.*` name is counted and ignored, never fatal. A
 *    future ramdisk must tolerate a directive added after it was baked —
 *    that is the whole point of moving these to runtime.
 *  - A repeated directive: LAST occurrence wins. --extra-boot-args appends
 *    to the base string, so last-wins is what lets a command line override a
 *    default rather than being silently ignored. (XNU's own
 *    PE_parse_boot_argn takes the FIRST match; this deliberately differs,
 *    because the appending direction here is the opposite.)
 */
static void blackb0x_parse_boot_args(const char *s, int len, struct blackb0x_directives *out) {
    if (!out) return;
    out->any = 0;
    out->skip_install = 0;
    out->rejected = 0;
    if (!s || len <= 0) return;

    int i = 0;
    while (i < len) {
        /* Skip leading whitespace, and stop dead at a NUL: the kernel hands
         * back a NUL-terminated string with the terminator inside oldlen, so
         * trusting `len` alone would parse the byte after it. */
        while (i < len && s[i] != '\0' && blackb0x_is_space(s[i])) i++;
        if (i >= len || s[i] == '\0') break;

        int start = i;
        while (i < len && s[i] != '\0' && !blackb0x_is_space(s[i])) i++;
        int tokenLen = i - start;

        if (tokenLen < BLACKB0X_DIRECTIVE_PREFIX_LEN) continue;
        if (!blackb0x_eq(s + start, BLACKB0X_DIRECTIVE_PREFIX, BLACKB0X_DIRECTIVE_PREFIX_LEN)) continue;

        const char *key = s + start + BLACKB0X_DIRECTIVE_PREFIX_LEN;
        int keyLen = tokenLen - BLACKB0X_DIRECTIVE_PREFIX_LEN;
        if (keyLen == 0) { out->rejected++; continue; } /* a bare `blackb0x.` */

        /* Split key from value at the FIRST '=' inside this token only. */
        int eq = -1;
        for (int j = 0; j < keyLen; j++) {
            if (key[j] == '=') { eq = j; break; }
        }
        const char *value = 0;
        int valueLen = 0;
        if (eq >= 0) {
            value = key + eq + 1;
            valueLen = keyLen - eq - 1;
            keyLen = eq;
            if (valueLen == 0) { out->rejected++; continue; } /* `blackb0x.x=` */
            if (keyLen == 0) { out->rejected++; continue; }   /* `blackb0x.=1` */
        }

        /* Flag form (`blackb0x.skip-install`) means 1; otherwise the value
         * must be a plain decimal number. */
        int number = 1;
        if (value && !blackb0x_parse_uint(value, valueLen, &number)) {
            out->rejected++;
            continue;
        }

        if (keyLen == 12 && blackb0x_eq(key, "skip-install", 12)) {
            out->skip_install = number != 0;
            out->any = 1;
        } else {
            out->rejected++;
        }
    }
}

#endif /* BLACKB0X_ENTRYPOINT_BOOTARGS_H */
