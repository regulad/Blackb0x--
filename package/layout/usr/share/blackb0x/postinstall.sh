#!/bin/bash

# The legacy behavior this file used to implement is fully documented in
# the legacy tether flow — nothing here needs to preserve it anymore. The
# target design is AGENTS.md's "Install-time design".
#
# Run directly by xyz.regulad.blackb0x's LaunchDaemon plist as
# `/bin/bash /usr/share/blackb0x/postinstall.sh` — bash, not sh. `set -ex`
# below is the only logging this script does itself: the plist's own
# StandardOutPath/StandardErrorPath redirect this whole script's stdout and
# stderr (every traced command plus every command's own output) straight to
# /usr/share/blackb0x/postinstall.out.log and postinstall.err.log
# respectively — two separate files, not one shared between both, since
# launchd has a real, long-documented bug where pointing both keys at the
# same path can silently drop or interleave one stream. No need to
# hand-echo progress messages to a log file line by line either way.
set -ex

# --- stage /var into place ---------------------------------------------------
#
# WHY THIS EXISTS AT ALL. The restore ramdisk that installs blackb0x cannot
# create a regular file on the data partition. open() with O_CREAT returns
# EPERM there even as root, while mkdir() on the same volume succeeds — the
# signature of iOS content protection, where a new regular file needs a
# per-file key wrapped by a class key from a keybag, and nothing on a restore
# ramdisk ever loads one (/usr/libexec/keybagd is declared in launchd's
# embedded bootstrap but is not even present on the image). Measured on real
# hardware: 130 entries failed in the ramdisk's merge, every one of them bound
# for /var, and every entry bound for the system partition succeeded.
#
# So everything destined for /var — dpkg's database, apt's lists and archives,
# this project's own state — is staged onto the SYSTEM partition at
# /usr/share/blackb0x/var, and moving it into place is the first thing that
# happens on the first fully booted run, where the keybag is loaded and /var
# behaves normally.
#
# THE STAGE IS THE "NOT YET MIGRATED" MARKER. Its existence means the move has
# not happened; its absence means it has. That is deliberate and it is
# strictly better than a flag file, because the marker and the thing it
# describes are the same object — there is no state in which one is right and
# the other is wrong. install-done still gates the apt install below, which is
# a different question: "has the network install finished" rather than "has
# /var been populated". A device can legitimately be past the first and not
# the second if a boot was interrupted.
#
# ORDER MATTERS. This runs before the install-done check, not after, because
# install-done LIVES in /var/.blackb0x — on a device where the stage has not
# been moved yet, that path does not exist to be read.
#
# cp -a, not mv: the two live on different filesystems (/usr is the system
# partition, /var is the data partition), so a rename is impossible and a
# copy is what mv would do anyway, except that mv would leave us unable to
# distinguish "copied" from "half-copied". The stage is removed only after
# cp reports success, under `set -e`, so an interrupted copy leaves the stage
# in place and the next boot does the whole thing again.
#
# PARTIALLY-APPLIED DEVICES ARE REAL. The development device is in exactly
# that state: system partition written by an earlier ramdisk run, /var never
# populated. `cp -a` of the stage's CONTENTS over an existing /var merges
# rather than replaces, which is what that device needs and what a device with
# a normally-populated /var also needs.
if [ -d /usr/share/blackb0x/var ]; then
	cp -a /usr/share/blackb0x/var/. /var/
	rm -rf /usr/share/blackb0x/var
fi

# One-shot: /var/.blackb0x/install-done is written at the very end
# of a successful run, with a timestamp. Its presence means the real,
# apt-driven install already completed — not just that this LaunchDaemon
# fired (it runs on every boot, RunAtLoad), so skip straight out rather
# than re-running the whole install pass for nothing.
if [ -f /var/.blackb0x/install-done ]; then
	exit 0
fi

