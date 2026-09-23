# icanhasrez

A MobileSubstrate tweak that lets Settings offer the television's **own native
resolution**, on panels whose native timing Apple never listed.

## Why it has to exist

QuartzCore decides whether a `(width, height)` pair can be a display mode *at
all* by consulting a hardcoded if-else chain of ten tuples, at
`__TEXT + 0xf1cbc`:

```
 720x480 -> 4    1280x720  ->  9
 720x576 -> 5    1280x1024 -> 10
 640x480 -> 6    1600x1200 -> 11
 800x600 -> 7    1920x1080 -> 12
1024x768 -> 8    1920x1200 -> 13
```

The mode-list builder calls it once per timing element and drops anything that
returns 0:

```c
id = lookupMode(display, hActive, vActive);
if (id == 0) continue;
```

A television whose native timing is not one of those ten therefore cannot be
driven natively, whatever its EDID says. The case that prompted this was a
1366x768 Sony that advertises **1360x768@60 as its preferred timing** and was
being given a downscaled 720p or 1080p instead.

## Why it can work anyway

That same function already ends with the case we need:

```c
if (w == self->nativeWidth && h == self->nativeHeight
    && (self->nativeWidth | self->nativeHeight) != 0)
    return 14;                           // "this is the panel's own size"
```

An escape hatch for exactly "this display's native resolution, whatever it
is". It never fires, because **both fields are zero** — measured on hardware,
every rejection logged `display native fields: 0 x 0`. Nothing on this
platform fills them in.

So this tweak patches nothing. It fills those two fields in, from the EDID's
own preferred detailed timing, and lets Apple's stock clause do the rest.

## How the layers were ruled out

Each step eliminated the obvious suspect above it. All addresses are in the
decrypted 12H1006 `/Applications/AppleTV.app/AppleTV`.

| Layer | Verdict |
|---|---|
| `-[SettingsTVResolutionController _buildSupportedResolutionList]` (`0x353448`) | Renders `allPotentialModes` verbatim. No filtering. |
| `-[BRDisplayManager _computeAllModesForDisplay]` (`0x496728`) | One filter, `width <= 1920 && height <= 1080`. Pairs each resolution with a hardcoded 60.0/50.0 Hz, since a `CADisplayMode` has no refresh rate. |
| `IOAVFamily` → `AppleRGBOUT` → `IOMobileFramebuffer` | Publishes every EDID timing, native one included, flagged `IsPreferred`. Eight resolutions in the kernel, seven in `CADisplay`. |
| QuartzCore `__TEXT + 0xf1cbc` | **The filter.** The ten tuples above. |

## Why it reads the EDID instead of hardcoding a resolution

The first working version hardcoded 1360x768, which is the wrong shape for
something that ships: useless to a 1280x768 panel, and actively wrong to offer
to a display that cannot show it.

The general statement of the bug is "the native size fields are empty", so the
general fix is to fill them from the display's own preferred timing. EDID
descriptor 0 is that timing by definition when the feature byte's
preferred-timing bit is set. A 1080p television ends up declaring 1920x1080,
which is already tuple 12, and nothing changes for it. Only a display whose
native size Apple never listed sees any difference — exactly the set this is
for.

## Where it runs

**backboardd, and nothing else.** Not a guess: an earlier build hooked the same
function inside `com.apple.lowtide`, logged a clean install with a matching
prologue signature, and then never fired once. lowtide's `CADisplay` receives
an already-built list over IPC; the render server is backboardd, whose
LaunchDaemon vends `com.apple.CARenderServer`.

## Safety

backboardd is the display server, so a crash here is worse than one in the UI
— it takes the whole display stack down. What makes hooking a raw offset
acceptable:

- The first 16 bytes of the target are compared against a recorded signature
  before anything is written. A different firmware, a different shared cache,
  or a slide computed wrongly all become a silent refusal.
- The base address comes from the loaded image's own `mach_header`, so the dyld
  shared cache slide is handled by construction.
- The native-size fields are written **only when both are zero**.
- Only the display's own preferred timing is ever accepted. It never invents a
  resolution.
- `touch /var/mobile/.blackb0x-no-icanhasrez` disables it without removing the
  package. Deleting the filter plist unloads it entirely.

Selecting a mode that does not display is also self-correcting: this firmware
reverts an unconfirmed display mode change on a countdown
(`BlackScreenSecsBeforeDisplayModeIsCanceled`).

## Building

```sh
make -C tweaks/icanhasrez                  # armv7 dylib, ldid-signed
tweaks/icanhasrez/build.sh out.deb 1.0     # the .deb
```

Needs the same **pinned, era-matched Xcode** as `entrypoint/`, `cainjector/`
and `tweaks/appliancetvtweak/` — see `tweaks/icanhasrez/Makefile`'s header. Substrate
comes from `debcache/mobilesubstrate_*.deb`, not the Theos checkout, so the
build links the same artifact the device loads.

## Install ordering

`Depends: mobilesubstrate`, so it is on `misc/prebake_package_blacklist.txt`
and is **not** bake-time-installed the way `xyz.regulad.blackb0x` and
`cainjector` are. `stageIcanhasrezPackage()` drops the built `.deb` into the
on-device apt archive cache; `postinstall.sh`'s `dpkg -i` fallback installs it
after `apt-get install "${PACKAGES[@]}"` has put mobilesubstrate in place.

## Diagnosing it

The shipped tweak writes no diagnostic log. It silently skips the fix when
the prologue does not match or the display supplies no usable preferred timing.
If no new mode appears, check `launchctl getenv DYLD_INSERT_LIBRARIES`:
an empty value means Substrate is inactive; `/etc/rc.d/blackb0x-substrate`
activates it.

The new row is labelled by `-[BRDisplayManager stringForDisplayMode:]`, which
only knows 1280x720, 1920x1080 and the 576/480 heights by name. Everything else
falls through to the `ResolutionOther` format string, so a working 1360x768
entry reads **`1360 x 768 - 60Hz`**, not "768p".
