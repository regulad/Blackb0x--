# cainjector -- build with the same pinned, era-matched Xcode toolchain
# entrypoint/ uses, for the same reasons.
#
# THIS IS A DELIBERATE COPY OF entrypoint/Makefile's TOOLCHAIN DISCOVERY, not
# an accident. entrypoint/Makefile is the canonical copy and carries the full
# reasoning; the short form is:
#
#   * Apple's current `ld` never implemented 32-bit ARM. It detects armv7 and
#     silently delegates to `ld-classic`, a frozen fork deprecated since
#     Xcode 15. A pinned Xcode's own ld64 carries armv7 as a first-class
#     native architecture with no delegation at all.
#   * The SDK pairing is load-bearing and NOT interchangeable. An 8.4 SDK
#     measured against a 7.1.2 device advertises 787 symbols the device does
#     not export -- the class of bug that links clean and dies at dyld load,
#     silently, on hardware with no console.
#
#   Xcode 6.4   -> iPhoneOS8.4.sdk, ld64-242.2  : AppleTV3,1 / AppleTV3,2 (12H1006)
#   Xcode 5.1.1 -> iPhoneOS7.1.sdk, ld64-236.4  : AppleTV2,1 (11D258)
#
# Factoring the two copies onto a shared include is worth doing and is
# deliberately NOT done here: entrypoint's build is the one thing between this
# project and a device that boots, and refactoring it to add a second consumer
# is a change to that path for the benefit of this one. Left as a follow-up.
#
# WHAT DIFFERS FROM entrypoint: this links Security.framework (for the
# SecTrustStore* calls) and CoreFoundation (CFData/CFRelease), and it is an
# ordinary LaunchDaemon-run program rather than something spliced into a
# ramdisk, so it is signed with its own identifier and nothing here cares
# about LC_UNIXTHREAD or freestanding-ness.

ifeq ($(origin CC),default)
CC := clang
endif
LDID ?= ldid

XCODE_TOOLCHAIN ?= auto
XCODE_SEARCH_DIR ?= $(HOME)/Downloads

XCODE_FOR_AppleTV2,1 := 5.1.1
XCODE_FOR_AppleTV3,1 := 6.4
XCODE_FOR_AppleTV3,2 := 6.4

DEVICE ?=
ifneq ($(strip $(DEVICE)),)
XCODE_VERSION ?= $(XCODE_FOR_$(strip $(DEVICE)))
ifeq ($(strip $(XCODE_VERSION)),)
$(error cainjector: no pinned Xcode is mapped for DEVICE=$(DEVICE). Add an XCODE_FOR_$(DEVICE) line, or pass XCODE_VERSION= explicitly)
endif
endif
XCODE_VERSION ?= 6.4

IOS_MIN_FOR_6.4   := 8.4
IOS_MIN_FOR_5.1.1 := 7.1

IOS_MIN ?= $(IOS_MIN_FOR_$(strip $(XCODE_VERSION)))
ifeq ($(strip $(IOS_MIN)),)
$(error cainjector: no iPhoneOS SDK version is mapped for XCODE_VERSION=$(XCODE_VERSION). Add an IOS_MIN_FOR_$(XCODE_VERSION) line, or pass IOS_MIN= explicitly)
endif

IPHONEOS_SDK ?=

# armv7, matching entrypoint: every dylib on these devices is armv7 THIN, so
# there is no other slice to link against.
CFLAGS := -arch armv7 -marm -fno-stack-protector -Wall -Wextra -O2 \
          -miphoneos-version-min=$(IOS_MIN)

# -nostdlib for the same reason entrypoint needs it: an SDK root has no
# crt1.o, so without it ld fails with "library not found for -lSystem". The
# output is still LC_MAIN with entryoff pointing at _main, which is what dyld
# calls. The dylibs are then named positionally.
LDFLAGS := -nostdlib

CAINJECTOR_DYLIBS := \
	usr/lib/libSystem.B.dylib \
	System/Library/Frameworks/CoreFoundation.framework/CoreFoundation \
	System/Library/Frameworks/Security.framework/Security

# The ad-hoc signing identifier, matching the LaunchDaemon plist's Label and
# the rest of this project's reverse-DNS prefix.
CAINJECTOR_IDENTIFIER := xyz.regulad.blackb0x.cainjector