# This LaunchDaemon is RunAtLoad, which fires well before WiFi/Ethernet has
# necessarily associated and gotten an address — there's no portable launchd
# knob on this old launchd implementation to delay a RunAtLoad job by a fixed
# interval after boot (StartInterval is a *repeat* period, not a one-time
# start delay), so a plain, naive wait here is the simplest thing that
# actually works: at least 60 real seconds since boot before touching
# apt-get. `sleep` itself is only typically available at this point, not
# guaranteed — before installing coreutils here, Apple doesn't ship things
# like a shell in a retail build (real evidence, not just assumption:
# debcache/dpkg_*.deb's own control file lists `bash` as one of dpkg's
# own Depends:, meaning this jailbreak ecosystem itself treats bash as
# something it has to supply, not something already on the stock OS), so
# there's no reason to trust a bare `sleep` any more than a bare `bash`
# would be. `coreutils-bin` has no Depends: of its own at all (confirmed
# from its own control file) and already has a cached copy waiting in apt's
# own archive directory (see BakeRamdisk.cpp's stageDebcache() — it's
# already in package/packages.txt), so installing it directly via
# dpkg — no apt-get, no network, no repo metadata needed at all — is enough
# to make a real `sleep` available before doing anything else. In practice
# this branch is usually moot: coreutils-bin has no postinst at all (see
# misc/prebake_package_blacklist.txt — it's not on it), so
# bakeRamdisk() typically tree-merges it directly and marks it installed at
# bake time (see BakeRamdisk.cpp's stagePreinstalledPackages()), meaning a
# real /bin/sleep is usually already sitting on disk before this script
# ever runs. This is the fallback for whenever that didn't happen — check
# the cache actually has the .deb before trying, since stagePreinstalledPackages()
# deliberately does NOT also copy a package's .deb into the cache once it's
# already been installed that way (see that function's own comment).
if ! command -v sleep >/dev/null 2>&1; then
	COREUTILS_BIN_DEB=$(ls /var/cache/apt/archives/coreutils-bin_*.deb 2>/dev/null | head -n1)
	if [ -n "$COREUTILS_BIN_DEB" ]; then
		dpkg -i "$COREUTILS_BIN_DEB"
	else
		echo "sleep not available, there will almost certainly be no internet connection!" >&2 || true
	fi
fi
command -v sleep >/dev/null 2>&1 && sleep 60 || true

# Real, unattended installs, no controlling terminal at all (this runs as a
# LaunchDaemon) — debconf (if anything in this old package set even uses
# it) needs to be told not to try prompting, same reasoning `-y
# --allow-unauthenticated` already covers for apt's own confirmation
# prompts below.
export DEBIAN_FRONTEND=noninteractive

# --- apt install ------------------------------------------------------------
#
# A fully offline install turned out to be impossible in practice — Kodi
# and a few other real packages are simply too big to fit in this old
# A4-era restore-ramdisk's 70MB budget, so they're deliberately never
# staged locally at all (see BakeRamdisk.cpp's kNeverStageDebs). Network is
# used opportunistically instead of being disabled outright:
#   - The real regulad/saurik/awkwardtv/xbmc sources.list.d entries are
#     always present (staged by bakeRamdisk() like everything else under
#     /blackb0x) — `apt-get update` below is free to actually reach them
#     over the network if it can.
#   - If it can't, apt knows NOTHING about those repos. The lists cache at
#     /private/var/lib/apt/lists/ is staged EMPTY on purpose (see
#     scripts/build_deb_cache_apt.py's own docstring): the lists generated
#     at bake time describe the synthetic file:// debcache repo, not the
#     real ones, so shipping them would actively mislead apt rather than
#     help it. The consequence is real and has to be designed around --
#     with no network, an `apt-get install` of anything not already staged
#     below has no candidate at all, and under this script's bare `set -ex`
#     that aborts the whole postinstall, so `install-done` is never written
#     and the device shows "nothing happened" even after a clean boot.
#   - Whatever .deb bytes bakeRamdisk() DID have room to stage sit directly
#     in apt's own real cache directory (/private/var/cache/apt/archives/)
#     — apt finds them itself via its normal cache-before-download check,
#     no local file:// source or synthetic Packages index needed.
#
# On this old on-device apt (apt7 0.7.25.3 — confirmed directly by
# extracting debcache/apt7_*.deb and reading its real apt-get binary's
# own strings): only apt-get exists, there's no unified `apt` command at
# all (that CLI wasn't introduced until APT 1.0/1.1, years after this
# build), and `full-upgrade` doesn't exist either (also a later addition to
# apt-get itself). `dist-upgrade` genuinely is real here too — the same
# binary's own strings confirm it (`apt-get(8)`'s embedded help text lists
# it directly) — but plain `upgrade` is used below regardless: it only
# upgrades packages already installed, never installs anything new or
# removes anything, so it can't itself demand more from the network than
# what's already staged the way `dist-upgrade`'s more complex dependency
# handling sometimes can.
#
# `apt-get update` is the one and only command in this script allowed to
# fail without stopping it — starting out with no network reachable at all
# is a normal, expected state (see above), not an error. Every step after
# this one is a real part of the install itself: under `set -e`, any of
# them failing stops the script right here, `install-done` never gets
# written, and — since this LaunchDaemon is RunAtLoad — the next boot just
# tries the whole thing again from scratch. That's deliberate: a device
# isn't done installing just because it got partway, and a device that
# only had network partway through gets another real chance at finishing
# on its next boot instead of this script quietly calling a degraded
# install "done" forever.
apt-get update || true

