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
}
