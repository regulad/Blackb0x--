# appliancetvtweak -- a MobileSubstrate tweak, built with the same pinned,
# era-matched Xcode toolchain entrypoint/ and cainjector/ use.
#
# THIS IS A DELIBERATE COPY of cainjector/Makefile's toolchain discovery,
# which is itself a copy of entrypoint/Makefile's. entrypoint/Makefile is the
# canonical copy and carries the full reasoning; the short form is that
# Apple's current `ld` never implemented 32-bit ARM (it detects armv7 and
# delegates to the deprecated `ld-classic`), and that the Xcode/SDK pairing is
# load-bearing rather than cosmetic.
#
#   Xcode 6.4   -> iPhoneOS8.4.sdk  : AppleTV3,1 / AppleTV3,2 (12H1006)
#   Xcode 5.1.1 -> iPhoneOS7.1.sdk  : AppleTV2,1 (11D258)
#
# There are now three copies of this block. Factoring them onto a shared
# include is worth doing and is still deliberately not done here, for the
# reason cainjector/Makefile gives: entrypoint's build is the one thing
# between this project and a device that boots.
#
# WHY NOT THEOS, when this is a Substrate tweak and Theos is right there:
# Theos's compilation half cannot target armv7 on this machine. `theos/sdks/`
# holds only AppleTVOS12.4.sdk and iPhoneOS16.5.sdk, both arm64-only (Apple
# stopped shipping 32-bit iOS SDKs after ~2018), and `theos/toolchain/` is
# empty. package/build.sh's own header already records the consequence: Theos
# supplies the PACKAGING half (dm.pl, plain Perl, no SDK involved) and nothing
# else. That split is unchanged here.
#
# WHY NOT LOGOS either: logos.pl does exist ($THEOS/bin/logos.pl) and would
# work, but it adds a Perl preprocessing pass between source and compiler for
# a tweak with exactly ONE hook. MSHookMessageEx against substrate.h is the
# same thing Logos generates, minus the generator -- and it keeps this
# Makefile's relationship to its source the same plain `clang in, Mach-O out`
# that entrypoint/ and cainjector/ have.
#
# WHERE SUBSTRATE COMES FROM: the vendored deb, not the Theos checkout.
# debcache/mobilesubstrate_*.deb carries both the header
# (CydiaSubstrate.framework/Headers/CydiaSubstrate.h, which is substrate.h)
# and a real armv7 CydiaSubstrate binary exporting _MSHookMessageEx. Linking
# against that binary positionally -- exactly how entrypoint names its dylibs
# -- means the LC_LOAD_DYLIB lands as its own install name
# (/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate), which is
# where mobilesubstrate installs it on the device. Building against the
# artifact the device will actually load beats building against a copy in
# someone's home directory.

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
$(error appliancetvtweak: no pinned Xcode is mapped for DEVICE=$(DEVICE). Add an XCODE_FOR_$(DEVICE) line, or pass XCODE_VERSION= explicitly)
endif
endif
XCODE_VERSION ?= 6.4

IOS_MIN_FOR_6.4   := 8.4
IOS_MIN_FOR_5.1.1 := 7.1

IOS_MIN ?= $(IOS_MIN_FOR_$(strip $(XCODE_VERSION)))
ifeq ($(strip $(IOS_MIN)),)
$(error appliancetvtweak: no iPhoneOS SDK version is mapped for XCODE_VERSION=$(XCODE_VERSION). Add an IOS_MIN_FOR_$(XCODE_VERSION) line, or pass IOS_MIN= explicitly)
endif

IPHONEOS_SDK ?=

# Where the vendored Substrate deb lives, and where its framework gets
# unpacked for the build. Overridable so CI can hand over a pre-unpacked one.
DEBCACHE_DIR ?= $(CURDIR)/../debcache
SUBSTRATE_DEB ?= $(firstword $(wildcard $(DEBCACHE_DIR)/mobilesubstrate_*.deb))
SUBSTRATE_DIR := $(CURDIR)/.substrate

# armv7, matching every other native artifact in this project.
#
# -fobjc-arc is deliberately NOT set: this is manual retain/release, matching
# the era's idiom and avoiding any question of ARC runtime support on a 2015
# SDK. The tweak's allocations are autoreleased or explicitly released.
#
# -fvisibility=hidden because a tweak exports nothing; its only entry point is
# the constructor.
CFLAGS := -arch armv7 -marm -fno-stack-protector -Wall -Wextra -O2 \
          -fvisibility=hidden -fobjc-exceptions \
          -miphoneos-version-min=$(IOS_MIN)

# -dynamiclib, not an executable, and -nostdlib for the same reason the other
# two need it (an SDK root has no crt1.o). The install_name is where
# mobilesubstrate expects to find us; dyld never resolves it (Substrate
# dlopens the file by path) but a dylib with a wrong install name is the kind
# of thing that goes wrong much later and much less legibly.
LDFLAGS := -dynamiclib -nostdlib \
           -install_name /Library/MobileSubstrate/DynamicLibraries/appliancetvtweak.dylib

