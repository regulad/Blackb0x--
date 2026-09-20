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

# There is deliberately NO diagnostic switch here, and none should be added.
# This used to carry a -D that compiled a diagnostic in or out, and it was the
# wrong shape for this project: the ramdisk this binary lives inside is baked
# AHEAD OF TIME -- by CI, published as the firmware-<device> artifact,
# consumed verbatim by a `blackb0x` binary that links no ramdisk-baking code
# at all. So flipping a compile-time flag meant re-baking the whole ramdisk
# under sudo with Theos and afsctool, the ~30-minute job CI exists to spare
# users, just to arm one diagnostic.
#
# Diagnostics are RUNTIME DIRECTIVES read out of the kernel boot-args now (see
# entrypoint/bootargs.h), so a stock CI-baked ramdisk can arm one without
# being rebuilt. Boot-args are compiled into the iBEC on this hardware, so
# arming one is a rootless, seconds-long re-bake of that instead:
#
#     ./build/bake-iboot --device <m> --build <b> --force \
#         --extra-boot-args "blackb0x.skip-install=1"
#
# and the binary built here is the same binary either way. Anything new that
# needs to be switchable on a device belongs there, not in a -D.

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

$(TARGET): entrypoint.c bootargs.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(LDID) -S -I$(LAUNCHD_IDENTIFIER) $@

clean:
	rm -f $(TARGET)
