cmake_minimum_required(VERSION 3.16)
project(blackb0x CXX C)

# ---------------------------------------------------------------------------
# Apple-only. This is a hard requirement, not a preference.
#
# Linux support was removed outright. checkm8 never worked there: the exploit
# runs to completion and the task-struct overwrite simply never lands, and four
# separate proposed causes were each refuted by measurement (host-stack
# limitation, overwrite acceptance, SETUP delivery, consumed-byte count) --
# docs/HISTORY.md carries the full trail. Every observable matched a working
# macOS run and the outcome still differed, so there was nothing left to fix
# from the host side.
#
# Fail here rather than deeper in: without this, a Linux configure gets a long
# way before dying inside a vendored ExternalProject, where the error has
# nothing to do with the actual reason.
# ---------------------------------------------------------------------------
if(NOT APPLE)
    message(FATAL_ERROR
        "blackb0x is macOS-only. Linux support was removed -- checkm8 does not "
        "work there (see docs/HISTORY.md). Build on macOS; Apple Silicon is the "
        "tested host.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

enable_testing()

find_package(PkgConfig REQUIRED)
# ZLIB used to be the one system/Homebrew-provided dependency left
# un-vendored (find_package(ZLIB), forced to its static archive) — it's a
# small, dependency-free leaf library, so there was never a correctness
# problem with that. It's vendored and built from source instead now
# (zlib_ext, below, alongside the other ExternalProject_Add(..._ext)
# targets), a leftover of a since-reverted experiment trying Fil-C (see
# docs/HISTORY.md) — Fil-C requires every object in the final binary to be
# compiled by its own compiler, with no exceptions, which is what forced
# zlib to stop being the one un-vendored piece. Kept vendored on reverting
# back to a normal compiler, by explicit request: it's more consistent
# with this project's own "every third-party dependency is a static
# archive built from source" goal (see "Full static linking" in docs/HISTORY.md)
# than reintroducing the one exception was. See zlib_ext's own comment
# further down for the ZLIB_FOUND/ZLIB_INCLUDE_DIR/etc FORCE-cache
# override that redirects find_package(ZLIB) callers, including xpwn's
# own vendored CMakeLists.txt files.
find_package(Threads REQUIRED)

# curl and libzip are vendored and built from source as static
# libraries below (curl_ext/libzip_ext), NOT found via
# find_package(CURL)/pkg_check_modules(LIBZIP)/pkg_check_modules(LIBUSB)
# the way earlier phases of this port did — see each ExternalProject_Add's
# own comment for why. No find_package/pkg_check_modules calls for them
# here at all, matching the "no find_package(OpenSSL)" pattern below.

# No find_package(OpenSSL) anywhere in this project, deliberately: wolfSSL
# (vendored below as wolfssl_ext, originally added for SSLv3 support — see
# that ExternalProject_Add's own comment) is this project's ONE crypto/TLS
# library. Every other vendored piece that wants "OpenSSL" (this project's
# own CertGen.cpp, xpwn's AES/SHA/HMAC image-crypto code, libimobiledevice's
# TLS layer) gets redirected to wolfSSL's OpenSSL-compatibility shim
# instead — see the cache-variable overrides right before
# add_subdirectory(third_party/xpwn) below, and the synthesized openssl.pc
# near libimobiledevice_ext, for exactly how each is redirected.
set(DEPS_PREFIX ${CMAKE_BINARY_DIR}/deps)
set(DEPS_INCLUDE ${DEPS_PREFIX}/include)
set(DEPS_LIB ${DEPS_PREFIX}/lib)
set(DEPS_PKGCONFIG ${DEPS_LIB}/pkgconfig)
file(MAKE_DIRECTORY ${DEPS_INCLUDE})
file(MAKE_DIRECTORY ${DEPS_PKGCONFIG})

# Synthetic openssl.pc: libimobiledevice_ext's own configure.ac probes for
# a pkg-config module literally named "openssl" (PKG_CHECK_MODULES(openssl,
# "openssl >= 0.9.8")) to decide whether/how to build its TLS layer. This
# redirects that probe to wolfSSL's shim without patching libimobiledevice's
# vendored configure.ac — DEPS_PKGCONFIG is already on every autotools
# sub-build's PKG_CONFIG_PATH via AUTOTOOLS_ENV below. Written at CMake
# configure time (not as part of any ExternalProject build step) so it's
# already in place before libimobiledevice_ext's own CONFIGURE_COMMAND runs.
# -include ${DEPS_INCLUDE}/wolfssl/options.h: unlike real OpenSSL's
# headers, wolfSSL's openssl/*.h shim headers are NOT self-contained —
# each one needs wolfssl/options.h (which brings in the wolfcrypt build
# config, e.g. wolfssl/wolfcrypt/settings.h) already processed before it,
# something every genuine wolfSSL-aware translation unit does but
# vendored code written against real OpenSSL (idevice.c, userpref.c) has
# no reason to do on its own. Force-including it via a compiler flag
# avoids patching that vendored source (confirmed necessary directly: a
# bare `#include <openssl/hmac.h>` without this fails with "unknown type
# name 'X509'"/"'BIGNUM'"/etc., since the shim's own type definitions
# never get pulled in without settings.h's macros already being defined).
file(WRITE ${DEPS_PKGCONFIG}/openssl.pc
"prefix=${DEPS_PREFIX}
libdir=${DEPS_LIB}
includedir=${DEPS_INCLUDE}/wolfssl

Name: OpenSSL
Description: Synthetic redirect to wolfSSL's OpenSSL-compatibility shim (see top-level CMakeLists.txt)
Version: 3.0.0
Libs: -L\${libdir} -lwolfssl
Cflags: -I\${includedir} -include ${DEPS_INCLUDE}/wolfssl/options.h
")

# ---------------------------------------------------------------------------
# Already-portable C sources kept in-tree (Blackb0x/Libraries) — built as-is.
# ---------------------------------------------------------------------------

# CBPatch.c/CBPUtils.c (cbpatcher) and finders.c/functions.c/patchers.c
# (iBoot32Patcher) were missing entirely from the repo — only their headers
# were vendored, matching the old prebuilt .a archives' headers-only pattern
# seen elsewhere in this port. Recovered from zzanehip/CBPatcher and
# zzanehip/iBoot32Patcher (the exact forks the README credits for "updated
# CBPatcher"/"updated iBoot32Patcher"), confirmed by exact/near-exact header
# match.
#
# BOTH have since moved OUT of Blackb0x/Libraries entirely: each is a
# submodule built as its own executable and fork/exec'd, for the same
# licensing reason — see their blocks below. Nothing recovered that way is
# still an in-tree source.

# CBPatcher: the kernel patcher, a SEPARATE EXECUTABLE that blackb0x
# fork/execs — not a static library linked into it.
#
# Same licensing constraint as iBoot32Patcher below, and it applied here all
# along without being noticed: zzanehip/CBPatcher is GPL-3.0, and it was a
# static library on blackb0x's own link line, which made the whole binary a
# GPLv3 derivative. The in-tree copy had no LICENSE file, which is how it went
# unexamined. Do not "simplify" this back into add_library().
#
# Unlike iBoot32Patcher, no fork was needed: upstream already ships a real
# main() (behind its own `#define MAIN`) whose CLI is a drop-in for the
# patch_kernel() call this project used to link —
#   cbpatcher <infile> <outfile> <version> [--nosb]
# with nukesb defaulting to 1, exactly what the old in-tree wrapper hardcoded.
# Returns 0 on success, -1 on failure.
#
# The whole local delta is therefore gone: the `#ifdef __APPLE__` that used to
# bracket CBPatch.c's Apple-only Mach-O includes, and the portable_macho.h that
# stood in for them, existed solely to build on Linux. With Linux support
# removed they are dead weight, so this is plain upstream at a pinned commit.
add_executable(CBPatcher
    third_party/CBPatcher/CBPatcher.c
    third_party/CBPatcher/CBPatch.c
    third_party/CBPatcher/CBPUtils.c)
target_include_directories(CBPatcher PRIVATE third_party/CBPatcher)

# iBoot32Patcher is a SEPARATE EXECUTABLE that blackb0x fork/execs -- not a
# static library linked into it. That is deliberate and is a licensing
# decision, not a packaging preference: iBoot32Patcher is GPL-3.0-or-later
# (iH8sn0w, 2013-2016) and blackb0x declares no license of its own, so
# statically linking it would make the whole binary a GPLv3 derivative.
# Invoking it as an independent program at arm's length keeps the two
# separate works. Do not "simplify" this back into add_library() -- see
# docs/HISTORY.md and Patcher.cpp's runIBoot32Patcher().
#
# Source is the submodule regulad/iBoot32Patcher@blackb0x, off
# zzanehip/iBoot32Patcher (the same fork the recovery note above pulled the
# in-tree copy from). The in-tree copy was a verbatim lift apart from one
# fix, so it carried no history and, critically, no LICENSE file; as a
# submodule the fixes are commits and upstream's own GPLv3 LICENSE travels
# with the code, which is what makes the arm's-length arrangement honest.
#
# The include directory is the submodule root, so upstream's own
# `#include <include/finders.h>` spelling resolves untouched -- the in-tree
# copy needed a sed to rewrite those to <libiboot32patcher/...>, and dropping
# that rewrite is what keeps this fork a clean diff against upstream.
add_executable(iBoot32Patcher
    third_party/iBoot32Patcher/iBoot32Patcher.c
    third_party/iBoot32Patcher/finders.c
    third_party/iBoot32Patcher/functions.c
    third_party/iBoot32Patcher/patchers.c)
target_include_directories(iBoot32Patcher PRIVATE third_party/iBoot32Patcher)

# Same vintage of C as xpwn (2013-2018), same GCC >= 14 tightened-diagnostics
# issue (see the blanket xpwn relaxation below) — this 32-bit-pointer-vs-
# 64-bit-host-pointer reverse-engineering code assigns pointers to uint32_t
# in a few places; not touching that logic, just relaxing the diagnostic.
target_compile_options(CBPatcher PRIVATE
    -Wno-error=implicit-int
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -Wno-error=return-mismatch
    -fcommon)
target_compile_options(iBoot32Patcher PRIVATE
    -Wno-error=implicit-int
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -Wno-error=return-mismatch
    -fcommon)

# xpwntool.c's decrypt() needs the full libxpwn implementation (AbstractFile,
# init_libxpwn, etc.) — linked below against third_party/xpwn's "xpwn" target
# once that's defined (see add_subdirectory(third_party/xpwn) below), since
# CMake targets must exist before they're referenced.
# Named blackb0x_xpwntool, not xpwntool — third_party/xpwn/ipsw-patch's own
# CMakeLists.txt already defines an executable target literally called
# "xpwntool" (its CLI tool of the same name), which collides otherwise.
add_library(blackb0x_xpwntool STATIC
    Blackb0x/Libraries/xpwntool.c)
target_include_directories(blackb0x_xpwntool PUBLIC Blackb0x/Libraries)

# idevicerestore's own img3.c/img3.h (src/img3.c, src/img3.h at
# libimobiledevice/idevicerestore's current HEAD), copied in nearly
# unchanged (see the file's own header comment for the diff) — the one
# piece --stock-securerom's personalization needs that libtatsu itself
# doesn't provide: stitching a TSS-response SHSH/ECID/CERT blob into an
# existing img3 file's own tags (img3_stitch_component()). Kept as its
# own small static library rather than folded into blackb0x's own
# sources, matching blackb0x_xpwntool above.
add_library(idevicerestore_img3 STATIC
    Blackb0x/Libraries/idevicerestore_img3.c)
target_include_directories(idevicerestore_img3 PUBLIC Blackb0x/Libraries)

# See deps::tatsu's own comment (near add_deps_imported_lib(tatsu ...)
# below) for why this exists: a handful of libplist convenience functions
# (plist_dict_copy_*()/plist_dict_get_bool()/plist_dict_get_uint()/
# plist_new_int()/plist_write_to_stream()) that libtatsu.a's tss.o calls
# but the historically-pinned libplist (deps::plist, third_party/libplist)
# doesn't implement -- faithfully ported from libplist-modern's own
# src/plist.c (see the file's own header comment for exactly which
# functions and the one behavioral simplification), built entirely on
# primitives deps::plist already has (plist_dict_get_item()/
# plist_dict_set_item()/plist_copy()/plist_get_*_val()/etc. -- confirmed
# unchanged across this whole version range already, per
# libimobiledevice_glue_ext's own comment above).
add_library(plist_compat STATIC
    Blackb0x/Libraries/libplist_compat.c)
# target_link_libraries(plist_compat PUBLIC deps::plist)/add_dependencies
# happen below, near add_deps_imported_lib(plist ...) -- same
# "target must exist first" reason as blackb0x_xpwntool's own PUBLIC xpwn
# link a few lines up.

# NOTE on provenance: Blackb0x/Libraries/xpwntool.c and libxpwntool/*.h were
# confirmed (by direct content diff) to come from zzanehip/xpwntool-swift @
# 58b69c595c (2020-09-13) — decrypt()'s body is byte-identical, only include
# paths/a header comment/one debug printf differ. This matches the README's
# own credit ("zzanehip - updated xpwntool (Created by planetbeing)"). That
# repo is a reduced xpwntool-only tree with no DMG/HFS+ construction code,
# matching the original app's own usage (it only ever called decrypt()).
#
# third_party/xpwn (planetbeing/xpwn, current HEAD) is therefore NOT a pin of
# something the original app used — it supplies NEW capability this port
# needs that the original never had: full libxpwn DMG/HFS+ construction
# (dmglib, hfsplus, img3, nor_files, etc.) to replace the hdiutil shell-outs
# in Patcher.mm (Phase 5). No historical version exists to pin here since the
# original app shelled out to hdiutil rather than linking any DMG library.
# Its own CMakeLists.txt (cmake_minimum_required 2.6) also builds unrelated
# tools (dfu-util, idevice, pwnmetheus2, dripwn) — pulled in wholesale rather
# than selectively.
# Redirect xpwn's own INCLUDE(FindOpenSSL) + FIND_LIBRARY(CRYPTO_LIBRARIES
# crypto ...) probes (see third_party/xpwn/dmg/CMakeLists.txt and
# ipsw-patch/CMakeLists.txt) to wolfSSL's OpenSSL-compatibility shim instead
# of a real system OpenSSL — see the "no find_package(OpenSSL)" comment
# near the top of this file for why. Both find_library() and the
# FindOpenSSL module treat an already-set CACHE entry as "already found"
# and skip their own search, so setting these here pre-empts xpwn's
# vendored CMakeLists.txt without patching it. wolfssl_ext itself isn't
# declared as a build target until later in this file, but that's fine —
# these are just path variables at CMake-configure time; the actual
# libwolfssl.a file only needs to exist by the time `make`/`ninja` links
# the `dmg`/`xpwn` targets, which add_dependencies(dmg/xpwn wolfssl_ext)
# near wolfssl_ext's own ExternalProject_Add enforces.
# The trailing `m`: wolfcrypt/src/dh.c (DH parameter checks) calls libm's
# pow()/log() — confirmed necessary directly, every xpwn executable that
# transitively links libwolfssl.a (hdutil, ipsw, xpwntool, imagetool,
# dripwn, dmg-bin) failed with "undefined reference to `pow'/`log'"
# without it. CRYPTO_LIBRARIES is a plain link-items list as far as
# target_link_libraries() is concerned, so appending the bare library
# name here (CMake turns it into -lm) covers every consumer at once
# rather than patching each xpwn CMakeLists.txt individually.
set(OPENSSL_FOUND TRUE CACHE BOOL "" FORCE)
set(OPENSSL_INCLUDE_DIR ${DEPS_INCLUDE}/wolfssl CACHE PATH "" FORCE)
set(OPENSSL_LIBRARIES ${DEPS_LIB}/libwolfssl.a CACHE STRING "" FORCE)
set(CRYPTO_LIBRARIES ${DEPS_LIB}/libwolfssl.a m CACHE STRING "" FORCE)

# ipsw-patch/CMakeLists.txt and minizip/CMakeLists.txt both run
# INCLUDE(FindBZip2)/INCLUDE(FindPNG)/INCLUDE(FindZLIB) directly (the
# older CMake style, not find_package()) to locate these three. Unlike
# the OPENSSL_* override above — which only forces the module's own
# public-facing result variables and leaves FindOpenSSL.cmake's internal
# OPENSSL_CRYPTO_LIBRARY/OPENSSL_SSL_LIBRARY cache entries unset, so the
# module still re-runs its own find_library() calls and silently
# recomputes a non-cache OPENSSL_LIBRARIES that happens to match by
# coincidence (documented at length near the OPENSSL_LIBRARIES use
# further down) — these force the exact internal cache variables
# FindBZip2.cmake/FindPNG.cmake/FindZLIB.cmake use (confirmed by reading
# all three modules directly), so the modules see already-populated cache
# entries and skip searching entirely, rather than relying on a lucky
# recomputation.
#
# ZLIB specifically is forced this way now (rather than left to a
# top-level find_package(ZLIB), as it was before vendoring — see the
# comment near the top of this file) purely to redirect callers at the
# vendored static archive instead of a system one, matching the
# BZip2/PNG pattern already established here.
set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
set(ZLIB_INCLUDE_DIR ${DEPS_INCLUDE} CACHE PATH "" FORCE)
set(ZLIB_LIBRARY ${DEPS_LIB}/libz.a CACHE FILEPATH "" FORCE)
set(ZLIB_LIBRARIES ${DEPS_LIB}/libz.a CACHE STRING "" FORCE)

set(BZIP2_FOUND TRUE CACHE BOOL "" FORCE)
set(BZIP2_INCLUDE_DIR ${DEPS_INCLUDE} CACHE PATH "" FORCE)
set(BZIP2_LIBRARY_RELEASE ${DEPS_LIB}/libbz2_static.a CACHE FILEPATH "" FORCE)
set(BZIP2_LIBRARIES ${DEPS_LIB}/libbz2_static.a CACHE STRING "" FORCE)

set(PNG_FOUND TRUE CACHE BOOL "" FORCE)
set(PNG_PNG_INCLUDE_DIR ${DEPS_INCLUDE} CACHE PATH "" FORCE)
set(PNG_INCLUDE_DIR ${DEPS_INCLUDE} CACHE PATH "" FORCE)
set(PNG_LIBRARY_RELEASE ${DEPS_LIB}/libpng16.a CACHE FILEPATH "" FORCE)
set(PNG_LIBRARY ${DEPS_LIB}/libpng16.a CACHE STRING "" FORCE)
set(PNG_LIBRARIES ${DEPS_LIB}/libpng16.a ${ZLIB_LIBRARY} CACHE STRING "" FORCE)

# bzip2/libpng are vendored (rather than relying on FindBZip2/FindPNG
# finding linuxbrew's shared .so, as xpwn's own ipsw-patch/CMakeLists.txt
# INCLUDE(FindBZip2)/INCLUDE(FindPNG) does by default) purely for the
# static-linking goal — no static libbz2.a/libpng.a exists anywhere with
# guaranteed availability, same rationale as libzip above.
# Declared here, before add_subdirectory(third_party/xpwn), rather than
# alongside the other ExternalProject_Add(..._ext) blocks further down —
# confirmed necessary directly: a later foreach() loop over every
# executable target xpwn's subdirectories define calls
# add_dependencies(${_t} bzip2_ext libpng_ext) to fix a real "No rule to
# make target 'deps/lib/libpng16.a'" build failure (xpwn's own static
# library re-exports ${BZIP2_LIBRARIES}/${PNG_LIBRARIES} PUBLIC to every
# consumer), and CMake silently drops an add_dependencies() call whose
# target doesn't exist yet instead of erroring — it must already exist
# by the time that loop runs.
include(ExternalProject)

# Vendored per the "keep it vendored" decision in the ZLIB comment near
# the top of this file — no historical pin applies (it was never a
# system/Homebrew dependency this project's original design cared about
# pinning), current HEAD of the canonical madler/zlib. Its own
# CMakeLists.txt always builds and installs both the shared (`zlib`/
# libz.so) and static (`zlibstatic`, installed as libz.a) targets
# unconditionally — there's no BUILD_SHARED_LIBS gate to turn the shared
# one off, unlike every other vendored CMake-based dependency here.
# Harmless: nothing links the .so, only ZLIB::ZLIB (declared below,
# pointing directly at libz.a) is ever referenced.
ExternalProject_Add(zlib_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/zlib
    CMAKE_ARGS      -DCMAKE_INSTALL_PREFIX=${DEPS_PREFIX}
                    -DCMAKE_INSTALL_LIBDIR=lib
    INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
)

ExternalProject_Add(bzip2_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/bzip2
    CMAKE_ARGS      -DCMAKE_INSTALL_PREFIX=${DEPS_PREFIX}
                    -DCMAKE_INSTALL_LIBDIR=lib
                    -DENABLE_LIB_ONLY=ON
                    -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=OFF
    INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
)

ExternalProject_Add(libpng_ext
    DEPENDS         zlib_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libpng
    CMAKE_ARGS      -DCMAKE_INSTALL_PREFIX=${DEPS_PREFIX}
                    -DCMAKE_INSTALL_LIBDIR=lib
                    -DZLIB_INCLUDE_DIR=${ZLIB_INCLUDE_DIR}
                    -DZLIB_LIBRARY=${ZLIB_LIBRARY}
                    -DPNG_SHARED=OFF -DPNG_STATIC=ON
                    -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
    INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(third_party/xpwn)
unset(CMAKE_POLICY_VERSION_MINIMUM)

target_link_libraries(blackb0x_xpwntool PUBLIC xpwn)
target_include_directories(blackb0x_xpwntool PRIVATE third_party/xpwn/includes)

# xpwn is circa-2010 C code that predates several diagnostics GCC >= 14
# tightened from warnings to hard errors by default (implicit int,
# implicit function declarations, incompatible pointer types, mismatched
# return types, etc). Relax those specifically for every target xpwn
# defines, rather than patching the vendored sources one file at a time.
set(_xpwn_subdirs common minizip dmg hdutil hfs ipsw-patch dfu-util xpwn idevice dripwn)
foreach(_d ${_xpwn_subdirs})
    get_property(_xpwn_targets DIRECTORY third_party/xpwn/${_d} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t ${_xpwn_targets})
        get_target_property(_t_type ${_t} TYPE)
        if(_t_type STREQUAL "STATIC_LIBRARY" OR _t_type STREQUAL "SHARED_LIBRARY" OR
           _t_type STREQUAL "MODULE_LIBRARY" OR _t_type STREQUAL "OBJECT_LIBRARY" OR
           _t_type STREQUAL "EXECUTABLE")
            target_compile_options(${_t} PRIVATE
                -Wno-error=implicit-int
                -Wno-error=implicit-function-declaration
                -Wno-error=incompatible-pointer-types
                -Wno-error=int-conversion
                -Wno-error=return-mismatch
                -fcommon)
        endif()
        if(_t_type STREQUAL "EXECUTABLE")
            # `xpwn` (the static library) links ${BZIP2_LIBRARIES}/${PNG_LIBRARIES}
            # as a plain (PUBLIC-by-default) target_link_libraries() call, so
            # CMake propagates those two paths into every executable's own
            # link command transitively. add_dependencies(xpwn bzip2_ext
            # libpng_ext) only orders `xpwn`'s own build step, not its
            # consumers' — confirmed directly: a clean build failed with
            # "No rule to make target 'deps/lib/libpng16.a'" for xpwntool,
            # imagetool, ipsw, and dripwn specifically, none of which had
            # this dependency. Applying it to every executable in xpwn's
            # subdirectories, needed or not, is simpler and harmless.
            add_dependencies(${_t} bzip2_ext libpng_ext)
            if(APPLE)
                # Same real macOS link failure blackb0x's own
                # target_link_libraries(blackb0x ...) block was fixed for
                # (see that comment for the full explanation): every one of
                # these executables also links ${CRYPTO_LIBRARIES} (=
                # libwolfssl.a, via the OPENSSL_LIBRARIES/CRYPTO_LIBRARIES
                # cache-variable redirect near the top of this file), and
                # once ANY symbol from wolfSSL's internal.o/ssl.o gets
                # pulled in, the whole object file comes with it --
                # including DoAppleNativeCertValidation/LoadSystemCaCertsMac,
                # which need CoreFoundation/Security regardless of whether
                # this specific tool ever calls into them. Confirmed for
                # real against dmg-bin specifically; applying to every
                # executable here rather than enumerating exactly which
                # ones hit it, same reasoning as the bzip2_ext/libpng_ext
                # dependency two lines up — none of these tools are
                # actually used by blackb0x/bake-all-ramdisks at runtime
                # (only the `xpwn` library and blackb0x_xpwntool are), so
                # there's no harm in over-applying this.
                #
                # Plain signature, not PRIVATE: every one of xpwn's own
                # CMakeLists.txt files already links these same targets
                # with the old plain (non-keyword) signature
                # (`target_link_libraries(dmg-bin dmg)` etc.) — CMake
                # hard-errors ("has already been used with the target...
                # All uses... must be either all-keyword or all-plain") the
                # moment a second call on the same target mixes styles,
                # confirmed for real against every target this loop
                # touches, not just one.
                target_link_libraries(${_t}
                    "-framework CoreFoundation"
                    "-framework Security")
            endif()
        endif()
    endforeach()
endforeach()

add_dependencies(blackb0x_xpwntool bzip2_ext libpng_ext zlib_ext)

# ---------------------------------------------------------------------------
# Vendored dependencies built from source (git submodules under third_party/)
# ---------------------------------------------------------------------------

# DEPS_PREFIX/DEPS_INCLUDE/DEPS_LIB/DEPS_PKGCONFIG are declared near the
# top of this file now (before add_subdirectory(third_party/xpwn)), since
# the OpenSSL-redirect cache variables set there need DEPS_INCLUDE/DEPS_LIB
# to already have real values.

set(AUTOTOOLS_ENV ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${DEPS_PKGCONFIG}")

set(DEPS_MODERN_PREFIX ${CMAKE_BINARY_DIR}/deps-modern)
set(DEPS_MODERN_PKGCONFIG ${DEPS_MODERN_PREFIX}/lib/pkgconfig)


# ENABLE_OPENSSL/ENABLE_COMMONCRYPTO/ENABLE_WINDOWS_CRYPTO OFF: libzip only
# needs OpenSSL/CommonCrypto at all for reading/writing AES-encrypted zip
# entries, a feature this project has no use for (it only ever reads a
# remote IPSW's plain, unencrypted central directory via libfragmentzip) —
# disabling it sidesteps yet another OpenSSL entanglement entirely rather
# than routing a third library through wolfSSL's shim.
# ENABLE_BZIP2/ENABLE_LZMA/ENABLE_ZSTD OFF: IPSW zips use plain DEFLATE:
# minimizes the static dependency footprint, not a functional requirement
# either way.
ExternalProject_Add(libzip_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libzip
    CMAKE_ARGS      -DCMAKE_INSTALL_PREFIX=${DEPS_PREFIX}
                    -DCMAKE_INSTALL_LIBDIR=lib
                    -DCMAKE_PREFIX_PATH=${DEPS_PREFIX}
                    -DBUILD_SHARED_LIBS=OFF
                    -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF
                    -DENABLE_OPENSSL=OFF -DENABLE_COMMONCRYPTO=OFF
                    -DENABLE_WINDOWS_CRYPTO=OFF
                    -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF
    INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
)

ExternalProject_Add(libplist_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libplist
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${AUTOTOOLS_ENV} ./autogen.sh --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared
                        --without-cython
    BUILD_COMMAND   ${AUTOTOOLS_ENV} make -j
    INSTALL_COMMAND make install
)

ExternalProject_Add(libusbmuxd_ext
    DEPENDS         libplist_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libusbmuxd
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${AUTOTOOLS_ENV} ./autogen.sh --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared
    BUILD_COMMAND   ${AUTOTOOLS_ENV} make -j
    # libusbmuxd's common/collection.o and libimobiledevice-glue's
    # src/collection.o (see libimobiledevice_glue_ext below) are both
    # externally-linked copies of the exact same generic collection ADT —
    # diffed directly: identical `struct collection` layout, byte-identical
    # function bodies, glue's is a strict superset (adds collection_copy(),
    # which nothing in libusbmuxd itself calls). Rather than paper over the
    # resulting duplicate-symbol link error with the GNU-ld-only
    # -Wl,--allow-multiple-definition (no equivalent on Apple's ld64 as of
    # Xcode 15+ -- see the blackb0x target's own link options below and
    # docs/HISTORY.md), drop libusbmuxd's copy of the object from its
    # installed archive here so only one definition of each symbol ever
    # reaches the final link, on every platform -- not just a Linux
    # workaround.
    INSTALL_COMMAND make install
              COMMAND ${CMAKE_AR} d ${DEPS_LIB}/libusbmuxd.a collection.o
)

# This pinned commit's own autogen.sh fails to bootstrap libtool correctly on
# a modern toolchain ("required file './ltmain.sh' not found"), unlike the
# other pinned libraries here. autoreconf -fi is the standard, robust
# replacement for a fragile vendored autogen.sh and regenerates the same
# files autogen.sh is meant to produce.
# Only build/install the library itself, not the bundled CLI tools/ (idevice_id,
# idevicebackup2, ideviceinfo, etc.) — Blackb0x doesn't use them, and several
# fail to link against this system's OpenSSL for reasons unrelated to the
# library itself.
#
# configure.ac's AC_CHECK_MEMBER/AC_TRY_COMPILE probes of usbmuxd.h (checking
# libusbmuxd is new enough) run before PKG_CHECK_MODULES(libusbmuxd)'s CFLAGS
# get folded into CPPFLAGS, so without an explicit -I${DEPS_INCLUDE} these
# probes can't find our built usbmuxd.h at all and fail with a misleading
# "not up-to-date" message that's actually "header not found".
set(LIBIMOBILEDEVICE_LIB_SUBDIRS "common src include")

# wolfSSL, built with genuine SSLv3 support (--enable-sslv3) — needed
# because the ~2010s-era embedded lockdownd on Normal-mode Apple TV 2/3
# hardware only speaks SSLv3, and that protocol's actual implementation was
# removed from real OpenSSL entirely years ago (not just disabled). wolfSSL
# is a currently-maintained library that still supports SSLv3 specifically
# for legacy/embedded compatibility use cases like this one — see
# third_party/libimobiledevice/src/idevice.c's
# LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_WOLFSSL branch for what actually uses
# it natively, and docs/HISTORY.md for why mbedTLS/GnuTLS were ruled out for this
# specific job (both dropped SSLv3 outright, years ago — GnuTLS support
# stays intact and selectable in the fork regardless, see idevice.c and
# configure.ac). --enable-arc4/--enable-md5 (both off by default) are
# needed too: a genuinely ancient SSLv3 peer like this is expected to only
# offer legacy RC4-MD5-family cipher suites.
#
# wolfSSL is also this project's ONE crypto/TLS library, full stop — there
# is no find_package(OpenSSL) anywhere in this file, deliberately. Every
# other call site that would otherwise want real OpenSSL (xpwn's AES/SHA/
# HMAC image-crypto code, and libimobiledevice's own common/userpref.c cert
# generation, built right after this block) compiles and links against
# wolfSSL's OpenSSL-compatibility shim instead (wolfssl/openssl/*.h headers
# resolving bare `#include <openssl/...>`, backed by wolfSSL's own
# implementation) — see the `OPENSSL_INCLUDE_DIR`/`OPENSSL_LIBRARIES`/
# `CRYPTO_LIBRARIES` cache-variable overrides before
# add_subdirectory(third_party/xpwn) above, and the synthesized
# ${DEPS_PKGCONFIG}/openssl.pc near the top of this file, for how each
# vendored build gets redirected. Rationale: this project already carries
# one from-scratch-vendored, statically-built, legacy-hardware-focused TLS
# library (wolfSSL itself) specifically because "ancient hardware, pick
# whatever crypto/TLS version it actually needs" is this project's entire
# theme — consolidating onto that one already-vendored stack avoids also
# depending on whatever OpenSSL happens to be installed on the build host,
# exactly the kind of "dependency versioning is insane" problem this
# project works around everywhere else already, and is a prerequisite for
# every project binary being fully statically linked (see the
# static-linking notes on the `blackb0x` target below).
# Declared here, before libimobiledevice_ext, specifically so
# libimobiledevice_ext's own DEPENDS can reference it by name (CMake
# target references must come after the target's declaration).
#
# --enable-debug: needed for wolfSSL_Debugging_ON()'s internal
# WOLFSSL_MSG/WOLFSSL_ERROR tracing (a no-op without it) — added while
# debugging why our own OpenSSL-generated PEM certs were initially
# rejected with the unhelpfully generic WOLFSSL_BAD_FILE (-463). Debug
# builds are slower, not smaller — fine for this tool (interactive CLI,
# not a hot loop), but worth revisiting if this ever needs trimming down
# for packaging.
# --enable-opensslall: the broad OpenSSL-compatibility surface — needed
# now that wolfSSL's shim headers are the ONLY `<openssl/...>`
# implementation in the whole project, not just an occasional diagnostic
# aid; also what exposes wolfSSL_ERR_get_error()'s real error queue (used
# in LegacyLockdownClient.cpp).
# --enable-certgen --enable-keygen: X.509 certificate generation and RSA
# key generation via the openssl-compat surface (X509_sign,
# RSA_generate_key_ex, etc.) — needed by CertGen.cpp's lockdown-pairing
# identity generation.
# --enable-des3: needed for third_party/xpwn's dmg/filevault.c
# (<openssl/evp.h>-based FileVault DMG support — unrelated to the actual
# jailbreak/exploit path, but part of what gets compiled).
# --enable-oldtls --enable-tlsv10: libimobiledevice's idevice.c has an
# `#if OPENSSL_VERSION_NUMBER < 0x10100000L` compatibility branch that
# calls `TLSv1_method()` — wolfSSL's shim reports itself as pre-1.1.0, so
# this branch is what actually gets compiled. `wolfTLSv1_method`'s
# implementation (not just its declaration) is specifically gated on
# `WOLFSSL_ALLOW_TLSV10` (confirmed by reading src/tls.c's guard
# directly), which only --enable-tlsv10 itself defines — --enable-oldtls
# alone (which just avoids `NO_OLD_TLS`) is not sufficient.
# --enable-certreq: wolfSSL_X509V3_set_ctx() (used by CertGen.cpp,
# transitively via X509_add_ext_helper's X509V3_set_ctx) is gated on
# `WOLFSSL_CERT_GEN && WOLFSSL_CERT_REQ` together (confirmed by reading
# src/x509.c's guard directly) — --enable-certgen alone isn't enough.
# LIBS=-lm: wolfcrypt/src/dh.c uses libm's pow()/log() for DH parameter
# checks; without explicitly linking libm, wolfSSL's OWN internal test/
# example binaries (built as part of its default `make` "all" target,
# though never installed or used by this project) fail at link time.
ExternalProject_Add(wolfssl_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/wolfssl
    BUILD_IN_SOURCE 1
    # WOLFSSL_STATIC_RSA: every genuine SSLv3-era cipher suite (RC4-MD5,
    # RC4-SHA, DES-CBC3-SHA, etc.) uses classic non-ephemeral RSA key
    # exchange, which wolfSSL compiles out by default (deprecated on
    # security grounds) unless this macro is defined — confirmed directly
    # in wolfssl/internal.h's BUILD_SSL_RSA_WITH_RC4_128_MD5 guard. There's
    # no dedicated --enable-staticrsa configure flag; the only place this
    # macro appears in configure.ac is bundled into the unrelated
    # --enable-qt-test path, which ALSO forces -DOPENSSL_NO_SSL3 (exactly
    # what we don't want), so it's set directly via EXTRA_CFLAGS instead.
    # Without it, wolfSSL has zero usable suites for real SSLv3 and sends
    # an empty-cipher-list ClientHello, which real hardware immediately
    # RSTs — confirmed directly against a real Apple TV (48-byte ClientHello
    # observed on the wire, then a TCP RST from the device 2ms later).
    #
    # WOLFSSL_ASN_ALLOW_0_SERIAL: real libimobiledevice's own common/userpref.c
    # generates every pairing cert (root/host/device) with serial 0
    # (ASN1_INTEGER_set(sn, 0)) on its OpenSSL code path. wolfSSL otherwise
    # enforces RFC 5280 4.1.2.2 (positive serials) and rejects serial 0 on any
    # non-self-signed-CA cert (wolfcrypt/src/asn.c, confirmed directly:
    # "Error serial number of 0 for non-root certificate", -463/ASN_PARSE_E)
    # — this exact macro is wolfSSL's own documented escape hatch for
    # existing certs like these that predate that RFC being enforced, rather
    # than patching userpref.c's cert generation to diverge from upstream.
    #
    # WOLFSSL_ALT_CERT_CHAINS: needed for curl's own (unrelated to the
    # Apple TV/idevice.c SSLv3 story above) HTTPS calls to api.ipsw.me —
    # wolfSSL's default chain validation requires each certificate's
    # issuer to exactly match the next certificate's subject, all the way
    # to a trusted root, and refuses anything else (unlike OpenSSL, which
    # tries harder to build an alternate valid path). api.ipsw.me's real
    # chain (CN=ipsw.me -> Google Trust Services WE1 -> GTS Root R4) hit
    # exactly this: confirmed directly via a throwaway wolfSSL_connect()
    # test program with wolfSSL_Debugging_ON(), which failed with
    # ASN_NO_SIGNER_E (-188, "Cert subject hash does not match issuer
    # hash... Consider using WOLFSSL_ALT_CERT_CHAINS") even though the
    # correct root (GTS Root R4) was present and independently confirmed
    # loadable in this system's CA bundle. This is wolfSSL's own
    # documented macro for exactly this ("Allow non-validated intermediate
    # CAs", src/internal.c) — no configure flag for it either, same
    # EXTRA_CFLAGS treatment as the other two macros here.
    CONFIGURE_COMMAND ${AUTOTOOLS_ENV} ./autogen.sh
              COMMAND ${AUTOTOOLS_ENV} ./configure --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared
                        --enable-sslv3 --enable-oldtls --enable-tlsv10
                        --enable-arc4 --enable-md5
                        --enable-debug --enable-opensslall
                        --enable-certgen --enable-certreq --enable-keygen
                        --enable-des3 LIBS=-lm
                        EXTRA_CFLAGS=-DWOLFSSL_STATIC_RSA\ -DWOLFSSL_ASN_ALLOW_0_SERIAL\ -DWOLFSSL_ALT_CERT_CHAINS
    BUILD_COMMAND   ${AUTOTOOLS_ENV} make -j
    INSTALL_COMMAND make install
)

# curl, built statically against wolfSSL rather than real OpenSSL (same
# "one crypto/TLS library in the whole binary" reasoning as the rest of
# this file) and with every optional protocol/feature this project has no
# use for disabled outright — a distro-provided static libcurl.a (checked
# directly: this system's own prebuilt one pulls in real OpenSSL, ldap,
# libssh2, krb5/gssapi, brotli, zstd, nghttp2/3, and ngtcp2, none of which
# IPSW.cpp's plain HTTPS GET calls need) would both reintroduce real
# OpenSSL and balloon the static binary far beyond what's actually used.
# CMAKE_PREFIX_PATH=${DEPS_PREFIX}: curl's own CMake/FindWolfSSL.cmake
# does a plain find_path/find_library for wolfssl/ssl.h and libwolfssl —
# this is what points that at our own already-built wolfSSL instead of
# (failing to find, since none exists) a system one.
# ENABLE_CURL_MANUAL=OFF: avoids an unrelated Perl build-time dependency
# for generating the curl CLI tool's man page — moot anyway since
# BUILD_CURL_EXE=OFF means that tool isn't even built.
ExternalProject_Add(curl_ext
    DEPENDS         wolfssl_ext zlib_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/curl
    CMAKE_ARGS      -DCMAKE_INSTALL_PREFIX=${DEPS_PREFIX}
                    -DCMAKE_INSTALL_LIBDIR=lib
                    -DCMAKE_PREFIX_PATH=${DEPS_PREFIX}
                    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON
                    -DBUILD_CURL_EXE=OFF -DENABLE_CURL_MANUAL=OFF
                    -DCURL_USE_WOLFSSL=ON -DCURL_USE_OPENSSL=OFF
                    -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON
                    -DUSE_NGHTTP2=OFF -DUSE_NGTCP2=OFF
                    -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF
                    -DCURL_USE_GSSAPI=OFF -DCURL_USE_LIBPSL=OFF
                    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=ON
                    -DUSE_LIBIDN2=OFF
    INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
)

# AC_SYS_LARGEFILE's LFS_CFLAGS substitution shells out to `getconf
# LFS_CFLAGS`, which returns empty on this modern glibc (64-bit off_t is
# already the default, so there is nothing to add) — but common/Makefile.am
# unconditionally does `-D_FILE_OFFSET_BITS=$(getconf-result)`, producing the
# syntactically invalid `-D_FILE_OFFSET_BITS=` and breaking <features.h>'s
# own `#if _FILE_OFFSET_BITS == 64` check. Override it empty at build time.
#
# DEPENDS wolfssl_ext: this pinned commit's own TLS layer (idevice.c) is
# built with HAVE_OPENSSL against whatever "openssl" pkg-config resolves
# to — the synthesized ${DEPS_PKGCONFIG}/openssl.pc redirects that to
# wolfSSL's shim (see wolfssl_ext's own comment above), so the actual
# libwolfssl.a file must exist before this target's configure/build steps
# run, not just by final link time.
# legacy branch (github.com/regulad/libimobiledevice): idevice.c's own
# connection-layer SSL/TLS is independently selectable via
# --with-ssl-implementation (see configure.ac) from the HAVE_OPENSSL used
# elsewhere in this same library (common/userpref.c's cert generation,
# governed by --disable-openssl below as it always was). wolfssl here
# specifically because it's the one currently-maintained TLS library that
# still supports the real SSLv3 this project's target hardware (pre-2013
# Apple TV/iOS lockdownd) requires -- see idevice.c's own comments for the
# full story. Device discovery/transport (idevice_connect,
# internal_connection_send/receive, idevice_get_device_list, etc.) are
# UNCHANGED from upstream -- this project talks to the real system usbmuxd
# (run with --no-preflight; see the README/AGENTS.md/docs/HISTORY.md for the systemd
# drop-in this requires), not an in-process reimplementation of it.
ExternalProject_Add(libimobiledevice_ext
    DEPENDS         libplist_ext libusbmuxd_ext wolfssl_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libimobiledevice
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${AUTOTOOLS_ENV} autoreconf -fi
              COMMAND ${AUTOTOOLS_ENV} ./configure --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared
                        --without-cython
                        --with-ssl-implementation=wolfssl
                        CPPFLAGS=-I${DEPS_INCLUDE}
                        LDFLAGS=-L${DEPS_LIB}
    BUILD_COMMAND   ${AUTOTOOLS_ENV} make -j LFS_CFLAGS= SUBDIRS=${LIBIMOBILEDEVICE_LIB_SUBDIRS}
    INSTALL_COMMAND make install SUBDIRS=${LIBIMOBILEDEVICE_LIB_SUBDIRS}
)

# third_party/libirecovery could NOT be pinned to a genuine 2020-era commit:
# checkm8's required irecv_async_usb_control_transfer_with_cancel() does not
# exist anywhere in synackuk/libirecovery's current git history before this
# commit, even though DeviceManager.m (dated Sept 2020) depends on it — the
# fork's history was evidently rewritten/rebased upstream at some point,
# discarding whatever commit originally introduced checkm8 support. This is
# the oldest commit in CURRENT history that has the function Blackb0x needs,
# not a date-accurate pin like the other vendored libraries (see docs/HISTORY.md).
# Being a much more recent commit than the others, it needs
# libimobiledevice-glue (a split-out dependency the old pins don't need),
# which in turn requires libplist >= 2.3.0 (pkg-config name "libplist-2.0")
# — well past our historically-pinned libplist 2.1.0 (pkg-config name plain
# "libplist"). Rather than upgrade the historical libplist pin (which would
# be wrong for libusbmuxd_ext/libimobiledevice_ext, which are genuinely
# pinned to that era), build a second, separate current-HEAD libplist
# (third_party/libplist-modern) used ONLY to satisfy glue's configure-time
# version check. glue itself still installs into the shared ${DEPS_PREFIX}
# every other target uses — at final static-link time its few plist_*
# symbol references resolve against whichever libplist.a is actually linked
# (deps::plist, the historical one), which is safe since libplist's public C
# API/ABI has stayed stable across this range.
ExternalProject_Add(libplist_modern_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libplist-modern
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${DEPS_MODERN_PKGCONFIG}"
                        ./autogen.sh --prefix=${DEPS_MODERN_PREFIX}
                        --enable-static --disable-shared
                        --without-cython
    BUILD_COMMAND   ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${DEPS_MODERN_PKGCONFIG}" make -j
    INSTALL_COMMAND make install
)

ExternalProject_Add(libimobiledevice_glue_ext
    DEPENDS         libplist_ext libplist_modern_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libimobiledevice-glue
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${DEPS_MODERN_PKGCONFIG}:${DEPS_PKGCONFIG}"
                        ./autogen.sh --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared
    BUILD_COMMAND   ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${DEPS_MODERN_PKGCONFIG}:${DEPS_PKGCONFIG}" make -j
    INSTALL_COMMAND make install
)

# libimobiledevice-glue-1.0.pc itself declares "Requires: libplist-2.0" (it
# was built against third_party/libplist-modern) — pkg-config resolves that
# transitively, so irecovery's own PKG_CONFIG_PATH must also see the modern
# prefix or it can't find libplist-2.0.pc, even though irecovery never uses
# libplist directly itself.
set(AUTOTOOLS_ENV_WITH_MODERN ${CMAKE_COMMAND} -E env
    "PKG_CONFIG_PATH=${DEPS_MODERN_PKGCONFIG}:${DEPS_PKGCONFIG}")

# libirecovery has a real, upstream native Darwin backend (configure.ac's
# --with-iokit, which defaults to yes whenever IOKit is detected) as an
# alternative to its libusb one -- confirmed by reading configure.ac and
# libirecovery.c's own iokit_usb_control_transfer/
# iokit_async_usb_control_transfer, not assumed. Passed explicitly on Apple
# rather than relying on that default, so a macOS build fails loudly at
# configure time if IOKit isn't available instead of silently producing a
# libusb-backed archive: IOKit is the configuration blackb0x-pwn's checkm8
# implementation is actually confirmed working against on real hardware, so
# quietly getting the other one there would be a genuinely misleading
# result rather than a mild inefficiency.
#
# blackb0x-pwn used to get its own second, private copy of this build for
# exactly that guarantee (libirecovery_iokit_ext, a separate prefix and a
# separate source copy). One explicit flag here gives the same guarantee
# without a duplicate build of the same source at a second version.
# --with-iokit unconditionally: the libusb backend is gone along with Linux
# support, and IOKit is the backend blackb0x-pwn is confirmed working against on
# real AppleTV3,2 hardware. Passing it explicitly rather than relying on
# configure's default keeps the failure loud if IOKit ever goes missing.
set(LIBIRECOVERY_BACKEND_ARGS --with-iokit)

ExternalProject_Add(libirecovery_ext
    DEPENDS         libplist_ext libimobiledevice_glue_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libirecovery
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${AUTOTOOLS_ENV_WITH_MODERN} ./autogen.sh --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared ${LIBIRECOVERY_BACKEND_ARGS}
    BUILD_COMMAND   ${AUTOTOOLS_ENV_WITH_MODERN} make -j
    INSTALL_COMMAND make install
)

# libtatsu: libimobiledevice's own TSS (Apple signing server) request/
# response client -- used by --stock-securerom (DeviceManager.cpp's
# sendiBSS()) to personalize a stock iBSS/iBEC with a real, ECID-bound
# SHSH ticket before sending it, the same way idevicerestore does for a
# genuine (non-exploited) DFU restore. Its own configure.ac
# PKG_CHECK_MODULES for "libplist-2.0" (not the historical "libplist"
# pkg-config name) is why this needs AUTOTOOLS_ENV_WITH_MODERN, same
# reasoning as libirecovery_ext's own comment above -- the actual linked
# libplist.a at blackb0x's final link step is still the historical one
# (deps::plist), just like libimobiledevice_glue_ext.
#
# CPPFLAGS=-I${DEPS_MODERN_PREFIX}/include: unlike libirecovery_ext/
# libimobiledevice_glue_ext above, libtatsu's own tss.c genuinely
# #includes <plist/plist.h> and calls current-API functions the
# historically-pinned libplist doesn't have (plist_dict_copy_bool(),
# plist_write_to_stream(), etc.) -- confirmed as a real build failure
# ("implicit declaration of function 'plist_dict_copy_bool'") without
# this. Its own Makefile.am orders AM_CFLAGS as
# "$(libcurl_CFLAGS) $(libplist_CFLAGS)", and libcurl_CFLAGS's
# pkg-config-derived -I${DEPS_INCLUDE} (where curl_ext, and also the
# HISTORICAL libplist_ext, both install) sorts before libplist_CFLAGS's
# -I${DEPS_MODERN_PREFIX}/include -- so `#include <plist/plist.h>`
# silently resolves to the historical, too-old header that also happens
# to live under DEPS_INCLUDE, never reaching the modern one at all. An
# explicit, earlier CPPFLAGS override (same shape as libfragmentzip_ext's
# own CPPFLAGS workaround above) wins that shadowing outright.
ExternalProject_Add(libtatsu_ext
    DEPENDS         libplist_modern_ext curl_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libtatsu
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${AUTOTOOLS_ENV_WITH_MODERN} ./autogen.sh --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared
                        CPPFLAGS=-I${DEPS_MODERN_PREFIX}/include
    BUILD_COMMAND   ${AUTOTOOLS_ENV_WITH_MODERN} make -j
    INSTALL_COMMAND make install
)

# CFLAGS/CXXFLAGS=-DCUSTOM_LOGGING: libgeneral's own include/libgeneral/
# macros.h unconditionally #defines bare, unscoped function-like macros
# named log/info/warning/error (its own printf-wrapper logging helpers)
# unless CUSTOM_LOGGING is already defined -- confirmed as a real macOS
# build failure: exception.cpp's own #include chain pulls in libc++'s
# <cmath>/<functional> before/during macros.h's expansion on Xcode's SDK,
# and the `log` macro (no real definition, just a printf) clobbers every
# subsequent declaration of the real log()/std::log() math function in
# those headers, breaking their own parse ("error: expected unqualified-id"
# on `extern double log(double);` and libc++'s templated log() overloads).
# CUSTOM_LOGGING is libgeneral's own documented escape hatch for exactly
# this (see macros.h's `#ifndef CUSTOM_LOGGING` guard) -- confirmed safe to
# just omit these macros outright rather than defining real replacements:
# libgeneral's own single source file (libgeneral/exception.cpp) never
# calls log()/info()/warning()/error() as macros internally. This clash is
# real on Linux too in principle (glibc's <math.h> also declares a real
# log()), it just happens not to be hit there because nothing in this
# translation unit's actual Linux include order re-declares log() after
# the macro takes effect the way libc++'s heavier template-based <cmath>
# does.
ExternalProject_Add(libgeneral_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libgeneral
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${AUTOTOOLS_ENV} ./autogen.sh --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared
                        CFLAGS=-DCUSTOM_LOGGING CXXFLAGS=-DCUSTOM_LOGGING
    BUILD_COMMAND   ${AUTOTOOLS_ENV} make -j
    INSTALL_COMMAND make install
)

# libfragmentzip.c at this pinned commit (2020-01-04) already #includes
# <libgeneral/macros.h> even though its own configure.ac does not yet
# PKG_CHECK_MODULES for it (added upstream slightly later), so there is no
# PKG_CONFIG_PATH-driven mechanism that teaches its build about libgeneral's
# location — pass CPPFLAGS/LDFLAGS explicitly instead. Its vendored autogen.sh
# also fails to bootstrap libtool on a modern toolchain (same "ltmain.sh not
# found" issue as libimobiledevice above) — use autoreconf -fi instead.
# CFLAGS=-DCUSTOM_LOGGING: libfragmentzip.c also #includes
# <libgeneral/macros.h> directly -- same real macOS build failure and same
# fix as libgeneral_ext's own CFLAGS/CXXFLAGS above; see that target's
# comment for the full explanation. Confirmed safe the same way: no
# log()/info()/warning()/error() macro calls anywhere in this file either.
ExternalProject_Add(libfragmentzip_ext
    DEPENDS         libgeneral_ext curl_ext libzip_ext wolfssl_ext
    SOURCE_DIR      ${CMAKE_SOURCE_DIR}/third_party/libfragmentzip
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${AUTOTOOLS_ENV} autoreconf -fi
              COMMAND ${AUTOTOOLS_ENV} ./configure --prefix=${DEPS_PREFIX}
                        --enable-static --disable-shared
                        CPPFLAGS=-I${DEPS_INCLUDE} LDFLAGS=-L${DEPS_LIB}
                        CFLAGS=-DCUSTOM_LOGGING
    BUILD_COMMAND   ${AUTOTOOLS_ENV} make -j
    INSTALL_COMMAND make install
)

# Imported targets for the five vendored libs, so later phases can just
# target_link_libraries(blackb0x PRIVATE deps::plist deps::irecovery ...).
function(add_deps_imported_lib name libfile)
    add_library(deps::${name} STATIC IMPORTED GLOBAL)
    set_target_properties(deps::${name} PROPERTIES
        IMPORTED_LOCATION ${DEPS_LIB}/${libfile}
        INTERFACE_INCLUDE_DIRECTORIES ${DEPS_INCLUDE})
endfunction()

add_deps_imported_lib(plist          libplist.a)
target_link_libraries(plist_compat PUBLIC deps::plist)
add_dependencies(plist_compat deps::plist)

add_deps_imported_lib(usbmuxd        libusbmuxd.a)
add_deps_imported_lib(imobiledevice  libimobiledevice.a)
add_deps_imported_lib(irecovery      libirecovery-1.0.a)
add_deps_imported_lib(imobiledevice_glue libimobiledevice-glue-1.0.a)
add_deps_imported_lib(general        libgeneral.a)
add_deps_imported_lib(fragmentzip    libfragmentzip.a)
add_deps_imported_lib(wolfssl        libwolfssl.a)
add_dependencies(deps::wolfssl wolfssl_ext)
# See the CRYPTO_LIBRARIES comment above for why libm is needed by any
# consumer of libwolfssl.a — deps::wolfssl's own consumer (blackb0x) goes
# through this imported target, not CRYPTO_LIBRARIES, so it needs the same
# fix applied here instead.
set_target_properties(deps::wolfssl PROPERTIES INTERFACE_LINK_LIBRARIES m)

# Named ZLIB::ZLIB (not deps::zlib, unlike every other imported target
# here) so blackb0x's own target_link_libraries() and deps::curl's
# INTERFACE_LINK_LIBRARIES below need no changes from when this was a real
# find_package(ZLIB)-provided imported target — see the ZLIB_FOUND
# override and zlib_ext's own ExternalProject_Add above for why it's
# vendored now instead.
add_library(ZLIB::ZLIB STATIC IMPORTED GLOBAL)
set_target_properties(ZLIB::ZLIB PROPERTIES
    IMPORTED_LOCATION ${DEPS_LIB}/libz.a
    INTERFACE_INCLUDE_DIRECTORIES ${DEPS_INCLUDE})
add_dependencies(ZLIB::ZLIB zlib_ext)

add_deps_imported_lib(zip            libzip.a)
add_dependencies(deps::zip libzip_ext)

add_deps_imported_lib(curl           libcurl.a)
add_dependencies(deps::curl curl_ext wolfssl_ext)
# libcurl.a's own consumers transitively need deps::wolfssl too —
# curl_ext is built with CURL_USE_WOLFSSL=ON, so its .a has undefined
# wolfSSL_* symbols exactly like any other consumer of libwolfssl.a — and
# ZLIB::ZLIB, since curl is built with CURL_ZLIB=ON for gzip/deflate
# transfer-encoding support.
set_target_properties(deps::curl PROPERTIES INTERFACE_LINK_LIBRARIES "deps::wolfssl;ZLIB::ZLIB")

add_deps_imported_lib(tatsu          libtatsu.a)
add_dependencies(deps::tatsu libtatsu_ext)
# Same shape as deps::curl above — libtatsu.a's own TSS-over-HTTPS
# requests go through libcurl, so any consumer of deps::tatsu needs
# deps::curl's own symbols (and transitively wolfssl/zlib) available too.
# libplist_compat (see Blackb0x/Libraries/libplist_compat.c) supplies the
# handful of newer libplist convenience functions tss.o itself calls that
# the historically-pinned deps::plist doesn't implement — linking two
# real copies of libplist (historical + libplist_modern_ext's -2.0
# archive) was tried first and produces genuine duplicate-symbol link
# errors for their shared core API (plist_new_dict()/plist_free()/etc.
# all live in the same object file as the newer functions in both
# archives, so there's no way to pull only the new ones) — a small,
# faithfully-ported compat shim avoids that architecturally rather than
# fighting it with symbol renaming.
set_target_properties(deps::tatsu PROPERTIES INTERFACE_LINK_LIBRARIES "deps::curl;plist_compat")

foreach(t plist usbmuxd imobiledevice irecovery imobiledevice_glue general fragmentzip)
    add_dependencies(deps::${t} ${t}_ext)
endforeach()
add_dependencies(deps::usbmuxd deps::plist)
add_dependencies(deps::imobiledevice deps::plist deps::usbmuxd)
add_dependencies(deps::irecovery deps::plist deps::imobiledevice_glue)
add_dependencies(deps::fragmentzip deps::general)

# xpwn's own `dmg` and `xpwn` CMake targets (declared by
# add_subdirectory(third_party/xpwn) above) link against
# ${DEPS_LIB}/libwolfssl.a directly via the CRYPTO_LIBRARIES cache-variable
# redirect set before that add_subdirectory call — that file doesn't exist
# until wolfssl_ext's ExternalProject actually builds, so (same Unix
# Makefiles generator limitation documented on `blackb0x` below) these two
# targets need an explicit add_dependencies to serialize behind it.
add_dependencies(dmg wolfssl_ext zlib_ext)
add_dependencies(xpwn wolfssl_ext bzip2_ext libpng_ext zlib_ext)

# minizip/CMakeLists.txt hardcodes `target_link_libraries(minizip z)` — a
# bare library name, not the ZLIB_LIBRARIES path its own INCLUDE(FindZLIB)
# just computed. The linker resolves a bare `-lz` search fresh, and prefers
# the shared object over the static archive even when both sit in the same
# -L directory, so this silently reintroduced a dynamic libz.so dependency
# despite ZLIB::ZLIB elsewhere resolving statically. Override post-hoc
# rather than patching the vendored file (matches the BEFORE include-dir
# fixes below: don't touch xpwn's own CMakeLists.txt content).
# target_link_libraries() only appends — it can't undo minizip's own
# `target_link_libraries(minizip z)` call, so a second call here just adds
# ZLIB::ZLIB alongside the original bad bare `z` rather than replacing it
# (confirmed directly: the bare `-lz` was still present in blackb0x's final
# link.txt after trying that). Overwrite the property outright instead.
set_target_properties(minizip PROPERTIES
    LINK_LIBRARIES ZLIB::ZLIB
    INTERFACE_LINK_LIBRARIES ZLIB::ZLIB)
add_dependencies(minizip zlib_ext)

# dmg/CMakeLists.txt has the identical bug one directory over:
# `target_link_libraries(dmg common hfs z)` (line 31) appends the same
# bare `z` on top of its own correct `target_link_libraries(dmg
# ${ZLIB_LIBRARIES})` (line 20) a few lines earlier. Surgically remove
# just the literal "z" entry rather than overwriting the whole property
# outright (unlike minizip above), since dmg's real link list also needs
# to keep common/hfs/CRYPTO_LIBRARIES/etc.
get_target_property(_dmg_link_libs dmg LINK_LIBRARIES)
list(REMOVE_ITEM _dmg_link_libs z)
set_target_properties(dmg PROPERTIES
    LINK_LIBRARIES "${_dmg_link_libs}"
    INTERFACE_LINK_LIBRARIES "${_dmg_link_libs}")
unset(_dmg_link_libs)

# Both targets compile vendored .c files with bare `#include <openssl/...>`
# (dmg/filevault.c; xpwn's 8900.c/img3.c/pwnutil.c) that resolve to
# wolfSSL's shim via OPENSSL_INCLUDE_DIR (set before add_subdirectory
# above) — but that shim isn't self-contained the way real OpenSSL's
# headers are; it needs wolfssl/options.h processed first (see the
# openssl.pc comment near the top of this file for the full explanation
# and confirmation this is genuinely required, not precautionary).
# ${DEPS_INCLUDE} itself (the parent of OPENSSL_INCLUDE_DIR's
# ${DEPS_INCLUDE}/wolfssl) is ALSO needed on the include path — confirmed
# necessary directly: wolfssl/options.h's own `#include
# <wolfssl/wolfcrypt/settings.h>` is a normal searched include, not
# resolved via the force-included absolute path, so without this the
# force-include itself fails with "wolfssl/wolfcrypt/settings.h: No such
# file or directory".
target_include_directories(dmg PRIVATE ${DEPS_INCLUDE})
target_include_directories(xpwn PRIVATE ${DEPS_INCLUDE})
target_compile_options(dmg PRIVATE -include ${DEPS_INCLUDE}/wolfssl/options.h)
target_compile_options(xpwn PRIVATE -include ${DEPS_INCLUDE}/wolfssl/options.h)

# hfs/hfscompress.c does a bare `#include <zlib.h>` with no
# include_directories(${ZLIB_INCLUDE_DIR}) anywhere in xpwn/hfs's own
# CMakeLists.txt (unlike minizip/ipsw-patch, which do call that) — never a
# problem while zlib was a system dependency (a normal host compiler
# always has /usr/include, where Fedora's own zlib.h also happens to
# live, on its default search path regardless), but now that zlib is
# vendored (see the ZLIB_FOUND override above) this needs an explicit
# include path to make sure it actually picks up the vendored zlib.h
# rather than silently falling back to whatever system copy happens to be
# installed.
target_include_directories(hfs PRIVATE ${DEPS_INCLUDE})
add_dependencies(hfs zlib_ext)

# BEFORE is load-bearing here, not decorative: xpwn's own dmg/CMakeLists.txt
# and ipsw-patch/CMakeLists.txt call include_directories(${PNG_INCLUDE_DIR})/
# include_directories(${BZIP2_INCLUDE_DIR}) — on THIS system, those resolve
# under linuxbrew's general /home/linuxbrew/.linuxbrew/include prefix, which
# ALSO happens to contain linuxbrew's own real OpenSSL headers at
# .../include/openssl/*.h (an entirely unrelated formula sharing the same
# prefix) — and those include_directories() calls run before
# OPENSSL_INCLUDE_DIR's own inside those same vendored CMakeLists.txt files.
# Confirmed directly by inspecting a real generated compile command: without
# BEFORE, the resulting -I order put linuxbrew's prefix ahead of wolfSSL's
# shim dir, so `#include <openssl/aes.h>` silently resolved to the REAL
# OpenSSL header instead — compiling fine, but leaving the .o files with
# undefined references to bare `AES_cbc_encrypt`/`SHA1_Init`/etc. at final
# link time (since only libwolfssl.a is actually linked, real libcrypto
# never is). BEFORE guarantees wolfSSL's shim wins regardless of whatever
# else those vendored files' own include_directories() calls add, in
# whatever order.
target_include_directories(dmg BEFORE PRIVATE ${DEPS_INCLUDE}/wolfssl)
target_include_directories(xpwn BEFORE PRIVATE ${DEPS_INCLUDE}/wolfssl)

# xpwn's small CLI-tool executables (each compiling one .c file directly,
# not just linking the dmg/xpwn libraries) transitively pull in the same
# openssl-touching headers via dmglib.h/img3.h's own includes — confirmed
# directly: dmg-bin's dmg.c.o failed with the exact same
# wolfssl/wolfcrypt/settings.h error before this was added. The two
# target_compile_options() calls above only apply to the dmg/xpwn library
# targets themselves, not these separate executables that merely link
# against them, so each needs the identical treatment.
#
# blackb0x_xpwntool (our own target wrapping Blackb0x/Libraries/xpwntool.c,
# declared far above — NOT the same target as xpwn's own "xpwntool" CLI
# tool in this list) needs it too, for the exact same reason: its
# transitively-included Blackb0x/Libraries/libxpwntool/img3.h has its own
# bare `#include <openssl/aes.h>`. This was silently getting away without
# it on Linux dev boxes that happen to have a real system libssl-dev
# installed (AES_KEY's struct layout has stayed ABI-stable across the
# whole OpenSSL-compatible-API ecosystem for decades, wolfSSL's shim
# included, so a real system openssl/aes.h resolving here instead of
# wolfSSL's shim went unnoticed) — confirmed as a real, not hypothetical,
# bug on a real macOS build, which has no such fallback header anywhere on
# its default include path and failed outright.
# dmg-bin doesn't exist as a target on Apple at all (third_party/xpwn's own
# dmg/CMakeLists.txt skips it there — it duplicates macOS's native
# `hdiutil`, see that file's own comment) — omit it from this list there,
# referencing a nonexistent target name is a hard CMake error.
# dmg-bin is deliberately absent: third_party/xpwn's own dmg/CMakeLists.txt
# skips that target on Apple entirely (it duplicates macOS's native hdiutil),
# and naming a nonexistent target here is a hard CMake error.
set(_wolfssl_include_targets ipsw xpwntool imagetool dripwn blackb0x_xpwntool)
foreach(t ${_wolfssl_include_targets})
    target_include_directories(${t} PRIVATE ${DEPS_INCLUDE})
    target_include_directories(${t} BEFORE PRIVATE ${DEPS_INCLUDE}/wolfssl)
    target_compile_options(${t} PRIVATE -include ${DEPS_INCLUDE}/wolfssl/options.h)
endforeach()
add_dependencies(blackb0x_xpwntool wolfssl_ext)

# ---------------------------------------------------------------------------
# blackb0x-pwn: Blackb0x's own original checkm8/SHAtter exploit, standalone.
#
# The ONLY pwntool this project has. It is built on every platform, and its
# own sources (Blackb0x/Source/Pwn/) contain no Apple-specific code at all --
# only backend-independent irecv_* calls, every one of which libirecovery
# implements for libusb as well as IOKit -- so the Apple-only gate this
# target used to sit behind was a packaging decision from the macOS effort,
# not a source requirement. Ports DeviceManager.m's original
# +SHAtter:/+checkm8: methods (deleted from the tree in 907b64b, recovered
# from git history -- see Blackb0x/Source/Pwn/Checkm8Pwn.c's own header
# comment) byte-for-byte.
#
# This target used to sit alongside a vendored `gaster`, selected by a
# --pwntool flag. gaster is gone: it was never made to work against an
# AppleTV3,2 on either Linux 7.1.x or macOS 26, across a long investigation
# (see docs/HISTORY.md). Nothing here depends on it.
#
# It links the same deps::irecovery every other target here does. On Apple
# that archive is IOKit-backed, which libirecovery_ext now guarantees by
# passing --with-iokit explicitly (see its own block above) rather than
# leaving it to configure's default. This target used to carry a second,
# private libirecovery build purely to get that guarantee: a duplicate
# ExternalProject, a duplicate install prefix, and a build-time copy of the
# submodule source, all to compile the same source with the same flags.

add_executable(blackb0x-pwn
    Blackb0x/Source/Pwn/main.c
    Blackb0x/Source/Pwn/Checkm8Pwn.c)
target_include_directories(blackb0x-pwn PRIVATE Blackb0x/Source)

# deps::imobiledevice_glue/deps::plist: libirecovery-1.0.a's own real
# (backend-independent) link dependencies -- confirmed via its installed
# .pc file's Requires.private, the same two archives the main blackb0x
# binary already links for the same reason.
target_link_libraries(blackb0x-pwn PRIVATE
    deps::irecovery
    deps::imobiledevice_glue
    deps::plist
    Threads::Threads)
add_dependencies(blackb0x-pwn libirecovery_ext libimobiledevice_glue_ext libplist_ext)

# The IOKit backend's own dependencies. With --with-iokit, libirecovery's
# Makefile.am never populates $(libusb_CFLAGS)/$(libusb_LIBS) at all (confirmed
# by reading configure.ac's AS_IF branching), so there is nothing
# libusb-related to link and these two frameworks are what it needs instead.
target_link_libraries(blackb0x-pwn PRIVATE
    "-framework CoreFoundation"
    "-framework IOKit")

# ---------------------------------------------------------------------------
# blackb0x CLI executable
#
# Cli.hpp/.cpp (Phase 6) is the only piece left to add once DeviceManager/
# IPSW/IPSWDownloader/Patcher are all wired together into an actual CLI flow.
# ---------------------------------------------------------------------------

add_executable(blackb0x
    Blackb0x/Source/main.cpp
    Blackb0x/Source/Cli.cpp
    Blackb0x/Source/Console.cpp
    Blackb0x/Source/DeviceManager.cpp
    Blackb0x/Source/IPSW.cpp
    Blackb0x/Source/IPSWDownloader.cpp
    Blackb0x/Source/Patcher.cpp
    Blackb0x/Source/Personalize.cpp
    Blackb0x/Source/ResourcePath.cpp)

target_include_directories(blackb0x PRIVATE third_party/xpwn/includes)

# NOTE: CBPatcher and iBoot32Patcher are deliberately absent here -- both are
# GPLv3 and are fork/exec'd as separate binaries instead of linked. See their
# add_executable() blocks above.
target_link_libraries(blackb0x PRIVATE
    blackb0x_xpwntool
    idevicerestore_img3
    xpwn
    deps::irecovery
    deps::imobiledevice_glue
    deps::imobiledevice
    deps::usbmuxd
    deps::plist
    deps::fragmentzip
    deps::tatsu
    deps::general
    deps::wolfssl
    deps::zip
    deps::curl
    ZLIB::ZLIB
    Threads::Threads)

# Real macOS link failure: the shared libirecovery_ext build picks IOKit
# by default there (its own configure.ac's --with-iokit defaults to yes
# when IOKit is available -- see the blackb0x-pwn block above for the
# full story on this default), and wolfSSL's own DoAppleNativeCertValidation/
# LoadSystemCaCertsMac (its native-macOS-cert-store support) and curl's
# Curl_macos_init (proxy detection) both call into Apple frameworks
# directly too -- none of that is optional/conditional on this project's
# own build flags, it's baked into those libraries' own Darwin code paths.
# Undefined symbols spanned CoreFoundation (irecovery + wolfSSL),
# IOKit (irecovery), Security (wolfSSL's Sec*/native cert validation), and
# SystemConfiguration (curl's SCDynamicStoreCopyProxies) specifically --
# confirmed against the real link error, not guessed. blackb0x-pwn's own
# Apple link line doesn't need Security/SystemConfiguration since it links
# neither wolfSSL nor curl at all.
target_link_libraries(blackb0x PRIVATE
    "-framework CoreFoundation"
    "-framework IOKit"
    "-framework Security"
    "-framework SystemConfiguration")

# libusbmuxd.a (needed by common/userpref.c's usbmuxd_read_buid/
# usbmuxd_read_pair_record/etc, its own daemon-side pairing-record cache
# path) and libimobiledevice-glue-1.0.a used to each vendor their own
# separate copy of common/collection.c with external linkage — a real,
# known conflict in this ecosystem when both get statically linked together
# (confirmed directly: exercising lockdownd_client_new_with_handshake(),
# which calls into userpref.c's pairing-record functions, is what actually
# pulls both in). Both implementations are the same generic collection ADT
# (confirmed byte-for-byte via diff, see libusbmuxd_ext's own comment
# above); libusbmuxd_ext's INSTALL_COMMAND now strips its copy of the
# object out of the installed archive so only libimobiledevice-glue's
# survives to link time, which fixes the duplicate-symbol conflict itself
# rather than papering over it here with the GNU-ld-only
# -Wl,--allow-multiple-definition (no equivalent on Apple's ld64 as of
# Xcode 15+, which is the only linker this project targets now).

# The Unix Makefiles generator does not reliably serialize a real target's
# link step behind ExternalProject-installed IMPORTED libraries via
# add_dependencies() on the imported target alone (it emits a file-based
# link dependency on e.g. deps/lib/libirecovery.a with no rule to build it).
# Depend on the ExternalProject targets directly from blackb0x to force them
# to finish before blackb0x links.
add_dependencies(blackb0x
    libplist_ext libusbmuxd_ext libimobiledevice_ext
    libirecovery_ext libimobiledevice_glue_ext libgeneral_ext libfragmentzip_ext
    libtatsu_ext
    wolfssl_ext libzip_ext curl_ext bzip2_ext libpng_ext zlib_ext)

# ---------------------------------------------------------------------------
# Tests
#
# patchRamdisk() itself needs a real root-owned loop mount, so it isn't
# exercised here — see the comment atop RamdiskOverlayTests.cpp. This covers
# what doesn't need root: path resolution, and the checked-in ramdisk/
# overlay trees actually containing what patchRamdisk() expects (and not
# what it doesn't, e.g. the dead `dirhelper` duplicate or macOS packaging
# junk that were cleaned up when Files/ became ramdisk/).
# ---------------------------------------------------------------------------

add_executable(blackb0x_tests
    Blackb0x/Tests/RamdiskOverlayTests.cpp
    Blackb0x/Source/ResourcePath.cpp)

add_test(NAME RamdiskOverlayTests
         COMMAND blackb0x_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ---------------------------------------------------------------------------
# bake-all-ramdisks
#
# The one piece of blackb0x that needs CAP_SYS_ADMIN/CAP_CHOWN on Linux (loop-
# mounting a real HFS+ image) or root for `hdiutil`/`diskutil` on macOS —
# split into its own binary, run as a separate manual step, not invoked by
# blackb0x itself (see BakeRamdisk.hpp's header comment and
# Patcher::patchRamdisk()). Batches bakeRamdisk() over every (device,
# firmware) combination under Blackb0x/ImageKeys/ in one run, writing
# results to dist/ — so it needs the HFS+/DMG patching stack AND the IPSW
# download stack (IPSW.cpp/IPSWDownloader.cpp), but none of the
# USB/device-communication libraries the main blackb0x binary needs.
#
# Was Linux-only for a while (.claude/TODO.md item 4/4a) because nobody had
# scoped or started actually porting BakeRamdisk.cpp's HFS+ volume-build
# sequence to macOS: third_party/xpwn's vendored hfsplus.h uses the
# `register` storage-class specifier (removed from the language in C++17;
# GCC still only warns, Clang on macOS hard-errors) and BakeRamdisk.cpp's own
# st_atim/st_mtim struct stat fields are Linux/glibc-specific spellings
# (Darwin's struct stat calls the same fields st_atimespec/st_mtimespec
# instead). Both are now handled directly in BakeRamdisk.cpp (see its own
# `#if defined(__APPLE__)` branches — an `hdiutil`-based volume build
# replaces Linux's loop-mount sequence there, and the `register` keyword is
# bracketed out around the one include that pulls it in), so this target now
# builds on both platforms. No Apple frameworks are linked pre-emptively
# here — this target doesn't link deps::wolfssl/deps::irecovery even on
# Linux, so add whatever a real macOS link actually demands, the same way
# blackb0x's own `if(APPLE)` framework block below was arrived at from a
# real link error rather than a guess.
# ---------------------------------------------------------------------------

add_executable(bake-all-ramdisks
    Blackb0x/Source/BakeAllRamdisks.cpp
    Blackb0x/Source/BakeRamdisk.cpp
    Blackb0x/Source/IPSW.cpp
    Blackb0x/Source/IPSWDownloader.cpp
    Blackb0x/Source/ResourcePath.cpp)

target_include_directories(bake-all-ramdisks PRIVATE third_party/xpwn/includes)

target_link_libraries(bake-all-ramdisks PRIVATE
    blackb0x_xpwntool
    xpwn
    deps::plist
    deps::fragmentzip
    deps::general
    deps::curl
    ZLIB::ZLIB)

add_dependencies(bake-all-ramdisks
    libplist_ext libfragmentzip_ext libgeneral_ext
    wolfssl_ext curl_ext bzip2_ext libpng_ext zlib_ext)

# ---------------------------------------------------------------------------
# bake-all-bootloaders
#
# The counterpart to bake-all-ramdisks for everything that ISN'T the ramdisk:
# iBSS, iBEC, KernelCache and DeviceTree, downloaded and patched ahead of
# time for every (device, firmware) combination under Blackb0x/ImageKeys/.
# That is .claude/TODO.md item 5 for the non-ramdisk half of the suite.
#
# Unlike bake-all-ramdisks this needs NO root at all — every step is
# download, decrypt and byte-level file patching, with no loop mount and no
# hdiutil. That is the whole reason it is a separate binary rather than a
# flag on the other one: it can be run and re-run freely while iterating on
# the patch logic itself, and it is the only way to exercise
# Patcher::patchiBSS()/patchiBEC()/patchKernel() (and therefore the
# iBoot32Patcher binary they fork/exec) across every known firmware with no
# hardware attached.
#
# Shares Patcher.cpp with blackb0x itself rather than reimplementing the
# patch calls, so the two cannot drift: if this tool patches a build
# successfully, blackb0x's live path runs the identical code on it.
# Deliberately does NOT link BakeRamdisk.cpp — Patcher::patchRamdisk()
# shells out to the bake-all-ramdisks binary rather than linking it (see
# resolveBakeAllRamdisksPath()), so the root-requiring code stays out of
# this binary entirely.
# ---------------------------------------------------------------------------

# No Personalize.cpp: that is TSS/SHSH personalization for the
# --stock-securerom diagnostic route only, and Patcher itself never calls it
# (it only mentions it in comments). Leaving it out keeps libtatsu and the
# img3-stitching code out of this binary.
add_executable(bake-all-bootloaders
    Blackb0x/Source/BakeAllBootloaders.cpp
    Blackb0x/Source/IPSW.cpp
    Blackb0x/Source/IPSWDownloader.cpp
    Blackb0x/Source/Patcher.cpp
    Blackb0x/Source/ResourcePath.cpp)

target_include_directories(bake-all-bootloaders PRIVATE third_party/xpwn/includes)

# CBPatcher absent for the same GPLv3 reason as on blackb0x above; this target
# fork/execs it through the same Patcher.cpp helper.
target_link_libraries(bake-all-bootloaders PRIVATE
    blackb0x_xpwntool
    xpwn
    deps::plist
    deps::fragmentzip
    deps::general
    deps::curl
    deps::wolfssl
    ZLIB::ZLIB)

# iBoot32Patcher: not linked (it is GPLv3 — see its own block above), but it
# IS fork/exec'd by Patcher.cpp at runtime, so it has to exist next to this
# binary for the tool to do anything. A build dependency rather than a link
# one.
add_dependencies(bake-all-bootloaders
    iBoot32Patcher
    libplist_ext libfragmentzip_ext libgeneral_ext
    wolfssl_ext curl_ext bzip2_ext libpng_ext zlib_ext)

