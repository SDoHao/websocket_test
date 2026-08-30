#include "ws_utils.h"
#include <vector>
#include <cstring>

namespace ws_utils {
static uint32_t rol32(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

void sha1(const uint8_t* msg, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint64_t bitlen = (uint64_t)len * 8;
    size_t padded = len + 1;
    while ((padded % 64) != 56) padded++;
    std::vector<uint8_t> buf(padded + 8, 0);
    memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    for (int i = 0; i < 8; ++i) buf[padded + i] = (uint8_t)(bitlen >> (56 - 8 * i));
    
    for (size_t off = 0; off < buf.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)buf[off + 4*i] << 24) | ((uint32_t)buf[off + 4*i + 1] << 16) |
                   ((uint32_t)buf[off + 4*i + 2] << 8)  | ((uint32_t)buf[off + 4*i + 3]);
        for (int i = 16; i < 80; ++i)
            w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[4*i] = (hs[i] >> 24) & 0xff; out[4*i+1] = (hs[i] >> 16) & 0xff;
        out[4*i+2] = (hs[i] >> 8) & 0xff; out[4*i+3] = hs[i] & 0xff;
    }
}

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        out.push_back(B64[(n >> 18) & 63]);
        out.push_back(B64[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? B64[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? B64[n & 63] : '=');
    }
    return out;
}
}
