#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <string>
#include <openssl/sha.h>

std::string sha1_to_hex(unsigned char *hash);
std::string calculate_sha1(const char *data, size_t len);

#endif // CRYPTO_UTILS_H
