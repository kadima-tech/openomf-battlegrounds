#include "de/objects_file.h"

namespace de {

namespace {

constexpr const char *kSignature1 = "Diversions Entertainment Objects File";
constexpr const char *kSignature2 = "Diversions Entertainment Objects File2";

// The size field is relative to the position after the size itself.
size_t read_size(Contents &c) {
	uint8_t b = c.u8();
	size_t n = b == 0xFF ? c.u32() : b;
	return c.pos() + n;
}

}  // namespace

Reference Reference::read(Contents &c) {
	uint8_t tag = c.u8();
	if (tag >= 0x32) {
		return {kLocal, uint32_t(tag - 0x32)};
	}
	switch (tag) {
	case 1:
		return {kLocal, c.u16()};
	case 2:
		return {kExternal, c.u8()};
	case 3:
		return {kExternal, c.u16()};
	case 4:
		return {kExternal, c.u32()};
	default:
		return {};
	}
}

Chunk begin_chunk(Contents &c) {
	uint8_t v = c.u8();
	return {v, read_size(c)};
}

size_t begin_sized(Contents &c) { return read_size(c); }

ObjectsFile::ObjectsFile(std::vector<uint8_t> data) : file_(std::move(data)) {
	std::string sig = file_.string();
	if (sig == kSignature2) {
		format_ = file_.u8();
		if (format_ > 1) {
			file_.u32();  // master object vtable +0xAC; zero in all shipped files
		}
	} else if (sig != kSignature1) {
		throw FormatError("not a DEObjects file: \"" + sig + "\"");
	}
	if (format_ != 2) {
		throw FormatError("unsupported DEObjects format " + std::to_string(format_));
	}

	Contents header(file_.block());
	Chunk agent = begin_chunk(header);
	if (agent.version != 9) {
		// Versions 1-8 exist in ProcessHeader but are not used by shipped data.
		throw FormatError("unsupported transfer agent header version " + std::to_string(agent.version));
	}
	master_ = read_object_info(header);

	// The agent's own ObjProcessContents (0x700545b0).
	Chunk own = begin_chunk(header);
	if (own.version == 1) {
		header.block();  // agent +0xd0, empty in shipped data
		header.guid();
		if (header.u8() == 1) {  // DE_CGUIDPool::ProcessContents
			uint16_t n = header.u16();
			for (uint16_t i = 0; i < n; ++i) {
				guid_pool_.push_back(header.guid());
			}
		}
		header.boolean();  // agent +0x108
	}
	header.seek(own.end);

	int32_t count = header.i32();
	for (int32_t i = 0; i < count; ++i) {
		objects_.push_back(read_object_info(header));
	}
	header.seek(agent.end);
}

ObjectInfo ObjectsFile::read_object_info(Contents &c) const {
	ObjectInfo info;
	Chunk base = begin_chunk(c);
	if (base.version >= 1 && base.version <= 4) {
		info.name = c.string();
		info.class_path = c.string();
		if (base.version >= 2) {
			info.guid = c.guid();
		}
		if (base.version >= 3) {
			info.flag = c.boolean();
			info.value84 = c.i32();
		}
		if (base.version >= 4) {
			info.flags = c.u32();
		}
	}
	c.seek(base.end);

	// DE_CTransferObjectInfoWinFile extension: format 2 has no version byte.
	size_t end = begin_sized(c);
	info.data_offset = c.u32();
	c.seek(end);
	return info;
}

ObjectsFile::ObjectData ObjectsFile::object_data(const ObjectInfo &info) const {
	size_t saved = file_.pos();
	file_.seek(info.data_offset);
	ObjectData d;
	d.contents = file_.block();
	d.references = file_.block();
	d.hierarchy = file_.block();
	uint32_t n = file_.u32();
	for (uint32_t i = 0; i < n; ++i) {
		auto header = file_.block();
		d.ex.push_back({std::move(header), file_.u32()});
	}
	file_.seek(saved);
	if (info.flags & kFlagCompressed) {
		d.contents = Contents::decompress(d.contents);
	}
	return d;
}

std::vector<uint32_t> ObjectsFile::children(const ObjectData &d) {
	std::vector<uint32_t> out;
	if (d.hierarchy.empty()) {
		return out;
	}
	Contents c(d.hierarchy);
	Chunk ch = begin_chunk(c);
	if (ch.version == 1) {
		while (c.pos() < ch.end && c.boolean()) {
			Reference r = Reference::read(c);
			if (r.kind == Reference::kLocal) {
				out.push_back(r.index);
			}
		}
	}
	return out;
}

std::vector<int32_t> ObjectsFile::parents() const {
	std::vector<int32_t> p(objects_.size(), -1);
	for (size_t i = 0; i < objects_.size(); ++i) {
		for (uint32_t ch : children(object_data(objects_[i]))) {
			if (ch < p.size()) {
				p[ch] = int32_t(i);
			}
		}
	}
	return p;
}

ObjectsFile::EXPayload ObjectsFile::ex_payload(const ObjectData::EX &ex) const {
	size_t saved = file_.pos();
	file_.seek(ex.value);
	EXPayload p;
	p.flag = file_.u8();
	p.data = file_.block();
	file_.seek(saved);
	return p;
}

std::optional<ObjectsFile::EXPayload> ObjectsFile::ex_payload(const ObjectData &d, std::string_view key) const {
	for (auto &e : d.ex) {
		if (e.is(key)) {
			return ex_payload(e);
		}
	}
	return std::nullopt;
}

}  // namespace de
