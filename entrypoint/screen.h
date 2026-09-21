/*
 * screen.h — put this binary's output on the TV.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS
 * ---------------------------------------------------------------------------
 *
 * We could not see anything. That is the whole story, and it was measured
 * rather than assumed. A diagnostic ramdisk carrying this binary, its plist
 * and nothing else was built specifically to answer the console question: it
 * printed a heartbeat to stdout every ten seconds, which launchd routes to
 * /dev/console exactly as it does for Apple's own daemons, and the device
 * sat at the Apple logo for as long as anyone cared to watch with NOTHING on
 * screen. So /dev/console on this ramdisk is wired up but does not reach the
 * display once restored_external has pointed the display pipe at its own
 * surfaces. Drawing pixels ourselves is the only remaining way to get output
 * off this device. (The same run also settled two other things: the full
 * ~26 MB overlay boots, so size is exonerated, and the device stayed up once
 * this binary stopped exiting, so this launchd treats a job exiting as a
 * reason to reboot. Neither is this file's business, but they are why the
 * stay-resident path below has something to say.)
 *
 * ---------------------------------------------------------------------------
 * HOW IT WORKS — AND WHAT IT DELIBERATELY DOES NOT DO
 * ---------------------------------------------------------------------------
 *
 * /usr/local/bin/restored_external creates exactly THREE IOSurfaces at
 * display-init time, each with kIOSurfaceIsGlobal = kCFBooleanTrue, and
 * programs them as compositor layers:
 *
 *     layer 0 <- surface[2]              opaque background, one solid colour
 *     layer 1 <- surface[0] / surface[1] alternating, bzero'd => ALPHA 0
 *     layer 2 <- NULL
 *
 * with src and dst rects both {0,0,width,height}. A GLOBAL surface can be
 * re-opened BY ID FROM ANY PROCESS with IOSurfaceLookup(), and the display
 * pipe scans out of those surfaces continuously — so storing into their
 * pixels changes the screen with no swap, no compositor call and no
 * cooperation from the process that owns them.
 *
 * WE DO NOT OPEN IOMobileFramebuffer. Not once. Its exclusive-access
 * behaviour could not be settled by reading it, and "probably it lets a
 * second client in" is not a thing to find out on a device whose only
 * failure signal is a black screen. Lookup-by-ID needs none of it.
 *
 * WHICH SURFACE: the OPAQUE BACKGROUND (surface[2], layer 0), identified by
 * reading pixel (0,0) — the background reads alpha 0xFF, the two
 * progress-layer buffers read 0x00000000. Two reasons it is the right
 * target. The layer above it is transparent, so our text shows through; and
 * the progress buffers are the ones restored_external rewrites on every
 * progress update, whereas the background is redrawn only on display init
 * and HDMI hot-plug. Its swap routine has exactly two callers in
 * restored_external — a progress update and an image blit — and no timer,
 * animation or polling. While it sits in accept() with no host attached,
 * which is the field state, it performs zero swaps and zero pixel writes, so
 * what we store stays up.
 *
 * ORDERING: this ramdisk's launchd unit graph has no Requires/After/Before,
 * so we cannot assume restored_external has run by the time we start. We
 * POLL for the surfaces instead — see "the poll is opportunistic" below.
 *
 * ---------------------------------------------------------------------------
 * THIS MUST NEVER BECOME A NEW WAY FOR THE BINARY TO FAIL
 * ---------------------------------------------------------------------------
 *
 * It is a diagnostic aid on a device we are already struggling to see; an
 * install that dies because the debug channel could not attach would be
 * strictly worse than no debug channel. So, concretely:
 *
 *   - Every failure here is a printf and a return. There is no path from
 *     this file to exit(), reboot() or panic().
 *   - The poll is OPPORTUNISTIC, not blocking. screen_init() tries once and
 *     returns immediately; later attempts are piggy-backed on log calls, at
 *     most one per second, until a bounded deadline. Nothing waits on the
 *     display. This costs nothing in fidelity because early lines are held
 *     in a backlog and replayed the moment a surface is found, so a blocking
 *     wait would buy only latency.
 *   - Writes happen inside an IOSurfaceLock window and the base address is
 *     re-fetched inside it every time, so we never dereference a mapping we
 *     are only assuming is still there. The lock is NOT held across the run:
 *     holding another process's surface lock for the length of an install is
 *     the sort of thing that wedges that process.
 *   - Geometry is sanity-checked before a single store, and fbtext.h clips
 *     every pixel to the reported width/height.
 *
 * ---------------------------------------------------------------------------
 * LINKAGE
 * ---------------------------------------------------------------------------
 *
 * This adds two dylibs to a binary that used to load exactly one. Both, and
 * their full transitive closures, were verified present on ALL THREE ramdisk
 * generations this project can bake (12H1006, 11D258, 10B329a) — 42, 38 and
 * 33 entries, zero missing — and all nine IOSurface entry points below are
 * real exported text symbols on each, checked with `nm -gU` against the
 * decrypted ramdisks rather than against an SDK stub. See
 * entrypoint/README.md, "Seeing anything at all: the framebuffer console".
 *
 * IOSurface is a private framework with no public header on either SDK (the
 * framework directory contains the binary and nothing else), so the
 * prototypes are declared here. CFRelease is declared rather than pulled in
 * via <CoreFoundation/CoreFoundation.h>: one line against a whole umbrella
 * header, for a file that uses no other CF facility.
 */

