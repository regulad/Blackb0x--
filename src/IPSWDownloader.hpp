//
//  IPSWDownloader.hpp
//  Blackb0x
//
//  CLI port of IPSWDownloader.h/.mm. Replaces FragmentDownloader's
//  fixed 5-slot design (dlone..dlfive), which only existed to drive 5
//  parallel NSProgressIndicator widgets, with one FragmentDownloader
//  instance per in-flight download whose progress is reported through a
//  callback the caller supplies. Concurrent downloads are just separate
//  instances driven from separate threads (std::thread/std::async) by the
//  caller — matching the original's one-dispatch-per-component concurrency,
//  just without a hardcoded slot limit.
//

#pragma once

#include <functional>
#include <string>

extern "C" {
#include <libfragmentzip/libfragmentzip.h>
}

class FragmentDownloader {
public:
    explicit FragmentDownloader(std::string ipswUrl);
    ~FragmentDownloader();

    FragmentDownloader(const FragmentDownloader&) = delete;
    FragmentDownloader& operator=(const FragmentDownloader&) = delete;

    // Opens the remote IPSW zip's central directory (fragmentzip_open).
    // Must succeed before downloadComponent() is called.
    bool open();

    // Downloads one file from within the (still-remote) IPSW zip to
    // localPath, creating parent directories as needed. onProgress (0-100),
    // if given, is invoked synchronously from within this call.
    bool downloadComponent(const std::string& remotePath, const std::string& localPath,
                            std::function<void(unsigned int)> onProgress = nullptr);

    const std::string& url() const { return ipswUrl_; }

private:
    std::string ipswUrl_;
    fragmentzip_t* ipsw_ = nullptr;
};

// Default local storage root for downloaded IPSW components — replaces the
// original's hardcoded "/Users/<name>/Documents/Blackb0x" with an
// XDG-respecting default.
std::string ipswDataRoot();
