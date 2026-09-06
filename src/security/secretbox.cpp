#include <array>
#include <cstddef>
#include <string>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "utils/base64/base64.h"
#include "secretbox.h"

bool SecretBox::init(const std::string &secret)
{
    if(secret.empty())
    {
        initialized_ = false;
        return false;
    }

    unsigned int digest_length = 0;
    if(EVP_Digest(secret.data(), secret.size(), key_.data(), &digest_length, EVP_sha256(), nullptr) != 1 || digest_length != key_.size())
    {
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    return true;
}

bool SecretBox::encrypt(const std::string &plaintext, std::string &sealed) const
{
    if(!initialized_)
        return false;

    constexpr int nonce_length = 12;
    constexpr int tag_length = 16;
    std::array<unsigned char, nonce_length> nonce{};
    if(RAND_bytes(nonce.data(), nonce.size()) != 1)
        return false;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if(!ctx)
        return false;

    std::string ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH, '\0');
    std::array<unsigned char, tag_length> tag{};
    int written = 0, final_written = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce_length, nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce.data()) == 1
        && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(ciphertext.data()), &written,
                             reinterpret_cast<const unsigned char *>(plaintext.data()), plaintext.size()) == 1
        && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(ciphertext.data()) + written, &final_written) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag_length, tag.data()) == 1;
    if(ok)
    {
        ciphertext.resize(static_cast<std::size_t>(written + final_written));
        std::string packed(reinterpret_cast<const char *>(nonce.data()), nonce.size());
        packed.append(reinterpret_cast<const char *>(tag.data()), tag.size());
        packed.append(ciphertext);
        sealed = base64Encode(packed);
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool SecretBox::decrypt(const std::string &sealed, std::string &plaintext) const
{
    if(!initialized_)
        return false;

    constexpr int nonce_length = 12;
    constexpr int tag_length = 16;
    const std::string packed = base64Decode(sealed);
    if(packed.size() < nonce_length + tag_length)
        return false;

    const unsigned char *nonce = reinterpret_cast<const unsigned char *>(packed.data());
    const unsigned char *tag = nonce + nonce_length;
    const unsigned char *ciphertext = tag + tag_length;
    const int ciphertext_length = static_cast<int>(packed.size() - nonce_length - tag_length);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if(!ctx)
        return false;

    plaintext.assign(static_cast<std::size_t>(ciphertext_length + EVP_MAX_BLOCK_LENGTH), '\0');
    int written = 0, final_written = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce_length, nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce) == 1
        && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plaintext.data()), &written, ciphertext, ciphertext_length) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag_length, const_cast<unsigned char *>(tag)) == 1
        && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plaintext.data()) + written, &final_written) == 1;
    if(ok)
        plaintext.resize(static_cast<std::size_t>(written + final_written));
    else
        plaintext.clear();
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

std::string sha256Hex(const std::string &value)
{
    std::array<unsigned char, 32> digest{};
    unsigned int digest_length = 0;
    if(EVP_Digest(value.data(), value.size(), digest.data(), &digest_length, EVP_sha256(), nullptr) != 1 || digest_length != digest.size())
        return "";

    static const char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2);
    for(unsigned char byte : digest)
    {
        output.push_back(hex[byte >> 4]);
        output.push_back(hex[byte & 0x0f]);
    }
    return output;
}

std::string randomUrlToken(std::size_t length)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
    constexpr unsigned int alphabet_size = sizeof(alphabet) - 1;
    constexpr unsigned int limit = 256U - (256U % alphabet_size);
    std::string output;
    output.reserve(length);
    while(output.size() < length)
    {
        std::array<unsigned char, 32> bytes{};
        if(RAND_bytes(bytes.data(), bytes.size()) != 1)
            return "";
        for(unsigned char byte : bytes)
        {
            if(static_cast<unsigned int>(byte) >= limit)
                continue;
            output.push_back(alphabet[byte % alphabet_size]);
            if(output.size() == length)
                break;
        }
    }
    return output;
}
