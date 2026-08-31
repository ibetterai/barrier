/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "base/Sha256.h"

#include <openssl/evp.h>

#include <array>
#include <memory>
#include <stdexcept>

namespace barrier {
namespace {

struct DigestContextDeleter {
    void operator()(EVP_MD_CTX* context) const
    {
        EVP_MD_CTX_free(context);
    }
};

} // namespace

std::string sha256Hex(const std::string& input)
{
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context(EVP_MD_CTX_new());
    if (!context) {
        throw std::runtime_error("failed to allocate SHA-256 context");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), digest.data(), &digestLength) != 1) {
        throw std::runtime_error("failed to compute SHA-256 digest");
    }

    static const char kHex[] = "0123456789abcdef";
    std::string result;
    result.resize(digestLength * 2);
    for (unsigned int i = 0; i < digestLength; ++i) {
        result[i * 2] = kHex[digest[i] >> 4];
        result[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return result;
}

} // namespace barrier
