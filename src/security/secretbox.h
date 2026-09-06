#ifndef SECRETBOX_H_INCLUDED
#define SECRETBOX_H_INCLUDED

#include <array>
#include <string>

class SecretBox
{
public:
    bool init(const std::string &secret);
    bool ready() const { return initialized_; }
    bool encrypt(const std::string &plaintext, std::string &sealed) const;
    bool decrypt(const std::string &sealed, std::string &plaintext) const;

private:
    std::array<unsigned char, 32> key_{};
    bool initialized_ = false;
};

std::string sha256Hex(const std::string &value);
std::string randomUrlToken(std::size_t length);

#endif // SECRETBOX_H_INCLUDED
