/*
 * icanhasrez -- let the Apple TV offer its television's own native resolution.
 *
 * THE BUG, in one sentence: QuartzCore decides whether a (width, height) pair
 * can be a display mode at all by consulting a hardcoded list of ten, and a
 * television whose native timing is not on that list can never be driven at
 * its native timing, no matter what its EDID says.
 *
 * ========================= HOW THAT WAS ESTABLISHED =========================
 *
 * By elimination, against the real decrypted 12H1006 root filesystem and then
 * against real hardware. Each step ruled out the obvious suspect above it:
 *
 *   The menu.  -[SettingsTVResolutionController _buildSupportedResolutionList]
 *   (0x353448 in /Applications/AppleTV.app/AppleTV) renders
 *   -[BRDisplayManager allPotentialModes] verbatim. No allow-list, no table.
 *
 *   BRDisplayManager.  -_computeAllModesForDisplay (0x496728) applies exactly
 *   one filter, `width <= 1920 && height <= 1080`, and pairs every surviving
 *   resolution with a hardcoded 60.0 and 50.0 Hz (the doubles at 0x496bc8 and
 *   0x496bd0) because a CADisplayMode carries no refresh rate of its own.
 *
 *   The kernel.  IOAVFamily parses the EDID correctly and publishes every
 *   timing element -- including the native one, flagged IsPreferred -- the
 *   whole way through AppleRGBOUT and IOMobileFramebuffer. Verified on
 *   hardware by dumping all three IORegistry entries: eight distinct
 *   resolutions in the kernel, seven in CADisplay.
 *
 *   QuartzCore.  __TEXT + 0xf1cbc is an if-else chain over TEN hardcoded
 *   tuples, called once per timing element by the mode-list builder:
 *
 *       id = lookupMode(display, hActive, vActive);
 *       if (id == 0) continue;            // everything not in the ten dies here
 *
 *        720x480 -> 4    1280x720  ->  9
 *        720x576 -> 5    1280x1024 -> 10
 *        640x480 -> 6    1600x1200 -> 11
 *        800x600 -> 7    1920x1080 -> 12
 *       1024x768 -> 8    1920x1200 -> 13
 *
 *   On the television this was found with -- a Sony whose EDID declares
 *   1360x768@60 as its preferred timing -- every resolution that survived was
 *   on that list and the only one that did not survive was the only one that
 *   was not.
 *
 * ===================== WHY THIS PATCHES NOTHING AT ALL =====================
 *
 * That function already ends with the case we need:
 *
 *     if (w == self->nativeWidth && h == self->nativeHeight
 *         && (self->nativeWidth | self->nativeHeight) != 0)
 *         return 14;                                  // "the panel's own size"
 *
 * an escape hatch for exactly "this display's native resolution, whatever it
 * is". It never fires, because both fields are ZERO -- measured, on hardware:
 * every rejection logged `display native fields: 0 x 0`. Nothing on this
 * platform ever fills them in.
 *
 * So this tweak does not patch code, force a return value, or add an eleventh
 * tuple. It fills in the two fields, from the EDID's own preferred detailed
 * timing, and lets Apple's stock clause do the rest. Downstream code that
 * resolves id 14 by reading those same two fields then gets the right
 * geometry rather than the 0 x 0 it would read if the return value had merely
 * been faked -- which was tried first, on hardware, and produced a menu entry
 * that read 0x0 and a gray screen.
 *
 * ==================== WHY IT READS THE EDID RATHER THAN ====================
 * ====================   HARDCODING A RESOLUTION        ====================
 *
 * The first working version hardcoded 1360x768, because that is what the one
 * television in front of it wanted. That is the wrong shape for something
 * that ships: it would do nothing for a 1280x768 panel, and it would
 * cheerfully offer 1360x768 to a display that cannot show it.
 *
 * The general statement of the bug is "the native size fields are empty", and
 * the general fix is to fill them from the display's own preferred timing.
 * EDID descriptor 0 is that timing by definition when the feature byte's
 * preferred-timing bit is set, which it is on essentially every digital
 * display made since 2000. A 1080p television therefore ends up declaring
 * 1920x1080, which is already tuple 12, and nothing changes for it. Only a
 * display whose native size Apple never listed sees any difference -- which
 * is exactly the set of displays this is for.
 *
 * ========================== WHERE THIS RUNS ==========================
 *
 * backboardd, and only backboardd. The Substrate filter names its executable
 * and nothing else. That is not a guess either: an earlier build hooked this
 * function inside com.apple.lowtide, logged a clean install with a matching
 * prologue signature, and then never fired once -- because lowtide's
 * CADisplay receives an already-built list from the render server, and the
 * render server is backboardd (see its LaunchDaemon's MachServices, which
 * vends com.apple.CARenderServer).
 *
 * ============================ SAFETY POSTURE ============================
 *
 * backboardd is the display server. A crash here is worse than a crash in the
 * UI: it takes the whole display stack with it. So:
 *
 *   - The hook address is an OFFSET, and an offset is meaningless without
 *     proof it still points at the right code. The first 16 bytes of the
 *     function are compared against a recorded signature before anything is
 *     written, and a mismatch leaves the function untouched. A different firmware, a
 *     different shared cache, or a slide computed wrongly all become "did
 *     nothing" instead of "corrupted a stranger's prologue".
 *   - The base address comes from the loaded image's own mach_header, so the
 *     dyld shared cache slide is accounted for by construction.
 *   - The two native-size fields are written ONLY when both are zero. A
 *     display that populates them for real is never overwritten.
 *   - Only the display's own preferred timing is ever accepted. This never
 *     invents a resolution.
 *   - Every failure skips the fix silently. The original is always called.
 *   - Creating /var/mobile/.blackb0x-no-icanhasrez disables the whole thing
 *     without removing the package, for a device whose screen is black and
 *     whose only door is SSH.
 */

