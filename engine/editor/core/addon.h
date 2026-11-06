// The editor's data model: the base game's Data folder (read-only) and a project, one add-on
// .DEObjects that adds objects with fresh GUIDs (docs/EDITOR_SPEC.md §1, §7).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "de/objects_model.h"

namespace ed {

using deo::Bytes;
using deo::Guid;

std::string guid_key(const Guid &g);
bool guid_zero(const Guid &g);
Guid fresh_guid();
Bytes text_bytes(const std::string &s);  // a stored string: the bytes and their NUL
std::string text_of(const Bytes &b);

// One versioned chunk of an object block, kept with its size form.
struct RawChunk {
	uint8_t version = 1;
	deo::ChunkSize size;
	Bytes body;
};
struct Chunks {
	std::vector<RawChunk> chunks;
	Bytes tail;  // bytes after the last whole chunk
};
Chunks split_chunks(const Bytes &block);
Bytes join_chunks(const Chunks &c);

// DE_CTransferAgent references (AddReference's encoding, docs/DEOBJECTS_FORMAT.md).
struct Ref {
	enum Kind : uint8_t { Null, Local, Pool } kind = Null;
	uint32_t index = 0;
	bool raw_form = false;  // read in a form AddReference would not write; kept as read
	Bytes raw;
};
Ref read_ref(deo::Reader &r);
void write_ref(deo::Writer &w, const Ref &ref);

// An object of the base data, from the files' headers.
struct BaseObject {
	size_t file = 0;  // index into BaseData::files()
	uint32_t index = 0;
	std::string name, class_path;
	Guid guid;
};

class BaseData {
public:
	explicit BaseData(const std::filesystem::path &dir);
	struct FileIndex {
		std::string stem;
		std::filesystem::path path;
		std::string agent_name;
		Guid agent;
		std::vector<BaseObject> objects;
	};
	const std::filesystem::path &dir() const { return dir_; }
	const std::vector<FileIndex> &files() const { return files_; }
	const BaseObject *by_guid(const Guid &g) const;
	const FileIndex *file(const std::string &stem) const;
	std::vector<const BaseObject *> of_class(const std::string &class_path) const;
	// The whole file, read once and kept.
	const deo::File &model(size_t file) const;
	size_t object_count() const;

private:
	std::filesystem::path dir_;
	std::vector<FileIndex> files_;
	std::map<std::string, const BaseObject *> guids_;
	mutable std::map<size_t, std::unique_ptr<deo::File>> models_;
};

// A project: the add-on file, edited in memory and written by save().
class Project {
public:
	Project(const BaseData &base, std::filesystem::path path, deo::File file);
	// A new, empty add-on whose agent is named after the file.
	static std::unique_ptr<Project> create(const BaseData &base, const std::filesystem::path &path);
	static std::unique_ptr<Project> open(const BaseData &base, const std::filesystem::path &path);
	void save();
	Bytes bytes() const { return file_.write(); }

	const BaseData &base() const { return base_; }
	const std::filesystem::path &path() const { return path_; }
	std::string agent_name() const { return file_.master.name_text(); }
	const Guid &agent() const { return file_.master.guid; }
	deo::File &file() { return file_; }
	const deo::File &file() const { return file_; }
	bool dirty() const { return dirty_; }
	void touch() { dirty_ = true; }

	// The GUID pool index for an object in another file, added when missing.
	uint32_t pool_index(const Guid &g);
	int find(const Guid &g) const;
	// Children of a local object (its hierarchy list).
	std::vector<uint32_t> children(uint32_t object) const;
	// Deep copy of base objects and everything under them, with fresh GUIDs. References to
	// copied objects follow the copies; others go through the GUID pool. Returns the new
	// indices of `roots`. Throws deo::Error for a class whose references are not understood.
	std::vector<uint32_t> copy_from_base(size_t file, const std::vector<uint32_t> &roots);
	// The object a reference in this file names: a local index or a GUID from the pool.
	struct Target {
		int local = -1;
		Guid guid;
		bool resolved = false;
	};
	Target target(const Ref &r) const;

private:
	const BaseData &base_;
	std::filesystem::path path_;
	deo::File file_;
	bool dirty_ = false;
};

// Rewrites the references in one chunk of a references block for `class_path`, mapping every
// reference through `map`. Returns false when the chunk's layout is not known.
bool map_reference_chunk(const std::string &class_path, size_t chunk_index, const RawChunk &in, RawChunk &out,
                         const std::function<Ref(const Ref &)> &map);
// Every reference in an object's references block (known layouts only).
std::vector<Ref> object_references(const std::string &class_path, const Bytes &references, bool *understood = nullptr);

}  // namespace ed
