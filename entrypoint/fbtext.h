/*
 * fbtext.h — dependency-free 8x8 bitmap text blitter for a 32-bit BGRA
 * surface, plus the line console built on top of it.
 *
 * THIS HEADER KNOWS NOTHING ABOUT IOSurface, IOKit, CoreFoundation OR THE
 * DEVICE. It is handed a base pointer, a width, a height and a stride, and
 * it stores 32-bit words. That separation is deliberate and load-bearing:
 * it is what lets the drawing logic be compiled and *checked on the host*,
 * where the output can actually be looked at, before it is ever run on a
 * device we cannot see. See screen.h for the part that finds a surface to
 * hand it. (The host check itself is a scratch harness, not repo code — it
 * renders to a PPM and dumps an ASCII-art view of the top-left; see
 * entrypoint/README.md, "Seeing anything at all: the framebuffer console".)
 *
 * PIXEL FORMAT. The on-device target is an IOSurface created by
 * /usr/local/bin/restored_external with, verbatim from its own
 * IOSurfaceCreate() dictionary:
 *
 *     kIOSurfaceWidth       = display width   (IOMobileFramebufferGetDisplaySize)
 *     kIOSurfaceHeight      = display height
 *     kIOSurfaceBytesPerRow = (width * 4 + 63) & ~63
 *     kIOSurfacePixelFormat = 'BGRA'  (0x42475241)
 *     kIOSurfaceCacheMode   = 0x400   (kIOMapWriteCombineCache)
 *     kIOSurfaceIsGlobal    = kCFBooleanTrue
 *
 * 'BGRA' means byte order B,G,R,A in memory, i.e. a little-endian uint32_t
 * of 0xAARRGGBB. Two consequences drive every line below:
 *
 *   - The mapping is WRITE-COMBINED. Sequential 32-bit stores are fast;
 *     reads are very slow. So there is no read-modify-write anywhere here,
 *     no alpha blending, and no "only touch the lit pixels" cleverness —
 *     every glyph cell is written in full, foreground where the bit is set
 *     and background where it is not. Opaque output also means the result
 *     is legible whichever compositor layer the surface turns out to be on.
 *   - The stride is NOT width*4. It is rounded up to 64 bytes, so a row
 *     pointer must be computed from `stride`, never from `width`. Getting
 *     that wrong produces a diagonal smear rather than an obvious failure,
 *     which is exactly the class of bug the host check exists to catch.
 *
 * FONT. font8x8_basic, public domain (Marcel Sondaar / Daniel Hepper),
 * printable ASCII 0x20..0x7E only. Row bytes are LSB-first: bit 0 is the
 * LEFTMOST pixel. Anything outside that range renders as '?' rather than
 * indexing out of the table.
 */

#ifndef FBTEXT_H
#define FBTEXT_H

#include <stddef.h>
#include <stdint.h>

static const unsigned char fbtext_font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20 ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* E */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

/* Colours are little-endian 0xAARRGGBB, i.e. B,G,R,A in memory. Opaque
 * throughout — see the header comment on why nothing here blends. */
#define FBTEXT_WHITE 0xFFFFFFFFu
#define FBTEXT_BLACK 0xFF000000u
/* NOT A COLOUR — a sentinel meaning "leave this pixel exactly as it is".
 *
 * Used as a console's background. With it, a row clear does nothing and a
 * glyph writes only the pixels the glyph is actually made of, so text lands
 * ON TOP of whatever the display already shows — the Apple logo, a progress
 * bar, another layer's content — instead of inside a rectangle we painted.
 *
 * This exists because the alternative was measured. Clearing rows to opaque
 * black turned a transparent compositor layer into a solid black sheet over
 * nearly the whole frame, and the TV went black. Not writing is strictly
 * safer than writing any colour, because there is no colour that is correct
 * on a surface whose contents belong to another process.
 *
 * The value is a BGRA one no caller would ever mean (alpha 0, blue 1), so a
 * real colour can never collide with it. */
#define FBTEXT_NOFILL 0x00000001u
#define FBTEXT_RED   0xFFFF3B30u
#define FBTEXT_AMBER 0xFFFFCC00u

