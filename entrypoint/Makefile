# Plain Makefile — no Theos (see README.md for why: Theos's tool.mk brings a
# packaging/Logos apparatus that a binary spliced straight into a ramdisk has
# no use for).
#
# **This builds a DYNAMICALLY LINKED armv7 binary against the iPhoneOS SDK
# MATCHED TO THE DEVICE.** It used to build a freestanding armv6 one linking
# nothing at all. The reason for the change is in README.md and in
# entrypoint.c's header; the short form is that Apple's own /sbin/launchd —
# the PID 1 this binary's content replaces, and the one that demonstrably
# boots on this hardware — is LC_MAIN + LC_LOAD_DYLINKER against libSystem on
# both target ramdisks, so a dynamic PID 1 is the conservative artifact here.
#
# **The SDK pairing is load-bearing and is NOT interchangeable.** Matched is
# a 0-phantom-symbol match on both branches (iPhoneOS 7.1 vs the 11D258
# ramdisk is 0/0 across 6640 symbols; iPhoneOS 8.4 vs 12H1006 is 0 phantom).
# Crossed is catastrophic: an 8.4 SDK measured against a 7.1.2 device
# advertises 787 symbols the device does not export — the class that links
# clean and dies at dyld load, silently, on hardware with no console. That is
# what the XCODE_FOR_<device> / IOS_MIN_FOR_<xcode> tables below exist for.
#
# **This builds with a PINNED, era-appropriate Xcode toolchain by default.**
# Not the stock macOS one. The reason is in README.md's "What actually holds
# this up: `ld-classic`, not the SDK", and the short form is: Apple's current
# `ld` never implemented 32-bit ARM at all. It detects armv6/armv7/armv7s and
# silently delegates to `ld-classic`, a frozen ld64-957.1 fork Apple has
# called deprecated since Xcode 15. The day that is removed, this build
# breaks — and so would any other way of compiling ARM32 on a Mac, because
# they all route through that same linker. A pinned Xcode's own `ld64` carries
# armv6/armv7/armv7s as first-class NATIVE architectures in one binary, with
# no delegation at all, which is the whole hedge.
#
#   Xcode 6.4   -> iPhoneOS8.4.sdk, ld64-242.2  : AppleTV3,1 / AppleTV3,2 (12H1006)
#   Xcode 5.1.1 -> iPhoneOS7.1.sdk, ld64-236.4  : AppleTV2,1 (11D258)
#
# The stock toolchain still works and still produces a correct artifact
# (verified on Apple clang 21 / ld-1267: Mach-O armv6, LC_UNIXTHREAD, zero
# LC_LOAD_DYLIB, `_entry` as the thread-state PC). It is kept only as a
# deliberate, explicitly-requested escape hatch — `XCODE_TOOLCHAIN=system`.
# **It is never reached by accident.** If a pinned toolchain is asked for and
# cannot be resolved, this build FAILS; it does not quietly substitute the
# system compiler. A build that silently used the wrong compiler is the exact
# failure the pin exists to prevent.
#
# NOTE on XCODE_TOOLCHAIN=system now that this links an SDK: the stock
# escape hatch needs one more thing than it used to. A current Xcode's
# iPhoneOS.sdk is arm64e-only (`targets: [arm64e-ios]`), so there is nothing
# in it to link an armv7 binary against. Set IPHONEOS_SDK=<path> to a real
# armv7-bearing iPhoneOS SDK when using `system`; without it this fails and
# says so, rather than producing a binary for the wrong architecture.
#
# This used to demand a cctools-port cross-toolchain named
# `arm-apple-darwin11-clang`, bootstrapped against an iPhoneOS 6.1 SDK
# extracted from a 1.7GB archive.org copy of Xcode 4.6. That is a different
# thing from the pin above and is still not required; cctools-port exists to
# supply Apple's ld64/as to hosts that lack them, and this host is a Mac.
# `make XCODE_TOOLCHAIN=system CC=arm-apple-darwin11-clang` still works if you
# want it.
# `CC ?= clang` would NOT work here: make predefines CC as `cc`, so `?=`
# sees it already set and never assigns. Testing $(origin CC) is the
# idiomatic way to say "default to clang, but let the caller override" --
# `make CC=arm-apple-darwin11-clang` still wins. (On macOS `cc` is clang
# anyway, so the old spelling worked by accident; this just makes the
# documented default the real one.) CC is consulted ONLY on the
# XCODE_TOOLCHAIN=system path.
ifeq ($(origin CC),default)
CC := clang
endif
LDID ?= ldid