#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fbtext.h"

typedef uint32_t IOSurfaceID;
typedef struct __IOSurface *IOSurfaceRef;

extern IOSurfaceRef IOSurfaceLookup(IOSurfaceID id);
extern IOSurfaceID  IOSurfaceGetID(IOSurfaceRef s);
extern size_t       IOSurfaceGetWidth(IOSurfaceRef s);
extern size_t       IOSurfaceGetHeight(IOSurfaceRef s);
extern size_t       IOSurfaceGetBytesPerRow(IOSurfaceRef s);
extern uint32_t     IOSurfaceGetPixelFormat(IOSurfaceRef s);
extern void        *IOSurfaceGetBaseAddress(IOSurfaceRef s);
extern int32_t      IOSurfaceLock(IOSurfaceRef s, uint32_t options, uint32_t *seed);
extern int32_t      IOSurfaceUnlock(IOSurfaceRef s, uint32_t options, uint32_t *seed);
extern void         CFRelease(const void *cf);

/* 'BGRA' — exactly what restored_external asks IOSurfaceCreate() for. */
#define SCREEN_PIXFMT_BGRA 0x42475241u

/* IOSurface IDs on this ramdisk are small and dense: restored_external's
 * three are the only ones anything creates. 256 is generous and the whole
 * sweep is a few hundred mach calls, done at most once a second. */
#define SCREEN_MAX_ID 256

/* Anything smaller than this is not a TV output. Guards against matching
 * some incidental scratch surface. */
#define SCREEN_MIN_W 480
#define SCREEN_MIN_H 270

/* And anything larger is not one either — a sanity bound on numbers that
 * come from another process and are about to index a pointer. */
#define SCREEN_MAX_W 8192
#define SCREEN_MAX_H 8192

/* How long to keep looking before giving up and saying so. Generous: it
 * costs nothing, since the poll never blocks. */
#define SCREEN_ATTACH_WINDOW_SECS 30

/* Backlog held while no surface has been found yet, replayed on attach.
 * 48 * 112 = 5.25 KB of BSS. Sized to cover everything this binary says
 * between process start and the point restored_external has a display up. */
#define SCREEN_BACKLOG_LINES 48
#define SCREEN_LINE_MAX      112

enum { SCREEN_SEARCHING = 0, SCREEN_ATTACHED = 1, SCREEN_GAVE_UP = -1 };

static int            g_screenState = SCREEN_SEARCHING;
static IOSurfaceRef   g_screenSurface;
static fbtext_console g_screenCon;
static time_t         g_screenDeadline;
static time_t         g_screenLastTry;

static char g_screenBacklog[SCREEN_BACKLOG_LINES][SCREEN_LINE_MAX];
static int  g_screenBacklogUsed;
static int  g_screenBacklogDropped;

/* Partial-line assembly: callers hand us arbitrary chunks, not lines. */
static char g_screenPending[SCREEN_LINE_MAX];
static int  g_screenPendingLen;
static int  g_screenPendingCut;   /* this line overflowed SCREEN_LINE_MAX */

/* ------------------------------------------------------------------------
 * Finding the surface.
 * ------------------------------------------------------------------------ */

/* Probe one ID. Returns the ref (caller owns it) and fills `out` and
 * `alpha0`, or NULL if this ID is not a plausible display surface.
 *
 * The (0,0) read is the only read this file ever does of write-combined
 * memory, and it is one 32-bit word per candidate — slow per byte, but four
 * bytes, three times, once a second at worst. */
