// Read side of DE_CContents, the engine's serialization stream (Core.ModuleDLL).
// All multi-byte values are little-endian, matching the original x86 build.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace de {

struct FormatError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

struct ObsoleteCompression : FormatError {
	using FormatError::FormatError;
};

struct Guid {
	uint8_t bytes[16];
	std::string str() const;
};

class Contents {
public:
	Contents() = default;
	explicit Contents(std::vector<uint8_t> data) : data_(std::move(data)) {}

	size_t pos() const { return pos_; }
	size_t size() const { return data_.size(); }
	size_t remaining() const { return data_.size() - pos_; }
	void seek(size_t p);
	void skip(size_t n);
	const std::vector<uint8_t> &data() const { return data_; }

	uint8_t u8();
	uint16_t u16();
	uint32_t u32();
	int32_t i32() { return static_cast<int32_t>(u32()); }
	float f32();
	bool boolean() { return u8() != 0; }  // ExtractBOOL (0x70019b00) reads one byte
	std::span<const uint8_t> bytes(size_t n);

	// ExtractSqueezed: <0xFE literal byte, 0xFE -> u16, 0xFF -> u32.
	uint32_t squeezed();
	// Extract(char**): u32 length (including NUL) followed by the bytes.
	std::string string();
	// Extract(DE_CDataBlock&) / Extract(DE_CContents&): u32 length + raw bytes.
	std::vector<uint8_t> block();
	Guid guid();

	// DE_CContents::Decompress: squeezed method id, then method-specific payload.
	// Consumes the whole remaining stream and returns the decoded bytes.
	static std::vector<uint8_t> decompress(std::span<const uint8_t> packed);

private:
	void need(size_t n) const;

	std::vector<uint8_t> data_;
	size_t pos_ = 0;
};

}  // namespace de
