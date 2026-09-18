//
//  Checkm8Pwn.h
//  blackb0x-pwn
//
//  Blackb0x's own original checkm8/SHAtter exploit implementations
//  (DeviceManager.m, git history — see docs/HISTORY.md's "Reopening macOS
//  support" entry), ported byte-for-byte to C. This is the project's only
//  checkm8 implementation. Builds on every platform, against whichever
//  backend libirecovery was configured with (IOKit on macOS, libusb
//  elsewhere) -- see this target's own block in CMakeLists.txt.
//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Both return 1 on success, 0 on failure -- same convention as the
// original Objective-C +SHAtter:/+checkm8: class methods they're ported
// from. ecid == 0 means "the first DFU-mode device found" (see get_tv()'s
// own comment in Checkm8Pwn.c).
int runSHAtter(uint64_t ecid);
int runCheckm8(uint64_t ecid);

#ifdef __cplusplus
}
#endif
