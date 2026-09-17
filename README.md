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

- **macOS + `blackb0x-pwn`: known-good.** `blackb0x-pwn` (see "checkm8 on
  macOS" below) has been run successfully against real AppleTV3,2 hardware.
- **macOS + gaster: does not work, no matter what has been tried.** Not "flaky" —
  genuinely non-functional on every macOS attempt so far. Use `--pwntool
  blackb0x-pwn` (the default on macOS) instead; see below.
- **Linux: should work per the code/design, but has not worked reliably on
  any Linux machine tested.** Confirmed on two different real PCs — one
  Intel 11th-gen, one AMD Zen 2 — both showing the same class of
  non-deterministic USB behavior during the checkm8/DFU exploit sequence
  (see `docs/HISTORY.md`'s "Reopening macOS support" entry for the
  specific symptoms this was chased through). This may be a property of
  Linux's USB stack/timing on the specific controllers tested rather than
  something fixable in this project's own code — not resolved as of this
  writing.
- **Apple Silicon tip:** a plain (non-Thunderbolt) USB hub between the Mac
  and the Apple TV, rather than a direct connection, has been reported to
  make `blackb0x-pwn` reliable — worth trying first if a direct connection
  is flaky.

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

Everything else — wolfSSL, curl, libusb, libimobiledevice, zlib, etc. — is vendored
and built from source as part of the build above; nothing else needs installing
system-wide. What *does* need to already be on the system (verified against a real
fresh clone + build, not just assumed):

- A C/C++ toolchain (gcc or clang) and **GNU make** — most of the dependency tree is
  autotools-based and shells out to `make` directly regardless of which CMake
  generator you use for the top-level build.
- **CMake ≥3.16.**
- **autoconf, automake, libtool, pkg-config** — for the autotools-based dependencies'
  own `./configure`/`autoreconf` steps. On Linux these mostly come along for free —
  most distros' base/`build-essential`-style GCC toolchain package pulls in `libtool`
  transitively as a dependency of something else in that group. **macOS/Homebrew
  doesn't bundle any of the four with anything** — install them explicitly:
  `brew install cmake autoconf automake libtool pkg-config`. Confirmed on a real
  macOS build attempt: without an explicit `brew install libtool`, `autoreconf`/
  `autogen.sh` steps in the vendored dependencies fail outright, the same way a
  from-scratch Linux distro missing that package would.
- **`xxd`** (usually in a `vim-common`/`xxd`/`vim` package) — used to embed `gaster`'s
  exploit payload binaries as C arrays at build time.

### Runtime system dependencies

- **`hfsprogs`** (`mkfs.hfsplus`/`fsck.hfsplus`) and a kernel built with
  `CONFIG_HFSPLUS_FS` (built-in or loadable module) — `patchRamdisk()` builds and
  loop-mounts a real HFS+ volume.
- **`mount`/`umount`/`blkid`/`cp`/`tar`** — used directly (as subprocesses, no shell)
  by `patchRamdisk()`. Present on any mainstream Linux distro as a matter of course.
- **`usbmuxd` itself must be installed** (most distros ship it as its own package,
  e.g. `usbmuxd`), separately from it needing to run with `--no-preflight` — see
  below. Without the daemon present at all, Normal-mode device discovery has nothing
  to talk to, full stop.
- **`ssh-keygen`** — you need a real SSH keypair of your own (see "Steps to
  jailbreak" below); this tool doesn't generate one for you.
- **`ssh`** — used by `scripts/push_authorized_keys.sh` to grant yourself SSH access
  to the device once it's jailbroken; see that step below.
- **`stdbuf`** (GNU coreutils) — **required**, not optional: `blackb0x` refuses to
  run the checkm8 exploit (gaster or `blackb0x-pwn`, see below) at all without it.
  Its progress output only gets flushed live through `stdbuf`; without it, a
  stuck/hanging exploit run would be silently indistinguishable from a working
  one, which is worse than just refusing to start. **On macOS, Homebrew's
  `coreutils` formula installs this prefixed as `gstdbuf`**, not plain `stdbuf`
  (avoids shadowing the BSD toolset) — `blackb0x` looks for both names, trying
  unprefixed `stdbuf` first, so either `brew install coreutils` alone, or also
  opting into coreutils' "gnubin" PATH shim for the unprefixed names, works.

### checkm8 on macOS: use `--pwntool blackb0x-pwn`

`blackb0x` normally runs the checkm8 exploit by shelling out to the vendored
`gaster` tool. **On macOS, gaster does not work — not intermittently, not
"needs a workaround," genuinely non-functional no matter what has been
tried.** `blackb0x` therefore builds a second executable on macOS,
`blackb0x-pwn` (see `Blackb0x/Source/Pwn/`), that runs this project's own
original checkm8/SHAtter exploit directly over libirecovery's native IOKit
backend instead — no gaster, no libusb. **This is the default on macOS**
(`--pwntool` defaults to `blackb0x-pwn` there; pass `--pwntool gaster` to
force the old, broken path anyway, e.g. for debugging gaster itself).
`blackb0x-pwn` is **known-good**: confirmed working against real AppleTV3,2
hardware. It can also be run standalone (`blackb0x-pwn checkm8` /
`blackb0x-pwn shatter`, both accepting `--ecid`) independent of `blackb0x`
entirely.

**Apple Silicon tip:** if `blackb0x-pwn` is unreliable over a direct
USB-C connection, try a plain (non-Thunderbolt) USB hub between the Mac and
the Apple TV instead — this has been reported to make it reliable.

`--pwntool` (and `blackb0x-pwn` itself) only exist on macOS; on Linux,
gaster is the only option, unconditionally, and passing `--pwntool` prints
a warning and is otherwise ignored.

### One-time system setup: blacklist `apple_mfi_fastcharge`

The in-tree `apple_mfi_fastcharge` driver (Apple Lightning fast-charge support) binds
to *any* USB device with Apple's vendor ID whose product ID falls in `0x1200`-`0x12ff`
— a range that includes the Apple TV's real DFU-mode PID (`0x1227`), so this driver
attaches to it even in DFU mode. `gaster` never claims the interface first, so this
driver stays attached and independently resets the device while `gaster`'s own
exploit-timing-sensitive USB transfers are in flight — two things resetting the same
device at once, which corrupts USB enumeration and can hang the exploit (sometimes
taking the whole USB stack down with it) in a way that reproduces across different
Linux machines, not just one host's controller. Removing the module once isn't
enough either — it reloads itself automatically the moment the device reconnects
(which `gaster`'s own exploit does several times per run) — so it needs to be
blacklisted, not just unloaded:

```sh
sudo mkdir -p /etc/modprobe.d
sudo tee /etc/modprobe.d/blacklist-apple-mfi-fastcharge.conf <<'EOF'
blacklist apple_mfi_fastcharge
EOF
sudo modprobe -r apple_mfi_fastcharge   # only if currently loaded
```

(If you actually use this same PC to fast-charge a real Apple device over USB,
removing this blacklist afterward — `sudo rm /etc/modprobe.d/
blacklist-apple-mfi-fastcharge.conf` — restores that.)

### One-time system setup: `usbmuxd --no-preflight`

The system `usbmuxd` daemon needs to run with `--no-preflight`, or this hardware's
Normal-mode discovery will silently never work. Add a systemd drop-in:

```sh
sudo mkdir -p /etc/systemd/system/usbmuxd.service.d
sudo tee /etc/systemd/system/usbmuxd.service.d/override.conf <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/bin/usbmuxd --user usbmuxd --systemd --no-preflight
EOF
sudo systemctl daemon-reload
sudo systemctl restart usbmuxd
```

(Adjust the `ExecStart=` path/args to match your distro's existing unit —
`systemctl cat usbmuxd` shows the original.)

### One-time system setup (optional): run `blackb0x` without root

`blackb0x` itself only ever needs root for one thing: opening a raw USB handle to
the Apple TV while it's in DFU, Recovery, or WTF mode (vendor `05ac`, product
`1222`/`1227`/`1280`-`1283` — the exact set of modes `libirecovery` ever opens a
handle for). The kernel's default device-node permissions restrict that to root;
a udev rule can hand it to your own user instead, via a real group rather than
a desktop-session ACL (`TAG+="uaccess"` only covers whoever's logged in at the
active graphical seat — this also needs to work headless/over SSH, which a
group membership doesn't care about):

```sh
# 1. Create the plugdev group if your distro doesn't already ship one
#    (Debian/Ubuntu do by default; Fedora, Arch, and others don't).
getent group plugdev >/dev/null || sudo groupadd --system plugdev

# 2. Add yourself to it.
sudo usermod -aG plugdev "$USER"

# 3. Grant that group access to the Apple TV's DFU/Recovery/WTF-mode USB device.
sudo tee /etc/udev/rules.d/70-blackb0x.rules <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="1222", GROUP="plugdev", MODE="0660"
SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="1227", GROUP="plugdev", MODE="0660"
SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="128[0-3]", GROUP="plugdev", MODE="0660"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Then **log out and back in** (or run `newgrp plugdev` in your current shell) —
group membership changes don't apply to sessions that already exist — and
**unplug/replug the Apple TV** so the device node gets re-created under the new
rule. After that, `./build/blackb0x` runs directly, no `sudo`.

This only affects `blackb0x` itself. `sudo ./build/bake-all-ramdisks` still
needs root regardless (it loop-mounts a real HFS+ image, which genuinely needs
`CAP_SYS_ADMIN`) — this setup doesn't change that.

If `blackb0x` still can't reach the device non-root after all of the above
(commonly: "ERROR: Unable to connect to device" appearing immediately, even
with the Apple TV visibly in DFU mode), check `lsusb -d 05ac:` shows the
device and that the matching `/dev/bus/usb/*/*` node's group is actually
`plugdev` (`ls -l`) — if it's still `root`, the rule didn't match or didn't
reload; skip straight to `sudo ./build/blackb0x` rather than debug it further.

## Steps to jailbreak

0. (3,1 only) PWN with Arduino + [synackuk's fork of checkm8-A5](https://github.com/synackuk/checkm8-a5) first.
1. Bake the ramdisks once, before ever running `blackb0x` itself:
   `sudo ./build/bake-all-ramdisks --signed-only` (root is required here too — this
   step loop-mounts a real HFS+ volume to patch it, the same raw-mount access
   `blackb0x` itself needs for USB). `--signed-only` restricts the run to firmware
   Apple is currently signing, typically just the latest one or two per device —
   drop the flag to bake every known combination instead, including older/unsigned
   ones, if your
   device is on an older firmware than what's currently signed.
   `blackb0x` refuses to run at all against an empty `dist/`, and refuses a specific
   device+firmware with no matching entry there — re-run this (without
   `--signed-only`, if your device needs an older build) rather than trying to work
   around either check.
2. Plug in your Apple TV via micro-USB **and** plug in the power cable.
3. Run `sudo ./build/blackb0x` (root is required by default — raw DFU/Recovery-mode
   USB access needs it; run `./build/blackb0x` without `sudo` instead if you've set
   up the udev rule above). Add `--dry-run` to preview the exploit/firmware steps
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

**nyan_satan**
* libbootkit (iBSS loader for AppleTV3,2)

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

**[verygenericname](https://github.com/verygenericname/gaster)**
* gaster (the checkm8 implementation this port shells out to)
