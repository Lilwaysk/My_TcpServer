#include "util/Crypto.h"

#include <fstream>
#include <stdexcept>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
/* 非本人实现 */
namespace {

std::vector<unsigned char> fromHex(const std::string& text)
{
    if (text.size() % 2 != 0) {
        throw std::runtime_error("hex input has odd length");
    }

    std::vector<unsigned char> output;
    output.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned int value = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const char ch = text[i + j];
            unsigned int digit = 0;
            if (ch >= '0' && ch <= '9') {
                digit = static_cast<unsigned int>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                digit = static_cast<unsigned int>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                digit = static_cast<unsigned int>(ch - 'A' + 10);
            } else {
                throw std::runtime_error("invalid hex character");
            }
            value = value * 16 + digit;
        }
        output.push_back(static_cast<unsigned char>(value));
    }
    return output;
}

std::string toHex(const unsigned char* data, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string output;
    output.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        output[i * 2] = digits[data[i] >> 4];
        output[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    return output;
}

} // namespace

namespace m2 {

std::string randomHex(std::size_t bytes)
{
    if (bytes == 0) {
        return std::string();
    }

    std::vector<unsigned char> buffer(bytes);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom) {
        throw std::runtime_error("cannot open /dev/urandom");
    }
    urandom.read(reinterpret_cast<char*>(&buffer[0]),
                 static_cast<std::streamsize>(buffer.size()));
    if (!urandom || static_cast<std::size_t>(urandom.gcount()) != bytes) {
        throw std::runtime_error("cannot read /dev/urandom");
    }
    return toHex(&buffer[0], buffer.size());
}

std::string sha256Hex(const std::string& data)
{
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestSize = 0;
    const bool ok =
        EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1
        && EVP_DigestUpdate(context, data.data(), data.size()) == 1
        && EVP_DigestFinal_ex(context, digest, &digestSize) == 1;
    EVP_MD_CTX_free(context);

    if (!ok) {
        throw std::runtime_error("SHA-256 failed");
    }
    return toHex(digest, digestSize);
}
