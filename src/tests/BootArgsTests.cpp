//
//  BootArgsTests.cpp
//  Blackb0x
//
//  Exercises entrypoint/bootargs.h — the parser that turns the kernel
//  boot-args string into blackb0x's runtime directives — on the HOST, over a
//  table of inputs including deliberately adversarial ones.
//
//  Why this suite exists at all: that parser's only production home is
//  entrypoint.c, a freestanding armv6 Mach-O that runs as PID 1 on a restore
//  ramdisk, on a device whose UART is on internal test-points and whose only
//  observable behaviour is whether it power-cycles. There is no debugger, no
//  console, no crash log and no second chance — a fault there is
//  indistinguishable from the boot failure the directives exist to diagnose,
//  and finding it costs a full hardware cycle. So the parser is deliberately
//  a self-contained, syscall-free header with no libc dependency, included
//  verbatim here and hammered with the inputs a real command line can
//  produce: an empty string, a directive with no `=`, an `=` with no value,
//  unknown names, non-numeric values, a directive repeated, and a string far
//  longer than any boot-args buffer.
//
//  NOTE ON COVERAGE. This suite used to exercise two directives; it now
//  exercises one. `blackb0x.beacon=<seconds>` was removed from the vocabulary
//  deliberately (see entrypoint/bootargs.h's header for why and for the
//  hedge), so every row that named it went with it. What matters is that the
//  PARSER's behaviour is still covered to the same depth -- the adversarial
//  rows below were never really about the beacon, they were about tokenizing,
//  bounds and rejection, and they all still run against `skip-install` and
//  against deliberately-unknown names.
//
//  This compiles the EXACT source entrypoint.c compiles. If it ever stops
//  doing so (a #ifdef, a divergent copy), the suite is worthless — keep the
//  header dependency-free.
//

extern "C" {
#include "../../entrypoint/bootargs.h"
}

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void expect(bool condition, const std::string& description) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description.c_str());
        failures++;
    }
}

