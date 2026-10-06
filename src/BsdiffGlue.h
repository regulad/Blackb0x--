//
//  BsdiffGlue.h
//  Blackb0x
//
//  C++-includable front for third_party/bsdiff. Upstream's own bsdiff.h and
//  bspatch.h name a parameter `new`, which is a keyword in C++, so they can
//  only be included from C -- this header and BsdiffGlue.c are that C. Same
//  two functions, with the stream structs flattened into a callback plus an
//  opaque pointer. Return 0 on success, as upstream does.
//

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int blackb0x_bsdiff(const uint8_t* oldData, int64_t oldSize, const uint8_t* newData, int64_t newSize,
                    int (*write)(void* opaque, const void* buffer, int size), void* opaque);

int blackb0x_bspatch(const uint8_t* oldData, int64_t oldSize, uint8_t* newData, int64_t newSize,
                     int (*read)(void* opaque, void* buffer, int length), void* opaque);

#ifdef __cplusplus
}
#endif
