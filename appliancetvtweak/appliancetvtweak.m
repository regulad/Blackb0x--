/*
 * appliancetvtweak -- put third-party Frontrow appliances back on the Apple TV
 * main menu.
 *
 * THE PROBLEM. This firmware (12H1006) does not implement the AppleTV2-era
 * drop-in appliance mechanism at all. Its /Applications/AppleTV.app/AppleTV
 * binary contains ZERO occurrences of "Appliances", ".frappliance", or any
 * path pointing at that directory -- verified by `strings` on the real binary
 * and confirmed on hardware (`strings ... | grep -ci frappliance` -> 0).
 * Nothing ever looks where Kodi and nitoTV install. Correct file placement,
 * correct ownership, correct ad-hoc signing and reboots therefore cannot
 * matter, and all four were tried on real hardware before this was understood.
 *
 * What replaced it: -[BRApplianceManager _loadAppliances] gets its list from
 * [[self bindingAdaptor] valueForBinding:...] -- MERCHANTS, synthesized by
 * BRMerchant.appDefinitions from Apple's remote vendor bag
 * (_updateMerchantsWithVendorBags:, ATVMerchantCoordinator). There is no code
 * path from a directory to a merchant, so no amount of installing files can
 * surface a third-party appliance.
 *
 * THE OPENING. The .appliance *format* is still completely live even though
 * the *scan* is gone. +[BRApplianceInfo infoForApplianceDescription:] takes a
 * plain NSDictionary and reads FR* keys straight out of it, and
 * -[BRApplianceManager _loadApplianceWithInfo:] still does
 * principalClassName -> NSClassFromString -> conformsToProtocol:
 * -> [[cls alloc] initWithApplianceInfo:]. Apple's own Settings.appliance,
 * Movies.appliance and friends still sit in the bundle as plists in exactly
 * that shape -- unread, but perfect templates.
 *
 * So this tweak does not reimplement anything. It hooks _loadAppliances,
 * lets the original run, and then hands the SAME loader the appliances it
 * would have found if the scan still existed.
 *
 * TWO GATES, both measured rather than guessed:
 *
 *   1. NOBODY LOADS THE CODE. No xrefs to contentsOfDirectoryAtPath: or
 *      bundleWithPath: survive in the binary, so the bundle's Mach-O is never
 *      mapped and its principal class does not exist to be looked up. We
 *      dlopen() it ourselves. dlopen rather than -[NSBundle load] on purpose:
 *      nitoTV's binary is MH_DYLIB while Kodi's is MH_BUNDLE, and NSBundle is
 *      entitled to object to the former.
 *
 *   2. NEITHER BUNDLE DECLARES <BRAppliance>. `strings | grep -c '^BRAppliance$'`
 *      is 0 for both -- they reference BRApplianceInfo and BRApplianceCategory
 *      but never the protocol itself -- so _loadApplianceWithInfo:'s
 *      conformsToProtocol: check returns NO and the loader silently drops
 *      them. class_addProtocol() at runtime fixes that, and it is honest: both
 *      bundles really do implement initWithApplianceInfo:, the protocol's
 *      actual requirement. We are correcting a missing declaration, not
 *      faking a capability.
 *
 * A KEY MAPPING THE PLAN DID NOT ANTICIPATE, found by reading both real
 * Info.plists against the real binary's string table: these bundles are from
 * 2012-2015 and declare NSPrincipalClass, but "NSPrincipalClass" does not
 * appear in this firmware's AppleTV binary AT ALL. It knows FRPrincipalClass.
 * Same story for the identifier: the bundles carry CFBundleIdentifier, the
 * loader wants FRApplianceIdentifier. So the description dictionary has to be
 * translated, not merely copied. That is done in descriptionForBundle() below,
 * and it is the single most likely thing to need adjusting if this does not
 * work on the first try.
 *
 * SAFETY POSTURE. This runs inside com.apple.lowtide, which is the device's
 * ONLY user interface. A crash here is a television that shows nothing at
 * all, with no way in but SSH. Every step below is therefore individually
 * guarded and every failure is a logged skip rather than an abort -- one
 * broken bundle must never take the menu down with it, and a tweak that
 * silently does nothing is enormously preferable to one that panics lowtide.
 */

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
#import <substrate.h>