TARGET := cainjector

define cainjector_compile
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
			|| echo "cainjector: warning: could not detach $$mounted" >&2; \
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
	         "$$1/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS$$iv.sdk" \
	         "$$1/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS$$iv.sdk"; do \
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
	echo "cainjector: XCODE_TOOLCHAIN=system -- building with the stock $(CC), pin bypassed"; \
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
			mounted=`mktemp -d "$${TMPDIR:-/tmp}/blackb0x-cainjector-xcode-XXXXXX"`; \
			echo "cainjector: attaching $$d read-only"; \
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
		echo "cainjector: FATAL -- no pinned Xcode $$xv found, and there is no fallback." >&2; \
		echo "  Searched \$$XCODE_SEARCH_DIR = $$sd for Xcode_$$xv.app / Xcode_$$xv.dmg," >&2; \
		echo "  and already-mounted /Volumes/Xcode_$$xv." >&2; \
		echo "  Same requirement and same fixes as entrypoint/ -- see entrypoint/README.md," >&2; \
		echo "  \"Pinned toolchain\". Or: make XCODE_TOOLCHAIN=system IPHONEOS_SDK=<path>" >&2; \
		echo "" >&2; \
		exit 1; \
	fi; \
else \
	tcbin=`resolve "$$xt"` || { \
		echo "cainjector: FATAL -- XCODE_TOOLCHAIN=$$xt is not a usable toolchain." >&2; \
		echo "  NOT falling back to the system compiler." >&2; \
		exit 1; \
	}; \
	tcroot="$$xt"; \
fi; \
if [ -z "$$sdk" ] && [ -n "$$tcroot" ]; then sdk=`resolve_sdk "$$tcroot"` || sdk=''; fi; \
if [ -z "$$sdk" ]; then \
	echo "" >&2; \
	echo "cainjector: FATAL -- no iPhoneOS $$iv SDK found, and this build needs one." >&2; \
	echo "  A current Xcode's iPhoneOS.sdk is arm64e-only; there is nothing in it to" >&2; \
	echo "  link an armv7 binary against. Fix: make IPHONEOS_SDK=<path to iPhoneOS$$iv.sdk>" >&2; \
	echo "" >&2; \
	exit 1; \
fi; \
libs=''; \
for l in $(CAINJECTOR_DYLIBS); do \
	[ -f "$$sdk/$$l" ] || { \
		echo "cainjector: FATAL -- $$sdk has no $$l." >&2; \
		exit 1; \
	}; \
	libs="$$libs $$sdk/$$l"; \
done; \
if [ -n "$$tcbin" ]; then \
	echo "cainjector: pinned toolchain $$tcbin"; \
	echo "cainjector: SDK $$sdk (-miphoneos-version-min=$$iv)"; \
	set -x; \
	"$$tcbin/clang" -B"$$tcbin" $(CFLAGS) -isysroot "$$sdk" $(LDFLAGS) \
		-o '$@' '$<' $$libs; \
else \
	echo "cainjector: SDK $$sdk (-miphoneos-version-min=$$iv)"; \
	set -x; \
	$(CC) $(CFLAGS) -isysroot "$$sdk" $(LDFLAGS) \
		-o '$@' '$<' $$libs; \
fi
endef

.PHONY: all clean

all: $(TARGET)

# The signature carries entitlements, and that is load-bearing rather than
# decorative: SecTrustStoreSetTrustSettings returns -34018
# (errSecMissingEntitlement) without `modify-anchor-certificates`, measured on
# real hardware across all 121 certificates. See entitlements.plist for where
# that name comes from (a literal in this firmware's own securityd) and why an
# ad-hoc signature can carry it at all (the untether's AMFI patch).
#
# `-S<file>` with no space is ldid's spelling -- `-S <file>` is a different
# thing (plain ad-hoc sign, with the path read as the binary).
$(TARGET): cainjector.c SecTrustStorePrivate.h entitlements.plist
	@$(cainjector_compile)
	$(LDID) -Sentitlements.plist -I$(CAINJECTOR_IDENTIFIER) $@

clean:
	rm -f $(TARGET)
