//
//  Console.hpp
//  Blackb0x
//
//  One serialization point for everything the blackb0x CLI prints. Several
//  threads write to the terminal at once here: the main flow in Cli.cpp,
//  libirecovery's device-event thread (blackb0x_irecv_device_event_cb),
//  libimobiledevice's (blackb0x_idevice_event_cb), the detached
//  checkJailbreak threads newDevice() spawns, and whatever a spawned
//  pwntool streams back through runLineBufferedSubprocess(). stdio locks
//  each individual call, but that alone does not stop one of those lines
//  from landing in the middle of a partially-written line another source
//  has left sitting in the buffer.
//

#pragma once

namespace console {

// Forces stdout to line buffering. Pointed at a pipe or a file, stdio
// silently switches stdout to full block buffering while leaving stderr
// unbuffered, so the two streams come out reordered against each other
// purely because of where the output was going.
void init();

// printf/fprintf(stderr) equivalents that hold the shared lock and flush
// before releasing it, so a whole line always lands as one unit.
void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void err(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Holds that same lock across several out()/err() calls, for output that
// only reads correctly as one contiguous run of lines (the DFU
// instructions, the device-selection menu). Reentrant, so out()/err()
// inside a Block are fine.
class Block {
public:
    Block();
    ~Block();
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
};

}  // namespace console
