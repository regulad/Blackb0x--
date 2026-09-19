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
# sshd also needs ssh-dss/ssh-rsa explicitly re-enabled (see ssh_opts
# below) — a modern ssh client refuses both by default.

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
            sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'
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
    # This device's sshd (OpenSSH 6.7p1, 2014) predates RFC 8332 rsa-sha2-*
    # and offers a DSA host key (Cydia openssh_6.7p1's own sshd_config: both
    # ssh_host_dsa_key and ssh_host_rsa_key) — modern OpenSSH clients disable
    # ssh-dss entirely (since 7.0) and plain ssh-rsa by default (since 8.8),
    # so without re-enabling both here, the initial handshake itself fails
    # ("no matching host key type found") before any password prompt.
    -o HostKeyAlgorithms=+ssh-dss,ssh-rsa
    -o PubkeyAcceptedAlgorithms=+ssh-dss
)

echo "Waiting for the device's sshd (127.0.0.1:$port)..."
attempts=30
until (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; do
    attempts=$((attempts - 1))
    if [[ $attempts -le 0 ]]; then
        echo "push_authorized_keys.sh: couldn't reach sshd on the device after several tries." >&2
        echo "Make sure the jailbreak has actually finished booting and try again." >&2
        exit 1
    fi
    sleep 2
done

echo "Pushing $keys_file (default root password is \"alpine\" unless you changed it)..."
ssh "${ssh_opts[@]}" root@127.0.0.1 \
    'mkdir -p /var/root/.ssh && chmod 700 /var/root/.ssh && cat > /var/root/.ssh/authorized_keys && chmod 600 /var/root/.ssh/authorized_keys' \
    < "$keys_file"

echo "Done — future SSH access uses your key, e.g. re-run this script's iproxy forward" \
     "by hand (\`$iproxy_bin <port> 22${udid:+ $udid}\`) and \`ssh -p <port> root@127.0.0.1\`."
