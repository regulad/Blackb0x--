# entrypoint/assets/

Binary assets needed to build `entrypoint/`, kept out of git (see
`.gitignore`) because they're Apple's copyrighted material, not this
project's own. Sourced legitimately, but redistributing them in our own
repo/git history would be a different and inappropriate act from just
fetching them locally to build with — so this directory documents exactly
how to reproduce them instead of shipping them.

## `iPhoneOS6.1.sdk.tar.xz`

The iPhoneOS 6.1 SDK, needed to bootstrap `cctools-port`'s cross-toolchain
build (`entrypoint/README.md` step 3) — it hard-requires a real
`libSystem.dylib`/`.tbd` on disk to construct the `arm-apple-darwin11-clang`
wrapper, even though `entrypoint.c` itself is freestanding and won't link
against it.

**Source**: [Xcode 4.6](https://archive.org/details/xcode460417218a) on
archive.org — the official Apple developer tools DMG (1.7GB), preserved
there for exactly this kind of legacy-development use. Xcode 4.6 bundles the
iOS 6 SDK, matching AppleTV2,1/3,x's software range.

**Reproduction steps** (needs `7z` — `brew install p7zip`):

```sh
curl -L -o xcode460417218a.dmg \
  https://archive.org/download/xcode460417218a/xcode460417218a.dmg

# The DMG is a UDIF image wrapping a nested HFS+ volume (confirmed via the
# "koly" trailer in the last 512 bytes) containing "Xcode.app" directly
# (this era predates the separate .pkg installer format). p7zip can walk
# into the nested HFS+ volume without mounting anything.
mkdir -p _extract && cd _extract
7z x ../xcode460417218a.dmg \
  "Xcode/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS6.1.sdk" -r
cd ..
```

This ran under podman on a debian:bookworm-slim image for as long as the
host was an immutable Linux box that shouldn't have p7zip installed on it.
On macOS there is no podman, and `7z` is one `brew install` away, so it
just runs directly. `hdiutil attach` would also open this DMG natively, but
7z walks the nested HFS+ volume without mounting anything, which is both
faster and what the symlink note below is written against.

p7zip refuses 5 symlinks as "dangerous link path" (its own path-traversal
safety check, not a real problem with these — they're ordinary
versioned-library/relative-header symlinks). Recreate them manually using
the exact targets p7zip reports in its own error output:

```sh
cd _extract/Xcode/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS6.1.sdk
ln -sf "../nameser.h" usr/include/arpa/nameser.h
ln -sf "libcharset.1.0.0.dylib" usr/lib/libcharset.dylib
ln -sf "libiconv.2.4.0.dylib" usr/lib/libiconv.dylib
ln -sf "../../System/Library/Frameworks/IOKit.framework/Versions/A/IOKit" usr/lib/libIOKit.A.dylib
ln -sf "libstdc++.6.dylib" usr/lib/libstdc++.dylib
```

Then package it (the version number in the filename matters —
`cctools-port`'s `build.sh` parses it from there):

```sh
tar -cJf iPhoneOS6.1.sdk.tar.xz -C .../SDKs iPhoneOS6.1.sdk
```

516MB extracted, 3175 files, 121MB compressed. Verified `usr/lib/
libSystem{.B,}.dylib` present before packaging.
