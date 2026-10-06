# Contributing to Blackb0x--

Thanks for helping keep old Apple TVs out of the e-waste pile. This page is short on
purpose. Everyone taking part is expected to follow the
[Code of Conduct](CODE_OF_CONDUCT.md).

**Issues are the main way to contribute here.** A good bug report from someone with
real hardware is worth more than most patches. Pull requests are welcome too, but
please open an issue first so we can agree on the approach before you write code.

## Building and running

The full instructions are in [`README.md`](README.md) (requirements, Homebrew packages,
the old Xcodes the baker needs) and [`AGENTS.md`](AGENTS.md) (repo conventions and the
"Build & run" section). The short version:

```sh
git clone --recurse-submodules https://github.com/regulad/Blackb0x--.git
cd Blackb0x--
cmake -S . -B build
cmake --build build --target jailbreak -j"$(sysctl -n hw.ncpu)"   # blackb0x, blackb0x-pwn
cmake --build build --target authoring -j"$(sysctl -n hw.ncpu)"   # bake-firmware, apt, tests, xpwntool

./build/blackb0x [--ecid <id> | --udid <id>] [--dry-run]
sudo env "PATH=$PATH" "THEOS=$THEOS" ./build/bake-firmware [--device <model>] [--build <buildID>]
```

Things to know before you change code:

- **macOS only, CLI only.** Linux support and the GUI were removed on purpose.
- **Two hosts are tested:** `arm64` on macOS 26 Tahoe, and `x86_64` on macOS 11 Big
  Sur. Big Sur's older clang rejects C++ that newer compilers accept, so a clean build
  on a new Mac doesn't prove your change is portable. Read the C++17 note in
  `AGENTS.md`.
- **Third-party code lives in `third_party/` as git submodules, built from source and
  statically linked.** Don't add system or Homebrew dependencies. After touching
  dependencies, check `otool -L build/blackb0x` against the list in `AGENTS.md`.
- **Don't change a submodule pin or patch a submodule in place** without reading the
  relevant section of `AGENTS.md` first.
- If something in `AGENTS.md` looks arbitrary, [`docs/HISTORY.md`](docs/HISTORY.md)
  probably explains why it's that way. Don't relitigate a convention without
  asking first.
- Run the tests (`ctest --test-dir build`) after an `authoring` build.

## Filing an issue

Use one of the [issue forms](https://github.com/regulad/Blackb0x--/issues/new/choose).
Blank issues are turned off. For bug reports, the most useful things you can give are:

- your **Apple TV model** (AppleTV2,1 / 3,1 / 3,2) and **firmware version**
- your **host Mac**: architecture (Apple Silicon or Intel) and macOS version
- the **commit** you built (`git rev-parse HEAD`)
- whether you used **prebuilt firmware** (fetched through `gh`) or **baked it
  yourself**
- your **hardware setup**: direct USB or through a hub, HDMI attached, and the
  Arduino if you have a 3,1
- the **complete terminal output**, not just the last line, plus a photo of the TV
  screen if anything showed up there

**Don't post screenshots of a terminal.** Copy the text and paste it into a
Markdown code block, in issues, comments and pull requests alike:

````markdown
```
paste the output here
```
````

The bug report form's output field already does this for you. Text can be
searched, quoted and diffed; a screenshot can't, and you'll be asked for the text
anyway. A photo of the TV is fine, since there's no text to copy.

Please search existing issues first. Don't use public issues for code of conduct
reports. See [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) for how to report those.

## Opening a pull request

1. Open or find an issue that describes the change, and link it.
2. Keep the change focused. One problem per PR.
3. Follow the conventions in `AGENTS.md`. Note which host(s) you built and tested on,
   and whether you tested on a real Apple TV.
4. Fill in the pull request template, including the AI disclosure section below.

## AI use disclosure (required)

AI tools are allowed here. Hiding their use is not. Every issue and pull request must
say **whether and how AI tools were used**:

- **Which tool**, e.g. Claude Code, ChatGPT, Copilot, Cursor. "None" is a fine answer.
- **What it did**: wrote code, wrote or rewrote the issue/PR text, diagnosed the
  problem or suggested a cause, or something else.

Every issue and pull request must also list **what you're unsure of or haven't
verified yourself**. Some examples: a root cause you're guessing at, a code path you
didn't run, a host or device you couldn't test on, or something an AI told you that
you didn't check. If there's nothing, write "nothing". Please don't guess or fill
this in to look thorough. An honest "I haven't checked this" is far more useful than
a confident-sounding guess, especially for AI-assisted diagnoses of
hardware-dependent behavior, which are often plausible and wrong.

Issues or PRs that leave these sections out, or turn out to have misrepresented
them, may be closed.
