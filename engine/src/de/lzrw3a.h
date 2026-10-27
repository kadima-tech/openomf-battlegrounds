// LZRW3-A decompressor (Ross Williams, 1991, public domain algorithm), as used by
// DE_CLZRWContentsCompressor::Decompress in Core.ModuleDLL (0x7000c200).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace de::lzrw3a {

std::vector<uint8_t> decompress(std::span<const uint8_t> src, size_t out_len);

}  // namespace de::lzrw3a
