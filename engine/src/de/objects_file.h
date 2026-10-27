// Reader for "Diversions Entertainment Objects File2" (.DEObjects) containers, following
// DE_CTransferAgentWinFile::LoadHeader (Core 0x70051bb0), DE_CTransferAgent::ProcessHeader
// (0x70055a70) and DE_CTransferObjectInfo[WinFile]::ProcessHeader (0x700557c0 / 0x7004fdd0).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

#include "de/contents.h"

namespace de {

// One entry of the header's table of contents (a DE_CTransferObjectInfoWinFile).
struct ObjectInfo {
	std::string name;        // e.g. "BHealth"
	std::string class_path;  // factory path, e.g. "Behavior", "UniqueActorProperty\Force\Common"
	Guid guid;
	bool flag = false;       // ProcessHeader v3+: when set the original calls New() eagerly
	int32_t value84 = 0;     // object-info +0x84, meaning unknown
	uint32_t flags = 0;      // object-info +0x2c (v4); bit 1 = contents are compressed
	uint32_t data_offset = 0;  // WinFile +0xa0: file offset of the object's contents blocks
};

// DE_CTransferAgent::ExtractReference (Core 0x70057690): one tag byte, then an index.
struct Reference {
	enum Kind : uint8_t { kNull, kLocal, kExternal };
	Kind kind = kNull;
	uint32_t index = 0;  // kLocal: object table index; kExternal: GUID pool index

	static Reference read(Contents &c);
	explicit operator bool() const { return kind != kNull; }
};

class ObjectsFile {
public:
	explicit ObjectsFile(std::vector<uint8_t> data);

	uint8_t format() const { return format_; }
	const ObjectInfo &master() const { return master_; }
	const std::vector<ObjectInfo> &objects() const { return objects_; }
	// Agent DE_CGUIDPool: GUIDs of objects in other files that this file references.
	const std::vector<Guid> &guid_pool() const { return guid_pool_; }

	static constexpr uint32_t kFlagCompressed = 2;

	// Stored at data_offset (RetrieveObjectContents 0x7004fbb0 + ReadEXDataHeader 0x700511f0).
	struct ObjectData {
		std::vector<uint8_t> contents;    // info +0x34, fed to the object's ObjProcessContents;
		                                  // decompressed by New() (0x70055140) when flags & 2
		std::vector<uint8_t> references;  // info +0x48 (role inferred from New())
		std::vector<uint8_t> hierarchy;   // info +0x5c (role inferred from ObjGenerateHierarchy)
		struct EX {
			// Raw key bytes: text keys include their NUL ("VMC2\0"); image keys are two
			// binary bytes {compressor id, sub-image index} with no terminator.
			std::vector<uint8_t> header;
			uint32_t value;  // file offset of the payload
			std::string_view key() const { return {reinterpret_cast<const char *>(header.data()), header.size()}; }
			// Matches either the exact bytes or a NUL-terminated text key.
			bool is(std::string_view k) const {
				auto raw = key();
				return raw == k || (raw.size() == k.size() + 1 && raw.back() == 0 && raw.substr(0, k.size()) == k);
			}
		};
		std::vector<EX> ex;  // extended data descriptors, fetched lazily by EXGetData
	};
	ObjectData object_data(const ObjectInfo &info) const;

	// DE_CObject::ObjProcessHierarchy (Core 0x7003c110): the hierarchy block is a v1 chunk with a
	// BOOL-terminated list of child references (ExtractChildReference 0x70057a80, local only).
	static std::vector<uint32_t> children(const ObjectData &d);
	// Index of each object's parent within this file, or -1 for roots.
	std::vector<int32_t> parents() const;

	// EXRequestData (0x70050130): at the entry's file offset, a u8 flag then a u32-length block.
	struct EXPayload {
		uint8_t flag;
		std::vector<uint8_t> data;
	};
	EXPayload ex_payload(const ObjectData::EX &ex) const;
	// Convenience: payload of the EX entry whose key (e.g. "VMC2") matches, if any.
	std::optional<EXPayload> ex_payload(const ObjectData &d, std::string_view key) const;

private:
	ObjectInfo read_object_info(Contents &c) const;

	mutable Contents file_;
	uint8_t format_ = 0;
	ObjectInfo master_;
	std::vector<ObjectInfo> objects_;
	std::vector<Guid> guid_pool_;
};

// Versioned chunk used by every Obj*/Process* method: u8 version, size (u8, or 0xFF + u32),
// then fields. Readers always seek to the end, so later fields can be skipped safely.
struct Chunk {
	uint8_t version;
	size_t end;
};
Chunk begin_chunk(Contents &c);
// WinFile's format-2 extension omits the version byte.
size_t begin_sized(Contents &c);

}  // namespace de
