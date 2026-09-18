# Blackb0x

Untethered jailbreak tool for the 2nd/3rd-gen Apple TV, via the checkm8/SHAtter DFU-mode
boot exploit — side-loads Cydia + Kodi. This is a portable CLI port of the original macOS
app; it runs entirely from the command line, no GUI.

Devices supported:
- Apple TV 3,2 (A1469) (tvOS 8.4.x untethered)
- Apple TV 3,1 (A1427) (tvOS 8.4.x untethered) — needs external
  hardware (an Arduino running [synackuk's fork of checkm8-A5](https://github.com/synackuk/checkm8-a5))
  to pwn DFU mode first; `blackb0x` picks up from there
- Apple TV 2,1 (A1378) (tvOS 6.1.4 untethered)

The original app also had a tethered-boot mode (`--tether-boot`) for re-booting an
already-installed tethered jailbreak on firmwares that have no untether — tvOS 7.x on
Apple TV 3, tvOS 7.1.2 on Apple TV 2,1. That path has been removed: this port installs
the untethered jailbreak only, and there is now exactly one send flow. See
`docs/HISTORY.md`.

**Tested hardware:** this portable port — **blackb0x--** — has only ever been
verified against a real AppleTV3,2 running tvOS 7.9. Every other device/
firmware combination listed above is implemented from protocol analysis and
disassembly, not confirmed on real hardware. Platform status, from real
hardware testing:

**macOS only.** Apple Silicon is the tested host. `blackb0x-pwn` has been run
successfully against real AppleTV3,2 hardware there.

> **Linux is no longer supported.** It used to be the primary target, and the
> build now refuses to configure off Apple outright. checkm8 never worked on
> Linux: the exploit sequence runs to completion and the task-struct overwrite
> simply never lands. Four separate explanations were each proposed and then
> refuted by measurement, and by the end every observable matched a working
> macOS run while the outcome still differed. `docs/HISTORY.md` carries the
> whole trail, including what was ruled out, so nobody has to repeat it. If you
> were relying on the Linux path, the last commit that supports it is tagged in
> the history below.

**Apple Silicon tip:** a plain (non-Thunderbolt) USB hub between the Mac and the
Apple TV, rather than a direct connection, has been reported to make
`blackb0x-pwn` reliable — worth trying first if a direct connection is flaky.

IMPORTANT: make sure your device is connected to the internet for the first boot. Do
not turn it off during first boot until Kodi appears.

## Build

```sh
git clone --recurse-submodules https://github.com/regulad/Blackb0x--.git
cd Blackb0x--
cmake -S . -B build
cmake --build build -j$(nproc)
```

**Clone recursively** (`--recurse-submodules`) — every third-party dependency is
vendored as a git submodule and built from source; without it, the build will fail
with missing headers. Already cloned without it? `git submodule update --init --recursive`.

### Build-time system dependencies

Everything else — wolfSSL, curl, libimobiledevice, zlib, etc. — is vendored
and built from source as part of the build above; nothing else needs installing
system-wide. What *does* need to already be on the system (verified against a real
fresh clone + build, not just assumed):

- A C/C++ toolchain (gcc or clang) and **GNU make** — most of the dependency tree is
  autotools-based and shells out to `make` directly regardless of which CMake
  generator you use for the top-level build.
- **CMake ≥3.16.**
- **autoconf, automake, libtool, pkg-config** — for the autotools-based dependencies'
  own `./configure`/`autoreconf` steps. **Homebrew doesn't bundle any of the four
  with anything** — install them explicitly:
  `brew install cmake autoconf automake libtool pkg-config`. Confirmed on a real
  macOS build attempt: without an explicit `brew install libtool`, `autoreconf`/
  `autogen.sh` steps in the vendored dependencies fail outright.
### Runtime system dependencies

- **`hdiutil`/`diskutil`** (built in) — the ramdisk baker attaches and resizes a
  real HFS+ image through them. No root, no loop-mount, no `hfsprogs`. **This path
  has never been run on real macOS** — it was written with no Mac available; see
  `BakeRamdisk.cpp`'s own caveat and `.claude/TODO.md` item 4a before trusting it.
- **`cp`/`tar`** — used directly (as subprocesses, no shell) by the baker.
- **`usbmuxd`** — macOS's own built-in daemon. Normal-mode device discovery has
  nothing to talk to without it.
- **`ssh-keygen`** — you need a real SSH keypair of your own (see "Steps to
  jailbreak" below); this tool doesn't generate one for you.
- **`ssh`** — used by `scripts/push_authorized_keys.sh` to grant yourself SSH access
  to the device once it's jailbroken; see that step below.
- **`stdbuf`** (GNU coreutils) — **required**, not optional: `blackb0x` refuses to
  run the checkm8 exploit (`blackb0x-pwn`, see below) at all without it.
  Its progress output only gets flushed live through `stdbuf`; without it, a
  stuck/hanging exploit run would be silently indistinguishable from a working
  one, which is worse than just refusing to start. **On macOS, Homebrew's
  `coreutils` formula installs this prefixed as `gstdbuf`**, not plain `stdbuf`
  (avoids shadowing the BSD toolset) — `blackb0x` looks for both names, trying
  unprefixed `stdbuf` first, so either `brew install coreutils` alone, or also
  opting into coreutils' "gnubin" PATH shim for the unprefixed names, works.

### The pwntool: `blackb0x-pwn`

`blackb0x` runs the checkm8 exploit by shelling out to `blackb0x-pwn` (see
`Blackb0x/Source/Pwn/`), a second executable built on every platform. It runs
this project's own original checkm8/SHAtter exploit directly over
libirecovery's native IOKit backend. There is no choice to make and no flag to
pass.

`blackb0x-pwn` is **known-good on macOS**: confirmed working against real
AppleTV3,2 hardware. It can also be run standalone (`blackb0x-pwn checkm8` /
`blackb0x-pwn shatter`, both accepting `--ecid`) independent of `blackb0x`
entirely.

This used to be a choice between `blackb0x-pwn` and a vendored `gaster`,
selected with `--pwntool`. **`gaster` never pwned an AppleTV3,2 on either
Linux 7.1.x or macOS 26**, across a long instrumentation campaign, so it has
been removed along with the flag; `docs/HISTORY.md` keeps the full record of
what was measured.

**Apple Silicon tip:** if `blackb0x-pwn` is unreliable over a direct
USB-C connection, try a plain (non-Thunderbolt) USB hub between the Mac and
the Apple TV instead — this has been reported to make it reliable.

### No one-time system setup

macOS needs none. This is a real simplification, not an omission — the Linux path
required three separate manual system changes, and all three are gone with it:

- **Blacklisting `apple_mfi_fastcharge`.** That in-tree Linux driver binds to any
  Apple-vendor USB device whose product ID falls in `0x1200`-`0x12ff`, which includes
  the Apple TV's DFU-mode PID `0x1227`, and independently reset the device mid-exploit.
  It needed a `/etc/modprobe.d` drop-in, since `modprobe -r` alone was confirmed
  insufficient (the kernel reloaded it on every stage reconnect). No equivalent on
  macOS.
- **`usbmuxd --no-preflight`** via a systemd drop-in, or Normal-mode discovery silently
  never fired. macOS's own built-in `usbmuxd` needs no such change.
- **A udev rule** to grant a non-root user raw DFU/Recovery-mode USB access. macOS has
  no udev; `blackb0x` needs no special device permissions there.

`bake-all-ramdisks` also no longer needs root. It used to loop-mount a real HFS+
volume, which genuinely required `CAP_SYS_ADMIN`; `hdiutil` needs neither root nor a
mount helper. (Bear in mind the caveat above: that hdiutil path has never been run on
real macOS.)

If `blackb0x` still can't reach the device non-root after all of the above
## Steps to jailbreak

0. (3,1 only) PWN with Arduino + [synackuk's fork of checkm8-A5](https://github.com/synackuk/checkm8-a5) first.
1. Bake the ramdisks once, before ever running `blackb0x` itself:
   `./build/bake-all-ramdisks --signed-only`. `--signed-only` restricts the run to firmware
   Apple is currently signing, typically just the latest one or two per device —
   drop the flag to bake every known combination instead, including older/unsigned
   ones, if your
   device is on an older firmware than what's currently signed.
   `blackb0x` refuses to run at all against an empty `dist/`, and refuses a specific
   device+firmware with no matching entry there — re-run this (without
   `--signed-only`, if your device needs an older build) rather than trying to work
   around either check.
2. Plug in your Apple TV via micro-USB **and** plug in the power cable.
3. Run `./build/blackb0x`. Add `--dry-run` to preview the exploit/firmware steps
   without actually running the exploit or uploading anything to the device.
4. Follow the on-screen instructions to enter DFU mode.
5. Once the jailbreak finishes installing, connect to your TV and wait 5–10 minutes
   until Kodi appears (be patient, go have a coffee).
6. (Optional) Want SSH access? Once `blackb0x` reports the jailbreak is running, run
   `scripts/push_authorized_keys.sh` — it pushes your own `~/.ssh/authorized_keys`
   (make sure you have a real keypair first, `ssh-keygen`) onto the device over a
   plain SSH connection tunneled through `usbmuxd`, so future connections use your
   key instead of the device's default `root`/`alpine` password (Apple's own
   long-standing default for every iOS/tvOS device, not something this project or
   Cydia's openssh sets — ssh will just prompt for it interactively, like an
   ordinary `ssh-copy-id` run). The script deliberately ignores your own
   `~/.ssh/config` for this one connection (so a pubkey-only `Host *` entry there
   doesn't kill that password fallback) and re-enables `ssh-dss`/`ssh-rsa`, since
   this device's sshd is a 2014-era OpenSSH 6.7 that a modern `ssh` client
   otherwise refuses to even handshake with. See the script's own `--help` for
   options (targeting a specific device, a non-default keys file, etc.) — it needs
   no root/sudo, unlike `blackb0x` itself.

## Development

See [`AGENTS.md`](AGENTS.md) for repo conventions, and
[`docs/HISTORY.md`](docs/HISTORY.md) for the full port/debugging history.

## Credits
**[NSSpiral](https://github.com/NSSpiral/Blackb0x)**
* Original Blackb0x — the macOS Cocoa/Objective-C app this project is a portable CLI port of

**dora2ios**
* iBSS loader for AppleTV3,1

**tihmstar**
* etasonATV jsc untether
* answering questions about patches

**zzanehip**
* updated CBPatcher (Created by Jonathan Seals)
* updated iBoot32Patcher (Created by iH8sn0w)
* updated xpwntool (Created by planetbeing)

**a1exdandy, synackuk, nyan_satan**
* checkm8-A5

**axi0mx**
* checkm8

**p0sixninja**
* SHAtter

