// Browsing and viewing content: object types for the Content Browser, best-effort reference
// decoding, decoded fields for the Details panel, and the "Referenced by" index.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon.h"

namespace ed {

// An object's references: the known layouts first, then a grammar of BOOL-terminated lists,
// counted lists, optional references and nested chunks. Indices outside `objects` / `pool`
// reject a parse. *complete is false when some bytes could not be explained.
std::vector<Ref> decode_references(const std::string &class_path, const Bytes &references, size_t objects, size_t pool,
                                   bool *complete = nullptr);
// A references chunk of no known layout, rewritten through the grammar decode_references uses
// (a best guess: lists, counted lists, optional references, nested chunks).
bool guess_map_reference_chunk(const RawChunk &in, RawChunk &out, size_t objects, size_t pool, const std::function<Ref(const Ref &)> &map);
// The GUID a reference in `file` names (local object or pool entry); zero when it is null.
Guid ref_guid(const deo::File &file, const Ref &r);

// A description's resource: the object GUID of its passive reference, or zero.
Guid description_resource(const deo::Object &description);
// A description's icon (references chunk v6, after the casts), or zero.
Guid description_icon(const deo::File &file, const deo::Object &description);

struct Field {
	Field(std::string l, std::string v, Guid g = {}) : label(std::move(l)), value(std::move(v)), link(g) {}
	std::string label, value;
	Guid link;  // a referenced object, when the value names one
};
// What the editor can decode of an object, by class. Names of referenced objects are looked up
// in `file`, then the base data. `addon` is the project when `file` is its add-on.
std::vector<Field> describe_object(const BaseData &base, const deo::File &file, uint32_t index, const Project *addon = nullptr);

// Parent of every object (hierarchy), -1 for roots.
std::vector<int> object_parents(const deo::File &file);

// Image\Standard header: type, width, height; false when it does not parse.
bool image_header(const deo::Object &image, int *type, int *width, int *height);
// Behavior source text (DE_CPCode elements), UTF-8; empty when it does not parse.
std::string behavior_source(const deo::Object &behavior);
// Noise2\Stream samples: the RIFF WAVE bytes, or empty.
Bytes sound_wave(const deo::Object &noise);

// Who references whom across the base data, built on a background thread (file by file, each
// file read and dropped). Only references decode_references explains are counted.
class ReferenceIndex {
public:
	explicit ReferenceIndex(const BaseData &base);
	~ReferenceIndex();
	void start();
	bool ready() const { return done_ == total_; }
	size_t progress() const { return done_; }
	size_t total() const { return total_; }
	// The objects whose references name `g`.
	std::vector<const BaseObject *> referencers(const Guid &g) const;

private:
	const BaseData &base_;
	std::thread worker_;
	std::atomic<bool> stop_{false};
	std::atomic<size_t> done_{0};
	size_t total_ = 0;
	bool started_ = false;
	mutable std::mutex mutex_;
	std::map<std::string, std::vector<const BaseObject *>> by_target_;
};

}  // namespace ed