# ---------------------------------------------------------------------------
# Toolchain selection
# ---------------------------------------------------------------------------
# XCODE_TOOLCHAIN picks the compiler/linker/assembler. Every variable in this
# section is `?=`, so all of them are settable from the ENVIRONMENT as well as
# the command line — which is the whole plumbing story. BakeRamdisk.cpp's
# buildEntrypointBinary() runs `make clean all` through fork()/execvp(), which
# inherits `environ`, and make imports the environment as make variables, so
# `XCODE_TOOLCHAIN=/path bake-firmware ...` reaches here with no C++ involved.
# A CI step needs to set one environment variable and nothing else.
#
# Three states:
#
#   auto      (DEFAULT) discover a pinned Xcode under $(XCODE_SEARCH_DIR).
#             Accepts an already-extracted tree, an already-mounted volume, or
#             a whole .dmg, which is attached -nobrowse -readonly for the
#             build and detached afterwards.
#
#   <path>    use exactly this toolchain and NOTHING else — $(XCODE_SEARCH_DIR)
#             discovery is bypassed entirely, so a CI runner never goes looking
#             in a home directory that will not exist there. Two shapes, both
#             supported because local and CI will differ:
#               * an Xcode.app bundle, i.e.
#                 <path>/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang
#               * a bare extracted toolchain root — any directory containing
#                 usr/bin/clang. This is also the shape of an unpacked
#                 .xctoolchain, and it is what CI is expected to hand over
#                 rather than a whole 2.6 GB app bundle.
#             A directory that merely *contains* `Xcode.app` (a mounted DMG's
#             volume root) also resolves, since it falls out of the same
#             probe. A path that matches none of the three is a hard error
#             naming the path and every shape that was tried.
#
#   system    explicit opt-out: build with $(CC) off $PATH. The escape hatch,
#             never a fallback.
#
# What this deliberately does NOT know about: how the toolchain got here. No
# Git LFS, no split parts, no reassembly, no fetching, no cache. It is handed
# an Xcode that is already in one piece — a directory or a whole .dmg — and
# builds. Assembling one from parts is a CI concern that lives in CI.
XCODE_TOOLCHAIN ?= auto

# Where `auto` looks. Xcode 6.4 and 5.1.1 need to be available here for a bake
# to run. Overridable, because a CI runner has no such directory.
XCODE_SEARCH_DIR ?= $(HOME)/Downloads

# ---- Per-device toolchain selection -------------------------------------
# Load-bearing, not placeholder. The current build is freestanding and links
# no SDK at all, so today either pinned Xcode yields an equivalent artifact.
# The dynamic-linking conversion queued behind the pending hardware test does
# link an SDK, and the pairing is not interchangeable: a cross-version pairing
# measured 787 phantom symbols — the class that links clean and dies at load
# time. So the device -> Xcode mapping is real here and works now.
#
# Pass DEVICE=<model> to select by device, or XCODE_VERSION=<v> to select
# directly (XCODE_VERSION wins if both are given).
#
# buildEntrypointBinary() currently builds ONE entrypoint shared by every
# firmware target, so nothing sets DEVICE yet. When the SDK starts to matter
# that build moves inside BakeFirmware.cpp's per-firmware loop and begins
# setting it; the mapping lives here so that is a caller change, not a
# redesign.
XCODE_FOR_AppleTV2,1 := 5.1.1
XCODE_FOR_AppleTV3,1 := 6.4
XCODE_FOR_AppleTV3,2 := 6.4

