# Plain Makefile — no Theos (see README.md for why: this binary is
# freestanding, Theos's tool.mk assumes dynamic linking against libSystem).
#
# Builds with the STOCK macOS toolchain. Apple's own clang still has the ARM
# backend and Apple's own ld still advertises armv6 in its supported-arch
# list (`ld -v`), so `-arch armv6` produces exactly the artifact this needs
# with nothing installed beyond Xcode Command Line Tools and ldid. Verified
# on Apple clang 21 / ld-1267: Mach-O armv6, LC_UNIXTHREAD, zero
# LC_LOAD_DYLIB, _entry as the thread-state PC.
#
# This used to demand a cctools-port cross-toolchain named
# `arm-apple-darwin11-clang`, which in turn needed a real iPhoneOS 6.1 SDK
# extracted from a 1.7GB archive.org copy of Xcode 4.6. None of that is
# required: cctools-port exists to supply Apple's ld64/as on hosts that lack
# them, and this host is a Mac. Override CC if you want to use such a
# toolchain anyway (`make CC=arm-apple-darwin11-clang`) — it still works,
# it is just no longer the documented path.
# `CC ?= clang` would NOT work here: make predefines CC as `cc`, so `?=`
# sees it already set and never assigns. Testing $(origin CC) is the
# idiomatic way to say "default to clang, but let the caller override" --
# `make CC=arm-apple-darwin11-clang` still wins. (On macOS `cc` is clang
# anyway, so the old spelling worked by accident; this just makes the
# documented default the real one.)
ifeq ($(origin CC),default)
CC := clang
endif
LDID ?= ldid

CFLAGS := -arch armv6 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -Wall -Wextra -O2
LDFLAGS := -static -Wl,-e,_entry -nostartfiles

# Diagnostic reboot beacon (see entry() in entrypoint.c). Integer = the
# approximate pre-reboot delay in seconds; 0 (the default) compiles it out
# entirely, so the shipped binary is unaffected. `?=` is correct here --
# unlike CC, make does not predefine this name, and make auto-imports the
# environment, which is what makes the env-var spelling below work.
#
# Set it as an ENVIRONMENT VARIABLE, not `make BLACKB0X_REBOOT_BEACON=...`:
# BakeRamdisk.cpp runs `make clean all` itself via fork+execvp, which
# inherits environ, so
#
#     BLACKB0X_REBOOT_BEACON=10 ./build/blackb0x ...
#
# plumbs all the way through a normal jailbreak run with no C++ changes. A
# hand-run `make BLACKB0X_REBOOT_BEACON=10` would just be overwritten by
# that bake-time rebuild.
BLACKB0X_REBOOT_BEACON ?= 0
ifneq ($(BLACKB0X_REBOOT_BEACON),0)
CFLAGS += -DBLACKB0X_REBOOT_BEACON=$(BLACKB0X_REBOOT_BEACON)
endif

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

.PHONY: all clean

all: $(TARGET)

$(TARGET): entrypoint.c
ifneq ($(BLACKB0X_REBOOT_BEACON),0)
	@echo "*** WARNING: reboot beacon ON ($(BLACKB0X_REBOOT_BEACON)s) -- this binary"
	@echo "*** reboots immediately as PID 1 and installs NOTHING. Diagnostic only."
endif
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(LDID) -S -I$(LAUNCHD_IDENTIFIER) $@

clean:
	rm -f $(TARGET)
