// SPDX-License-Identifier: GPL-3.0
//
//  Img3Crypt.cpp
//  Blackb0x
//
//  PROVENANCE / LICENSE
//  --------------------
//  decrypt() is our own composition of third_party/xpwn's public AbstractFile
//  API. xpwn exposes the equivalent only as its `xpwntool` command-line tool
//  (ipsw-patch/xpwntool.c, a main()), never as a linkable function, so the
//  logic here mirrors that tool's flow: init_libxpwn() -> open the input via
//  openAbstractFile{,2,3}() -> optionally clone an IMG3 template with
//  duplicateAbstractFile2() -> copy the bytes across. Because it derives from
//  xpwn (GPL-3.0) and calls straight into it, this file is GPL-3.0 too. That
//  is compatible with the project's AGPL-3.0 (LICENSE.md); the whole thing is
//  a copyleft work regardless (bake-firmware also links the GPL-3.0 xpwn
//  library directly and fork/execs the GPL patchers).
//
//  This replaces the former src/libraries/xpwntool.{c,h}, which was a copied
//  version of xpwn's CLI file wrapped as a function and built as its own
//  static library (blackb0x_xpwntool). Now it is first-party glue compiled
//  straight into bake-firmware against the submodule's public headers.
//
//  Retained from that copy: the log level is raised so xpwn's two purely
//  diagnostic level-4/5 XLOG lines stop spamming every call, and each of the
//  three open/duplicate error paths bails out instead of dereferencing the
//  NULL it just diagnosed (the original fell through to a SIGSEGV -- the real
//  root cause behind the "decrypt() a nonexistent file corrupts the heap"
//  hazard Patcher documents). decrypt() stays void; callers detect failure by
//  the zero-byte output file it leaves behind.
//
#include "Img3Crypt.hpp"

// img3ValidateFile()/img3FileHasMagic(): the post-republish guard. It lives in
// Patcher.cpp -- the crypto-free TU compiled into BOTH binaries -- precisely
// so that including it here adds nothing to anyone's link line; it is pure
// <cstdio>/<cstdint> and pulls in no xpwn. See its definition for the
// predicate and its provenance.
#include "Patcher.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <abstractfile.h>
#include <common.h>
#include <xpwn/libxpwn.h>
#include <xpwn/nor_files.h>
}

