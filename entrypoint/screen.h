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
 * WHICH SURFACE: ALL OF THEM. This started out picking only the opaque
 * background (surface[2], layer 0), on the reasoning that the layer above it
 * is transparent so our text would show through, and that the background is
 * the one buffer restored_external does not rewrite on every progress update.
 * HARDWARE SAID OTHERWISE. On a real AppleTV3,2 at 12H1006 the text was
 * written successfully and was not visible: it appeared only in the moment the
 * boot graphics were torn down at the end of the run. The background layer is
 * composited UNDER the logo/progress layer, and that layer is not transparent
 * where we were drawing — so the single most defensible-sounding choice of
 * target was the one choice that could not be seen.
 *
 * The fix is to stop choosing. Every plausible BGRA surface gets the same
 * text, so whichever one is composited on top, or scanned out, or swapped in
 * next, is carrying it. Three surfaces is three times the blitting of one,
 * which is nothing: these are a few dozen glyphs per line, at most one line
 * per log call.
 *
 * What we give up by not choosing is that we now also paint the two progress
 * buffers, which restored_external rewrites on every progress update — our
 * text can be erased from those. It cannot be erased from all of them at once
 * by anything this ramdisk does (the background is redrawn only on display
 * init and HDMI hot-plug; restored_external's swap routine has exactly two
 * callers, a progress update and an image blit, and no timer, animation or
 * polling), and being erased from one layer is a strictly better failure than
 * being invisible on the correct one.
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
 * 48 * 256 = 12 KB of BSS. Sized to cover everything this binary says
 * between process start and the point restored_external has a display up.
 *
 * LINE_MAX WAS 112 AND THAT WAS ITS OWN TRUNCATION, separate from the one
 * the display was doing. The error that motivated wrapping is 118 characters
 * long, so fixing only the renderer would have moved the cut rather than
 * removed it. 256 is past any message this binary formats — the longest are
 * a path plus an errno plus a clause — and the console now wraps rather than
 * cuts, so a long line costs rows instead of information. */
#define SCREEN_BACKLOG_LINES 96
#define SCREEN_LINE_MAX      256

/* How many surfaces we are willing to paint at once. restored_external makes
 * three; the bound exists so a surprise does not turn into an unbounded loop
 * over another process's objects. */
#define SCREEN_MAX_TARGETS 8

enum { SCREEN_SEARCHING = 0, SCREEN_ATTACHED = 1, SCREEN_GAVE_UP = -1 };

static int            g_screenState = SCREEN_SEARCHING;
static IOSurfaceRef   g_screenSurface[SCREEN_MAX_TARGETS];
static fbtext_console g_screenCon[SCREEN_MAX_TARGETS];
static unsigned       g_screenAlpha[SCREEN_MAX_TARGETS];
static int            g_screenCount;
static time_t         g_screenDeadline;
static time_t         g_screenLastTry;

static char g_screenRing[SCREEN_BACKLOG_LINES][SCREEN_LINE_MAX];
static int  g_screenRingHead;
static int  g_screenRingCount;
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

/* One full sweep. Collects EVERY plausible BGRA surface rather than choosing
 * between them — see "WHICH SURFACE" in the header comment for why choosing
 * was the bug. Returns how many were kept; `refs` and `out` are filled in
 * parallel and the caller owns each ref. */