static IOSurfaceRef screen_probe(IOSurfaceID id, fbtext_surface *out, unsigned *alpha0)
{
    IOSurfaceRef s = IOSurfaceLookup(id);
    size_t w, h, bpr;
    uint32_t *p;

    if (!s) return NULL;

    if (IOSurfaceGetPixelFormat(s) != SCREEN_PIXFMT_BGRA) goto reject;

    w   = IOSurfaceGetWidth(s);
    h   = IOSurfaceGetHeight(s);
    bpr = IOSurfaceGetBytesPerRow(s);

    if (w < SCREEN_MIN_W || w > SCREEN_MAX_W) goto reject;
    if (h < SCREEN_MIN_H || h > SCREEN_MAX_H) goto reject;
    if (bpr < w * 4 || bpr > (w + 64) * 4)    goto reject;

    if (IOSurfaceLock(s, 0, NULL) != 0) goto reject;
    p = (uint32_t *)IOSurfaceGetBaseAddress(s);
    if (!p) { IOSurfaceUnlock(s, 0, NULL); goto reject; }

    *alpha0 = (unsigned)(p[0] >> 24);
    out->base   = (unsigned char *)p;
    out->width  = (uint32_t)w;
    out->height = (uint32_t)h;
    out->stride = bpr;
    IOSurfaceUnlock(s, 0, NULL);

    printf("screen: IOSurface id=%u %ux%u stride=%u alpha0=0x%02x\n",
           (unsigned)id, (unsigned)w, (unsigned)h, (unsigned)bpr, *alpha0);
    return s;

reject:
    CFRelease(s);
    return NULL;
}

/* One full sweep. `requireOpaque` picks the background layer; the relaxed
 * pass is used only once, on the very last attempt, and is announced — a
 * progress buffer is a worse target (restored_external rewrites it on every
 * update, and only one of the two is on screen at a time) but it is a great
 * deal better than nothing on a device with no other output channel. */
static IOSurfaceRef screen_find(int requireOpaque, fbtext_surface *out)
{
    IOSurfaceRef best = NULL;
    uint32_t bestW = 0;
    IOSurfaceID id;

    for (id = 0; id < SCREEN_MAX_ID; id++) {
        fbtext_surface cand;
        unsigned alpha0 = 0;
        IOSurfaceRef s = screen_probe(id, &cand, &alpha0);
        if (!s) continue;
        if (requireOpaque && alpha0 != 0xFF) { CFRelease(s); continue; }
        if (cand.width <= bestW)             { CFRelease(s); continue; }
        if (best) CFRelease(best);
        best  = s;
        bestW = cand.width;
        *out  = cand;
    }
    return best;
}

/* ------------------------------------------------------------------------
 * Drawing.
 * ------------------------------------------------------------------------ */

/* Open a write window: take the lock and re-fetch the base address inside
 * it. Returns 0 if the window could not be opened, in which case nothing is
 * drawn and nothing is dereferenced. */
static int screen_draw_begin(void)
{
    void *base;
    if (g_screenState != SCREEN_ATTACHED) return 0;
    if (IOSurfaceLock(g_screenSurface, 0, NULL) != 0) return 0;
    base = IOSurfaceGetBaseAddress(g_screenSurface);
    if (!base) { IOSurfaceUnlock(g_screenSurface, 0, NULL); return 0; }
    g_screenCon.s.base = (unsigned char *)base;
    return 1;
}

static void screen_draw_end(void)
{
    IOSurfaceUnlock(g_screenSurface, 0, NULL);
}

/* ------------------------------------------------------------------------
 * Attaching.
 * ------------------------------------------------------------------------ */

static void screen_attach_try(void)
{
    time_t now = time(NULL);
    int lastChance;
    fbtext_surface surf;
    IOSurfaceRef s;

    if (g_screenState != SCREEN_SEARCHING) return;

    lastChance = (now >= g_screenDeadline);
    if (!lastChance && now == g_screenLastTry) return;  /* at most 1/sec */
    g_screenLastTry = now;

    s = screen_find(1, &surf);
    if (!s && lastChance) {
        printf("screen: no opaque background surface; trying any BGRA surface\n");
        s = screen_find(0, &surf);
    }

    if (!s) {
        if (lastChance) {
            g_screenState = SCREEN_GAVE_UP;
            printf("screen: no usable display IOSurface after %d s — "
                   "output stays on stdout only (%d line(s) never reached the screen)\n",
                   SCREEN_ATTACH_WINDOW_SECS, g_screenBacklogUsed + g_screenBacklogDropped);
        }
        return;
    }

    g_screenSurface = s;
    if (!fbtext_console_init(&g_screenCon, &surf)) {
        printf("screen: surface %ux%u is too small for a console — giving up\n",
               surf.width, surf.height);
        CFRelease(s);
        g_screenSurface = NULL;
        g_screenState = SCREEN_GAVE_UP;
        return;
    }
    g_screenState = SCREEN_ATTACHED;

    printf("screen: attached to IOSurface id=%u %ux%u stride=%u, "
           "%dx%d chars at scale %d\n",
           (unsigned)IOSurfaceGetID(s), surf.width, surf.height,
           (unsigned)surf.stride, g_screenCon.cols, g_screenCon.rows,
           g_screenCon.scale);

    /* Replay everything said before the display existed. This is why the
     * poll does not have to block. */
    if (screen_draw_begin()) {
        int i;
        if (g_screenBacklogDropped) {
            char note[SCREEN_LINE_MAX];
            snprintf(note, sizeof(note), "[%d earlier line(s) lost]", g_screenBacklogDropped);
            fbtext_console_line(&g_screenCon, note);
        }
        for (i = 0; i < g_screenBacklogUsed; i++) {
            fbtext_console_line(&g_screenCon, g_screenBacklog[i]);
        }
        screen_draw_end();
    }
    g_screenBacklogUsed = 0;
}

