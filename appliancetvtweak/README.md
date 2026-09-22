# appliancetvtweak

A MobileSubstrate tweak that puts third-party Frontrow appliances — Kodi and
nitoTV — back on the Apple TV main menu.

## Why it has to exist

This firmware does not implement the AppleTV2-era drop-in appliance mechanism
at all. `/Applications/AppleTV.app/AppleTV` contains **zero** occurrences of
`Appliances`, `.frappliance`, or any path pointing at that directory —
verified by `strings` on the real binary and confirmed on hardware:

```sh
strings /Applications/AppleTV.app/AppleTV | grep -ci frappliance   # 0
```

Nothing ever looks where both packages install. That is why correct file
placement, correct `chown`, ad-hoc signing, and reboots all failed in turn —
each was a real fix to a real problem, and none of them was *this* problem.

What replaced the scan: `-[BRApplianceManager _loadAppliances]` reads a bound
list of **merchants**, synthesized by `BRMerchant.appDefinitions` from Apple's
remote vendor bag. There is no code path from a directory to a merchant, so no
amount of installing files can surface a third-party appliance.

## Why it can work anyway

The `.appliance` *format* is still completely live even though the *scan* is
gone. `+[BRApplianceInfo infoForApplianceDescription:]` takes a plain
`NSDictionary` and reads `FR*` keys straight out of it, and
`-[BRApplianceManager _loadApplianceWithInfo:]` still does
`principalClassName` → `NSClassFromString` → `conformsToProtocol:` →
`[[cls alloc] initWithApplianceInfo:]`. Apple's own `Settings.appliance` and
friends still sit in the bundle as plists in exactly that shape — unread, but
perfect templates.

So the tweak reimplements nothing. It hooks `_loadAppliances`, lets the
original run, and hands the **same loader** the appliances it would have found
if the scan still existed.

## The three gates, all measured

| Gate | Why it blocks | Fix |
|---|---|---|
| Bundle code is never mapped | no `bundleWithPath:`/`contentsOfDirectoryAtPath:` xrefs survive | `dlopen` it ourselves (`RTLD_LAZY`) |
| Neither bundle declares `<BRAppliance>` | `strings \| grep -c '^BRAppliance$'` is 0 for both, so `conformsToProtocol:` returns NO and the loader drops them | `class_addProtocol` at runtime |
| Wrong plist keys | both bundles are 2012–2015 and declare `NSPrincipalClass` + `CFBundleIdentifier`; **`NSPrincipalClass` does not appear in this firmware's binary at all** — it reads `FRPrincipalClass` and `FRApplianceIdentifier` | translate the dictionary in `descriptionForBundle()` |

That third one was not in the original plan and is the most likely thing to
need adjusting if this does not work first try. `dlopen` rather than
`-[NSBundle load]` because nitoTV's binary is `MH_DYLIB` while Kodi's is
`MH_BUNDLE`.

Adding the protocol is honest rather than a trick: both bundles really do
implement `initWithApplianceInfo:`, the protocol's actual requirement. The
tweak checks for that explicitly and refuses to load a class that lacks it —
otherwise `conformsToProtocol:` would pass (we just made it pass) and the
loader would crash on an unrecognized selector.

## Safety

This runs inside `com.apple.lowtide`, the device's **only** user interface. A
crash here is a television that shows nothing, reachable only by SSH. So:
Apple's own `_loadAppliances` always runs first and completely; every step is
individually guarded; every failure is a logged skip rather than an abort; the
`_loadApplianceWithInfo:` call is wrapped in `@try`; and the Substrate filter
names exactly one bundle identifier.

The biggest genuine unknown is not loading but **survival** — these classes
call BackRow API from 2010–2015 against a 2022 build. They may load, conform,
init, and then crash later on since-changed internals.

## Building

```sh
make -C appliancetvtweak                            # armv7 dylib, ldid-signed
package/build_appliancetvtweak.sh out.deb 1.0       # the .deb
```

Needs a **pinned, era-matched Xcode** for the same reasons `entrypoint/` and
`cainjector/` do — see `appliancetvtweak/Makefile`'s header. Theos supplies
only the packaging half (`dm.pl`); its own SDKs are arm64-only and cannot
build this. Logos is deliberately not used for a single hook — `MSHookMessageEx`
against `substrate.h` is what Logos would have generated anyway.

Substrate comes from `debcache/mobilesubstrate_*.deb`, not from the Theos
checkout: that deb carries both the header and a real armv7 `CydiaSubstrate`
binary exporting `_MSHookMessageEx`, so the build links against the same
artifact the device loads.

## Install ordering

The package `Depends: mobilesubstrate` and is on
`misc/prebake_package_blacklist.txt`, so it is **not** bake-time-installed the
way `xyz.regulad.blackb0x` and `cainjector` are. `stageAppliancetvtweakPackage()`
drops the built `.deb` into the on-device apt archive cache;
`postinstall.sh`'s existing `dpkg -i` fallback installs it after
`apt-get install "${PACKAGES[@]}"` has put mobilesubstrate in place.

## Diagnosing it (there is no log)

**This firmware has no working log, and both routes were tried.** `NSLog`
goes to ASL, which ships disabled — Apple's `com.apple.syslogd` job carries
`EnvironmentVariables = { ASL_DISABLE = 1 }` and no `-bsd_out`, and editing
the on-disk plist changes nothing because launchd defines that job internally
(it survives a reboot unchanged; see `docs/HISTORY.md`). Writing our own file
was implemented and removed: the UI runs as `mobile` and cannot create files
under root-owned `/usr/share/blackb0x`, and it failed *silently* by design, so
"no log" and "tweak never loaded" became indistinguishable.

So diagnose from the filesystem and the menu instead:

| symptom | meaning |
|---|---|
| `launchctl getenv DYLD_INSERT_LIBRARIES` is empty | **check this first.** Substrate is inactive and no tweak on the device loads. `/etc/rc.d/blackb0x-substrate` (main package) fixes it at boot; run it by hand to fix it now |
| appliances missing entirely | almost always the above. Every other cause is rarer |
| appliance present, no icon | the icon files are missing or misnamed — see the two conventions above, and check both `NewUI/` and the `MainMenu` cache |
| one appliance present, the other missing | that bundle specifically failed to load: its principal class, its `dlopen`, or an exception during `_loadApplianceWithInfo:`. `%orig` runs first, so the stock menu and any working appliance survive |
| stock menu intact, nothing of ours | the hook never installed: `BRApplianceManager` absent (wrong process) or the Substrate filter did not match `com.apple.lowtide` |

If a real log is ever needed again, the fix is a `mobile`-writable path under
`/var/mobile` and one line in `ATVT_LOG`.
