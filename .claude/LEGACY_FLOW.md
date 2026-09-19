# LEGACY_FLOW.md — what the current ramdisk/launchd/setup.sh actually do

Snapshot of the *existing* install-time behavior, as reverse-engineered and
reimplemented in `entrypoint/entrypoint.c`, before the NEO_FLOW rewrite (see
`.claude/NEO_FLOW.md`). This is a record of what ships today, not a design
document — kept so the rewrite has a precise baseline to diverge from
deliberately, rather than by accident.

## Stage 0 — `rc.boot` (real ramdisk boot, before our own code runs)

`Blackb0x/Misc/rc.boot` is spliced (content-only, permissions preserved) into
the *pristine Apple restore ramdisk's own* `/etc/rc.boot` — the actual
first thing that executes in the restore ramdisk environment. It:

1. `mount /` — remounts root read-write (the pristine ramdisk boots read-only).
2. Deletes everything under `/usr/local/standalone/firmware/*` and
   `/usr/standalone/firmware/*` — frees cache space so there's room to work.
3. Renames `/sbin/reboot` to `/sbin/reboot_bak` — disables the real `reboot`
   binary (our own `entry()`/`sys_reboot()` uses the raw syscall directly, not
   this binary, so this is purely defensive — presumably to stop some other
   part of the restore-ramdisk environment from rebooting prematurely).
4. `nvram auto-boot=1` — makes sure the device actually continues booting on
   its own instead of sitting in DFU/recovery.
5. Chains into the ramdisk's own real boot sequence: `restored_external`,
   `restored_update`, `restored`, `ramrod` — i.e. rc.boot's replacement work is
   additive, not a full replacement of Apple's own ramdisk boot chain.

Our own PID-1 replacement (`sbin/launchd`, stage 1 below) then runs somewhere
inside/after this chain — the pristine binary that would have been the real
`launchd` is what our stub is spliced in place of.

## Stage 1 — `sbin/launchd` stub (`entrypoint/entrypoint.c`'s `entry()`)

Not real launchd. A freestanding (`-ffreestanding -nostdlib`, raw syscalls
only — no libSystem, no dyld available yet) Mach-O that Apple's own
`LC_UNIXTHREAD` mechanism jumps straight into as PID 1. Sequence, exactly as
implemented:

1. Opens `/dev/console`, dup2's it onto fd 1/2 (stdout/stderr).
2. Busy-waits (a literal CPU spin, no real sleep syscall exists in this
   binary) on `stat("/dev/disk0s1s1")` succeeding — i.e. waits for the
   device's real internal disk to be visible to the kernel.