/* THERE IS NO WORKING LOG ON THIS PLATFORM, AND BOTH OPTIONS WERE TRIED.
 *
 * NSLog goes to ASL, and ASL ships DISABLED: Apple's own
 * com.apple.syslogd job carries `EnvironmentVariables = { ASL_DISABLE = 1 }`
 * and no `-bsd_out`, so syslogd neither stores messages nor reads
 * /etc/syslog.conf. Editing the on-disk plist does nothing either -- launchd
 * defines that job internally and the change does not survive, not even a
 * reboot (see docs/HISTORY.md, "ASL is disabled on this firmware").
 *
 * The obvious answer, writing our own file, was implemented and then removed:
 * the UI runs as `mobile` (UserName in com.apple.frontrow.plist) and cannot
 * create a file under root-owned /usr/share/blackb0x. It failed silently by
 * design -- logging must never break the menu -- which made "no log file" and
 * "tweak never loaded" indistinguishable, and cost a debugging round.
 *
 * So the calls below go to NSLog and, today, nowhere. They are kept rather
 * than deleted because they document every failure point in the load path at
 * the exact spot it can fail, and because a future logging route is then one
 * line here instead of a rewrite. A mobile-writable path (somewhere under
 * /var/mobile) is the obvious candidate if this is ever needed again. */
#define ATVT_LOG(fmt, ...) NSLog(@"[appliancetvtweak] " fmt, ##__VA_ARGS__)

static NSString * const kAppliancesDir = @"/Applications/AppleTV.app/Appliances";

static void (*orig_loadAppliances)(id self, SEL _cmd);

/* Translate a .frappliance's own Info.plist into the description dictionary
 * +[BRApplianceInfo infoForApplianceDescription:] expects.
 *
 * The FR* keys are passed through untouched when present; the two legacy
 * spellings are rewritten, and a display name is synthesized if the bundle
 * never carried one (nitoTV does not -- it has neither FRApplianceName nor
 * FRApplianceLocalizableNameKey, so without this it would load and then
 * render as an untitled row).
 *
 * Returns nil when the bundle is missing the one thing that cannot be
 * invented: a principal class. */
static NSDictionary *descriptionForBundle(NSString *bundlePath, NSDictionary *info) {
    NSMutableDictionary *desc = [[info mutableCopy] autorelease];

    /* FRPrincipalClass is what this firmware reads; NSPrincipalClass is what
     * these bundles were built to declare, years earlier. */
    NSString *principal = [info objectForKey:@"FRPrincipalClass"];
    if (![principal isKindOfClass:[NSString class]] || [principal length] == 0) {
        principal = [info objectForKey:@"NSPrincipalClass"];
    }
    if (![principal isKindOfClass:[NSString class]] || [principal length] == 0) {
        ATVT_LOG(@"%@: no FRPrincipalClass or NSPrincipalClass -- skipping", bundlePath);
        return nil;
    }
    [desc setObject:principal forKey:@"FRPrincipalClass"];

    /* The loader identifies appliances by FRApplianceIdentifier. These
     * bundles only carry CFBundleIdentifier (which is, conveniently, already
     * in the com.*.frontrow.appliance.* shape Apple's own use). */
    id identifier = [info objectForKey:@"FRApplianceIdentifier"];
    if (![identifier isKindOfClass:[NSString class]] || [identifier length] == 0) {
        identifier = [info objectForKey:@"CFBundleIdentifier"];
    }
    if (![identifier isKindOfClass:[NSString class]] || [identifier length] == 0) {
        ATVT_LOG(@"%@: no usable appliance identifier -- skipping", bundlePath);
        return nil;
    }
    [desc setObject:identifier forKey:@"FRApplianceIdentifier"];

    /* A name, one way or another. Apple's appliances use a localizable key
     * resolved against FRLocalizableStringsFileName; Kodi hardcodes
     * FRApplianceName; nitoTV supplies neither. */
    BOOL haveName = NO;
    id name = [info objectForKey:@"FRApplianceName"];
    if ([name isKindOfClass:[NSString class]] && [name length] > 0) haveName = YES;
    id nameKey = [info objectForKey:@"FRApplianceLocalizableNameKey"];
    if ([nameKey isKindOfClass:[NSString class]] && [nameKey length] > 0) haveName = YES;

    if (!haveName) {
        id fallback = [info objectForKey:@"CFBundleName"];
        if (![fallback isKindOfClass:[NSString class]] || [fallback length] == 0) {
            fallback = [[bundlePath lastPathComponent] stringByDeletingPathExtension];
        }
        [desc setObject:fallback forKey:@"FRApplianceName"];
        ATVT_LOG(@"%@: no name key in Info.plist, using '%@'", bundlePath, fallback);
    }

    /* Both bundles already set FRHideIfNoCategories=false, which is the only
     * thing _shouldLoadApp: actually gates on -- it returns NO only when the
     * appliance has no categories AND asked to be hidden in that case. Set it
     * defensively anyway for any future bundle that forgets. */
    if ([desc objectForKey:@"FRHideIfNoCategories"] == nil) {
        [desc setObject:[NSNumber numberWithBool:NO] forKey:@"FRHideIfNoCategories"];
    }

    return desc;
}