DEVICE ?=
ifneq ($(strip $(DEVICE)),)
XCODE_VERSION ?= $(XCODE_FOR_$(strip $(DEVICE)))
ifeq ($(strip $(XCODE_VERSION)),)
$(error entrypoint: no pinned Xcode is mapped for DEVICE=$(DEVICE). Add an XCODE_FOR_$(DEVICE) line to entrypoint/Makefile, or pass XCODE_VERSION= explicitly)
endif
endif
XCODE_VERSION ?= 6.4

# ---- Per-Xcode iOS deployment target ------------------------------------
# The SDK a given Xcode carries, and therefore the -miphoneos-version-min
# this binary is stamped with. Kept as its own table rather than derived
# from XCODE_VERSION by string surgery, because the two numbers have no
# relationship to each other (Xcode 6.4 -> iPhoneOS 8.4; 5.1.1 -> 7.1) and a
# wrong guess here is the 787-phantom-symbol failure described at the top of
# this file.
IOS_MIN_FOR_6.4   := 8.4
IOS_MIN_FOR_5.1.1 := 7.1

IOS_MIN ?= $(IOS_MIN_FOR_$(strip $(XCODE_VERSION)))
ifeq ($(strip $(IOS_MIN)),)
$(error entrypoint: no iPhoneOS SDK version is mapped for XCODE_VERSION=$(XCODE_VERSION). Add an IOS_MIN_FOR_$(XCODE_VERSION) line to entrypoint/Makefile, or pass IOS_MIN= explicitly)
endif

# An explicit SDK path. Normally empty: the SDK is found inside whatever
# toolchain was resolved, which is what keeps the compiler and its headers
# from ever coming out of different Xcodes. Settable for XCODE_TOOLCHAIN=system
# (where there is no pinned Xcode to look inside) and for a cctools-port
# toolchain.
IPHONEOS_SDK ?=

# armv7, not armv6: every dylib on both ramdisks is armv7 THIN, so there is
# no armv6 slice to link against. (The old freestanding build got away with
# armv6 only because it linked nothing.) Both A4 and A5 Apple TVs are armv7.
#
# -marm, not Thumb. This used to be self-enforcing — the `rsbcs` in the
# hand-rolled syscall trampolines simply would not assemble outside an IT
# block — and with that asm gone it is not, so CI asserts ARM mode explicitly
# (LC_MAIN's entryoff carries ld64's Thumb bit in bit 0, so an odd entryoff
# is a Thumb entry point).
#
# -fno-stack-protector is kept from the freestanding build. __stack_chk_fail
# and __stack_chk_guard are both exported by both ramdisks, so it is not a
# requirement — it just keeps the undefined-symbol closure as small as it can
# be, which is the thing that has to hold at dyld load time.
CFLAGS := -arch armv7 -marm -fno-stack-protector -Wall -Wextra -O2 \
          -miphoneos-version-min=$(IOS_MIN)

# -nostdlib is REQUIRED on the link: an SDK root has no crt1.o, so without it
# ld fails with "library not found for -lSystem". It costs nothing — the
# output is still LC_MAIN with entryoff pointing straight at _main, which is
# exactly what dyld calls and what Apple's own launchd on both ramdisks uses.
# libSystem.B.dylib is then named as a positional input rather than via -l.
# Deliberately NOT here: -undefined dynamic_lookup (it would turn a missing
# symbol from a link error into a boot-time death) and -platform_version
# (-miphoneos-version-min already stamps LC_VERSION_MIN_IPHONEOS).
LDFLAGS := -nostdlib

