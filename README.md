# Blackb0x

Untethered jailbreak tool for the 2nd/3rd-gen Apple TV, via the checkm8/SHAtter
DFU-mode boot exploit. Side-loads Cydia + Kodi. A portable CLI port of the original
macOS app, no GUI.

Devices supported:

| device | firmware | notes |
|---|---|---|
| Apple TV 3,2 (A1469) | tvOS 8.4.x | the only combination verified on real hardware |
| Apple TV 3,1 (A1427) | tvOS 8.4.x | needs an Arduino running [synackuk's checkm8-A5](https://github.com/synackuk/checkm8-a5) to pwn DFU first; `blackb0x` picks up from there |
| Apple TV 2,1 (A1378) | tvOS 6.1.4 | |

**macOS only, Apple Silicon tested.** Everything except the AppleTV3,2 path is
implemented from protocol analysis rather than confirmed on hardware. Linux support
was removed; `docs/HISTORY.md` has the reasoning.

**Tip:** if `blackb0x-pwn` is unreliable over a direct USB-C connection, put a plain
(non-Thunderbolt) USB hub between the Mac and the Apple TV.

**Important:** the device needs internet access on first boot. Do not power it off
until Kodi appears.

## Requirements

Xcode Command Line Tools, plus Homebrew packages. Which ones depends on what you are
building.

To build and run the jailbreak:

```sh
brew install cmake autoconf automake libtool pkg-config
```

To additionally build the authoring tools (firmware baker, vendored apt, tests):

```sh
brew install git-lfs ldid afsctool dpkg python3 \
             berkeley-db@5 openssl@3 xxhash lz4 xz gettext
```

The baker also needs [Theos](https://theos.dev) for `dm.pl` (`$THEOS`, default
`~/theos`) and a cross-compiler for `entrypoint/` — see `entrypoint/README.md`.

Almost everything else is vendored under `third_party/` and built from source.

## Build

```sh
git clone --recurse-submodules https://github.com/regulad/Blackb0x--.git
cd Blackb0x--
cmake -S . -B build
```

Then pick a target:

```sh
cmake --build build --target jailbreak   # blackb0x and blackb0x-pwn
cmake --build build --target authoring   # bake-firmware, vendored apt, tests, xpwntool
```

`--recurse-submodules` is required; without it the build fails on missing headers
(`git submodule update --init --recursive` fixes an existing clone). **Install
`git-lfs` before cloning** if you want the authoring tools: `debcache/` is stored
through LFS, and without it you get pointer files instead of packages, which only
surfaces much later as a bake that cannot read anything. `git lfs pull` fixes it.

`bake-firmware` **must run as root**, including `--only bootchain`. It writes into
root-owned files on a mounted ramdisk. Its output in `dist/` is handed back to the
user who invoked `sudo`, so you will not need `sudo` to read or delete your own
build artifacts. The IPSW download cache is not covered by that; clean it with
`sudo chown -R "$USER" ~/.local/share/blackb0x` if a root-owned cache gets in the way.

## Jailbreaking

0. **(3,1 only)** Pwn DFU with the Arduino first.
1. Bake the firmware once:
   ```sh
   sudo env "PATH=$PATH" "THEOS=$THEOS" ./build/bake-firmware
   ```
   That does every known device/firmware combination. Narrow it with
   `--device AppleTV3,2` and `--build 12H606`. Existing output is skipped unless you
   pass `--force`.
2. Connect the Apple TV by micro-USB **and** plug in its power cable.
3. Run `./build/blackb0x`. Add `--dry-run` to preview without touching the device.
4. Follow the on-screen instructions to enter DFU mode.
5. Wait 5-10 minutes after install for Kodi to appear on the TV.
6. **(Optional)** For SSH access, once `blackb0x` reports the jailbreak is running:
   ```sh
   scripts/push_authorized_keys.sh
   ```
   It tunnels to the device's sshd over `usbmuxd` and installs your
   `~/.ssh/authorized_keys`, like `ssh-copy-id`. The device's default password is
   `root`/`alpine`. Needs no root. See its `--help` for options.

`blackb0x` refuses to run against an empty `dist/`, and refuses any device/firmware
with no matching entry. Bake the tuple you need rather than working around either
check.

## Development

[`AGENTS.md`](AGENTS.md) has the repo conventions and build details.
[`docs/HISTORY.md`](docs/HISTORY.md) has the full port and debugging history.

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

