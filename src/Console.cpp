//
//  Console.cpp
//  Blackb0x
//
//  See Console.hpp.
//

#include "Console.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {

// Recursive so Block can wrap out()/err() calls instead of needing a second,
// lock-free spelling of each.
std::recursive_mutex& consoleMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

}  // namespace

namespace console {

void init() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
}

void out(const char* fmt, ...) {
    std::lock_guard<std::recursive_mutex> lock(consoleMutex());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

void err(const char* fmt, ...) {
    std::lock_guard<std::recursive_mutex> lock(consoleMutex());
    // Anything already written to stdout was logically earlier than this;
    // stderr is unbuffered, so without this it overtakes whatever is still
    // sitting in stdout's buffer.
    fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

Block::Block() { consoleMutex().lock(); }

Block::~Block() { consoleMutex().unlock(); }

}  // namespace console
