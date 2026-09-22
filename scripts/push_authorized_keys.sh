#!/usr/bin/env bash
#
# push_authorized_keys.sh — grant SSH access to a jailbroken Apple TV by
# writing your own ~/.ssh/authorized_keys onto it.
#
# blackb0x itself no longer does this (it used to, over a hand-rolled AFC2
# write in DeviceManager::pushAuthorizedKeys() — see docs/HISTORY.md/the
# TODO list for why that got pulled out into this script instead). Once the
# jailbreak finishes booting, Cydia's own openssh package is already running
# real sshd on the device, reachable over usbmuxd exactly like any other
# service blackb0x talks to — so this just forwards a local TCP port to the
# device's port 22 (via `iproxy`, already built as part of this project's
# libusbmuxd, no separate install) and then behaves like a normal
# `ssh-copy-id` against that port. No root/sudo needed: unlike blackb0x
# itself (which needs raw DFU-mode USB access), usbmuxd/iproxy run at your
# normal user privilege.
#
# Usage:
#   scripts/push_authorized_keys.sh [--udid UDID] [--keys-file PATH] [--port PORT]
#
# --udid       Target a specific device (see `idevicepair list`-style UDIDs).
#              Defaults to whichever single device usbmuxd currently sees —
#              fine as long as only one Apple TV is plugged in.
# --keys-file  Local file to push, in authorized_keys format. Defaults to
#              ~/.ssh/authorized_keys. Overwrites the device's copy wholesale
#              (matching the old AFC2 path's behavior) rather than appending.
# --port       Local TCP port to forward through iproxy. Defaults to a
#              randomly chosen high port each run.
#
# WHY root AND NOT mobile. Newer jailbreaks tell you to log in as `mobile`,
# and that advice does not transfer here. It comes from the ROOTLESS era
# (iOS 15+, Dopamine/palera1n rootless), where the real root filesystem stays
# sealed and everything lives under /var/jb. This device is a 2015-era
# ROOTFUL untether: there is no rootless split to respect, and every single
# thing this project asks of a shell — `dpkg -i`, editing /etc/rc.d,
# `launchctl load` of system LaunchDaemons, reading /var/mobile's caches — is
# root's work. Cydia's own sshd_config leaves `PermitRootLogin` at its
# default of yes, so root over SSH is the configuration as shipped, and
# /var/root/.profile is staged by the bake precisely because root is the
# account you land in.
#
# Logging in as mobile would also be a dead end rather than a mild
# inconvenience: there is no `sudo` anywhere in packages.txt, and the only
# `su` on the device (coreutils-bin's, setuid root) is gated by
# /etc/pam.d/su's `auth required pam_wheel.so use_uid group=admin group=wheel`
# — mobile is in neither group. So a mobile session could not escalate at
# all.
#
# The device's root password is Apple's own long-standing default for every
# iOS/tvOS device, "alpine" (not something this project or Cydia's openssh
# package sets), unless you've changed it — ssh will prompt for it
# interactively the first time, exactly like an ordinary ssh-copy-id run.
# `-F /dev/null` deliberately ignores your own ~/.ssh/config for this one
# connection: a `Host *`/pubkey-only entry there (IdentitiesOnly,
# PreferredAuthentications=publickey, etc.) would otherwise silently kill
# that password fallback before the device has any key to authenticate with.
# The host key for 127.0.0.1:<port> is intentionally never written to your
# real ~/.ssh/known_hosts either (a throwaway localhost port doesn't
# identify anything worth remembering across runs/devices). This 2014-era
# sshd also needs ssh-rsa explicitly re-enabled in TWO separate places (see
# ssh_opts below) — a modern ssh client refuses it by default both as a host
# key type and as a key type it will offer for authentication.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

udid=""
keys_file="${HOME}/.ssh/authorized_keys"
port=$(( (RANDOM % 20000) + 20000 ))

while [[ $# -gt 0 ]]; do
    case "$1" in
        --udid) udid="$2"; shift 2 ;;
        --keys-file) keys_file="$2"; shift 2 ;;
        --port) port="$2"; shift 2 ;;
        -h|--help)
            # Print the header block itself rather than a fixed line range,
            # which silently drifts out of date every time the header is
            # edited (it already had).
            awk 'NR > 1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ ! -s "$keys_file" ]]; then
    echo "push_authorized_keys.sh: $keys_file is missing or empty." >&2
    echo "Generate a keypair first (ssh-keygen) and put your public key there." >&2
    exit 1