namespace {

// One row per input. `input` is fed verbatim (its real length, no implicit
// NUL handling on the parser's side beyond what it does itself).
struct Case {
    const char* what;
    const char* input;
    int skipInstall;
    int any;
    int rejected;
};

const Case kCases[] = {
    // --- the shipped default: nothing of ours in a real boot-args string ---
    {"the real ramdisk boot-args carry no directives",
     "rd=md0 -v amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1 pio-error=0", 0, 0, 0},
    {"the real tether boot-args carry no directives",
     "rd=disk0s1s1 -v amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1 pio-error=0", 0, 0, 0},

    // --- empty / degenerate inputs: all must be silent no-ops ---
    {"empty string", "", 0, 0, 0},
    {"only whitespace", "   \t \n  ", 0, 0, 0},
    {"a single space", " ", 0, 0, 0},

    // --- the happy paths ---
    {"skip-install explicit", "blackb0x.skip-install=1", 1, 1, 0},
    {"skip-install flag form (no '=' at all)", "rd=md0 blackb0x.skip-install", 1, 1, 0},
    {"skip-install explicitly off", "blackb0x.skip-install=0", 0, 1, 0},
    {"skip-install at the very end with no trailing space", "-v blackb0x.skip-install=1", 1, 1, 0},
    {"skip-install at the very start", "blackb0x.skip-install=1 -v", 1, 1, 0},
    {"a known directive alongside real kernel args", "rd=md0 blackb0x.skip-install -v", 1, 1, 0},
    {"tab-separated", "rd=md0\tblackb0x.skip-install=1\t-v", 1, 1, 0},
    {"newline-separated", "rd=md0\nblackb0x.skip-install=1\n", 1, 1, 0},
    {"leading and trailing whitespace", "   blackb0x.skip-install=1   ", 1, 1, 0},

    // --- adversarial: malformed values ---
    {"'=' with no value is rejected, not guessed", "blackb0x.skip-install=", 0, 0, 1},
    {"non-numeric value is rejected", "blackb0x.skip-install=abc", 0, 0, 1},
    {"partially numeric value is rejected", "blackb0x.skip-install=10s", 0, 0, 1},
    {"hex-looking value is rejected (decimal only)", "blackb0x.skip-install=0x1", 0, 0, 1},
    {"negative value is rejected", "blackb0x.skip-install=-1", 0, 0, 1},
    {"empty key with a value", "blackb0x.=1", 0, 0, 1},
    {"a bare prefix", "blackb0x.", 0, 0, 1},

    // --- adversarial: unknown names ---
    {"unknown directive is counted and ignored", "blackb0x.nonsense=1", 0, 0, 1},
    {"unknown directive does not disturb a known one", "blackb0x.nonsense=1 blackb0x.skip-install=1", 1, 1, 1},
    {"a near-miss name is not fuzzy-matched", "blackb0x.skip-installs=1", 0, 0, 1},
    {"a prefix of a known name is not matched", "blackb0x.skip=1", 0, 0, 1},

    // --- adversarial: near-collisions with real kernel boot-args ---
    {"a non-prefixed arg with our directive's name is not ours", "skip-install=1", 0, 0, 0},
    {"a different vendor prefix is not ours", "notblackb0x.skip-install=1", 0, 0, 0},
    {"a bare word starting with our prefix text but no dot", "blackb0xskip-install=1", 0, 0, 0},

    // --- adversarial: repeats ---
    {"repeated directive: last one wins (on)", "blackb0x.skip-install=0 blackb0x.skip-install=1", 1, 1, 0},
    {"a later malformed repeat does not clobber an earlier good one",
     "blackb0x.skip-install=1 blackb0x.skip-install=oops", 1, 1, 1},
    {"repeated skip-install: last one wins (off)", "blackb0x.skip-install=1 blackb0x.skip-install=0", 0, 1, 0},

    // --- saturation: the parser clamps rather than wrapping into something
    // plausible. skip-install only cares about zero/non-zero, so this is a
    // test of blackb0x_parse_uint()'s own saturation, not of the directive.
    {"a huge value is accepted and saturated, not wrapped", "blackb0x.skip-install=99999", 1, 1, 0},
    {"an absurd digit run saturates rather than overflowing to zero",
     "blackb0x.skip-install=999999999999999999999999999999", 1, 1, 0},
};

void testTable() {
    for (const Case& c : kCases) {
        blackb0x_directives d;
        memset(&d, 0xAA, sizeof(d)); // the parser must zero this itself
        blackb0x_parse_boot_args(c.input, (int)strlen(c.input), &d);
        const std::string label = std::string(c.what) + " [\"" + c.input + "\"]";
        expect(d.skip_install == c.skipInstall, label + ": skip_install");
        expect(d.any == c.any, label + ": any");
        expect(d.rejected == c.rejected, label + ": rejected");
    }
}

// The kernel hands back a NUL-terminated string whose length (oldlen)
// INCLUDES the terminator, and entrypoint.c passes that length straight in.
// Parsing must stop at the NUL rather than walking the byte after it.
void testStopsAtNul() {
    char buf[64];
    memset(buf, 'Z', sizeof(buf));
    const char* s = "blackb0x.skip-install=1";
    memcpy(buf, s, strlen(s) + 1); // trailing NUL, then 'Z' padding

    blackb0x_directives d;
    blackb0x_parse_boot_args(buf, (int)sizeof(buf), &d); // length past the NUL, on purpose
    expect(d.skip_install == 1, "a length that runs past the NUL still parses correctly");
    expect(d.rejected == 0, "bytes after the NUL are not parsed as tokens");
}

// A non-NUL-terminated span: the parser must honour `len` and never read
// past it. Built so the byte immediately after the span would change the
// answer if it were read -- here the trailing 'Z' would turn the value into
// the non-numeric "1Z" and make this a rejection instead of a match.
void testHonoursLength() {
    const char raw[] = "blackb0x.skip-install=1Z";
    blackb0x_directives d;
    blackb0x_parse_boot_args(raw, 23, &d); // 23 = up to but not including 'Z'
    expect(d.skip_install == 1, "a length-bounded, unterminated span parses exactly that span");
    expect(d.rejected == 0, "the byte past the length is not read");
}

// A string far longer than any real boot-args buffer, with a valid directive
// buried in the middle. Bounded loops, so this must simply work.
void testVeryLongString() {
    std::string big(8000, 'x');
    big += " blackb0x.skip-install=1 ";
    big += std::string(8000, 'y');

    blackb0x_directives d;
    blackb0x_parse_boot_args(big.c_str(), (int)big.size(), &d);
    expect(d.skip_install == 1, "a 16KB boot-args string still finds a directive in the middle");
    expect(d.rejected == 0, "long non-directive tokens are not rejections");
}

// A single token longer than the whole real boot-args budget, starting with
// our prefix. It must be rejected, not matched, and must not read past it.
void testVeryLongToken() {
    std::string tok = "blackb0x.";
    tok += std::string(4000, 'a');
    blackb0x_directives d;
    blackb0x_parse_boot_args(tok.c_str(), (int)tok.size(), &d);
    expect(d.any == 0 && d.rejected == 1, "a 4KB directive name is one rejection, not a match");
}

// Defensive contract: a NULL string, and a negative/zero length, are normal
// inputs (the sysctl read failed, or boot-args were empty) and must leave
// the struct at the shipped defaults.
void testNullAndNonPositiveLength() {
    blackb0x_directives d;
    memset(&d, 0xAA, sizeof(d));
    blackb0x_parse_boot_args(nullptr, 0, &d);
    expect(d.skip_install == 0 && d.any == 0 && d.rejected == 0,
           "a NULL boot-args string yields the shipped defaults");

    memset(&d, 0xAA, sizeof(d));
    blackb0x_parse_boot_args("blackb0x.skip-install=1", 0, &d);
    expect(d.skip_install == 0 && d.any == 0, "a zero length parses nothing");

    memset(&d, 0xAA, sizeof(d));
    blackb0x_parse_boot_args("blackb0x.skip-install=1", -5, &d);
    expect(d.skip_install == 0 && d.any == 0, "a negative length parses nothing");

    // Must not fault on a NULL out-pointer either.
    blackb0x_parse_boot_args("blackb0x.skip-install=1", 23, nullptr);
}

// The real thing, end to end: the exact string BAKED INTO iBEC for a ramdisk
// boot, with a directive appended the way `bake-iboot --extra-boot-args`
// composes it.
void testRealWorldComposedString() {
    const char* composed =
        "rd=md0 -v amfi=0xff cs_enforcement_disable=1 amfi_get_out_of_my_way=1 pio-error=0 "
        "blackb0x.skip-install=1";
    blackb0x_directives d;
    blackb0x_parse_boot_args(composed, (int)strlen(composed), &d);
    expect(d.skip_install == 1 && d.any == 1 && d.rejected == 0,
           "the real composed boot-args string arms skip-install and nothing else");

    // And it fits the budget bake-iboot enforces. NOT 127: that is
    // DeviceManager::kMaxRecoveryCommandLength, the ceiling on a recovery
    // COMMAND, and a baked string never passes through that buffer -- there
    // is no `setenv boot-args` on the boot path any more, because this
    // bootloader never reads the variable back.
    //
    // THERE ARE TWO REGIMES, not one ceiling, and the base string above is
    // already in the second one:
    //   * at or under 39 bytes -- the length of iBoot's own compiled-in
    //     "rd=md0 nand-enable-reformat=1 -progress" -- iBoot32Patcher's
    //     patch_boot_args() strcpy()s the new string straight over the old
    //     one IN PLACE and applies no relocation at all. Nothing useful fits:
    //     even "rd=md0 amfi=0xff cs_enforcement_disable=1" is 41.
    //   * above 39 it repoints the literal-pool word at the "Reliance on this
    //     certificate..." string and strcpy()s there instead, unbounded. That
    //     string's pure-ASCII boilerplate is 179 bytes (measured on the real
    //     decrypted AppleTV3,2 10B329a iBEC; the whole C string to its NUL is
    //     193, the last 14 being build-varying DER), which is where
    //     bootargs::kMaxBakedBootArgsLength (Patcher.hpp) comes from. That is
    //     tighter than the ~214 left by iBoot's 256-byte kernel command line.
    // The 81-byte ramdisk base takes the relocation path, and so does the
    // 87-byte tether one. Spelled as a literal here deliberately -- this
    // suite compiles nothing but entrypoint/bootargs.h, and keeping it
    // dependency-free is the point (see the file header).
    expect(strlen(composed) <= 179,
           "the composed baked boot-args string fits the 179-byte relocation budget");
    expect(strlen("rd=md0 nand-enable-reformat=1 -progress") == 39,
           "iBoot's own default is 39 bytes -- the in-place/relocation threshold");
}

} // namespace

int main() {
    testTable();
    testStopsAtNul();
    testHonoursLength();
    testVeryLongString();
    testVeryLongToken();
    testNullAndNonPositiveLength();
    testRealWorldComposedString();

    if (failures == 0) {
        printf("All BootArgsTests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d BootArgsTests failure(s).\n", failures);
    return 1;
}
