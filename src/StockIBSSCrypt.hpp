//
//  StockIBSSCrypt.hpp
//  Blackb0x
//
//  SPDX-License-Identifier: AGPL-3.0-or-later
//
//  First-party AES-CBC decrypt of a stock (Apple-signed, still-encrypted)
//  iBSS img3, for the ONE blackb0x delivery route that needs plaintext: the
//  checkm8 boot_client() soft-DFU path (DeviceManager.cpp), which uploads the
//  img3 DATA tag's bytes directly as code and so cannot run ciphertext. This
//  is the deliberate, narrow exception to "blackb0x does no decryption" -- see
//  useStockIBSS() in Patcher.cpp and docs/HISTORY.md. It uses wolfSSL's AES
//  (already linked into blackb0x via deps::wolfssl), NOT xpwn's decrypt(), so
//  it pulls no GPL code onto blackb0x's link line.
//

#pragma once

#include <string>

// Parse the img3 at srcImg3Path, AES-CBC-decrypt its DATA tag in place with
// the given hex key/iv, and write the result -- same img3 wrapper, now with
// plaintext DATA -- to dstPath. keyHex/ivHex are the exact hex strings from
// keys/ (iBSS: 64-hex key = AES-256, 32-hex iv). Returns true on success;
// prints the reason to stderr and returns false otherwise (bad img3, empty
// key, key/iv length mismatch, I/O failure).
bool decryptStockIBSS(const std::string& srcImg3Path, const std::string& dstPath, const std::string& keyHex,
                      const std::string& ivHex);
