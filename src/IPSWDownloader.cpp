//
//  IPSWDownloader.cpp
//  Blackb0x
//

#include "IPSWDownloader.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

// fragmentzip_download_file's callback is a plain C function pointer with no
// user-data parameter, so there is no way to smuggle a `this`/std::function
// pointer through it directly. Since the call is synchronous (it blocks the
// calling thread until the download finishes, with progress callbacks firing
// from that same thread), a thread_local slot set immediately before the
// call and cleared immediately after is sufficient — and unlike the
// original's 5 fixed global slots, this scales to any number of concurrent
// downloads as long as each runs on its own thread.
static thread_local std::function<void(unsigned int)>* t_activeProgressCallback = nullptr;

static void fragmentzip_progress_trampoline(unsigned int progress) {
    if (t_activeProgressCallback && *t_activeProgressCallback) {
        (*t_activeProgressCallback)(progress);
    }
}

FragmentDownloader::FragmentDownloader(std::string ipswUrl) : ipswUrl_(std::move(ipswUrl)) {}

FragmentDownloader::~FragmentDownloader() {
    if (ipsw_) fragmentzip_close(ipsw_);
}

bool FragmentDownloader::open() {
    ipsw_ = fragmentzip_open(ipswUrl_.c_str());
    return ipsw_ != nullptr;
}

bool FragmentDownloader::downloadComponent(const std::string& remotePath, const std::string& localPath,
                                            std::function<void(unsigned int)> onProgress) {
    if (!ipsw_) return false;

    std::error_code ec;
    fs::path out(localPath);
    if (out.has_parent_path()) {
        fs::create_directories(out.parent_path(), ec);
    }

    t_activeProgressCallback = &onProgress;
    int ret = fragmentzip_download_file(ipsw_, remotePath.c_str(), localPath.c_str(),
                                         onProgress ? fragmentzip_progress_trampoline : nullptr);
    t_activeProgressCallback = nullptr;

    return ret == 0;
}

std::string ipswDataRoot() {
    if (const char* xdg = getenv("XDG_DATA_HOME")) {
        return std::string(xdg) + "/blackb0x";
    }
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/blackb0x";
}
