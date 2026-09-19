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
- `build.sh` — runs Theos's `dm.pl` (`$THEOS`, default `~/theos`).

## Why Theos, and which half of it

`dm.pl` builds a correct `.deb` without root and without fakeroot, and it is
the tool this ecosystem actually uses. Not needing root matters here:
nothing else in the bake needs root any more (the one thing that did, the
Linux HFS+ loop mount, is gone), and reintroducing a reason to need it just
to build a package would be a step backwards.

We use **none** of Theos's compilation half. This package is pure data — apt
sources, gpg keys, a plist, shell scripts, and the bundled `.deb`s. Nothing is
compiled, so Theos's SDKs never come into it. That is fortunate, because no
current SDK can target this project's armv7 devices. **If this package ever
grows a real binary, that is a genuine blocker, not a detail.**

This is the opposite call from `entrypoint/`, which explicitly rejected Theos
("this binary is freestanding, Theos's `tool.mk` assumes exactly the
opposite") and uses a `cctools-port` cross-compiler instead. Both are right:
`entrypoint/` needs a compiler and no packaging; this needs packaging and no
compiler.

## Ownership and modes are asserted, not inherited

`build.sh` runs a directories-0755/files-0644 pass over the staging tree
immediately before `dm.pl` tars it, then re-marks the maintainer scripts and
`postinstall.sh` executable. Modes are asserted; a `.deb` records them per
entry and a checkout's umask is not a spec.

Ownership is **not** chown'd, and `build.sh` refuses to run as root. `dm.pl`
picks entry ownership off its own euid:

```perl
if ($< == 0) { $tf->chown($stat[4], $stat[5]); }   # root: preserve on-disk
else         { $tf->chown("root", "wheel"); }      # non-root: force 0:0
```

So a non-root `dm.pl` stamps `root:wheel` on everything by construction —
exactly what this package wants — and running as root is the broken case,
because it would record whatever the staging tree happens to carry. An
ordinary user cannot chown a file to root anyway, so there is no way to
prepare a correct tree for the root path.

The containerized build hit exactly that: `dm.pl` ran as container root, so it
needed an explicit `chown -R 0:0` to put the ownership back. Before that was
added it only *looked* right — under rootless podman the container's root maps
to the invoking host user, so a host-owned bind mount appeared root-owned
inside and `tar` recorded `0/root` by accident. Running natively removes the
whole class of problem.

Everything is `root:wheel` (0:0), which is correct for all of it — `/etc/apt`
sources and keyrings, the LaunchDaemon plist, root's own `.profile`, and
`/var/.blackb0x`. **launchd refuses to load a plist that is not root-owned or
is group/world-writable**, and that failure presents as "the daemon simply
never ran" with nothing pointing at permissions.

Verified by deliberately breaking three modes in a staging tree (`777` on an
apt source, `600` on the plist, `644` on `postinstall.sh`) and confirming the
built `.deb` carried `0644`/`0644`/`0755`, all `0/root`.

## Requirements

- Theos, for `bin/dm.pl`. `$THEOS` if set, else `~/theos`.
- `dpkg-scanpackages`, for the bundled local repo's `Packages` index —
  `brew install dpkg`. A hand-rolled index is not an option; apt is strict
  about the fields and checksums it expects there.

This used to run inside `ghcr.io/regulad/dotfiles:latest` under podman, which
existed only to supply a Linux box with Theos on it. Theos is macOS-native and
`dm.pl` is plain Perl, so on this project's only supported host the container
had nothing left to provide — and, per the section above, running natively is
also what makes `dm.pl` get ownership right without help.

## Package lists

- `packages.txt` — the flat set of package names blackb0x installs. This is
  the *request*; what apt can actually satisfy is the resolved closure.
- `local_only_debs.txt` — which of those ship from the bundled local repo at
  `/var/.blackb0x/local-debs` rather than a live one.

Both live here rather than in `misc/` because they describe what this
package installs. `misc/prebake_package_blacklist.txt` deliberately
does **not** move: it is baker policy about what may be unpacked at bake time,
not a statement about package content.

## The bundled local repository

`/var/.blackb0x/local-debs` is a real file-backed apt repo shipped inside the
package. Its contents are exactly `local_only_debs.txt`: every `.deb` that can
only ever come from a local repo, because no live repo carries a usable stanza
for it.

There is deliberately **no** filtering against the bake's picklist.
`build_deb_cache.py` adds every local-only filename to that picklist
unconditionally — it only checks the file exists, fatally (`:500-509`), then
unions them in at `:583` — so `picklist ∩ local_only_debs.txt` is always just
`local_only_debs.txt`. Filtering would be a guaranteed no-op that made this
script depend on bake state it otherwise does not need.

The `Packages` index is generated in the container by the real
`dpkg-scanpackages` — apt is strict about the fields and checksums it expects,
so a hand-rolled index is not an option. That index is unsigned, which is why
`postinstall.sh` installs with `--allow-unauthenticated`. (Note
`build_deb_cache.py`'s comment describes the source line as
`deb [trusted=yes] ...`; the real `local.list` has never carried that flag.)

### What is *not* in here

`net.tihmstar.etasonuntether` is applied statically by `BakeRamdisk.cpp`'s
`stageEtasonatv()`, not shipped through this repo: it is extracted from its
real `.deb`, **its `untether.bin` is replaced with this project's own**, and it
is registered with `Status: hold ok installed`. The hold exists because of the
override — `postinstall.sh` runs `apt-get upgrade`, which would otherwise
resolve the real package and silently clobber the replaced binary on first
boot. Shipping it here as well would have been a second install path competing
with the one that actually matters.

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