#import <Foundation/Foundation.h>
#import <substrate.h>
#import <mach-o/dyld.h>
#import <dlfcn.h>
#import <string.h>

/* Opt-OUT, not opt-in. This is a shipped fix rather than an experiment, and
 * it is inert on any display whose native size Apple already listed. A file
 * rather than a preference because it can be created over SSH on a device
 * showing nothing at all, which is the only situation it is for. */
static NSString * const kDisableFlagPath =
    @"/var/mobile/.blackb0x-no-icanhasrez";

static BOOL disabled(void) {
    return [[NSFileManager defaultManager] fileExistsAtPath:kDisableFlagPath];
}

#pragma mark - The display's own preferred timing, out of its EDID

/* IOKit's IOAVService* API. Not a guess: -[BRDisplayManager _recordEDID]
 * (0x495534 in the real AppleTV binary) does exactly
 *
 *     IOAVServiceRef svc = IOAVServiceCreate(kCFAllocatorDefault);
 *     CFTypeRef edid = IOAVServiceCopyProperty(svc, CFSTR("EDID"));
 *
 * and `nm -m` on that binary reports both as coming from IOKit. dlopen by
 * absolute path rather than leaf name, and dlsym rather than a link-time
 * dependency, so a firmware that does not export these skips the fix
 * instead of a dyld abort at launch inside the display server. */
typedef CFTypeRef (*IOAVServiceCreate_t)(CFAllocatorRef);
typedef CFTypeRef (*IOAVServiceCopyProperty_t)(CFTypeRef, CFStringRef);

/* Descriptor 0 of EDID block 0, decoded, or {0,0}.
 *
 * Only descriptor 0, and only when the feature byte says it is the preferred
 * timing. The other three descriptors are whatever the manufacturer felt like
 * including and carry no claim about the panel; treating one of those as
 * "native" is how you end up driving a television at a mode it merely
 * tolerates. */
static void preferredTimingFromEDID(const uint8_t *e, size_t len,
                                    uint32_t *outW, uint32_t *outH) {
    *outW = 0; *outH = 0;
    if (len < 128) return;

    static const uint8_t header[8] = {0,0xff,0xff,0xff,0xff,0xff,0xff,0};
    if (memcmp(e, header, 8) != 0) return;

    /* Feature byte bit 1: "descriptor 0 is the preferred timing mode". */
    if ((e[24] & 0x02) == 0) return;

    const uint8_t *d = e + 54;
    if (d[0] == 0 && d[1] == 0) return;          /* a monitor descriptor, not a timing */

    uint32_t hActive = (uint32_t)(((d[4] & 0xf0) << 4) | d[2]);
    uint32_t vActive = (uint32_t)(((d[7] & 0xf0) << 4) | d[5]);
    if (hActive == 0 || vActive == 0) return;

    /* Interlaced timings describe a field, not a frame, and the display mode
     * machinery here is entirely progressive. Refuse rather than report half
     * a picture's height as the panel's native size. */
    if (d[17] & 0x80) return;

    *outW = hActive;
    *outH = vActive;
}

