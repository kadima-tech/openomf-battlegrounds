// Between the lossless model and the editable JSON form (see tools/deobjects-editor/README.md).
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "json.h"
#include "de/objects_model.h"

namespace deo {

// Where an object's large binary payloads go when converting to JSON. Called with the object
// index, a short tag ("ex0", "packed", ...) and the bytes; returns the path to record in the
// JSON, or "" to leave the payload out (the JSON then only gives its size and cannot be packed).
using SaveBlob = std::function<std::string(size_t object, const std::string &tag, const Bytes &data)>;
// Reads back a path recorded by SaveBlob.
using LoadBlob = std::function<Bytes(const std::string &path)>;

// Payloads up to this many bytes are written inline as hex.
constexpr size_t kInlineLimit = 64;

// Optional names for objects in other files, by GUID text (see scan_names); shown after
// references to the GUID pool so they can be told apart.
using GuidNames = std::map<std::string, std::string>;

json::Value index_to_json(const File &f, const std::vector<std::string> &object_paths);
json::Value object_to_json(const File &f, size_t index, const SaveBlob &save, const GuidNames *names = nullptr);

// Builds a file from an index and its objects' JSON (in the index's order). Problems that do
// not stop packing (a reference whose name no longer matches its index) go to *warnings.
File file_from_json(const json::Value &index, const std::vector<json::Value> &objects, const LoadBlob &load,
                    std::vector<std::string> *warnings);

// Unpacks `f` to JSON text in memory and packs it again, as unpack then pack would.
File repack(const File &f);

// A file name for an object's JSON: "0004 The Desert.json", safe on Windows and macOS.
std::string object_file_name(const File &f, size_t index);

}  // namespace deo
