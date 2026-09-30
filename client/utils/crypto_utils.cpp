#include "utils/crypto_utils.h"
#include <cstdio>

using namespace std;

string sha1_to_hex(unsigned char *hash)
{
    char hex_str[41];
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
    {
        sprintf(&hex_str[i * 2], "%02x", hash[i]);
    }
    hex_str[40] = '\0';
    return string(hex_str);
}

string calculate_sha1(const char *data, size_t len)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)data, len, hash);
    return sha1_to_hex(hash);
}
