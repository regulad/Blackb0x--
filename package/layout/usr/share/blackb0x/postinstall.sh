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

# cydia itself goes first, on its own — its postinst is meant to do its own
# first-run /var/stash relocation of /Applications (a real, compiled Mach-O
# binary, not a script; confirmed directly from its own strings — see
# AGENTS.md's "Install-time design"). Nothing else should assume Cydia's own
# environment exists until that's actually finished.
#
# --force-overwrite is required here specifically. This project's own layout
# (see BakeRamdisk.cpp's tree merge) lays several of the same paths Cydia's
# package payload ships directly onto the system partition — most notably
# etc/apt/sources.list.d/saurik.list, which we stage ourselves so a source
# list exists even if apt-get install cydia never gets the chance to run.
# dpkg tracks file ownership per package, and those files were never
# written by dpkg unpacking anything, so dpkg sees an unowned file already
# sitting where cydia's payload wants to unpack its own copy and refuses by
# default ("trying to overwrite ... which is different from other instances
# of package cydia"). --force-overwrite tells dpkg to take the file anyway,
# which is exactly right here: cydia's own copy is the one that should win.
DPKG_FORCE_OVERWRITE=(-o 'Dpkg::Options::=--force-overwrite')
apt-get install -y --allow-unauthenticated "${DPKG_FORCE_OVERWRITE[@]}" cydia

# --- stash /Applications if cydia's own postinst didn't --------------------
#
# It doesn't, reliably, on real hardware. Confirmed directly: running
# /var/lib/dpkg/info/cydia.postinst configure by hand on a real device exits
# without ever creating /var/stash. Cydia's real postinst is a compiled
# Mach-O binary (not a script), so the exact reason it no-ops isn't visible
# without disassembling it — but /usr/libexec/cydia/move.sh, which cydia's
# own payload also ships and which pam/pam-modules already call successfully
# on their own paths (Depends: on cydia being unpacked first, same as here),
# has the identical shape of gate in its own shift_() function: it only
# stashes a real, non-symlinked directory if `du`'s used size plus a 512KB
# margin comes out under `df`'s free-space reading for /var, and simply does
# nothing at all if that arithmetic doesn't come out as expected — no error,
# no message. Cydia's postinst likely has the same kind of environment
# assumption somewhere in it, silently unmet on this device.
#
# Rather than reverse-engineer which check is failing inside a compiled
# binary, just call the same real, already-shipped move.sh helper ourselves
# — the identical mechanism pam/pam-modules already rely on, not a
# reimplementation of its logic. Guarded exactly the way move.sh's own
# shift_() guards itself: only if /Applications is a real directory and not
# already a symlink, so this is a no-op on any device where cydia's postinst
# already did its job correctly.
if [ -d /Applications ] && [ ! -L /Applications ]; then
	/usr/libexec/cydia/move.sh /Applications
fi

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

# Same --force-overwrite reasoning as the cydia install above applies to
# every apt-get invocation that unpacks a payload: any of these packages
# (saurik's, bigboss's, awkwardtv's own bootstrap packages included) could
# just as easily ship one of the same sources.list.d/trusted.gpg.d paths we
# already staged directly onto the system partition at bake time.
apt-get install -y --allow-unauthenticated "${DPKG_FORCE_OVERWRITE[@]}" "${PACKAGES[@]}"
apt-get install -f -y --allow-unauthenticated "${DPKG_FORCE_OVERWRITE[@]}"

# --- direct dpkg -i fallback for archive-only packages -----------------------
#
# apt.awkwardtv.org force-redirects plain http:// to https:// (302), and apt
# genuinely follows redirects (Acquire::http::AllowRedirect, on by default —
# verified in the bundled apt7-lib method binary's own strings, which also
# carries the real "Redirection loop" guard from apt's method/http.cc), so
# awkwardtv.list has to stay https:// — there is no working plain-HTTP path
# to it at all, unlike bigboss/saurik/xbmc/regulad.
#
# https:// itself is not obviously broken here the way an earlier pass at
# this comment assumed: /usr/lib/apt/methods/https is a symlink to the same
# http method binary, but that binary is built on CFNetwork
# (CFReadStreamCreateForHTTPRequest, imports kCFStreamErrorDomainSSL for
# real handshake-error reporting — confirmed directly from its own undefined
# symbol table) rather than linking libssl/curl at all, so it dispatches on
# the URI scheme it's handed at runtime (via the method protocol's own "URI
# Acquire" message, not argv[0]) and does a genuine TLS handshake through
# Apple's own SecureTransport for an https:// request — a completely
# separate TLS stack from the openssl_0.9.8zg userland package, which this
# method never touches. Whatever is actually failing against
# apt.awkwardtv.org on real hardware (observed directly: apt-get update
# against it reliably reports SSL errors) is therefore most likely this
# OS-era device's old root CA trust store not trusting whatever CA issued
# that host's current certificate, not a protocol-version ceiling — see
# docs/HISTORY.md for the full corrected writeup, including where the
# earlier (wrong) "https is fake" theory came from.
#
# Whatever the exact cause, it isn't ours to fix: apt.awkwardtv.org is
# third-party infrastructure. So apt-get update against it stays a
# permanent, tolerated failure here, and anything ONLY described by that
# repo's own Packages index — nitoTV chief among them — can never be
# resolved by name through the apt-get install above, no matter how long
# the device waits for a network.
#
# Its .deb bytes are staged into the real apt archive cache regardless of
# which repo they were originally resolved from (see BakeRamdisk.cpp's
# stageDebcache() — build time runs on a machine with working TLS/trust, so
# resolution there never hits this wall). So skip apt's by-name resolution
# entirely for whatever apt itself didn't already claim, and hand dpkg the
# bytes directly instead — the same real-file-not-real-index situation
# essential's own local_only_debs.txt entry exists to solve, just
# generalized instead of hand-listed, so anything else that ever ends up
# archive-only and index-less gets picked up here too, not just nitoTV.
TO_DPKG_I=()
for deb in /var/cache/apt/archives/*.deb; do
	[ -e "$deb" ] || continue
	pkg=$(dpkg-deb -f "$deb" Package)
	if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q '^install ok installed$'; then
		TO_DPKG_I+=("$deb")
	fi
done
if [ "${#TO_DPKG_I[@]}" -gt 0 ]; then
	dpkg -i --force-overwrite "${TO_DPKG_I[@]}"
	apt-get install -f -y --allow-unauthenticated "${DPKG_FORCE_OVERWRITE[@]}"
fi

apt-get upgrade -y --allow-unauthenticated "${DPKG_FORCE_OVERWRITE[@]}"
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
