//
//  StockIBSSCrypt.cpp
//  Blackb0x
//
//  SPDX-License-Identifier: AGPL-3.0-or-later
//
//  See StockIBSSCrypt.hpp. First-party AES-CBC decrypt of a stock iBSS img3,
//  for the checkm8 boot_client() route only. Uses wolfSSL's OpenSSL-compat AES
//  (deps::wolfssl, already on blackb0x's link line) -- no xpwn, no GPL. The
//  img3 walk here mirrors DeviceManager.cpp's check_img3_file_format() exactly
//  (same tag layout, same DATA-tag location), since that is the parser the
//  decrypted output has to satisfy immediately afterward.
//

#include "StockIBSSCrypt.hpp"

#include <openssl/aes.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

// Same constants/layout as DeviceManager.cpp's img3 parser.
constexpr uint32_t kImg3Magic = 0x496d6733;  // 'Img3'
constexpr uint32_t kTagData = 0x44415441;    // 'DATA'

#pragma pack(push, 1)
struct Img3Tag {
    uint32_t magic;
    uint32_t totalLength;
    uint32_t dataLength;
};
struct Img3File {
    uint32_t magic;
    uint32_t fullSize;
    uint32_t sizeNoPack;
    uint32_t sigCheckArea;
    uint32_t ident;
};
#pragma pack(pop)

// Decode an even-length hex string into bytes. Returns false on any non-hex
// digit or odd length.
bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nibble = [](char c, int& v) -> bool {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        return false;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = 0, lo = 0;
        if (!nibble(hex[i], hi) || !nibble(hex[i + 1], lo)) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

uint32_t readU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

}  // namespace

bool decryptStockIBSS(const std::string& srcImg3Path, const std::string& dstPath, const std::string& keyHex,
                      const std::string& ivHex) {
    if (keyHex.empty() || ivHex.empty()) {
        fprintf(stderr, "decryptStockIBSS: no iBSS key/iv available -- cannot decrypt %s\n", srcImg3Path.c_str());
        return false;
    }

    std::vector<uint8_t> key, iv;
    if (!hexToBytes(keyHex, key) || (key.size() != 16 && key.size() != 24 && key.size() != 32)) {
        fprintf(stderr, "decryptStockIBSS: iBSS key is not 16/24/32-byte hex\n");
        return false;
    }
    if (!hexToBytes(ivHex, iv) || iv.size() != 16) {
        fprintf(stderr, "decryptStockIBSS: iBSS iv is not 16-byte hex\n");
        return false;
    }

    std::ifstream in(srcImg3Path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "decryptStockIBSS: cannot open %s\n", srcImg3Path.c_str());
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    if (buf.size() < sizeof(Img3File) || readU32(buf.data()) != kImg3Magic) {
        fprintf(stderr,
                "decryptStockIBSS: %s is not an img3 (a stock iBSS always is) -- nothing to decrypt\n",
                srcImg3Path.c_str());
        return false;
    }

    const uint32_t fullSize = readU32(buf.data() + offsetof(Img3File, fullSize));
    const uint32_t sizeNoPack = readU32(buf.data() + offsetof(Img3File, sizeNoPack));
    if (fullSize > buf.size() || sizeNoPack > fullSize) {
        fprintf(stderr, "decryptStockIBSS: img3 header sizes are out of range in %s\n", srcImg3Path.c_str());
        return false;
    }

    // Tag walk identical to check_img3_file_format(): tags begin at
    // fullSize - sizeNoPack and each advances by its own totalLength.
    uint32_t dataOffset = 0;
    uint32_t dataLength = 0;
    for (uint32_t tag = fullSize - sizeNoPack; tag + sizeof(Img3Tag) <= fullSize;) {
        const uint32_t magic = readU32(buf.data() + tag + offsetof(Img3Tag, magic));
        const uint32_t totalLength = readU32(buf.data() + tag + offsetof(Img3Tag, totalLength));
        const uint32_t tagDataLength = readU32(buf.data() + tag + offsetof(Img3Tag, dataLength));
        if (magic == kTagData) {
            dataOffset = tag + offsetof(Img3Tag, dataLength) + 4;  // = tag + 12, same as the parser
            dataLength = tagDataLength;
        }
        if (totalLength == 0) break;  // malformed; avoid an infinite loop
        tag += totalLength;
    }

    if (dataLength == 0 || dataOffset == 0) {
        fprintf(stderr, "decryptStockIBSS: no DATA tag found in %s\n", srcImg3Path.c_str());
        return false;
    }

    // Decrypt only the block-aligned span of the DATA payload, in place --
    // exactly the region an encrypted img3 stores ciphertext for. AES-CBC
    // rewrites each block, leaving the img3 wrapper (and its DATA tag header)
    // untouched, so check_img3_file_format() extracts now-plaintext bytes.
    const uint32_t alignedLen = dataLength - (dataLength % 16);
    if (alignedLen == 0 || static_cast<uint64_t>(dataOffset) + alignedLen > buf.size()) {
        fprintf(stderr, "decryptStockIBSS: DATA tag length (%u) is out of range in %s\n", dataLength,
                srcImg3Path.c_str());
        return false;
    }

    AES_KEY aesKey;
    if (AES_set_decrypt_key(key.data(), static_cast<int>(key.size() * 8), &aesKey) != 0) {
        fprintf(stderr, "decryptStockIBSS: AES_set_decrypt_key failed\n");
        return false;
    }
    // AES_cbc_encrypt mutates the iv buffer as it chains blocks; hand it a copy.
    unsigned char ivCopy[16];
    std::memcpy(ivCopy, iv.data(), sizeof(ivCopy));
    AES_cbc_encrypt(buf.data() + dataOffset, buf.data() + dataOffset, alignedLen, &aesKey, ivCopy, AES_DECRYPT);

    std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "decryptStockIBSS: cannot write %s\n", dstPath.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return static_cast<bool>(out);
}
