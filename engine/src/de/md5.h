// MD5 (RFC 1321), used to verify method-7 compressed blocks.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace de {

std::array<uint8_t, 16> md5(std::span<const uint8_t> data);

}  // namespace de