static int screen_find(IOSurfaceRef *refs, fbtext_surface *out, unsigned *alphas,
                       int max)
{
    int n = 0;
    IOSurfaceID id;

    for (id = 0; id < SCREEN_MAX_ID && n < max; id++) {
        fbtext_surface cand;
        unsigned alpha0 = 0;
        IOSurfaceRef s = screen_probe(id, &cand, &alpha0);
        if (!s) continue;
        refs[n]   = s;
        out[n]    = cand;
        alphas[n] = alpha0;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------
 * Drawing.
 * ------------------------------------------------------------------------ */

/* Open a write window on ONE target: take its lock and re-fetch its base
 * address inside it. Returns 0 if the window could not be opened, in which
 * case nothing is drawn and nothing is dereferenced. */
static int screen_draw_begin(int i)
{
    void *base;
    if (g_screenState != SCREEN_ATTACHED) return 0;
    if (IOSurfaceLock(g_screenSurface[i], 0, NULL) != 0) return 0;
    base = IOSurfaceGetBaseAddress(g_screenSurface[i]);
    if (!base) { IOSurfaceUnlock(g_screenSurface[i], 0, NULL); return 0; }
    g_screenCon[i].s.base = (unsigned char *)base;
    return 1;
}

static void screen_draw_end(int i)
{
    IOSurfaceUnlock(g_screenSurface[i], 0, NULL);
}

/* Do one drawing operation on every attached surface. The locks are taken and
 * released one target at a time rather than all at once: we never hold two of
 * another process's surface locks simultaneously. */
#define SCREEN_FOR_EACH(stmt)                                   \
    do {                                                        \
        int _i;                                                 \
        for (_i = 0; _i < g_screenCount; _i++) {                \
            if (!screen_draw_begin(_i)) continue;               \
            { fbtext_console *con = &g_screenCon[_i]; stmt; }   \
            screen_draw_end(_i);                                \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------------
 * Attaching.
 * ------------------------------------------------------------------------ */

static void screen_attach_try(void)
{
    time_t now = time(NULL);
    int lastChance;
    fbtext_surface surf[SCREEN_MAX_TARGETS];
    IOSurfaceRef refs[SCREEN_MAX_TARGETS];
    unsigned alpha[SCREEN_MAX_TARGETS];
    int found, i, kept = 0;

    if (g_screenState != SCREEN_SEARCHING) return;

    lastChance = (now >= g_screenDeadline);
    if (!lastChance && now == g_screenLastTry) return;  /* at most 1/sec */
    g_screenLastTry = now;

    found = screen_find(refs, surf, alpha, SCREEN_MAX_TARGETS);

    if (found == 0) {
        if (lastChance) {
            g_screenState = SCREEN_GAVE_UP;
            printf("screen: no usable display IOSurface after %d s — "
                   "output stays on stdout only (%d line(s) never reached the screen)\n",
                   SCREEN_ATTACH_WINDOW_SECS, g_screenBacklogUsed + g_screenBacklogDropped);
        }
        return;
    }

    /* Keep the ones a console actually fits in; release the rest.
     *
     * NOTHING HERE PAINTS A BACKGROUND — see FBTEXT_NOFILL, which every
     * console now uses. That is the lesson of the second hardware run:
     * painting all the layers was right, but every console also cleared its
     * rows to OPAQUE BLACK, which turned a transparent compositor layer into
     * a solid black sheet over nearly the whole frame and took the TV to
     * black. The text was doing its job; the rectangle behind it was not.
     * Glyphs are drawn on top of whatever is already on screen and every
     * other pixel is left alone, so the failure mode "our debug channel
     * blanked the display" is gone rather than mitigated.
     *
     * The alpha read at (0,0) no longer decides anything, and is kept only
     * because it is the one cheap observation we have of how these layers are
     * actually composited — it goes on screen below. */
    for (i = 0; i < found; i++) {
        if (!fbtext_console_init(&g_screenCon[kept], &surf[i])) {
            printf("screen: IOSurface id=%u is %ux%u, too small for a console — skipped\n",
                   (unsigned)IOSurfaceGetID(refs[i]), surf[i].width, surf[i].height);
            CFRelease(refs[i]);
            continue;
        }
        g_screenAlpha[kept]   = alpha[i];
        g_screenSurface[kept] = refs[i];
        kept++;
    }

    if (kept == 0) {
        g_screenState = SCREEN_GAVE_UP;
        printf("screen: %d surface(s) found, none large enough for a console — "
               "giving up\n", found);
        return;
    }

    g_screenCount = kept;
    g_screenState = SCREEN_ATTACHED;

    printf("screen: attached to %d surface(s), %dx%d chars at scale %d\n",
           kept, g_screenCon[0].cols, g_screenCon[0].rows, g_screenCon[0].scale);

    /* And say the same thing ON THE SCREEN, one line per target. Everything
     * this file has ever reported about which surfaces it found went to
     * stdout, i.e. to /dev/console, i.e. — as two hardware runs established —
     * nowhere. The layer layout is the single fact that would most change
     * what we do next, so it goes where it can actually be read. */
    {
        char note[SCREEN_LINE_MAX];
        snprintf(note, sizeof(note), "screen: %d surface(s), %dx%d chars, scale %d",
                 kept, g_screenCon[0].cols, g_screenCon[0].rows, g_screenCon[0].scale);
        SCREEN_FOR_EACH(fbtext_console_line(con, note));
        for (i = 0; i < kept; i++) {
            snprintf(note, sizeof(note), "  id=%u %ux%u stride=%u alpha=0x%02x",
                     (unsigned)IOSurfaceGetID(g_screenSurface[i]),
                     g_screenCon[i].s.width, g_screenCon[i].s.height,
                     (unsigned)g_screenCon[i].s.stride, g_screenAlpha[i]);
            SCREEN_FOR_EACH(fbtext_console_line(con, note));
        }
    }

    /* Replay everything said before the display existed. This is why the
     * poll does not have to block. */
    if (g_screenBacklogDropped) {
        char note[SCREEN_LINE_MAX];
        snprintf(note, sizeof(note), "[%d earlier line(s) lost]", g_screenBacklogDropped);
        SCREEN_FOR_EACH(fbtext_console_line(con, note));
    }
    for (i = 0; i < g_screenBacklogUsed; i++) {
        SCREEN_FOR_EACH(fbtext_console_line(con, g_screenBacklog[i]));
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

/* SCROLLING, AND WHY IT IS A HALF-PAGE JUMP RATHER THAN A LINE AT A TIME.
 *
 * The console used to wrap to row 0 when it filled, overwriting the top of
 * its own output. With the diagnostic block that is no longer a corner case:
 * it is ninety-odd rows of mount state, device inventory and log tails, and
 * the end of it was landing on top of the beginning.
 *
 * A pixel-domain scroll -- memmove the framebuffer up one row -- is not
 * available. The surface is WRITE-COMBINED: sequential stores are fast and
 * reads are very slow, and a memmove is half reads. Shifting a 1280x720
 * surface up by eight pixels would mean reading ~3.6 MB through that mapping
 * per line of output.
 *
 * So the scroll happens in the TEXT domain. Every line is kept in a ring
 * buffer in ordinary memory, and when the region fills, the area is cleared
 * and the most recent half of the ring is redrawn at the top. The cost is one
 * full redraw per half screen rather than per line -- about forty-five lines
 * of amortisation -- and a reader keeps the immediately preceding context
 * instead of losing everything on the jump.
 *
 * The clear is the one place this file paints a colour rather than only the
 * lit pixels of a glyph (see FBTEXT_NOFILL). It has to: a reused row that is
 * not erased is two lines of text on top of each other. So the console draws
 * over the boot graphics until it first fills, and owns an opaque rectangle
 * from then on -- which is the right trade at exactly that point, since by
 * the time ninety rows have been written, what is underneath them is no
 * longer the interesting thing on screen.
 *
 * The ring is the same storage that holds pre-attach output, deliberately.
 * Before a surface exists it is a backlog to replay; after one exists it is
 * scrollback to redraw from. One buffer, two uses of the same lines. */
static void screen_ring_push(const char *line)
{
    size_t n = strlen(line);
    if (n >= SCREEN_LINE_MAX) n = SCREEN_LINE_MAX - 1;
    if (g_screenRingCount == SCREEN_BACKLOG_LINES) {
        g_screenRingHead = (g_screenRingHead + 1) % SCREEN_BACKLOG_LINES;
        g_screenRingCount--;
    }
    {
        int slot = (g_screenRingHead + g_screenRingCount) % SCREEN_BACKLOG_LINES;
        memcpy(g_screenRing[slot], line, n);
        g_screenRing[slot][n] = '\0';
        g_screenRingCount++;
    }
}

/* Clear and redraw the newest lines that fit in half the region. Chooses how
 * many to replay by measuring backwards from the newest, so a few long
 * wrapped lines take the space of many short ones rather than overflowing. */
static void screen_scroll(void)
{
    int keepRows = g_screenCon[0].rows / 2;
    int count = 0, used = 0, i;

    for (i = g_screenRingCount - 1; i >= 0; i--) {
        int slot = (g_screenRingHead + i) % SCREEN_BACKLOG_LINES;
        int need = fbtext_console_rows_for(&g_screenCon[0], g_screenRing[slot]);
        if (used + need > keepRows) break;
        used += need;
        count++;
    }

    SCREEN_FOR_EACH(fbtext_console_clear(con, FBTEXT_BLACK));

    for (i = g_screenRingCount - count; i < g_screenRingCount; i++) {
        int slot = (g_screenRingHead + i) % SCREEN_BACKLOG_LINES;
        SCREEN_FOR_EACH(fbtext_console_line(con, g_screenRing[slot]));
    }
}

static void screen_emit_line(const char *line)
{
    if (g_screenState == SCREEN_SEARCHING) screen_attach_try();

    if (g_screenState == SCREEN_ATTACHED) {
        screen_ring_push(line);
        if (fbtext_console_would_overflow(&g_screenCon[0],
                                          fbtext_console_rows_for(&g_screenCon[0], line))) {
            screen_scroll();
        }
        SCREEN_FOR_EACH(fbtext_console_line(con, line));
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
    SCREEN_FOR_EACH(fbtext_console_status(con, s));
}

/* Switch the console to an alert colour for everything printed from here on.
 * panic() uses it; there is no way back, which matches panic(). */
static void screen_alert(void)
{
    int i;
    if (g_screenState != SCREEN_ATTACHED) return;
    for (i = 0; i < g_screenCount; i++) g_screenCon[i].fg = FBTEXT_RED;
}

#endif /* SCREEN_H */
