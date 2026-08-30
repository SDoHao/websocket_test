#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace ws_utils {
    void sha1(const uint8_t* msg, size_t len, uint8_t out[20]);
    std::string base64_encode(const uint8_t* data, size_t len);
}