# There is deliberately NO diagnostic switch here, and none should be added.
# This used to carry a -D that compiled a diagnostic in or out, and it was the
# wrong shape for this project: the ramdisk this binary lives inside is baked
# AHEAD OF TIME -- by CI, published as the firmware-<device> artifact,
# consumed verbatim by a `blackb0x` binary that links no ramdisk-baking code
# at all. So flipping a compile-time flag meant re-baking the whole ramdisk
# under sudo with Theos and afsctool, the ~30-minute job CI exists to spare
# users, just to arm one diagnostic.
#
# The runtime alternative that briefly replaced it is gone too. See
# entrypoint.c's header comment.

# entrypoint replaces /sbin/launchd's content directly (see
# BakeRamdisk.cpp's spliceFileContentInPlace() call), so it's signed under
# launchd's own well-known identifier rather than ldid's default (the
# binary's own filename). A mid-rewrite detour signed this as `com.apple.rc`
# and targeted /etc/rc.boot instead. That was reverted, and the reasoning
# recorded for it turned out to be wrong as well — the kernel never execs
# /etc/rc.boot on any firmware; launchctl does, downstream. See README.md's
# "What actually runs as PID 1" for the disassembly-backed account.
LAUNCHD_IDENTIFIER := com.apple.launchd

TARGET := entrypoint

# The compile step, as ONE shell command, because the pinned path has a mount
# lifecycle to own: whatever it attaches it detaches — on success, on failure,
# and on signal — and it never detaches a volume it did not attach. The owner
# routinely has Xcode volumes open while working; a leaked or stolen mount on
# their machine is not harmless the way it would be on a runner.
#
# Single code path on purpose: `tcbin` non-empty means pinned, empty means the
# explicitly-requested system compiler. There is no place here where an
# unresolved pinned toolchain can reach the `else`.
define entrypoint_compile
set -e; \
xt='$(strip $(XCODE_TOOLCHAIN))'; \
sd='$(strip $(XCODE_SEARCH_DIR))'; \
xv='$(strip $(XCODE_VERSION))'; \
iv='$(strip $(IOS_MIN))'; \
sdk='$(strip $(IPHONEOS_SDK))'; \
mounted=''; tcbin=''; tcroot=''; \
cleanup() { \
	{ set +x; } 2>/dev/null; \
	if [ -n "$$mounted" ]; then \
		hdiutil detach -quiet "$$mounted" >/dev/null 2>&1 \
			|| hdiutil detach -quiet -force "$$mounted" >/dev/null 2>&1 \
			|| echo "entrypoint: warning: could not detach $$mounted" >&2; \
		rmdir "$$mounted" >/dev/null 2>&1 || true; \
		mounted=''; \
	fi; \
}; \
trap cleanup EXIT INT TERM; \
resolve() { \
	if [ -x "$$1/usr/bin/clang" ]; then echo "$$1/usr/bin"; return 0; fi; \
	if [ -x "$$1/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang" ]; then \
		echo "$$1/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"; return 0; fi; \
	if [ -x "$$1/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang" ]; then \
		echo "$$1/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"; return 0; fi; \
	return 1; \
}; \
resolve_sdk() { \
	for c in "$$1/SDKs/iPhoneOS$$iv.sdk" "$$1/SDKs/iPhoneOS.sdk" \
	         "$$1/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS$$iv.sdk" \
	         "$$1/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk" \
	         "$$1/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS$$iv.sdk" \
	         "$$1/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk" \
	         "$$1/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS$$iv.sdk" \
	         "$$1/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk"; do \
		[ -f "$$c/usr/lib/libSystem.B.dylib" ] || continue; \
		echo "$$c"; return 0; \
	done; \
	return 1; \
}; \
version_ok() { \
	for p in "$$1/Contents/version.plist" "$$1/Xcode.app/Contents/version.plist"; do \
		[ -f "$$p" ] || continue; \
		got=`plutil -extract CFBundleShortVersionString raw -o - "$$p" 2>/dev/null`; \
		[ -n "$$got" ] || continue; \
		[ "$$got" = "$$2" ] || return 1; \
	done; \
	return 0; \
}; \
if [ "$$xt" = system ]; then \
	echo "entrypoint: XCODE_TOOLCHAIN=system — building with the stock $(CC), pin bypassed"; \