TWEAK_DYLIBS := \
	usr/lib/libSystem.B.dylib \
	usr/lib/libobjc.A.dylib \
	System/Library/Frameworks/CoreFoundation.framework/CoreFoundation \
	System/Library/Frameworks/Foundation.framework/Foundation

TWEAK_IDENTIFIER := xyz.regulad.blackb0x.appliancetvtweak

TARGET := appliancetvtweak.dylib

define tweak_compile
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
			|| echo "appliancetvtweak: warning: could not detach $$mounted" >&2; \
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
	echo "appliancetvtweak: XCODE_TOOLCHAIN=system -- building with the stock $(CC), pin bypassed"; \
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
			mounted=`mktemp -d "$${TMPDIR:-/tmp}/blackb0x-tweak-xcode-XXXXXX"`; \
			echo "appliancetvtweak: attaching $$d read-only"; \
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
		echo "appliancetvtweak: FATAL -- no pinned Xcode $$xv found, and there is no fallback." >&2; \
		echo "  Searched \$$XCODE_SEARCH_DIR = $$sd for Xcode_$$xv.app / Xcode_$$xv.dmg," >&2; \
		echo "  and already-mounted /Volumes/Xcode_$$xv." >&2; \
		echo "  Same requirement and same fixes as entrypoint/ -- see entrypoint/README.md." >&2; \
		echo "" >&2; \
		exit 1; \
	fi; \
else \
	tcbin=`resolve "$$xt"` || { \
		echo "appliancetvtweak: FATAL -- XCODE_TOOLCHAIN=$$xt is not a usable toolchain." >&2; \
		echo "  NOT falling back to the system compiler." >&2; \
		exit 1; \
	}; \
	tcroot="$$xt"; \
fi; \
if [ -z "$$sdk" ] && [ -n "$$tcroot" ]; then sdk=`resolve_sdk "$$tcroot"` || sdk=''; fi; \
if [ -z "$$sdk" ]; then \
	echo "" >&2; \
	echo "appliancetvtweak: FATAL -- no iPhoneOS $$iv SDK found, and this build needs one." >&2; \
	echo "  A current Xcode's iPhoneOS.sdk is arm64e-only; there is nothing in it to" >&2; \
	echo "  link an armv7 dylib against. Fix: make IPHONEOS_SDK=<path to iPhoneOS$$iv.sdk>" >&2; \
	echo "" >&2; \
	exit 1; \
fi; \
libs=''; \
for l in $(TWEAK_DYLIBS); do \
	[ -f "$$sdk/$$l" ] || { \
		echo "appliancetvtweak: FATAL -- $$sdk has no $$l." >&2; \
		exit 1; \
	}; \
	libs="$$libs $$sdk/$$l"; \
done; \
subbin='$(SUBSTRATE_DIR)/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate'; \
subinc='$(SUBSTRATE_DIR)/Library/Frameworks/CydiaSubstrate.framework/Headers'; \
[ -f "$$subbin" ] || { echo "appliancetvtweak: FATAL -- no CydiaSubstrate at $$subbin" >&2; exit 1; }; \
if [ -n "$$tcbin" ]; then \
	echo "appliancetvtweak: pinned toolchain $$tcbin"; \
	echo "appliancetvtweak: SDK $$sdk (-miphoneos-version-min=$$iv)"; \
	set -x; \
	"$$tcbin/clang" -B"$$tcbin" $(CFLAGS) -isysroot "$$sdk" -I"$$subinc" $(LDFLAGS) \
		-o '$@' '$<' $$libs "$$subbin"; \
else \
	echo "appliancetvtweak: SDK $$sdk (-miphoneos-version-min=$$iv)"; \
	set -x; \
	$(CC) $(CFLAGS) -isysroot "$$sdk" -I"$$subinc" $(LDFLAGS) \
		-o '$@' '$<' $$libs "$$subbin"; \
fi
endef

.PHONY: all clean substrate

all: $(TARGET)

# Unpack the vendored Substrate deb for its header and its armv7 binary.
# `substrate.h` is a symlink to the framework's own CydiaSubstrate.h, because
# that is the name the source includes and the name every tweak in this
# ecosystem includes.
$(SUBSTRATE_DIR)/.stamp:
	@[ -n "$(SUBSTRATE_DEB)" ] || { \
		echo "appliancetvtweak: FATAL -- no mobilesubstrate_*.deb in $(DEBCACHE_DIR)" >&2; \
		exit 1; }
	@rm -rf $(SUBSTRATE_DIR)
	@mkdir -p $(SUBSTRATE_DIR)
	dpkg-deb -x "$(SUBSTRATE_DEB)" $(SUBSTRATE_DIR)
	@ln -sf CydiaSubstrate.h \
		$(SUBSTRATE_DIR)/Library/Frameworks/CydiaSubstrate.framework/Headers/substrate.h
	@touch $@

substrate: $(SUBSTRATE_DIR)/.stamp

$(TARGET): appliancetvtweak.m $(SUBSTRATE_DIR)/.stamp
	@$(tweak_compile)
	$(LDID) -S -I$(TWEAK_IDENTIFIER) $@

clean:
	rm -rf $(TARGET) $(SUBSTRATE_DIR)