/* Map the bundle's Mach-O and make its principal class satisfy <BRAppliance>.
 * Returns the class, or Nil (having logged why) on any failure. */
static Class loadPrincipalClass(NSString *bundlePath, NSDictionary *desc, NSDictionary *info) {
    NSString *principal = [desc objectForKey:@"FRPrincipalClass"];

    /* Already mapped? A second _loadAppliances pass (the method is called
     * again on merchant changes) must not dlopen the same image repeatedly. */
    Class cls = NSClassFromString(principal);

    if (cls == Nil) {
        NSString *exec = [info objectForKey:@"CFBundleExecutable"];
        if (![exec isKindOfClass:[NSString class]] || [exec length] == 0) {
            exec = [[bundlePath lastPathComponent] stringByDeletingPathExtension];
        }
        NSString *binPath = [bundlePath stringByAppendingPathComponent:exec];

        if (![[NSFileManager defaultManager] fileExistsAtPath:binPath]) {
            ATVT_LOG(@"%@: no executable at %@ -- skipping", bundlePath, binPath);
            return Nil;
        }

        /* RTLD_LAZY, not RTLD_NOW: these are 2010-2015 binaries linked against
         * a BackRow that has moved on, and a missing symbol they never
         * actually call must not abort the load of the whole image. */
        void *handle = dlopen([binPath fileSystemRepresentation], RTLD_LAZY | RTLD_GLOBAL);
        if (handle == NULL) {
            ATVT_LOG(@"%@: dlopen failed: %s", bundlePath, dlerror());
            return Nil;
        }

        cls = NSClassFromString(principal);
        if (cls == Nil) {
            ATVT_LOG(@"%@: dlopen succeeded but class '%@' is still not registered "
                     @"-- wrong principal class name?", bundlePath, principal);
            return Nil;
        }
    }

    /* The protocol gate. objc_getProtocol rather than @protocol(BRAppliance)
     * because BRAppliance is declared inside the host binary, not in any
     * header we have; @protocol() would demand a local definition and would
     * create a DIFFERENT protocol object than the one _loadApplianceWithInfo:
     * compares against. */
    Protocol *brAppliance = objc_getProtocol("BRAppliance");
    if (brAppliance == NULL) {
        ATVT_LOG(@"BRAppliance protocol not found in this process -- is this really lowtide?");
        return Nil;
    }

    if (!class_conformsToProtocol(cls, brAppliance)) {
        if (!class_addProtocol(cls, brAppliance)) {
            ATVT_LOG(@"%@: class_addProtocol(BRAppliance) failed on %@", bundlePath, principal);
            return Nil;
        }
        ATVT_LOG(@"%@: added <BRAppliance> to %@", bundlePath, principal);
    }

    /* The protocol's real requirement. If this is missing, conformsToProtocol:
     * would pass because we just made it, and then the loader would crash on
     * an unrecognized selector -- inside the only UI process on the device. */
    if (![cls instancesRespondToSelector:@selector(initWithApplianceInfo:)]) {
        ATVT_LOG(@"%@: %@ does not implement initWithApplianceInfo: -- refusing to load it",
                 bundlePath, principal);
        return Nil;
    }

    return cls;
}

