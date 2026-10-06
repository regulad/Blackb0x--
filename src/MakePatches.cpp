//
//  MakePatches.cpp
//  Blackb0x
//
//  Standalone `make-patches`: turn a finished bake in dist/ into the patch
//  bundle CI publishes in its place. See FirmwarePatch.hpp for the scheme.
//
//  AUTHORING-only and rootless. With no --device/--build it processes every
//  tuple that has a Manifest-<tuple>.txt in <dist>, which is what CI runs.
//
//  Usage: make-patches [--device <model> --build <buildID>] [--dist <dir>] [--out <dir>] [--check]
//

#include "FirmwarePatch.hpp"
#include "ResourcePath.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static const char* kProg = "make-patches";

static void usage() {
    fprintf(stderr,
            "usage: %s [--device <model> --build <buildID>] [--dist <dir>] [--out <dir>] [--check]\n"
            "  Derives a patch bundle from a finished bake: one bsdiff per modified\n"
            "  component plus a Patches-<device>_<buildID>.plist index, round-tripped\n"
            "  against the baked files before anything is written. No Apple bytes land\n"
            "  in <out>. Defaults: <dist> = the checkout's dist/, <out> = patches/.\n"
            "  Without --device/--build, every tuple with a Manifest-*.txt in <dist>.\n"
            "  --check  then rebuild each suite from the bundle into an empty directory,\n"
            "           exactly as blackb0x will, and fail unless every file matches.\n",
            kProg);
}

int main(int argc, char** argv) {
    std::string device;
    std::string build;
    std::string distDir = resolveDistPath();
    std::string outDir = resolveRepoPath("patches");
    bool check = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--build") == 0 && i + 1 < argc) {
            build = argv[++i];
        } else if (strcmp(argv[i], "--dist") == 0 && i + 1 < argc) {
            distDir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (strcmp(argv[i], "--check") == 0) {
            check = true;
        } else {
            fprintf(stderr, "%s: unrecognized or incomplete argument: %s\n", kProg, argv[i]);
            usage();
            return 2;
        }
    }
    if (device.empty() != build.empty()) {
        usage();
        return 2;
    }

    std::vector<std::pair<std::string, std::string>> tuples;
    if (!device.empty()) {
        tuples.emplace_back(device, build);
    } else {
        // Manifest-<device>_<build>.txt. Device names contain a comma but never
        // an underscore, so the last underscore splits the pair.
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(distDir, ec)) {
            const std::string name = entry.path().filename().string();
            const std::string prefix = "Manifest-";
            const std::string ext = ".txt";
            if (name.size() <= prefix.size() + ext.size() || name.compare(0, prefix.size(), prefix) != 0 ||
                name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
                continue;
            }
            const std::string tuple = name.substr(prefix.size(), name.size() - prefix.size() - ext.size());
            const size_t us = tuple.rfind('_');
            if (us == std::string::npos) continue;
            tuples.emplace_back(tuple.substr(0, us), tuple.substr(us + 1));
        }
        if (tuples.empty()) {
            fprintf(stderr, "%s: no finished bakes (Manifest-*.txt) in %s\n", kProg, distDir.c_str());
            return 1;
        }
    }

    int failures = 0;
    for (const auto& t : tuples) {
        printf("%s %s:\n", t.first.c_str(), t.second.c_str());
        fflush(stdout);
        if (!fwpatch::makeSuitePatches(distDir, outDir, t.first, t.second)) {
            fprintf(stderr, "%s: FAILED for %s %s\n", kProg, t.first.c_str(), t.second.c_str());
            failures++;
            continue;
        }
        if (check) {
            // assembleSuite() checks every file against the hash recorded from
            // the bake, so success into an empty directory is the whole test.
            const fs::path scratch = fs::path(outDir) / (".check-" + t.first + "_" + t.second);
            std::error_code ec;
            fs::remove_all(scratch, ec);
            const bool ok = fwpatch::assembleSuite(outDir, scratch.string(), t.first, t.second);
            fs::remove_all(scratch, ec);
            if (!ok) {
                fprintf(stderr, "%s: --check FAILED: the bundle does not rebuild %s %s\n", kProg,
                        t.first.c_str(), t.second.c_str());
                failures++;
                continue;
            }
            printf("Checked: the bundle rebuilds %s %s exactly.\n", t.first.c_str(), t.second.c_str());
        }
    }
    return failures == 0 ? 0 : 1;
}
