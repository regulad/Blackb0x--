#!/usr/bin/env python3
"""Sweep blackb0x-pwn's DEBUG_CANCEL_DELAY_US across a fresh DFU device each time.

checkm8's bug-setup step submits a 2048-byte DFU_DNLOAD and aborts it partway
through, deliberately leaving the device's DFU handler holding a
partially-filled buffer. How much the device actually consumed before the
abort is the value everything after it depends on: it has to be greater than
zero (or the dangling-buffer state never exists) and no greater than the
overwrite offset (or the overwrite lands past its target).

On macOS/IOKit the hardcoded 100us produces a usable value. On Linux/libusb a
usbmon capture shows it producing zero -- the host controller never starts the
data stage that fast. This sweeps the delay looking for the window where it
doesn't.

Each attempt needs a device in clean, un-pwned DFU: a failed attempt leaves
the descriptors scribbled (the serial string stops being readable), so the
Apple TV has to be power-cycled between runs. This waits for that rather than
assuming it, and it reads the per-attempt result out of blackb0x-pwn's own
"bug setup:" line, so a run still yields a data point even when it doesn't
pwn.

Needs no root: device identity comes from sysfs, which is world-readable.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import time

APPLE_VID = "05ac"
DFU_PID = "1227"

# A clean DFU serial looks like:
#   CPID:8947 CPRV:00 ... ECID:000002713C84D50E IBFL:00 SRTG:[iBoot-1458.2]
# A pwned one has " PWND:[...]" appended. A device left corrupted by a failed
# attempt loses the whole structure (it reads back as "Apple Inc." or similar),
# so requiring SRTG is also what distinguishes "freshly power-cycled" from
# "still wedged from the last run".
SRTG_RE = re.compile(r"SRTG:\[")
PWND_RE = re.compile(r"PWND:\[")

SENT_RE = re.compile(r"bug setup: cancel delay (\d+) us -> device consumed (-?\d+) of (\d+) bytes")


def read(path):
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return None


def dfu_devices():
    """Every Apple DFU-mode device currently enumerated, as (syspath, serial)."""
    found = []
    for path in glob.glob("/sys/bus/usb/devices/*"):
        if read(os.path.join(path, "idVendor")) != APPLE_VID:
            continue
        if read(os.path.join(path, "idProduct")) != DFU_PID:
            continue
        found.append((path, read(os.path.join(path, "serial")) or ""))
    return found


def classify(serial):
    if PWND_RE.search(serial):
        return "pwned"
    if SRTG_RE.search(serial):
        return "clean"
    return "wedged"


def wait_for_fresh(timeout=None, poll=0.5):
    """Block until exactly one clean, un-pwned DFU device is present.

    Returns (syspath, serial). Prints a prompt only once per state change, so
    a long wait for someone to walk over and pull the power doesn't scroll.
    """
    start = time.monotonic()
    last_state = None
    while True:
        devices = dfu_devices()
        states = [classify(s) for _, s in devices]
        if states.count("clean") == 1 and len(devices) == 1:
            return devices[0]

        if not devices:
            # Not the same as "wedged": a failed attempt can leave the device
            # off the bus entirely rather than enumerated-but-broken, and
            # MENU + P/P does nothing for a device that isn't drawing USB.
            # It needs its power pulled first.
            state = ("device is not on the USB bus at all -- unplug its POWER, plug it "
                     "back in, then hold MENU + P/P for DFU")
        elif "pwned" in states:
            state = "device reports PWND already -- power-cycle it for a clean run"
        elif "wedged" in states:
            # Still enumerating, so it is still listening to the remote --
            # re-entering DFU is enough here, unlike the off-the-bus case
            # above.
            state = ("device is enumerated but wedged from a previous attempt (serial "
                     "unreadable) -- hold MENU + P/P for DFU")
        else:
            state = "%d DFU devices connected, expected exactly 1" % len(devices)
        if state != last_state:
            print("  waiting: %s" % state, flush=True)
            last_state = state

        if timeout is not None and time.monotonic() - start > timeout:
            raise TimeoutError(state)
        time.sleep(poll)


def wait_until_gone(syspath, timeout=120, poll=0.5):
    """Wait for a specific device instance to disappear (i.e. be power-cycled)."""
    start = time.monotonic()
    while os.path.exists(syspath):
        if time.monotonic() - start > timeout:
            return False
        time.sleep(poll)
    return True


def run_attempt(binary, delay_us, ecid, timeout):
    # DEBUG_RECONNECT_ATTEMPTS: blackb0x-pwn waits 30 one-second retries by
    # default, which is the macOS-confirmed behaviour and stays the default in
    # the binary. For sweeping, a stage that is never coming back costs 30s
    # every time, and the Apple TV can be put back into DFU far faster.
    env = dict(os.environ,
               DEBUG_CANCEL_DELAY_US=str(delay_us),
               DEBUG_RECONNECT_ATTEMPTS="5")
    argv = [binary, "checkm8"]
    if ecid:
        argv += ["--ecid", str(ecid)]
    try:
        proc = subprocess.run(argv, env=env, timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True)
        return proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as exc:
        return None, (exc.output or "") + "\n[timed out after %ds]" % timeout


def parse_sent(output):
    match = SENT_RE.search(output)
    return int(match.group(2)) if match else None


def parse_delays(spec):
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part.lstrip("-"):
            lo, _, rest = part.partition("-")
            hi, _, step = rest.partition(":")
            step = int(step) if step else 100
            out.extend(range(int(lo), int(hi) + 1, step))
        else:
            out.append(int(part))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default="./build/blackb0x-pwn")
    ap.add_argument("--ecid", default=None,
                    help="pass through to blackb0x-pwn --ecid")
    ap.add_argument("--delays", default="0,25,50,75,100,250,500,1000,2000,3000",
                    help="comma-separated microsecond values; 'lo-hi:step' expands "
                         "to a range (default: %(default)s)")
    ap.add_argument("--attempt-timeout", type=int, default=180,
                    help="seconds to let one blackb0x-pwn run take (default: %(default)s)")
    ap.add_argument("--stop-on-success", action="store_true", default=True)
    ap.add_argument("--keep-going", dest="stop_on_success", action="store_false",
                    help="sweep every delay even after one pwns, to map the whole window")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        raise SystemExit("%s not found -- build it first: cmake --build build "
                         "--target blackb0x-pwn" % args.binary)

    delays = parse_delays(args.delays)
    if not delays:
        raise SystemExit("no delays to sweep")

    print("sweeping DEBUG_CANCEL_DELAY_US over: %s" % ", ".join(str(d) for d in delays))
    print("power-cycle the Apple TV and put it back in DFU between attempts; "
          "this waits for that.\n")

    results = []
    try:
        for index, delay in enumerate(delays, 1):
            print("[%d/%d] DEBUG_CANCEL_DELAY_US=%d" % (index, len(delays), delay), flush=True)
            syspath, serial = wait_for_fresh()
            print("  device ready: %s" % serial, flush=True)

            code, output = run_attempt(args.binary, delay, args.ecid, args.attempt_timeout)
            sent = parse_sent(output)
            pwned = code == 0

            for line in output.splitlines():
                print("    | %s" % line)

            # What the attempt did to the device is its own data point, and it
            # varies by configuration in a way the exit code doesn't capture:
            # some settings leave it enumerated with scribbled descriptors
            # ("wedged"), others make it reboot off the bus entirely ("gone"),
            # which is what an ordinary un-exploited DFU device does when it
            # rejects a bad image. Sampled after a short settle so a device
            # mid-reboot isn't mistaken for one that stayed up.
            time.sleep(2.0)
            after = [classify(s) for _, s in dfu_devices()]
            if not after:
                post_state = "gone (rebooted)"
            elif len(after) == 1:
                post_state = after[0]
            else:
                post_state = "%d devices" % len(after)

            results.append((delay, sent, pwned, code, post_state))
            print("  -> consumed %s bytes, %s (exit %s), device after: %s" % (
                "?" if sent is None else sent,
                "PWNED" if pwned else "not pwned",
                "timeout" if code is None else code,
                post_state), flush=True)

            if pwned and args.stop_on_success:
                print(flush=True)
                break

            # blackb0x-pwn gives up after five one-second reconnect attempts,
            # so a failure surfaces in about five seconds and the only thing
            # to do is put the device back into DFU. Spelled out, since this
            # is the one line the operator is actually watching for.
            print("FAILED: Restart DFU by holding MENU + P/P", flush=True)
            if index < len(delays):
                wait_until_gone(syspath)
            print(flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted", flush=True)
    except TimeoutError as exc:
        print("\ngave up waiting: %s" % exc, flush=True)

    if not results:
        return 1

    print("\n%-12s %-16s %-10s %s" % ("delay (us)", "bytes consumed", "result", "device after"))
    for delay, sent, pwned, code, post_state in results:
        print("%-12d %-16s %-10s %s" % (
            delay,
            "?" if sent is None else sent,
            "PWNED" if pwned else ("timeout" if code is None else "failed"),
            post_state))

    # Deliberately NOT advising a direction any more. This used to say "every
    # attempt consumed 0 bytes ... try larger delays", which is now known to be
    # backwards: a confirmed-working macOS run consumes 0, and a Linux sweep of
    # 0-90us reproduced consumed == 0 at every step and still failed nine for
    # nine. Both the count and the delay are ruled out as the differentiator --
    # see docs/HISTORY.md, "The overwrite was never the problem".
    consumed_any = [(d, s) for d, s, _, _, _ in results if s not in (None, 0)]
    if consumed_any:
        print("\nDelays that got the device to consume anything: %s" %
              ", ".join("%dus -> %d bytes" % (d, s) for d, s in consumed_any))
    else:
        print("\nEvery attempt consumed 0 bytes -- which is what a working macOS run "
              "does too, so this is not by itself a failure signal.")
    print("\nThe consumed count is NOT known to be the thing that matters: a macOS run "
          "that pwns the device consumes 0, and 0-90us on Linux all consume 0 and all "
          "fail. If nothing here pwned, the next measurement is a single run with "
          "DEBUG_TRACE_TRANSFERS=1, reading 'payload-upload moved' -- 0 means the "
          "overwrite took and the failure is later (the reset); 678 means it did not.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