static void injectAppliances(id manager) {
    NSFileManager *fm = [NSFileManager defaultManager];

    BOOL isDir = NO;
    if (![fm fileExistsAtPath:kAppliancesDir isDirectory:&isDir] || !isDir) {
        ATVT_LOG(@"%@ does not exist -- nothing to inject", kAppliancesDir);
        return;
    }

    NSError *err = nil;
    NSArray *entries = [fm contentsOfDirectoryAtPath:kAppliancesDir error:&err];
    if (entries == nil) {
        ATVT_LOG(@"cannot list %@: %@", kAppliancesDir, err);
        return;
    }

    Class applianceInfoClass = NSClassFromString(@"BRApplianceInfo");
    if (applianceInfoClass == Nil) {
        ATVT_LOG(@"BRApplianceInfo not found -- wrong process, doing nothing");
        return;
    }
    SEL infoSel = @selector(infoForApplianceDescription:);
    if (![applianceInfoClass respondsToSelector:infoSel]) {
        ATVT_LOG(@"+[BRApplianceInfo infoForApplianceDescription:] missing -- firmware changed?");
        return;
    }
    SEL loadSel = @selector(_loadApplianceWithInfo:);
    if (![manager respondsToSelector:loadSel]) {
        ATVT_LOG(@"-[BRApplianceManager _loadApplianceWithInfo:] missing -- firmware changed?");
        return;
    }

    NSUInteger loaded = 0, seen = 0;

    for (NSString *entry in entries) {
        if (![[entry pathExtension] isEqualToString:@"frappliance"]) continue;
        seen++;

        NSString *bundlePath = [kAppliancesDir stringByAppendingPathComponent:entry];
        NSString *plistPath = [bundlePath stringByAppendingPathComponent:@"Info.plist"];

        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:plistPath];
        if (![info isKindOfClass:[NSDictionary class]]) {
            ATVT_LOG(@"%@: unreadable Info.plist -- skipping", bundlePath);
            continue;
        }

        NSDictionary *desc = descriptionForBundle(bundlePath, info);
        if (desc == nil) continue;

        Class cls = loadPrincipalClass(bundlePath, desc, info);
        if (cls == Nil) continue;

        id applianceInfo = ((id (*)(id, SEL, id))objc_msgSend)(applianceInfoClass, infoSel, desc);
        if (applianceInfo == nil) {
            ATVT_LOG(@"%@: infoForApplianceDescription: returned nil -- bad description dict",
                     bundlePath);
            continue;
        }

        @try {
            ((void (*)(id, SEL, id))objc_msgSend)(manager, loadSel, applianceInfo);
            loaded++;
            ATVT_LOG(@"%@: loaded", entry);
        } @catch (NSException *ex) {
            /* These classes call BackRow API from 2010-2015 against a 2022
             * build. Surviving the load is genuinely not guaranteed, and an
             * exception here must not take lowtide with it. */
            ATVT_LOG(@"%@: EXCEPTION during _loadApplianceWithInfo: -- %@: %@",
                     entry, [ex name], [ex reason]);
        }
    }

    ATVT_LOG(@"injected %lu of %lu .frappliance bundle(s)",
             (unsigned long)loaded, (unsigned long)seen);
}

static void hooked_loadAppliances(id self, SEL _cmd) {
    /* Apple's own work first, always. If injection is going to fail it should
     * fail with a working stock menu still on screen. */
    orig_loadAppliances(self, _cmd);

    @autoreleasepool {
        @try {
            injectAppliances(self);
        } @catch (NSException *ex) {
            ATVT_LOG(@"EXCEPTION in injectAppliances -- %@: %@", [ex name], [ex reason]);
        }
    }
}

__attribute__((constructor))
static void appliancetvtweak_init(void) {
    @autoreleasepool {
        /* LOG UNCONDITIONALLY, FIRST THING, BEFORE ANY CHECK CAN BAIL.
         *
         * This line exists to make the absence of a log file MEAN something.
         * An earlier version returned silently when BRApplianceManager was
         * missing -- reasonable on its own terms (a mis-scoped filter should
         * be inert, not noisy) -- with the result that "no log file" was
         * indistinguishable between two completely different failures:
         * Substrate never injected us at all, or it did and we bailed. Those
         * have opposite fixes, and on real hardware that ambiguity cost a
         * debugging round.
         *
         * With this line, the rule is unambiguous: NO FILE AT ALL means the
         * dylib was never loaded into anything, full stop. Anything else, we
         * ran and the file says how far we got.
         *
         * The process name is included because the Substrate filter matches
         * on bundle identifier (com.apple.lowtide) while everything else on
         * this platform -- ps, killall -- speaks in executable names
         * (AppleTV). Printing what we actually landed in removes the guessing
         * if the filter is ever wrong. */
        ATVT_LOG(@"loaded into pid %d (%@)", (int)getpid(),
                 [[NSProcessInfo processInfo] processName] ?: @"?");

        Class managerClass = NSClassFromString(@"BRApplianceManager");
        if (managerClass == Nil) {
            /* Now a logged skip rather than a silent one. Still not an error:
             * the filter should keep us out of other processes. But if this
             * fires inside lowtide itself, it is a real finding -- it would
             * mean the class is not registered by the time an inserted
             * dylib's constructor runs, and the hook would have to move to a
             * later point (dlopen of the right image, or a first-call
             * trampoline) rather than the constructor. */
            ATVT_LOG(@"BRApplianceManager not found in this process -- not hooking");
            return;
        }

        SEL sel = @selector(_loadAppliances);
        if (!class_getInstanceMethod(managerClass, sel)) {
            ATVT_LOG(@"-[BRApplianceManager _loadAppliances] not found -- firmware changed, "
                     @"not hooking");
            return;
        }

        MSHookMessageEx(managerClass, sel,
                        (IMP)&hooked_loadAppliances, (IMP *)&orig_loadAppliances);
        ATVT_LOG(@"hooked -[BRApplianceManager _loadAppliances]");
    }
}
