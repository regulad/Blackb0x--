// SPDX-License-Identifier: GPL-3.0
//
//  Img3Crypt.hpp
//  Blackb0x
//
//  decrypt(): AES-CBC decrypt an Apple IMG3 to its raw payload, or re-encrypt
//  a payload back into an IMG3 using an original as the template. Built
//  directly on third_party/xpwn's public AbstractFile API (openAbstractFile*,
//  duplicateAbstractFile2, createAbstractFileFromFile), which bake-firmware
//  already links -- so this is first-party glue over the submodule, not a
//  vendored copy. It replaces the former src/libraries/xpwntool.{c,h}, which
//  was a copy of xpwn's own `xpwntool` CLI (xpwn ships this logic only as a
//  main(), never as a linkable function). See Img3Crypt.cpp for the full
//  provenance/license note.
//
//  AUTHORING-ONLY: compiled into bake-firmware, never the blackb0x jailbreak
//  binary, which does no decryption. See CMakeLists.txt.
//
#pragma once

// Same signature the old xpwntool.h exported, so the two call sites
// (PatcherPatch.cpp, BakeRamdisk.cpp) are unchanged:
//   input_path    IMG3 (or raw) to read
//   output_path   file to write
//   ip_key/ip_iv  hex-string AES key/IV, or null/empty for none
//   decrypt       "TRUE"  -> decrypt input, write payload (optionally re-wrap
//                            via template_path)
//                 "FALSE" -> encrypt/pass through (re-wrap via template_path)
//   template_path original IMG3 to clone tag layout/KBAG from, or null/empty
//
// decrypt() returns void and reports failure only by printing to stderr and
// leaving a zero-byte output file (which the callers' emptiness checks catch)
// -- see Img3Crypt.cpp for why.
void decrypt(char* input_path, char* output_path, char* ip_key, char* ip_iv, char* decrypt, char* template_path);