elif [ "$$xt" = auto ]; then \
	for c in "$$sd/Xcode_$$xv.app" "$$sd/Xcode-$$xv.app" "$$sd/Xcode_$$xv" "$$sd/xcode_$$xv" \
	         "/Volumes/Xcode_$$xv" "/Volumes/Xcode-$$xv" "/Volumes/Xcode"; do \
		[ -d "$$c" ] || continue; \
		version_ok "$$c" "$$xv" || continue; \
		if tcbin=`resolve "$$c"`; then tcroot="$$c"; break; fi; tcbin=''; \
	done; \
	if [ -z "$$tcbin" ]; then \
		for d in "$$sd/Xcode_$$xv.dmg" "$$sd/xcode_$$xv.dmg" "$$sd/Xcode-$$xv.dmg"; do \
			[ -f "$$d" ] || continue; \
			mounted=`mktemp -d "$${TMPDIR:-/tmp}/blackb0x-xcode-XXXXXX"`; \
			echo "entrypoint: attaching $$d read-only"; \
			if ! yes | PAGER=cat hdiutil attach -quiet -nobrowse -readonly -noverify -noautoopen \
			     -mountpoint "$$mounted" "$$d" >/dev/null 2>&1; then \
				rmdir "$$mounted" >/dev/null 2>&1 || true; mounted=''; continue; \
			fi; \
			if version_ok "$$mounted" "$$xv"; then \
				if tcbin=`resolve "$$mounted"`; then tcroot="$$mounted"; break; fi; \
			fi; \
			tcbin=''; cleanup; \
		done; \
	fi; \
	if [ -z "$$tcbin" ]; then \
		echo "" >&2; \
		echo "entrypoint: FATAL — no pinned Xcode $$xv found, and there is no fallback." >&2; \
		echo "" >&2; \
		echo "  Searched \$$XCODE_SEARCH_DIR = $$sd for:" >&2; \
		echo "    Xcode_$$xv.app, Xcode_$$xv, xcode_$$xv   (extracted tree or .xctoolchain root)" >&2; \
		echo "    Xcode_$$xv.dmg, xcode_$$xv.dmg           (whole disk image)" >&2; \
		echo "  and already-mounted /Volumes/Xcode_$$xv and /Volumes/Xcode." >&2; \
		echo "" >&2; \
		echo "  Fixes, in order of least effort:" >&2; \
		echo "    * put Xcode_6.4.dmg (AppleTV3,1 / AppleTV3,2) and Xcode_5.1.1.dmg" >&2; \
		echo "      (AppleTV2,1) in ~/Downloads. Both come from" >&2; \
		echo "      https://download.developer.apple.com/Developer_Tools/ and need an" >&2; \
		echo "      authenticated Apple ID — an anonymous fetch returns an HTML" >&2; \
		echo "      unauthorized page with HTTP 200, so check the payload, not the status." >&2; \
		echo "    * make XCODE_SEARCH_DIR=<dir>   to look somewhere else" >&2; \
		echo "    * make XCODE_TOOLCHAIN=<dir>    to name a toolchain outright" >&2; \
		echo "    * make XCODE_TOOLCHAIN=system   to deliberately use the stock compiler" >&2; \
		echo "" >&2; \
		echo "  See entrypoint/README.md, \"Pinned toolchain\"." >&2; \
		echo "" >&2; \
		exit 1; \
	fi; \