void decrypt(char* input_path, char* ouput_path, char* ip_key, char* ip_iv, char* decrypt, char* template_path) {
    char* inData;
    size_t inDataSize;
    init_libxpwn();
    // init_libxpwn() sets GlobalLogLevel = 0xFF ("suppress nothing"), so every
    // XLOG() in third_party/xpwn fires. Only 3 of ~130 call sites are level
    // >= 4, and both are pure noise on every decrypt (a payload-hash dump in
    // img3.c's createAbstractFileFromImg3, and an LZSS length "match" line in
    // lzssfile.c). Silence just those.
    libxpwn_loglevel(4);
    AbstractFile* templateFile = NULL;
    AbstractFile* certificate = NULL;
    unsigned int* key = NULL;
    unsigned int* iv = NULL;
    int hasKey = FALSE;
    int hasIV = FALSE;
    int doDecrypt = FALSE;

    if (strcmp(decrypt, "TRUE") == 0) {
        doDecrypt = TRUE;
        templateFile = createAbstractFileFromFile(fopen(input_path, "rb"));
    }

    if (template_path && template_path[0]) {
        templateFile = createAbstractFileFromFile(fopen(template_path, "rb"));
    }

    if (ip_key && ip_key[0]) {
        hasKey = TRUE;
        hasIV = TRUE;
    }

    if (hasKey == TRUE) {
        size_t bytes;
        hexToInts(ip_key, &key, &bytes);
    }
    if (hasIV == TRUE) {
        size_t bytes;
        hexToInts(ip_iv, &iv, &bytes);
    }

    AbstractFile* inFile;
    if (doDecrypt) {
        if (hasKey) {
            inFile = openAbstractFile3(createAbstractFileFromFile(fopen(input_path, "rb")), key, iv, 0);
        } else {
            inFile = openAbstractFile3(createAbstractFileFromFile(fopen(input_path, "rb")), NULL, NULL, 0);
        }
    } else {
        if (hasKey) {
            inFile = openAbstractFile2(createAbstractFileFromFile(fopen(input_path, "rb")), key, iv);
        } else {
            inFile = openAbstractFile(createAbstractFileFromFile(fopen(input_path, "rb")));
        }
    }
    // Each of the three error paths below bails out rather than dereferencing
    // the NULL it just diagnosed (see this file's header for why). Bailing
    // leaves a zero-byte output file, exactly what the callers' post-decrypt()
    // emptiness checks look for.
    if (!inFile) {
        fprintf(stderr, "error: cannot open infile\n");
        if (templateFile) templateFile->close(templateFile);
        if (key) free(key);
        if (iv) free(iv);
        return;
    }

    AbstractFile* outFile = createAbstractFileFromFile(fopen(ouput_path, "wb"));
    if (!outFile) {
        fprintf(stderr, "error: cannot open outfile\n");
        inFile->close(inFile);
        if (templateFile) templateFile->close(templateFile);
        if (key) free(key);
        if (iv) free(iv);
        return;
    }

    AbstractFile* newFile;
    if (templateFile) {
        if (hasKey && !doDecrypt) {
            newFile = duplicateAbstractFile2(templateFile, outFile, key, iv, certificate);
        } else {
            newFile = duplicateAbstractFile2(templateFile, outFile, NULL, NULL, certificate);
        }
        if (!newFile) {
            fprintf(stderr, "error: cannot duplicate file from provided template\n");
            inFile->close(inFile);
            outFile->close(outFile);
            if (key) free(key);
            if (iv) free(iv);
            return;
        }
    } else {
        newFile = outFile;
    }

    if (hasKey && !doDecrypt) {
        if (newFile->type == AbstractFileTypeImg3) {
            AbstractFile2* abstractFile2 = (AbstractFile2*)newFile;
            abstractFile2->setKey(abstractFile2, key, iv);
        }
    }

    inDataSize = (size_t)inFile->getLength(inFile);
    inData = (char*)malloc(inDataSize);
    inFile->read(inFile, inData, inDataSize);
    inFile->close(inFile);

    newFile->write(newFile, inData, inDataSize);
    newFile->close(newFile);

    free(inData);
    if (key) free(key);
    if (iv) free(iv);

    // -----------------------------------------------------------------------
    // POST-REPUBLISH GUARD
    // -----------------------------------------------------------------------
    // Every IMG3 this project publishes is written by the code above, so this
    // is the one choke point that covers all of them -- PatcherPatch.cpp's
    // patchiBSS()/patchiBEC()/patchKernel() and BakeRamdisk.cpp's ramdisk
    // re-encrypt alike. Each of those also asserts explicitly on its own
    // output (a guard is worth having twice; the caller can name the stage and
    // stop the bake with its own message), but this one catches any FUTURE
    // re-encrypt site for free, which matters because the field that shipped
    // broken -- sigCheckArea, stale from the template -- was wrong on the
    // RestoreRamDisk too and went unnoticed only because that image grew
    // instead of shrinking. A guard scoped to kernelcaches would have missed
    // it.
    //
    // The sniff first: decrypt()'s output is an IMG3 only on the re-wrap paths
    // (a template was given). A plain decrypt writes a raw payload -- a
    // kernelcache, a DMG, a raw iBSS -- and there is nothing here to check.
    //
    // On failure the output file is DELETED, not left in place. img3Reject()
    // has already said everything on stderr; removing the file means every
    // existing caller's exists()/file_size()/stat check turns this into a hard
    // bake failure even where the caller predates this guard, and -- more to
    // the point -- an image iBoot is known to reject can never survive into
    // dist/ to be discovered on hardware later.
    if (img3FileHasMagic(ouput_path)) {
        if (!img3ValidateFile(ouput_path, "Img3Crypt::decrypt() re-encrypt output")) {
            fprintf(stderr, "Img3Crypt: deleting the malformed image at %s so it cannot be published\n",
                    ouput_path);
            remove(ouput_path);
        }
    }
}
