//
//  BsdiffGlue.c
//  Blackb0x
//
//  See BsdiffGlue.h.
//

#include "BsdiffGlue.h"

#include <stdlib.h>

#include <bsdiff.h>
#include <bspatch.h>

struct writer {
    int (*write)(void*, const void*, int);
    void* opaque;
};

struct reader {
    int (*read)(void*, void*, int);
    void* opaque;
};

static int writeTrampoline(struct bsdiff_stream* stream, const void* buffer, int size) {
    struct writer* w = (struct writer*)stream->opaque;
    return w->write(w->opaque, buffer, size);
}

static int readTrampoline(const struct bspatch_stream* stream, void* buffer, int length) {
    const struct reader* r = (const struct reader*)stream->opaque;
    return r->read(r->opaque, buffer, length);
}

int blackb0x_bsdiff(const uint8_t* oldData, int64_t oldSize, const uint8_t* newData, int64_t newSize,
                    int (*write)(void* opaque, const void* buffer, int size), void* opaque) {
    struct writer w = {write, opaque};
    struct bsdiff_stream stream;
    stream.opaque = &w;
    stream.malloc = malloc;
    stream.free = free;
    stream.write = writeTrampoline;
    return bsdiff(oldData, oldSize, newData, newSize, &stream);
}

int blackb0x_bspatch(const uint8_t* oldData, int64_t oldSize, uint8_t* newData, int64_t newSize,
                     int (*read)(void* opaque, void* buffer, int length), void* opaque) {
    struct reader r = {read, opaque};
    struct bspatch_stream stream;
    stream.opaque = &r;
    stream.read = readTrampoline;
    return bspatch(oldData, oldSize, newData, newSize, &stream);
}
