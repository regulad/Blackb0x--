# Blackb0x

Jailbreak tool for the 2nd/3rd-gen Apple TV, via the checkm8/SHAtter DFU-mode boot
exploit. Side-loads Cydia + Kodi. A portable CLI port of the original macOS app, no
GUI.

Each device is targeted at the newest firmware Apple ever shipped it:

| device | firmware | notes |
|---|---|---|
| Apple TV 3,2 (A1469) | tvOS 8.4.x | untethered (tihmstar's etasonATV); the only model any of this has been run against |
| Apple TV 3,1 (A1427) | tvOS 8.4.x | untethered; needs an Arduino running [synackuk's checkm8-A5](https://github.com/synackuk/checkm8-a5) to pwn DFU first; `blackb0x` picks up from there |
| Apple TV 2,1 (A1378) | tvOS 7.1.2 | **tethered** — no untether exists for 7.x, so the boot has to be redone from the Mac after each power cycle |

The exact build each device targets lives in one table, `kJailbreakTargets[]` in
`src/Cli.cpp`. Everything else — what CI bakes, what `blackb0x` asks for — reads it
from there.

**macOS only, Apple Silicon tested.** Everything except the AppleTV3,2 path is
implemented from protocol analysis rather than confirmed on hardware, and **no
device has yet come up jailbroken**: the firmware suite bakes for all three models
and the exploit stage (`blackb0x-pwn`) works on an AppleTV3,2, which is not the
same thing as a finished jailbreak. `docs/HISTORY.md` tracks where that stands.
Linux support was removed; the same file has the reasoning.

**Tip:** if `blackb0x-pwn` is unreliable over a direct USB-C connection, put a plain
(non-Thunderbolt) USB hub between the Mac and the Apple TV.

**Important:** the device needs internet access on first boot. Do not power it off
until Kodi appears.

## Requirements

Xcode Command Line Tools, plus Homebrew packages. Which ones depends on what you are
building.

To build and run the jailbreak:

```sh
brew install cmake autoconf automake libtool pkg-config gh
```

`gh` is GitHub's CLI, and it is what lets `blackb0x` fetch a prebuilt firmware
suite instead of baking one. Sign in once with `gh auth login`. Without it you
can still jailbreak, but you have to bake the firmware yourself, which needs
root and everything in the next list.

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
cmake --build build --target jailbreak -j"$(sysctl -n hw.ncpu)"   # blackb0x and blackb0x-pwn
cmake --build build --target authoring -j"$(sysctl -n hw.ncpu)"   # bake-firmware, vendored apt, tests, xpwntool
```

`sysctl -n hw.ncpu` is the macOS equivalent of `nproc`, which does not exist
here.

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

## Firmware

`blackb0x` sends a prepared firmware suite from `dist/`. It never patches anything
itself. When `dist/` has no suite for the device and build it needs, it resolves one
in this order and stops at the first that works:

1. **Already in `dist/`.** Nothing is ever re-fetched or re-baked over an existing
   suite, so a hand-built one always wins.
2. **Downloaded from CI.** If `gh` is on your `PATH`, it pulls the suite published by
   `.github/workflows/ci.yml`. No root, no Theos, no apt, no waiting. This is the
   normal path and needs nothing from the authoring list.
3. **Baked locally.** With no `gh` but running as root, it shells out to
   `bake-firmware` and builds the suite itself. Minutes, and the full authoring
   toolchain.

Neither `gh` nor root is the one combination that cannot work, and it says so, with
both fixes spelled out.

Point it at a different repository's artifacts with `BLACKB0X_ARTIFACT_REPO`
(`owner/name`), for a fork or a private mirror.

CI bakes all three devices, one runner each, and publishes each suite as the
artifact `firmware-<device>` — the name `blackb0x` downloads by. `AppleTV2,1` and
`AppleTV3,1` used to be excluded because their kernelcaches failed to decrypt; that
was a real defect in the vendored xpwn's img3 reader and it is fixed.

## Jailbreaking

0. **(3,1 only)** Pwn DFU with the Arduino first.
1. **(Optional.)** Nothing to do here if you have `gh` — `blackb0x` fetches the
   firmware it needs on its first run, as described under "Firmware" above. To bake
   it yourself instead:
   ```sh
   sudo env "PATH=$PATH" "THEOS=$THEOS" ./build/bake-firmware
   ```
   That does every known device/firmware combination, which is far more than you
   need. Narrow it with `--device` and `--build`: when a suite is missing,
   `blackb0x` names the exact device and build it wants — and, if it cannot bake
   one itself, spells out the `bake-firmware` command for it. Existing output is
   skipped unless you pass `--force`.
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

