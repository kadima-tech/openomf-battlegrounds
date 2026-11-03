// Field layouts of the versioned chunks inside an object's contents, references and hierarchy
// blocks, per class. Each block is a run of chunks, one per class level of the object (base
// class first). A schema names the fields of one of those chunks; a chunk without a schema (or
// whose bytes do not fit it) is shown as hex and written back unchanged.
//
// To document another class, add an entry to the table in schema.cpp. Wrong guesses cannot
// corrupt a file: a chunk is only shown decoded when re-encoding the decoded fields gives back
// exactly the original bytes.
#pragma once

#include <string>
#include <vector>

namespace deo {

enum class Block { Contents, References, Hierarchy };

enum class FieldType {
	U8,
	Bool,        // one byte; shown as true/false when 0/1, else as a number
	I32,
	U32,
	F32,
	Text,        // u32 length then the bytes, NUL included (DE_CDataBlock)
	Guid,
	PassiveRef,  // CPassiveReference: BOOL set, then a sized {file GUID, object GUID}
	Ref,         // DE_CTransferAgent reference (tag byte, then an index)
	OptRef,      // BOOL present, then a Ref
	RefList,     // BOOL-terminated list of Refs (each preceded by BOOL 1)
};

struct Field {
	const char *name;
	FieldType type;
	int min_version = 0;  // present from this chunk version on
};

struct ChunkSchema {
	Block block;
	const char *class_prefix;  // matches this class path and everything below it ("" = any)
	int index;                 // chunk position within the block
	const char *label;         // the class level the chunk belongs to
	std::vector<Field> fields;
	const char *source;        // where the layout comes from
};

// The most specific schema for a chunk, or null.
const ChunkSchema *find_schema(Block block, const std::string &class_path, size_t index);
const std::vector<ChunkSchema> &all_schemas();
const char *type_name(FieldType t);

}  // namespace deo
