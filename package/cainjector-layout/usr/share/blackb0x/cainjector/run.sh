#!/bin/bash
# Run-once wrapper for cainjector, started by
# /Library/LaunchDaemons/xyz.regulad.blackb0x.cainjector.plist (see that file
# for why the unit lives there and not in /System/Library/LaunchDaemons).
#
# `set -ex` is the only logging this script does: the plist redirects this
# whole script's stdout and stderr to two separate files under
# /usr/share/blackb0x/cainjector/, two files rather than one because launchd
# has a real, long-documented bug where pointing both keys at the same path
# can drop or interleave a stream.
set -ex

MARKER=/var/.blackb0x/cainjector-done
CERTS=/usr/share/blackb0x/cainjector/certs

# THE MARKER LIVES IN /var AND THAT IS DELIBERATE, matching install-done's own
# reasoning in postinstall.sh. By the time this unit can run at all, the
# untether has already patched the kernel and the device is fully booted with
# a loaded keybag, so /var is an ordinary writable filesystem here -- unlike
# at ramdisk time, where nothing can create a regular file on it. A marker on
# the system partition would work too, but /var/.blackb0x is where this
# project already keeps exactly this kind of state.
if [ -f "$MARKER" ]; then
	exit 0
fi

# cainjector itself is idempotent -- it re-verifies every certificate with
# SecTrustStoreContains whether or not it just installed it -- so a re-run
# after an interrupted boot costs nothing but time. The marker exists to keep
# 121 trust-store writes off every subsequent boot, not to protect
# correctness.
#
# No `|| true`: a nonzero exit means some certificates are NOT verifiably in
# the trust store, and the right response is to leave the marker unwritten and
# try again on the next boot rather than record a half-installed store as
# done. Under `set -e` that happens by itself.
/usr/libexec/blackb0x/cainjector "$CERTS"

mkdir -p /var/.blackb0x
date -u "+%Y-%m-%dT%H:%M:%SZ" > "$MARKER"