/* A surface we are allowed to scribble on. Four numbers, nothing else. */
typedef struct {
    unsigned char *base;   /* IOSurfaceGetBaseAddress()  */
    uint32_t       width;  /* IOSurfaceGetWidth()        */
    uint32_t       height; /* IOSurfaceGetHeight()        */
    size_t         stride; /* IOSurfaceGetBytesPerRow()  */
} fbtext_surface;

/* One glyph, scaled by an integer factor, clipped to the surface edge.
 * Writes every pixel of the scale*8 x scale*8 cell — unless `bg` is
 * FBTEXT_NOFILL, in which case only the lit pixels of the glyph are written
 * and the rest of the cell keeps whatever was already on the display. */
static void fbtext_glyph(const fbtext_surface *s, int px, int py, int scale,
                         unsigned char c, uint32_t fg, uint32_t bg)
{
    const unsigned char *g;
    int row, col, sy, sx, x, y;

    if (c < 0x20 || c > 0x7E) c = '?';
    g = fbtext_font8x8[c - 0x20];

    for (row = 0; row < 8; row++) {
        for (sy = 0; sy < scale; sy++) {
            uint32_t *p;
            y = py + row * scale + sy;
            if (y < 0 || (uint32_t)y >= s->height) continue;
            p = (uint32_t *)(s->base + (size_t)y * s->stride);
            for (col = 0; col < 8; col++) {
                int lit = (g[row] & (1u << col)) != 0;
                uint32_t v;
                if (!lit && bg == FBTEXT_NOFILL) continue;
                v = lit ? fg : bg;
                for (sx = 0; sx < scale; sx++) {
                    x = px + col * scale + sx;
                    if (x < 0 || (uint32_t)x >= s->width) continue;
                    p[x] = v;
                }
            }
        }
    }
}

/* Draw at most `maxchars` glyphs of `str`. Stops at a NUL or a newline —
 * line splitting is the caller's job (screen.h does it), and a stray '\n'
 * reaching here must not render as '?'. Returns the pen x after the last
 * glyph drawn. */
static int fbtext_string(const fbtext_surface *s, int px, int py, int scale,
                         const char *str, int maxchars, uint32_t fg, uint32_t bg)
{
    int n = 0;
    for (; *str && n < maxchars; str++, n++) {
        if (*str == '\n' || *str == '\r') break;
        fbtext_glyph(s, px, py, scale, (unsigned char)*str, fg, bg);
        px += 8 * scale;
    }
    return px;
}

/* ------------------------------------------------------------------------
 * The line console.
 *
 * Top-left, newest work appended downward, wrapping back to the top when it
 * runs off the bottom. No word wrap: a line longer than the text area is cut
 * off at the right edge. That is the right trade here — these are progress
 * lines and errno strings, and the informative part is at the front.
 *
 * TWO REGIONS, and the split matters. Rows [0, rows) scroll; the row below
 * them is a STATUS line that is rewritten in place and never scrolls.
 * Without it the ten-second heartbeat in the stay-resident diagnostic path
 * would wrap the console every few minutes and erase the early boot lines —
 * i.e. destroy the only thing that run exists to show a human.
 *
 * OVERSCAN INSET. The text area is inset from the physical edges by a
 * fraction of the display, because a TV may overscan and the top-left corner
 * is precisely the part it eats first. Apple TV over HDMI normally maps 1:1
 * and would not need this, but "normally" is not a property we can check
 * from here and the cost is a few dozen pixels.
 * ------------------------------------------------------------------------ */
#define FBTEXT_INSET_DIVISOR 32   /* ~3% each side; action-safe is ~5% */

typedef struct {
    fbtext_surface s;
    int x0, y0;        /* pixel origin of the text area            */
    int scale;
    int cols;          /* glyph columns that fit                   */
    int rows;          /* SCROLLING rows (the status row is extra) */
    int line;          /* next scrolling row                       */
    uint32_t fg, bg;
} fbtext_console;