else \
	tcbin=`resolve "$$xt"` || { \
		echo "" >&2; \
		echo "entrypoint: FATAL — XCODE_TOOLCHAIN=$$xt is not a usable toolchain." >&2; \
		echo "  It is used verbatim (no ~/Downloads discovery), and none of the three" >&2; \
		echo "  accepted shapes is present under it:" >&2; \
		echo "    $$xt/usr/bin/clang" >&2; \
		echo "        (bare extracted toolchain root, or an unpacked .xctoolchain)" >&2; \
		echo "    $$xt/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang" >&2; \
		echo "        (an Xcode.app bundle)" >&2; \
		echo "    $$xt/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang" >&2; \
		echo "        (a directory containing one, e.g. a mounted DMG's volume root)" >&2; \
		echo "" >&2; \
		echo "  NOT falling back to the system compiler. Pass XCODE_TOOLCHAIN=system if" >&2; \
		echo "  that is genuinely what you want." >&2; \
		echo "" >&2; \
		exit 1; \
	}; \
	tcroot="$$xt"; \
fi; \
if [ -z "$$sdk" ] && [ -n "$$tcroot" ]; then sdk=`resolve_sdk "$$tcroot"` || sdk=''; fi; \
if [ -z "$$sdk" ]; then \
	echo "" >&2; \
	echo "entrypoint: FATAL — no iPhoneOS $$iv SDK found, and this build needs one." >&2; \
	echo "  entrypoint links against libSystem now (it used to be freestanding and" >&2; \
	echo "  link nothing), so it needs the SDK MATCHED to the target device:" >&2; \
	echo "    AppleTV2,1              -> Xcode 5.1.1 -> iPhoneOS 7.1" >&2; \
	echo "    AppleTV3,1 / AppleTV3,2 -> Xcode 6.4   -> iPhoneOS 8.4" >&2; \
	echo "  Crossing them is not a warning, it is 787 symbols that link clean and" >&2; \
	echo "  fail to bind at dyld load — silently, on hardware with no console." >&2; \
	if [ -n "$$tcroot" ]; then \
		echo "" >&2; \
		echo "  Looked under the resolved toolchain root $$tcroot for:" >&2; \
		echo "    SDKs/iPhoneOS$$iv.sdk, SDKs/iPhoneOS.sdk        (bare toolchain root)" >&2; \
		echo "    Platforms/iPhoneOS.platform/Developer/SDKs/...  (Xcode.app layout)" >&2; \
	else \
		echo "" >&2; \
		echo "  XCODE_TOOLCHAIN=system has no pinned Xcode to look inside, and a" >&2; \
		echo "  current Xcode's iPhoneOS.sdk is arm64e-only — there is nothing in it" >&2; \
		echo "  to link an armv7 binary against." >&2; \
	fi; \
	echo "" >&2; \
	echo "  Fix: make IPHONEOS_SDK=<path to an iPhoneOS$$iv.sdk>" >&2; \
	echo "" >&2; \
	exit 1; \
fi; \
if [ -n "$$tcbin" ]; then \
	echo "entrypoint: pinned toolchain $$tcbin"; \
	echo "entrypoint: SDK $$sdk (-miphoneos-version-min=$$iv)"; \
	"$$tcbin/ld" -v 2>&1 | head -1; \
	set -x; \
	"$$tcbin/clang" -B"$$tcbin" $(CFLAGS) -isysroot "$$sdk" $(LDFLAGS) \
		-o '$@' '$<' "$$sdk/usr/lib/libSystem.B.dylib"; \
else \
	echo "entrypoint: SDK $$sdk (-miphoneos-version-min=$$iv)"; \
	set -x; \
	$(CC) $(CFLAGS) -isysroot "$$sdk" $(LDFLAGS) \
		-o '$@' '$<' "$$sdk/usr/lib/libSystem.B.dylib"; \
fi
endef

.PHONY: all clean

all: $(TARGET)

$(TARGET): entrypoint.c
	@$(entrypoint_compile)
	$(LDID) -S -I$(LAUNCHD_IDENTIFIER) $@

clean:
	rm -f $(TARGET)
