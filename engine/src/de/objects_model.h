// A lossless in-memory model of a "Diversions Entertainment Objects File2" (.DEObjects), shared
// by tools/deobjects-editor and the engine's write_objects_file.
//
// Every byte of a shipped file is kept: fields the editor understands are decoded, anything
// after them is carried as raw "rest" bytes. Writing lays the file out the way the original
// DE_CTransferAgentWinFile::SaveToFile does (header, per-object data, extended data payloads),
// so reading and writing an unmodified file gives back the same bytes. docs/DEOBJECTS_FORMAT.md
// describes the format.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace deo {

using Bytes = std::vector<uint8_t>;

struct Error : std::runtime_error {
	using std::runtime_error::runtime_error;
};

struct Guid {
	uint8_t b[16] = {};
	bool operator==(const Guid &o) const;
	// Windows text form (first three groups little-endian), as the game's tools print it.
	std::string str() const;
	static Guid parse(const std::string &text);  // throws Error
};

// A versioned chunk's size field: a u8, or 0xFF then a u32. "long_size" records a u32 size
// that would have fitted in a byte, so such (unusual) chunks are written back the same way.
struct ChunkSize {
	bool long_size = false;
};

// DE_CTransferObjectInfo (one header entry) and its WinFile extension.
struct Info {
	uint8_t version = 4;  // base chunk version, 1..4
	ChunkSize base_size;
	Bytes name;        // a stored string: u32 length then the bytes, NUL included
	Bytes class_path;  // factory path, e.g. "Description\Environment"
	Guid guid;         // v2+
	uint8_t flag = 0;  // v3+ BOOL; nonzero = the original creates the object eagerly
	int32_t value84 = 0;  // v3+ info +0x84
	uint32_t flags = 0;   // v4+ info +0x2c; bit 1 (value 2) = contents are compressed
	Bytes base_rest;      // bytes after the known fields
	ChunkSize ext_size;
	uint32_t data_offset = 0;  // recomputed when writing
	Bytes ext_rest;

	std::string name_text() const;   // without the NUL, Latin-1 bytes as they are
	std::string class_text() const;
};

// One extended-data entry (DE_CTransferObjectInfoWinFile::ReadEXDataHeader / EXRequestData).
struct Ex {
	Bytes key;           // text keys keep their NUL ("VMC2\0"); image keys are {0, index}
	uint8_t flag = 0;    // payload's leading byte
	Bytes data;          // payload block
};

struct Object {
	Info info;
	// Contents as the class reads them (decompressed when info.flags & 2).
	Bytes contents;
	// For compressed objects: the original stored block. Written back as it is while it still
	// decompresses to `contents`; otherwise new contents are stored with method 0 (no packing).
	Bytes packed;
	// Write `packed` as it is, without checking it against `contents`.
	bool packed_as_is = false;
	Bytes references;
	Bytes hierarchy;
	std::vector<Ex> ex;

	static constexpr uint32_t kCompressed = 2;
	bool compressed() const { return (info.flags & kCompressed) != 0; }
};

struct File {
	Bytes signature;  // "Diversions Entertainment Objects File2" with its NUL
	uint8_t format = 2;
	uint8_t header_version = 9;
	ChunkSize header_size;
	Info master;  // the transfer agent itself (class TransferAgent\Win\File)
	// The agent's own contents chunk (DE_CTransferAgent::ObjProcessContents).
	uint8_t own_version = 1;
	ChunkSize own_size;
	Bytes own_block;      // agent +0xd0, empty in shipped data
	Guid own_guid;
	uint8_t pool_flag = 1;  // 1 = the GUID pool follows
	std::vector<Guid> guid_pool;  // objects in other files that this file refers to
	uint8_t own_bool = 1;   // agent +0x108
	Bytes own_rest;
	Bytes header_rest;    // inside the header chunk, after the object table
	std::vector<Object> objects;
	Bytes trailing;       // bytes after the last payload (none in shipped data)

	// Notes from reading: places where the layout differs from what write() produces.
	std::vector<std::string> warnings;

	// header_only: stop after the object table (objects then have only their info); `data`
	// then needs to hold only the start of the file, up to the end of the header block.
	static File read(const Bytes &data, bool header_only = false);
	Bytes write() const;
};

// Reading helpers shared with the field decoders.
class Reader {
public:
	explicit Reader(const Bytes &b, size_t begin = 0, size_t end = SIZE_MAX)
	    : b_(b), p_(begin), end_(end == SIZE_MAX ? b.size() : end) {}
	size_t pos() const { return p_; }
	size_t end() const { return end_; }
	size_t remaining() const { return end_ - p_; }
	void seek(size_t p);
	uint8_t u8();
	uint16_t u16();
	uint32_t u32();
	Bytes bytes(size_t n);
	Bytes block() { return bytes(u32()); }
	Guid guid();
	// A chunk header: returns the version and sets *end to the chunk's end.
	uint8_t chunk(size_t *end, ChunkSize *size = nullptr);
	size_t sized(ChunkSize *size = nullptr);  // size field only (no version)

private:
	void need(size_t n) const;
	const Bytes &b_;
	size_t p_;
	size_t end_;
};

class Writer {
public:
	Bytes &data() { return d_; }
	size_t size() const { return d_.size(); }
	void u8(uint8_t v) { d_.push_back(v); }
	void u16(uint16_t v);
	void u32(uint32_t v);
	void bytes(const Bytes &b) { d_.insert(d_.end(), b.begin(), b.end()); }
	void block(const Bytes &b) {
		u32(uint32_t(b.size()));
		bytes(b);
	}
	void guid(const Guid &g) { d_.insert(d_.end(), g.b, g.b + 16); }
	// begin_chunk writes the version; end_chunk inserts the size before the body.
	size_t begin_chunk(uint8_t version) {
		u8(version);
		return d_.size();
	}
	size_t begin_sized() { return d_.size(); }
	void end_chunk(size_t at, ChunkSize size = {});

private:
	Bytes d_;
};

// Hex text for raw bytes, and back (whitespace ignored).
std::string to_hex(const Bytes &b);
Bytes from_hex(const std::string &s);

// Text bytes (Latin-1) to UTF-8 and back. from_utf8 throws for characters outside Latin-1.
std::string latin1_to_utf8(const Bytes &b);
Bytes utf8_to_latin1(const std::string &s);

// Decompress a stored contents block (DE_CContents::Decompress: methods 0, 6, 7).
Bytes decompress(const Bytes &packed);

}  // namespace deo