fi

# Refuse to push a file the device's sshd cannot use. OpenSSH 6.7 knows
# ssh-rsa, ssh-dss, ecdsa-sha2-nistp* and ssh-ed25519 — nothing else. A file
# of only FIDO/security-key (sk-*) or post-quantum (mldsa) keys would push
# perfectly, report success, and then never authenticate, with the device
# falling back to the password prompt and no error anywhere saying why.
# Matched against the whole line rather than its first field because an
# authorized_keys entry may legitimately begin with an options list.
usable_keys=0
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    if [[ "$line" =~ (^|[[:space:]])(ssh-rsa|ssh-dss|ssh-ed25519|ecdsa-sha2-nistp(256|384|521))[[:space:]] ]]; then
        usable_keys=$((usable_keys + 1))
    fi
done < "$keys_file"
if [[ "$usable_keys" -eq 0 ]]; then
    echo "push_authorized_keys.sh: no key in $keys_file is of a type this device's" >&2
    echo "sshd (OpenSSH 6.7p1, 2014) supports — it understands only ssh-rsa," >&2
    echo "ssh-dss, ecdsa-sha2-nistp256/384/521 and ssh-ed25519. Pushing this file" >&2
    echo "would succeed and then silently never authenticate." >&2
    exit 1
fi

iproxy_bin="$REPO_ROOT/build/deps/bin/iproxy"
if [[ ! -x "$iproxy_bin" ]]; then
    if command -v iproxy >/dev/null 2>&1; then
        iproxy_bin="$(command -v iproxy)"
    else
        echo "push_authorized_keys.sh: no iproxy found at $iproxy_bin or on PATH." >&2
        echo "Build this project first (cmake --build build) — iproxy is built" >&2
        echo "as part of the vendored libusbmuxd." >&2
        exit 1
    fi
fi