/* Resolved once, on the first lookup, and cached. The EDID cannot change
 * without a hotplug, and a hotplug re-enumerates through this same path with
 * the same answer; re-reading it per timing element would mean an IOKit round
 * trip several times a second during enumeration. */
static uint32_t gNativeW = 0, gNativeH = 0;
static BOOL     gNativeResolved = NO;

static void resolveNativeSize(void) {
    if (gNativeResolved) return;
    gNativeResolved = YES;

    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (iokit == NULL) {
        return;
    }

    IOAVServiceCreate_t create =
        (IOAVServiceCreate_t)dlsym(iokit, "IOAVServiceCreate");
    IOAVServiceCopyProperty_t copyProp =
        (IOAVServiceCopyProperty_t)dlsym(iokit, "IOAVServiceCopyProperty");
    if (create == NULL || copyProp == NULL) {
        return;
    }

    CFTypeRef svc = create(kCFAllocatorDefault);
    if (svc == NULL) {
        /* Retry when a display is not attached yet. */
        gNativeResolved = NO;
        return;
    }

    CFTypeRef edid = copyProp(svc, CFSTR("EDID"));
    if (edid != NULL && CFGetTypeID(edid) == CFDataGetTypeID()) {
        preferredTimingFromEDID(CFDataGetBytePtr((CFDataRef)edid),
                                (size_t)CFDataGetLength((CFDataRef)edid),
                                &gNativeW, &gNativeH);
    }

    if (edid != NULL) CFRelease(edid);
    CFRelease(svc);
}

#pragma mark - The hook

/* QuartzCore __TEXT-relative offset of the lookup function, and its prologue.
 * See the SAFETY POSTURE note at the top: the signature is the only thing
 * standing between a wrong offset and a corrupted function in the display
 * server. Do not shorten it and do not skip it. */
static const uint32_t kLookupModeOffset = 0xf1cbc;
static const uint8_t  kLookupModeSig[16] = {
    0x90,0xb5,0x01,0xaf,0xb1,0xf5,0x34,0x7f,
    0x02,0xbf,0xb2,0xf5,0xf0,0x7f,0x04,0x20,
};

/* The display object's native-size pair, the two fields the stock function's
 * final clause compares against. */
static const size_t kNativeWidthOffset  = 0xa0;
static const size_t kNativeHeightOffset = 0xa4;

static int (*orig_lookupMode)(void *display, int width, int height);

static int hooked_lookupMode(void *display, int width, int height) {
    if (!disabled() && display != NULL) {
        @try {
            resolveNativeSize();

            if (gNativeW != 0 &&
                (uint32_t)width == gNativeW && (uint32_t)height == gNativeH) {

                volatile uint32_t *nw =
                    (volatile uint32_t *)((uint8_t *)display + kNativeWidthOffset);
                volatile uint32_t *nh =
                    (volatile uint32_t *)((uint8_t *)display + kNativeHeightOffset);

                /* Only when empty. A display that fills these in for real has
                 * a better claim to them than we do. */
                if (*nw == 0 && *nh == 0) {
                    *nw = gNativeW;
                    *nh = gNativeH;
                }
            }
        } @catch (NSException *ex) {
            /* Preserve the stock lookup if resolving the display fails. */
        }
    }

    return orig_lookupMode(display, width, height);
}

static void installHook(void) {
    const char *needle = "/System/Library/Frameworks/QuartzCore.framework/QuartzCore";

    const void *base = NULL;
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (name != NULL && strcmp(name, needle) == 0) {
            base = _dyld_get_image_header(i);
            break;
        }
    }
    if (base == NULL) {
        return;
    }

    const uint8_t *fn = (const uint8_t *)base + kLookupModeOffset;
    if (memcmp(fn, kLookupModeSig, sizeof kLookupModeSig) != 0) {
        return;
    }

    /* Bit 0 set marks a Thumb entry point, which this is: the signature above
     * is `push {r4, r7, lr}; add r7, sp, #4` in Thumb-2. Substrate reads that
     * bit to choose the instruction set for its trampoline, and getting it
     * wrong gives a process that dies on the first call rather than an error
     * anyone can read. */
    MSHookFunction((void *)((uintptr_t)fn | 1),
                   (void *)&hooked_lookupMode,
                   (void **)&orig_lookupMode);

}

__attribute__((constructor))
static void icanhasrez_init(void) {
    @autoreleasepool {
        if (disabled()) return;
        installHook();
    }
}
