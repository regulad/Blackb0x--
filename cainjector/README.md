# cainjector

A small armv7 tool that installs a modern CA root store onto an Apple TV 2/3,
plus the packaging that ships it as `xyz.regulad.blackb0x.cainjector`.

## Why

`apt-get update` against any `https://` source fails on this hardware, and the
transport is not the reason. `apt7-lib`'s `/usr/lib/apt/methods/http` (which
`https` is a symlink to) is built on CFNetwork — it imports
`CFReadStreamCreateForHTTPRequest` and `kCFStreamErrorDomainSSL`, links no
`libssl` at all, and dispatches on the scheme of the URI it is handed at
runtime. So it does a real TLS handshake through Apple's own SecureTransport.

What fails is trust. This firmware's built-in root store is compiled into
`Security.framework` itself (`certsTable.data` + `certsIndex.data` inside the
Apple-signed bundle; `/System/Library/Keychains` is empty — there is no
`SystemRootCertificates.keychain` on this OS at all), frozen at the firmware's
ship date. Measured against the one repo that needs it:

```
apt.awkwardtv.org
  leaf  CN=awkwardtv.org
    <-  C=US, O=Let's Encrypt, CN=YR2
    <-  C=US, O=ISRG, CN=Root YR
    <-  C=US, O=Internet Security Research Group, CN=ISRG Root X1
```

`ISRG Root X1` is from 2015. The firmware predates it.

## What it does

Calls `SecTrustStoreSetTrustSettings()` — the same mechanism a Configuration
Profile's certificate payload uses — to add each root to the **admin** trust
domain. Purely additive; no Apple-signed file is touched.

It also verifies its own work: every certificate is checked back with
`SecTrustStoreContains()` and the tool exits nonzero unless every one is
verifiably present. That is deliberate insurance, because the prototypes in
`SecTrustStorePrivate.h` are hand-written (see that file), so a wrong ABI
shows up as `0 of 121 certificates verified present` rather than as a silent
no-op that looks like success.

The same store is also dropped at `/usr/lib/ssl/cert.pem` for the vendored
`openssl 0.9.8zg`, which is a completely separate trust store that knows
nothing about Apple's.

## What it does NOT do

**It does not give anything TLS 1.2.** Trust and protocol are different
problems. `openssl 0.9.8zg` tops out at TLS 1.0 because the 0.9.8 branch never
implemented anything newer, and `curl`/`openssh` link it. This tool buys trust,
and only for consumers that were already capable of a modern handshake —
i.e. the CFNetwork/SecureTransport ones.

**It does not fix `curl` by file placement.** `libcurl.4.dylib` imports
`_SSL_CTX_load_verify_locations` but **not**
`_SSL_CTX_set_default_verify_paths`, and `curl-config --ca` is empty, so it was
built with no compiled-in bundle and never consults OpenSSL's defaults. curl
has to be told, so the package ships a profile drop-in:

```sh
# /etc/profile.d/blackb0x-cainjector.sh
export CURL_CA_BUNDLE=/usr/lib/ssl/cert.pem
```

A drop-in, not `/etc/profile` itself — that file belongs to the `profile.d`
package (already installed: line 48 of `package/packages.txt`, and `coreutils`
depends on it), whose `/etc/profile` is the loop that sources
`/etc/profile.d/*.sh`. `coreutils` and `less` ship drop-ins the same way.
Hence this package's `Depends: profile.d`.

**That covers shells only.** There is no global environment mechanism on this
device: `/etc/environment` is a `pam_env` convention and no `pam_env.so` ships
in either the `pam` or `pam-modules` package, and this firmware's `launchd`
has no reference to `/etc/launchd.conf` at all (its only `.conf` string is an
unrelated XPC label). A daemon gets nothing from the drop-in — which is why
the postinstall LaunchDaemon declares `CURL_CA_BUNDLE` in its own
`EnvironmentVariables`. Anything else that needs curl to validate should do
the same, or pass `--cacert /usr/lib/ssl/cert.pem`.

## Certificate store provenance

`ca-certificates.pem` is the concatenation of every
`usr/share/ca-certificates/mozilla/*.crt` from Debian's own `ca-certificates`
package — 121 roots, Mozilla's set as Debian ships it.

```
source:  http://deb.debian.org/debian/pool/main/c/ca-certificates/ca-certificates_20260816_all.deb
sha256:  1cb8b73c74c47677b329fad4095fb9370227d4b8f1969372411478a4586c46cf
certs:   121
```

To refresh it, fetch the current `ca-certificates_*_all.deb` from that pool,
`dpkg-deb -x` it, and `cat usr/share/ca-certificates/mozilla/*.crt` into this
file — then update the version, checksum and count above.

## Building

```sh
make -C cainjector                      # armv7 binary, ldid-signed
package/build_cainjector.sh out.deb 1.0 # the .deb
```

The binary needs a **pinned, era-matched Xcode** for the same reasons
`entrypoint/` does — Apple's current `ld` has no native 32-bit ARM support and
delegates to the deprecated `ld-classic`, and the SDK pairing is load-bearing
(`Xcode 6.4` → `iPhoneOS8.4.sdk` for AppleTV3,x; `Xcode 5.1.1` →
`iPhoneOS7.1.sdk` for AppleTV2,1). `cainjector/Makefile` carries the same
discovery logic; see its header. Theos's own SDKs cannot be used — the
vendored ones are arm64-only.

Packaging uses Theos's `dm.pl` exactly like `package/build.sh`, which is also
where the ownership reasoning lives (non-root `dm.pl` stamps `root:wheel` by
construction, which is what launchd requires of a plist or it silently refuses
to load it).

## Where the LaunchDaemon lives, and why it matters

`/Library/LaunchDaemons`, **not** `/System/Library/LaunchDaemons`. launchd only
ever scans the latter — but the binary is ad-hoc signed by us, so AMFI rejects
it on a clean boot, and a unit in the scanned directory would fire at exactly
the moment its program cannot execute. `/Library/LaunchDaemons` is loaded by
the untether's own `launchctl load` after it patches AMFI, which is the only
point in the boot where an unsigned third-party binary can run at all. This is
the same trap that killed an earlier loader unit outright — see
`docs/HISTORY.md`.