# cydia itself goes first, on its own — its postinst does its own
# first-run setup (including its own /var/stash relocation of
# /Applications, /usr/include, /usr/share, with integrity checks on that
# state; confirmed directly from the real postinst binary's own strings —
# see AGENTS.md's "Install-time design"). Nothing else should assume Cydia's own
# environment exists until that's actually finished.
apt-get install -y --allow-unauthenticated cydia

# Baked in at bake time (see BakeRamdisk.cpp's stagePostinstallScript()),
# from the exact package-name set scripts/build_deb_cache.py actually
# resolved through real apt for this build — not hand-copied from
# package/packages.txt, which still carries a few leaked-in
# non-package names (bigboss/modmyifone/saurik/zodttd — see
# misc/README.md) that would abort this whole `apt-get install`
# line outright if ever passed to it directly. Letting apt-get resolve
# each of these names' own dependencies itself (rather than also listing
# the full transitive closure here) is the same thing apt would do on any
# real system.
PACKAGES=(__BLACKB0X_PACKAGES__)

apt-get install -y --allow-unauthenticated "${PACKAGES[@]}"
apt-get install -f -y --allow-unauthenticated

apt-get upgrade -y --allow-unauthenticated
apt-get autoremove -y --allow-unauthenticated

# Neither Kodi's nor nitoTV's frontrow icon needs any help from us.
# Kodi's postinst copies its own AppIcon.png to
# com.apple.frontrow.appliance.kodi@720p.png itself; nitoTV ships its icon
# as a plain file directly in its own package payload, already at
# /Applications/AppleTV.app/com.nito.frontrow.appliance.nitoTV@720.png —
# both confirmed by reading the real .debs directly. The frontrow icon
# naming convention turned out to be `<the appliance's own
# CFBundleIdentifier>@<size>.png` (from each .frappliance's own Info.plist),
# not a fixed prefix/suffix guessable by pattern-matching one app's
# convention onto another's, which is exactly the mistake an earlier pass
# here made for nitoTV. Nothing to do here at all.

# /var/.blackb0x exists by now: the stage-into-place step at the top of this
# script created it, along with everything else that was bound for /var. It is
# no longer entrypoint.c that creates it — the ramdisk cannot create files on
# that volume at all, which is the whole reason the stage exists. mkdir -p
# anyway, because this one line is the difference between a device that knows
# it finished and a device that reinstalls itself on every boot, and it costs
# nothing to not depend on a directory another step was supposed to leave.
mkdir -p /var/.blackb0x
date -u "+%Y-%m-%dT%H:%M:%SZ" > /var/.blackb0x/install-done