/* Start the bounded attach window. Tries once and returns; never blocks. */
static void screen_init(void)
{
    g_screenDeadline = time(NULL) + SCREEN_ATTACH_WINDOW_SECS;
    g_screenLastTry  = 0;
    screen_attach_try();
}

/* ------------------------------------------------------------------------
 * Output.
 * ------------------------------------------------------------------------ */

static void screen_emit_line(const char *line)
{
    if (g_screenState == SCREEN_SEARCHING) screen_attach_try();

    if (g_screenState == SCREEN_ATTACHED) {
        if (screen_draw_begin()) {
            fbtext_console_line(&g_screenCon, line);
            screen_draw_end();
        }
        return;
    }
    if (g_screenState == SCREEN_GAVE_UP) return;

    if (g_screenBacklogUsed < SCREEN_BACKLOG_LINES) {
        size_t n = strlen(line);
        if (n >= SCREEN_LINE_MAX) n = SCREEN_LINE_MAX - 1;
        memcpy(g_screenBacklog[g_screenBacklogUsed], line, n);
        g_screenBacklog[g_screenBacklogUsed][n] = '\0';
        g_screenBacklogUsed++;
    } else {
        g_screenBacklogDropped++;
    }
}

static void screen_flush_pending(void)
{
    if (g_screenPendingCut && g_screenPendingLen < SCREEN_LINE_MAX - 1) {
        /* Belt and braces: the caller's own truncation marker should already
         * be in the text, but if a line overflowed OUR assembly buffer as
         * well, say so rather than render a tail-less line that looks whole. */
        g_screenPending[g_screenPendingLen++] = '>';
    }
    g_screenPending[g_screenPendingLen] = '\0';
    /* Blank lines are dropped rather than spending a row: this binary prints
     * runs of newlines as vertical spacing on a console that has thousands of
     * rows, and the screen has about thirty. */
    if (g_screenPendingLen > 0) screen_emit_line(g_screenPending);
    g_screenPendingLen = 0;
    g_screenPendingCut = 0;
}

/* Feed arbitrary text. Splits on '\n'; text without a trailing newline stays
 * pending until one arrives, so a two-call message renders as one line.
 *
 * NON-ASCII IS COLLAPSED TO '-'. This file's messages are written in prose
 * and use real em-dashes; the 8x8 font is printable ASCII only, so a UTF-8
 * em-dash would otherwise render as three '?' glyphs and eat three of about
 * fifty columns. A multi-byte sequence is recognised by its lead byte
 * (>= 0xC0) and its continuation bytes (0x80..0xBF) are dropped, so one
 * character in becomes one character out. */
static void screen_puts(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    for (; *p; p++) {
        unsigned char c = *p;
        if (c == '\n') { screen_flush_pending(); continue; }
        if (c == '\r') continue;
        if (c >= 0x80 && c <= 0xBF) continue;   /* UTF-8 continuation byte */
        if (c >= 0xC0) c = '-';                 /* UTF-8 lead byte         */
        if (g_screenPendingLen < SCREEN_LINE_MAX - 1) {
            g_screenPending[g_screenPendingLen++] = (char)c;
        } else {
            g_screenPendingCut = 1;
        }
    }
}

/* Rewrite the non-scrolling status row. Used by the stay-resident heartbeat,
 * which would otherwise wrap the console every few minutes and erase the
 * boot lines that run exists to show. */
static void screen_status(const char *s)
{
    if (g_screenState == SCREEN_SEARCHING) screen_attach_try();
    if (screen_draw_begin()) {
        fbtext_console_status(&g_screenCon, s);
        screen_draw_end();
    }
}

/* Switch the console to an alert colour for everything printed from here on.
 * panic() uses it; there is no way back, which matches panic(). */
static void screen_alert(void)
{
    if (g_screenState == SCREEN_ATTACHED) g_screenCon.fg = FBTEXT_RED;
}

#endif /* SCREEN_H */