3. Mounts the two real HFS+ partitions read-write:
   - `/dev/disk0s1s1` → `/mnt` (the device's real root filesystem)
   - `/dev/disk0s1s2` → `/mnt/private/var` (the device's real `/var`)

   Both via the same `mount("hfs", ..., 0, argsStruct)` convention — the
   device path is NOT a normal `mount(2)` argument on Darwin; it's word 0 of
   an 11-word args struct passed as the 4th (`data`) parameter.
4. Mounts `devfs` at `/mnt/dev` (a synthetic filesystem, no device path
   needed — `data` is genuinely `NULL` here, unlike the two calls above).
5. Runs `do_install()` (stage 2, below) — everything is installed against
   `/mnt/...` paths, i.e. directly onto the real device filesystem, not some
   ramdisk-local staging area.
6. Unmounts everything, `sync()`s, and calls `reboot(1)` directly — the device
   boots into its real OS next, no `execve` of anything, no chaining into a
   real `launchd` at the end of this process. This stub's job ends at reboot.

## Stage 2 — `do_install()`: what actually lands on the real device

Everything below only runs if `/Applications/AppleTV.app/AppleTV` exists (a
crude "is this actually an AppleTV" check) and `SystemVersion.plist`'s
`ProductVersion` can be read.

**Every run** (even if `/var/mobile/Media/.blackb0x` already exists, i.e. a
repeat install):
- Drops a `.blackb0x` marker file, a `.profile`, and (critically) copies
  `files/setup.sh` → `/private/etc/setup.sh` — this is staged for LATER
  execution, not run here. It's wired to run on every subsequent *real OS*
  boot via the `com.blackb0x.postinstall.plist` LaunchDaemon
  (`RunAtLoad = true`, runs `/bin/bash /etc/setup.sh` as root, every time the
  device boots into its real OS — not a one-shot).
- Installs a small set of standalone SSH/shell binaries directly by path
  (`bash`, `sh`, `ls`, `sshd`, `scp`, a handful of dylibs, `sftp-server`,
  `mount.sh`, `mktar.sh`, `device_infos`). **Corrected**: an earlier version
  of this doc claimed these "come from the ramdisk's own `/bin`, `/sbin`,
  `/usr/bin`, `/usr/lib` etc." — never verified, and false (see
  `.claude/NEO_FLOW.md`'s own retraction: a real, freshly-decrypted stock
  AppleTV2,1 10B809 `RestoreRamdisk` has none of this). The real source was
  identified as `msftguy/ssh-rd`'s `java/gui/sshtar/` resource tree, a
  byte-for-byte match for this entire set — but that tool turns out to be
  a ramdisk-stage rescue SSH shell, a different capability than anything
  NEO_FLOW needs (it doesn't run a shell at all, just copies files and
  reboots), so `ssh-rd` was removed from the repo entirely rather than
  wired in — see `Blackb0x/Misc/README.md`.

**Only on first install** (`/var/mobile/Media/.blackb0x` doesn't exist yet):
- Creates ~80 base Cydia directories (`/private/etc/apt`, `/var/lib/dpkg`,
  `/usr/lib/apt`, etc.) — all created with a confirmed pre-existing bug in the
  original binary (`MYSTERY_DIR_MODE`, mode argument is never actually read;
  every directory gets a nonsense literal mode instead). Preserved verbatim
  in `entrypoint.c` for fidelity, not yet decided whether NEO_FLOW keeps it.
- Branches by `ProductVersion` to pick exactly one persistence mechanism:
  - **8.4.x** → `install_etasonatv()`: copies `daemonload`/
    `orphan_commander`, then symlinks Apple's own signed `jsc` over
    `/usr/libexec/rtbuddyd` and symlinks `/untether/expl.js` to
    `/--early-boot`. `rtbuddyd --early-boot` is a real daemon launchd starts
    every boot; `jsc` treats its argv as a script path, so this makes launchd
    unknowingly re-run the WebKit/JSC exploit fresh on every real-OS boot.
    This is the only branch with a genuine untether.
  - **7.x / 8.x (non-8.4)** → `install_ios7_dirhelper()`: copies the
    ramdisk's own `dirhelper` over `/usr/libexec/dirhelper` and nothing else.
    Explicitly logged as `"Installing iOS 7 tether"` — genuinely tethered,
    no untether exists for this range at all.
  - **6.1.4** → `install_p0sixspwn()`: stages `p0sixspwn`'s own untether
    payload (`_.dylib`, `untether` binary, a `launchd.conf` using
    `DYLD_INSERT_LIBRARIES`, dpkg control files) under `/private/var/untether`
    and `/private/etc/launchd.conf`.
  - Anything else → logs `"unsupported version"` and aborts the rest of
    `do_install()`.
  - Installs the matching `com.openssh.sshd.plist` /
    `com.blackb0x.postinstall.plist` LaunchDaemons for that branch, plus
    `ldid`/`otool`/`plutil`.
- Installs repo list files directly (`joshtv.list`, `pubkey.key`, `nito.png`)
  and, first-install only, `xbmc.list` + `kodi.png` + the small hardcoded set
  of `.deb`s that setup.sh later moves into apt's cache and `dpkg -i`'s.
- **The bulk of the payload**: `clone_directory()`/`install_file()` calls that
  copy a large pre-extracted Cydia filesystem tree
  (`files/cydia/bin`, `sbin`, `usr/bin`, `usr/libexec`, `usr/sbin`,
  `usr/include`, `usr/lib` incl. `apt`/`dpkg` method dirs, `usr/share/*`,
  `private/etc/*`, `private/var/lib/*`, SSH host keys, etc.) directly onto
  `/mnt/...` — i.e. onto the real device filesystem. This is content that
  originally came from real Cydia `.deb` packages, already unpacked, with
  dpkg `.list` manifests still present under `dpkg/info/` as the only
  remaining evidence of which package each file came from (see
  `Blackb0x/Misc/README.md`) — but the packages themselves were never
  reassembled or installed through `dpkg` at ramdisk-install time. It's a
  flat filesystem overlay, not a package install.
- Rewrites ~100 compatibility symlinks (`bash`→`/bin/sh`, various `.dylib`
  version-suffix aliases, terminfo aliases, etc.) via `unlink()` +
  `symlink()`.

## Stage 3 — `setup.sh` (runs on every real-OS boot, not once)

Wired via `com.blackb0x.postinstall.plist` (`RunAtLoad = true`, root) to run
on **every** real-OS boot, indefinitely — not gated on "already ran once"
except for the `/var/stash` check below. Requires network access:

1. Runs Cydia's own `firmware.sh` to refresh its device-info cache.
2. Removes a few third-party repo list files, installs `regulad.gpg.key` +
   `regulad.list` as the default repo.
3. `apt-get update` — **live network fetch**, every single boot.
4. First run only (`/var/stash` doesn't exist yet): on `AppleTV2,1`, patches
   the `AppleTV.app` binary's entitlements (strips `seatbelt-profiles` via
   `ldid -e`/`sed`/`ldid -S`); "stashes" `/Applications`, `/usr/include`,
   `/usr/share` into `/var/stash/` and replaces them with symlinks back (the
   standard iOS jailbreak "stash" trick so Cydia can manage these paths).
5. Moves a small hardcoded set of `.deb`s (rtadvd, sqlite3-dylib,
   sqlite3-lib, patcyh, ldid, uikittools, beigelist, updatebegone) into
   apt's own cache dir and `dpkg -i`'s each directly, then
   `apt-get install -y mobilesubstrate` — **live network fetch** — then
   `dpkg -i`'s updatebegone, then `apt-get upgrade -y` — **live network
   fetch, unconditional system upgrade, every boot**.
6. `apt-get -y --force-yes install org.xbmc.kodi-atv2` (first run only, if
   Kodi isn't already present) and `apt-get -y install com.saurik.afc2d`
   (every run) — both live network fetches.
7. `apt-get install -f -y` to paper over anything left half-configured.
8. Drops `/private/var/mobile/.blackb0x_installed` at the end.

## Why this is the baseline being replaced

Two structural problems NEO_FLOW exists to fix, both confirmed by the above:

- **Network dependency at boot, forever.** `setup.sh` isn't a one-shot
  installer — it's a LaunchDaemon that hits the network on literally every
  boot of the device (`apt-get update`/`upgrade`/`install` four separate
  times), for the lifetime of the device. A device with no network, a
  changed/dead repo URL, or a hostile network all silently degrade or hang
  boot-time behavior.
- **The base Cydia filesystem is a flat, unattributed overlay, not a real
  package install.** `files/cydia/` is pre-extracted `.deb` content with no
  `dpkg` database entries created for any of it at install time — `dpkg`
  itself doesn't know these packages exist even though their files are
  present. `debcache/` is a separate, actually-`dpkg`-installed set that
  only covers a handful of packages `setup.sh` explicitly `dpkg -i`'s.