iproxy_pid=""
cleanup() {
    if [[ -n "$iproxy_pid" ]]; then
        kill "$iproxy_pid" 2>/dev/null || true
        wait "$iproxy_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "Forwarding 127.0.0.1:$port -> device:22 via iproxy..."
"$iproxy_bin" "$port" 22 ${udid:+"$udid"} >/dev/null 2>&1 &
iproxy_pid=$!

# shellcheck disable=SC2054  # the commas below are inside a single -o VALUE, not array separators
ssh_opts=(
    -F /dev/null
    -p "$port"
    -o UserKnownHostsFile=/dev/null
    -o StrictHostKeyChecking=accept-new
    -o ConnectTimeout=5
    -o LogLevel=ERROR
    # This device's sshd is OpenSSH 6.7p1 (2014), which predates RFC 8332:
    # it cannot sign or verify with rsa-sha2-256/512, only with SHA-1
    # `ssh-rsa`. Modern clients disabled that by default in 8.8. It must be
    # re-enabled in BOTH lists, for two unrelated reasons:
    #
    #   HostKeyAlgorithms       — the device presents an RSA host key, and
    #                             without ssh-rsa here the transport never
    #                             comes up at all ("no matching host key type
    #                             found"), long before any password prompt.
    #   PubkeyAcceptedAlgorithms— an RSA key in the file we just pushed is
    #                             otherwise never even OFFERED on the next
    #                             login. OpenSSH 6.7 also predates the
    #                             `server-sig-algs` extension (7.2), so the
    #                             client cannot discover that SHA-1 is all the
    #                             server has, skips the key with
    #                             "no mutual signature algorithm", and falls
    #                             back to asking for the password — which
    #                             looks exactly like "the key didn't take".
    #
    # DO NOT put ssh-dss back here. It was in both lists and it broke this
    # script outright: OpenSSH 10 REMOVED DSA entirely, so `+ssh-dss` is not a
    # disabled algorithm that gets re-enabled, it is an unknown one, and the
    # client aborts on the option itself — `Bad key types '+ssh-dss'` — before
    # it opens a socket. Nothing is lost by dropping it: sshd_config lists
    # ssh_host_rsa_key alongside ssh_host_dsa_key and the bake stages an RSA
    # host key, so there is always an RSA host key to match, and a DSA *client*
    # key is not something a modern ssh-keygen can even produce.
    -o HostKeyAlgorithms=+ssh-rsa
    -o PubkeyAcceptedAlgorithms=+ssh-rsa
)

# Wait for the SSH BANNER, not merely for the port to accept a connection.
# iproxy binds the local port the moment it starts, whether or not there is a
# device behind it, so a bare connect test passes instantly even with nothing
# plugged in — and the run then fails several steps later with
# "kex_exchange_identification: Connection reset by peer", which reads like a
# protocol problem rather than "no device". A real sshd announces itself
# ("SSH-2.0-OpenSSH_6.7") as the first thing it sends, so requiring that line
# tests the thing we actually care about.
echo "Waiting for the device's sshd (127.0.0.1:$port)..."
attempts=30
banner=""
while :; do
    banner=$( { exec 3<>"/dev/tcp/127.0.0.1/$port"; IFS= read -r -t 5 line <&3 && printf '%s' "$line"; } 2>/dev/null ) || banner=""
    [[ "$banner" == SSH-* ]] && break
    attempts=$((attempts - 1))
    if [[ $attempts -le 0 ]]; then
        echo "push_authorized_keys.sh: nothing is answering SSH on the device." >&2
        echo "iproxy is listening, so this is the device side: check that the Apple TV" >&2
        echo "is plugged in (USB *and* power) and that the jailbreak has finished" >&2
        echo "booting — sshd only comes up once openssh's LaunchDaemon is loaded." >&2
        exit 1
    fi
    sleep 2
done
echo "Found ${banner%$'\r'}"

echo "Pushing $keys_file (default root password is \"alpine\" unless you changed it)..."

# The chown and the StrictModes check are not belt-and-braces. sshd runs with
# StrictModes at its default of yes (Cydia's sshd_config leaves it commented
# out), so it silently ignores authorized_keys — falling through to the
# password prompt, logging nothing the user will ever see — unless the home
# directory AND .ssh are owned by root and are not group- or world-writable.
# /var/root's metadata on this device is not something this script controls:
# the bake stages a /var tree onto the system partition and the first-boot
# postinstall merges it with `cp -a`, so a mode or ownership accident anywhere
# in that path lands here as "the key just doesn't work". Better to assert
# what we can and say so loudly about what we can't. `stat` is GNU's, from the
# installed coreutils 8.12, hence -c rather than BSD -f.
ssh "${ssh_opts[@]}" root@127.0.0.1 \
    'mkdir -p /var/root/.ssh && chmod 700 /var/root/.ssh && cat > /var/root/.ssh/authorized_keys && chmod 600 /var/root/.ssh/authorized_keys && chown -R root:wheel /var/root/.ssh && { home=$(stat -c "%a %U" /var/root 2>/dev/null) || home=""; case "$home" in "") : ;; *" root") case "${home%% *}" in *[2367]?|*?[2367]) echo "WARNING: /var/root is $home — sshd StrictModes will IGNORE the key it just accepted. Fix with: chmod go-w /var/root" >&2 ;; esac ;; *) echo "WARNING: /var/root is $home, not owned by root — sshd StrictModes will IGNORE the key it just accepted. Fix with: chown root:wheel /var/root" >&2 ;; esac; }' \
    < "$keys_file"

# Spell the reconnect command out in full. A bare `ssh -p <port> root@127.0.0.1`
# fails on this device for the same two algorithm reasons ssh_opts documents, so
# printing the short form would hand the user a command that cannot work.
cat <<EOF
Done — future SSH access uses your key. To reconnect later, forward the port:

    $iproxy_bin <port> 22${udid:+ $udid} &

and connect with the legacy algorithms this 2014 sshd needs:

    ssh -p <port> \\
        -o HostKeyAlgorithms=+ssh-rsa \\
        -o PubkeyAcceptedAlgorithms=+ssh-rsa \\
        root@127.0.0.1

(Or put those two options under a \`Host\` block in your ~/.ssh/config and just
\`ssh <that host>\`.)
EOF