/* How far to scale an 8x8 cell up for a display `width` pixels across.
 *
 * This was derived from ten-foot legibility guidance — minimum comfortable
 * glyph height about 1/30 of the frame — which gave scale = width/416, i.e. 3
 * on the 1280x720 this device drives: a 24-pixel glyph and ~50 columns.
 * SEEN ON A TV, that is too big. The guidance is written for UI a viewer
 * reads from a sofa; this console is read by someone debugging, who is
 * looking at the screen deliberately and can walk closer. Optimising for
 * glanceability spent rows and columns that the log actually needed, and a
 * wrapped 50-column console throws away the thing the run exists to show.
 *
 * So: width/640, clamped to [2, 6]. 1280 wide lands on 2 — a 16-pixel glyph,
 * ~75 columns and ~40 rows after the overscan inset, which is half again as
 * much log on screen. 1920 lands on 3 and keeps the same apparent size. The
 * floor of 2 stays; an 8-pixel glyph on a TV is not readable from anywhere. */
static int fbtext_scale_for(uint32_t width)
{
    int scale = (int)(width / 640u);
    if (scale < 2) scale = 2;
    if (scale > 6) scale = 6;
    return scale;
}

/* Paint one glyph-row band of the text area to `colour`, or do nothing at all
 * if that "colour" is FBTEXT_NOFILL — which is the normal case now. Not
 * clearing means a reused row overdraws the one before it rather than
 * replacing it; that is accepted, because the scrolling region is about forty
 * rows and a diagnostic run prints well under that, while the status row
 * rewrites itself with a CONSTANT string, so its repeats land on identical
 * pixels. Blanking the display is the worse failure by a wide margin. */
static void fbtext_clear_row(const fbtext_console *c, int row, uint32_t colour)
{
    int cell = 8 * c->scale;

    if (colour == FBTEXT_NOFILL) return;
    int wpx  = c->cols * cell;
    int y;
    for (y = c->y0 + row * cell; y < c->y0 + (row + 1) * cell; y++) {
        uint32_t *p;
        int x;
        if (y < 0 || (uint32_t)y >= c->s.height) return;
        p = (uint32_t *)(c->s.base + (size_t)y * c->s.stride);
        for (x = c->x0; x < c->x0 + wpx && (uint32_t)x < c->s.width; x++) p[x] = colour;
    }
}

/* Returns 0 if the surface is too small to hold even one row and column. */
static int fbtext_console_init(fbtext_console *c, const fbtext_surface *s)
{
    int cell, totalRows;

    c->s     = *s;
    c->scale = fbtext_scale_for(s->width);
    c->x0    = (int)(s->width  / FBTEXT_INSET_DIVISOR);
    c->y0    = (int)(s->height / FBTEXT_INSET_DIVISOR);
    c->fg    = FBTEXT_WHITE;
    c->bg    = FBTEXT_NOFILL;   /* draw over what is there; see the sentinel */
    c->line  = 0;

    cell      = 8 * c->scale;
    c->cols   = (int)(s->width  - 2u * (uint32_t)c->x0) / cell;
    totalRows = (int)(s->height - 2u * (uint32_t)c->y0) / cell;
    c->rows   = totalRows - 1;   /* the last row is the status line */

    return (c->cols > 0 && c->rows > 0);
}

/* Append one line to the scrolling region. */
static void fbtext_console_line(fbtext_console *c, const char *str)
{
    if (c->line >= c->rows) c->line = 0;   /* wrap; cheapest possible */
    fbtext_clear_row(c, c->line, c->bg);
    fbtext_string(&c->s, c->x0, c->y0 + c->line * 8 * c->scale, c->scale,
                  str, c->cols, c->fg, c->bg);
    c->line++;
}

/* Rewrite the non-scrolling status row in place. */
static void fbtext_console_status(fbtext_console *c, const char *str)
{
    fbtext_clear_row(c, c->rows, c->bg);
    fbtext_string(&c->s, c->x0, c->y0 + c->rows * 8 * c->scale, c->scale,
                  str, c->cols, c->fg, c->bg);
}

#endif /* FBTEXT_H */
