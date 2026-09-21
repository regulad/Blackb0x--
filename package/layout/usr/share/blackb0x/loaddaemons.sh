#!/bin/sh
#
# Load /Library/LaunchDaemons and run /etc/rc.d, because this platform's
# launchd does not.
#
# WHAT THIS IS STANDING IN FOR. Read off the real decrypted 12H1006 root
# filesystem: /Library/LaunchDaemons DOES NOT EXIST on a stock Apple TV,
# /System/Library/LaunchDaemons holds 174 plists, and the device's own
# /sbin/launchd contains the string /System/Library/LaunchDaemons and contains
# /Library/LaunchDaemons nowhere. /etc/rc.d does not exist either, and neither
# launchd, launchctl nor xpcproxy references it.
#
# Both directories are jailbreak conventions borrowed from iOS proper, where
# launchd really does scan /Library/LaunchDaemons. On this build nothing does
# -- except the untether, which carries this, in plain text, gated behind
# getting tfp0:
#
#     echo 'really jailbroken';
#     ls /Library/LaunchDaemons | while read a; do launchctl load /Library/LaunchDaemons/$a; done;
#     ls /etc/rc.d | while read a; do /etc/rc.d/$a; done;
#
# So every package that ships a daemon to /Library/LaunchDaemons -- cydia,
# openssh, com.firecore.freemem-watcher, com.nito.tssagent,
# org.tihmstar.fuzzyparrot, five of the seven in debcache/ -- works only on a
# boot where the exploit succeeded. On a TETHER boot the patched kernel
# supplies the same privileges directly, the untether is never triggered, and
# none of those daemons ever start. That is why sshd never came up.
#
# This script is that loop, reached from a unit in the directory launchd DOES
# scan, so it runs on every boot. It fixes the whole class rather than one
# daemon: nothing here is about sshd specifically.
#
# IDEMPOTENT BY CONSTRUCTION, which matters because on an untethered boot the
# untether will do all of this again (and /etc/rc.d/daemonload, staged by
# stageEtasonatv(), is itself another `launchctl load /Library/LaunchDaemons`
# wrapped in orphan_commander -- so it can happen three times). `launchctl
# load` of an already-loaded job fails and changes nothing; that failure is
# expected here, not a problem, which is why every command swallows its
# status.
#
# NOT `set -e`. A single unloadable plist or a failing rc.d script must not
# stop the rest from loading -- partial success is the whole point of running
# a directory.
set -u

# -w, NOT a bare load. This launchctl is the launchd-2.0 one (its subcommands
# include bootstrap, enable, kickstart and print-disabled), and its own help
# text for -w says:
#
#   "If the service is disabled, it will be enabled. In previous versions of
#    launchd, being disabled meant that a service was not loaded. Now,
#    services are always loaded. If a service is disabled, launchd does not
#    advertise its service endpoints (sockets, Mach ports, etc.)"
#
# So on this generation a disabled service LOADS SILENTLY AND NEVER BINDS ITS
# SOCKET. A bare `launchctl load` would report success and openssh -- which is
# socket-activated on port 22 -- would still never accept a connection, with
# nothing anywhere saying why. -w clears that state as well as loading.
#
# The untether's own loop uses a bare load, which is why this is worth writing
# down rather than matching it exactly: it is the one place this script
# deliberately differs, and it differs in the safer direction.
LD=/Library/LaunchDaemons
if [ -d "$LD" ]; then
    for plist in "$LD"/*; do
        [ -f "$plist" ] || continue
        echo "loaddaemons: launchctl load -w $plist"
        launchctl load -w "$plist" 2>&1 || true
    done
else
    echo "loaddaemons: $LD absent, nothing to load"
fi

# NOT kickstarted. Whether a job needs a nudge after loading depends entirely
# on what kind of job it is, and getting this wrong is worse than doing
# nothing:
#
#   RunAtLoad jobs   start on load. A kickstart is redundant.
#   socket jobs      bind on load and start when a connection arrives. Forcing
#                    one to start means running `sshd -i` with no accepted
#                    connection on stdin, which exits immediately and, under
#                    inetdCompatibility, invites launchd to throttle-respawn a
#                    job that was working correctly.
#
# openssh is the second kind, so the correct action after loading it is
# nothing at all.
echo "loaddaemons: services of interest after load:"
launchctl list 2>&1 | grep -iE "openssh|blackb0x|cydia|tihmstar|firecore|nito" || \
    echo "  (none matched -- launchctl list returned nothing for any of them)"

# /etc/rc.d after the plists, matching the untether's own order. Note these
# run SYNCHRONOUSLY and one of them, tihmstar's daemonload, is
# `orphan_commander -w syslogd '...'` -- it waits for syslogd before doing its
# work, so this script can sit here a while on an early boot. That is
# acceptable: this job is RunAtLoad with no KeepAlive and nothing waits on it.
RCD=/etc/rc.d
if [ -d "$RCD" ]; then
    for script in "$RCD"/*; do
        [ -x "$script" ] || continue
        echo "loaddaemons: running $script"
        "$script" 2>&1 || true
    done
else
    echo "loaddaemons: $RCD absent, nothing to run"
fi

echo "loaddaemons: done"
exit 0
