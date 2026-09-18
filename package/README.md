# `package/` — the real `xyz.regulad.blackb0x` .deb

Everything blackb0x installs on the device, as one actual package instead of
loose files dpkg has no record of (`.claude/TODO.md` item 11).

Before this, `stageBlackb0xTree()` (`BakeRamdisk.cpp`) copied blackb0x's own
content onto the ramdisk with plain `stageFile()` calls — the apt source
list/gpg pairs, `postinstall.sh`, the first-boot LaunchDaemon, and the local
package repository. Every *other* thing that ends up on the device goes
through real dpkg/apt. That made blackb0x's own content the one piece with no
`Status:`/`.list`/`.md5sums` entry anywhere: not upgradeable, not removable,
invisible to apt.

## What's here

- `layout/DEBIAN/control` — package metadata. `__BLACKB0X_VERSION__` is
  substituted by `build.sh`.
- `layout/DEBIAN/postinst` — a bare `launchctl load` of the first-boot
  LaunchDaemon. Read its header comment before editing it; its triviality is
  load-bearing for the ramdisk baker.
- `build.sh` — runs Theos's `dm.pl` inside `ghcr.io/regulad/dotfiles:latest`.

## Why Theos, and which half of it

`dm.pl` builds a correct `.deb` without root and without fakeroot, and it is
the tool this ecosystem actually uses. Not needing root matters here:
`bake-all-ramdisks` already needs it for the loop mount, and a second,
different reason to need it is worth avoiding.

We use **none** of Theos's compilation half. This package is pure data — apt
sources, gpg keys, a plist, shell scripts, and the bundled `.deb`s. Nothing is
compiled, so the image's SDKs never come into it. That is fortunate, because
the image ships only `AppleTVOS12.4.sdk` and `iPhoneOS16.5.sdk`, both
arm64-era and neither able to target this project's armv7 devices. **If this
package ever grows a real binary, that is a genuine blocker, not a detail.**

This is the opposite call from `entrypoint/`, which explicitly rejected Theos
("this binary is freestanding, Theos's `tool.mk` assumes exactly the
opposite") and builds its own cctools-port image. Both are right: `entrypoint/`
needs a compiler and no packaging; this needs packaging and no compiler.

## Container notes

The image sets `THEOS` from the login profile of its `regulad.linux` user, but
`build.sh` passes `THEOS` explicitly and uses a non-login shell instead. The
profile also triggers a Homebrew API fetch, which would make every bake slow,
network-dependent and non-deterministic.

`build.sh` deliberately does **not** pass `-u`. Under rootless podman the
container's root maps to the invoking host user, so running as container root
is what makes the output land owned by the caller; passing `-u 1000` maps to a
subuid owning nothing on the host and `dm.pl` fails with a bare "Permission
denied" that reads like a `dm.pl` bug rather than a uid-mapping one.
`BLACKB0X_THEOS_UID` is the escape hatch for a rootful setup.

## Package lists

- `packages.txt` — the flat set of package names blackb0x installs. This is
  the *request*; what apt can actually satisfy is the resolved closure.
- `local_only_debs.txt` — which of those ship from the bundled local repo at
  `/var/.blackb0x/local-debs` rather than a live one.

Both live here rather than in `Blackb0x/Misc/` because they describe what this
package installs. `Blackb0x/Misc/prebake_package_blacklist.txt` deliberately
does **not** move: it is baker policy about what may be unpacked at bake time,
not a statement about package content.

## What's assembled at bake time

`layout/` is static except for one file. `var/.blackb0x/postinstall.sh` ships
as a template and `build.sh` substitutes `__BLACKB0X_PACKAGES__` with the
resolved package list at package-build time, from the
`<resolved-packages-file>` argument. The bundled local-repo `.deb`s are added
to the staging tree by the baker for the same reason: both depend on what a
particular bake actually resolved.

So `BakeRamdisk.cpp` assembles a complete staging tree — `layout/` plus the
resolved local-repo `.deb`s — and then calls `build.sh`.

## Usage

```
package/build.sh <staging-dir> <output.deb> [version] [resolved-packages-file]
```

`<resolved-packages-file>` is whitespace-separated package names (newline or
space), so `resolved_packages.txt` can be handed over as-is. It is required
whenever the staged `postinstall.sh` still contains the placeholder.

`<staging-dir>` must contain `DEBIAN/control`; the payload sits alongside it at
final on-device paths (e.g. `var/.blackb0x/...`).
