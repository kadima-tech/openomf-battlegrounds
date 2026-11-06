#include "de/objects_model.h"

#include <cstdio>
#include <cstring>

#include "de/contents.h"

namespace deo {

namespace {

constexpr const char kSignature2[] = "Diversions Entertainment Objects File2";

std::string text_of(const Bytes &b) {
	size_t n = b.size();
	if (n && b[n - 1] == 0) --n;
	return std::string(b.begin(), b.begin() + long(n));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// GUIDs, hex and text

bool Guid::operator==(const Guid &o) const { return memcmp(b, o.b, 16) == 0; }

std::string Guid::str() const {
	char buf[40];
	snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2],
	         b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return buf;
}

Guid Guid::parse(const std::string &text) {
	std::string hex;
	for (char c : text) {
		if (c == '-' || c == '{' || c == '}') continue;
		hex += c;
	}
	if (hex.size() != 32) throw Error("not a GUID: \"" + text + "\"");
	Bytes v = from_hex(hex);
	static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	Guid g;
	for (int i = 0; i < 16; ++i) g.b[order[i]] = v[size_t(i)];
	return g;
}

std::string to_hex(const Bytes &b) {
	static const char *digits = "0123456789abcdef";
	std::string s;
	s.reserve(b.size() * 2);
	for (uint8_t v : b) {
		s += digits[v >> 4];
		s += digits[v & 15];
	}
	return s;
}

Bytes from_hex(const std::string &s) {
	Bytes out;
	int hi = -1;
	for (char c : s) {
		int v;
		if (c >= '0' && c <= '9') v = c - '0';
		else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
		else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
		else throw Error(std::string("bad hex digit '") + c + "'");
		if (hi < 0) {
			hi = v;
		} else {
			out.push_back(uint8_t(hi << 4 | v));
			hi = -1;
		}
	}
	if (hi >= 0) throw Error("hex text has an odd number of digits");
	return out;
}

std::string latin1_to_utf8(const Bytes &b) {
	std::string out;
	for (uint8_t c : b) {
		if (c < 0x80) {
			out += char(c);
		} else {
			out += char(0xC0 | (c >> 6));
			out += char(0x80 | (c & 0x3F));
		}
	}
	return out;
}

Bytes utf8_to_latin1(const std::string &s) {
	Bytes out;
	for (size_t i = 0; i < s.size(); ++i) {
		uint8_t c = uint8_t(s[i]);
		if (c < 0x80) {
			out.push_back(c);
		} else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
			uint32_t cp = uint32_t(c & 0x1F) << 6 | (uint8_t(s[i + 1]) & 0x3F);
			if (cp > 0xFF) throw Error("character U+" + std::to_string(cp) + " cannot be stored (Latin-1 only)");
			out.push_back(uint8_t(cp));
			++i;
		} else {
			throw Error("text \"" + s + "\" has a character outside Latin-1, which the game cannot store");
		}
	}
	return out;
}

Bytes decompress(const Bytes &packed) {
	try {
		return de::Contents::decompress(packed);
	} catch (const std::exception &e) {
		throw Error(e.what());
	}
}

// ---------------------------------------------------------------------------------------------
// Reader / Writer

void Reader::need(size_t n) const {
	if (n > remaining()) {
		throw Error("read of " + std::to_string(n) + " bytes at offset " + std::to_string(p_) + " runs past the end (" +
		            std::to_string(end_) + ")");
	}
}

void Reader::seek(size_t p) {
	if (p > end_) throw Error("seek past the end");
	p_ = p;
}

uint8_t Reader::u8() {
	need(1);
	return b_[p_++];
}

uint16_t Reader::u16() {
	need(2);
	uint16_t v = uint16_t(b_[p_] | b_[p_ + 1] << 8);
	p_ += 2;
	return v;
}

uint32_t Reader::u32() {
	need(4);
	uint32_t v = uint32_t(b_[p_]) | uint32_t(b_[p_ + 1]) << 8 | uint32_t(b_[p_ + 2]) << 16 | uint32_t(b_[p_ + 3]) << 24;
	p_ += 4;
	return v;
}

Bytes Reader::bytes(size_t n) {
	need(n);
	Bytes out(b_.begin() + long(p_), b_.begin() + long(p_ + n));
	p_ += n;
	return out;
}

Guid Reader::guid() {
	need(16);
	Guid g;
	memcpy(g.b, b_.data() + p_, 16);
	p_ += 16;
	return g;
}

size_t Reader::sized(ChunkSize *size) {
	uint8_t b = u8();
	size_t n = b;
	if (b == 0xFF) {
		n = u32();
		if (size) size->long_size = n < 0xFF;
	} else if (size) {
		size->long_size = false;
	}
	need(n);
	return p_ + n;
}

uint8_t Reader::chunk(size_t *end, ChunkSize *size) {
	uint8_t v = u8();
	*end = sized(size);
	return v;
}

void Writer::u16(uint16_t v) {
	u8(uint8_t(v));
	u8(uint8_t(v >> 8));
}

void Writer::u32(uint32_t v) {
	for (int i = 0; i < 4; ++i) u8(uint8_t(v >> (8 * i)));
}

void Writer::end_chunk(size_t at, ChunkSize size) {
	size_t n = d_.size() - at;
	if (n < 0xFF && !size.long_size) {
		d_.insert(d_.begin() + long(at), uint8_t(n));
		return;
	}
	uint8_t b[5] = {0xFF, uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
	d_.insert(d_.begin() + long(at), b, b + 5);
}

// ---------------------------------------------------------------------------------------------
// Header entries

std::string Info::name_text() const { return text_of(name); }
std::string Info::class_text() const { return text_of(class_path); }

namespace {

// DE_CTransferObjectInfo::ProcessHeader (Core 0x700557c0) and the WinFile extension (0x7004fdd0).
Info read_info(Reader &r) {
	Info info;
	size_t end;
	info.version = r.chunk(&end, &info.base_size);
	if (info.version < 1 || info.version > 4) {
		throw Error("object info version " + std::to_string(info.version) + " is not supported");
	}
	info.name = r.block();
	info.class_path = r.block();
	if (info.version >= 2) info.guid = r.guid();
	if (info.version >= 3) {
		info.flag = r.u8();
		info.value84 = int32_t(r.u32());
	}
	if (info.version >= 4) info.flags = r.u32();
	if (r.pos() > end) throw Error("object info overruns its chunk");
	info.base_rest = r.bytes(end - r.pos());
	// Format 2: the extension has a size but no version byte.
	end = r.sized(&info.ext_size);
	info.data_offset = r.u32();
	if (r.pos() > end) throw Error("object info extension overruns its chunk");
	info.ext_rest = r.bytes(end - r.pos());
	return info;
}

void write_info(Writer &w, const Info &info) {
	size_t at = w.begin_chunk(info.version);
	w.block(info.name);
	w.block(info.class_path);
	if (info.version >= 2) w.guid(info.guid);
	if (info.version >= 3) {
		w.u8(info.flag);
		w.u32(uint32_t(info.value84));
	}
	if (info.version >= 4) w.u32(info.flags);
	w.bytes(info.base_rest);
	w.end_chunk(at, info.base_size);
	at = w.begin_sized();
	w.u32(info.data_offset);
	w.bytes(info.ext_rest);
	w.end_chunk(at, info.ext_size);
}

size_t data_size(const Object &o, const Bytes &stored) {
	size_t n = 4 + stored.size() + 4 + o.references.size() + 4 + o.hierarchy.size() + 4;
	for (const Ex &e : o.ex) n += 4 + e.key.size() + 4;
	return n;
}

// What write() stores as the contents block.
Bytes stored_contents(const Object &o) {
	if (!o.compressed()) return o.contents;
	if (o.packed_as_is) return o.packed;
	if (!o.packed.empty()) {
		try {
			if (decompress(o.packed) == o.contents) return o.packed;
		} catch (const Error &) {
		}
	}
	// Method 0 ("stored"): squeezed 0, then the contents as a block.
	Writer w;
	w.u8(0);
	w.block(o.contents);
	return w.data();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The file

File File::read(const Bytes &data, bool header_only) {
	File f;
	Reader r(data);
	f.signature = r.block();
	if (text_of(f.signature) != kSignature2) {
		if (text_of(f.signature) == "Diversions Entertainment Objects File") {
			throw Error("this is an old-format (\"File\", not \"File2\") DEObjects file, which is not supported");
		}
		throw Error("not a DEObjects file");
	}
	f.format = r.u8();
	if (f.format != 2) throw Error("DEObjects format " + std::to_string(f.format) + " is not supported (only 2)");
	// The agent's extended data header: a count of entries (always 0 in shipped files).
	if (uint32_t n = r.u32()) {
		throw Error("the transfer agent has " + std::to_string(n) + " extended data entries, which is not supported");
	}
	Bytes header = r.block();
	size_t header_end = r.pos();

	Reader h(header);
	size_t end;
	f.header_version = h.chunk(&end, &f.header_size);
	if (f.header_version != 9) {
		throw Error("transfer agent header version " + std::to_string(f.header_version) + " is not supported (only 9)");
	}
	f.master = read_info(h);
	size_t own_end;
	f.own_version = h.chunk(&own_end, &f.own_size);
	if (f.own_version == 1) {
		f.own_block = h.block();
		f.own_guid = h.guid();
		f.pool_flag = h.u8();
		if (f.pool_flag == 1) {
			uint16_t n = h.u16();
			for (uint16_t i = 0; i < n; ++i) f.guid_pool.push_back(h.guid());
		}
		f.own_bool = h.u8();
	}
	if (h.pos() > own_end) throw Error("transfer agent contents overrun their chunk");
	f.own_rest = h.bytes(own_end - h.pos());
	uint32_t count = h.u32();
	std::vector<Info> infos;
	for (uint32_t i = 0; i < count; ++i) infos.push_back(read_info(h));
	if (h.pos() > end) throw Error("object table overruns the header");
	f.header_rest = h.bytes(end - h.pos());
	if (h.pos() != header.size()) {
		throw Error("the header block has " + std::to_string(header.size() - h.pos()) + " bytes after its chunk");
	}

	if (header_only) {
		for (Info &info : infos) f.objects.push_back(Object{std::move(info), {}, {}, false, {}, {}, {}});
		return f;
	}

	// Per-object data, at each entry's offset.
	size_t expect = header_end;
	struct Pending {
		size_t object, entry;
		uint32_t offset;
	};
	std::vector<Pending> payloads;
	for (size_t i = 0; i < infos.size(); ++i) {
		Object o;
		o.info = infos[i];
		if (o.info.data_offset != expect) {
			f.warnings.push_back("object " + std::to_string(i) + " data is at " + std::to_string(o.info.data_offset) +
			                     ", not " + std::to_string(expect));
		}
		r.seek(o.info.data_offset);
		Bytes stored = r.block();
		o.references = r.block();
		o.hierarchy = r.block();
		uint32_t n = r.u32();
		for (uint32_t k = 0; k < n; ++k) {
			Ex e;
			e.key = r.block();
			payloads.push_back({i, k, r.u32()});
			o.ex.push_back(std::move(e));
		}
		if (o.compressed()) {
			o.packed = stored;
			o.contents = decompress(stored);
		} else {
			o.contents = std::move(stored);
		}
		expect = r.pos();
		f.objects.push_back(std::move(o));
	}
	for (const Pending &p : payloads) {
		if (p.offset != expect) {
			f.warnings.push_back("object " + std::to_string(p.object) + " extended data " + std::to_string(p.entry) +
			                     " is at " + std::to_string(p.offset) + ", not " + std::to_string(expect));
		}
		r.seek(p.offset);
		Ex &e = f.objects[p.object].ex[p.entry];
		e.flag = r.u8();
		e.data = r.block();
		expect = r.pos();
	}
	if (expect < data.size()) {
		f.trailing.assign(data.begin() + long(expect), data.end());
	}
	return f;
}

Bytes File::write() const {
	std::vector<Bytes> stored;
	for (const Object &o : objects) stored.push_back(stored_contents(o));

	auto header = [&](const std::vector<uint32_t> &offsets) {
		Writer w;
		size_t at = w.begin_chunk(header_version);
		write_info(w, master);
		size_t own = w.begin_chunk(own_version);
		if (own_version == 1) {
			w.block(own_block);
			w.guid(own_guid);
			w.u8(pool_flag);
			if (pool_flag == 1) {
				if (guid_pool.size() > 0xFFFF) throw Error("the GUID pool has more than 65535 entries");
				w.u16(uint16_t(guid_pool.size()));
				for (const Guid &g : guid_pool) w.guid(g);
			} else if (!guid_pool.empty()) {
				throw Error("the GUID pool has entries but pool_flag is not 1");
			}
			w.u8(own_bool);
		}
		w.bytes(own_rest);
		w.end_chunk(own, own_size);
		w.u32(uint32_t(objects.size()));
		for (size_t i = 0; i < objects.size(); ++i) {
			Info info = objects[i].info;
			info.data_offset = offsets[i];
			write_info(w, info);
		}
		w.bytes(header_rest);
		w.end_chunk(at, header_size);
		return std::move(w.data());
	};

	Writer f;
	f.block(signature);
	f.u8(format);
	f.u32(0);
	// Offsets are fixed-width, so the header's size does not depend on them.
	std::vector<uint32_t> offsets(objects.size(), 0);
	size_t pos = f.size() + 4 + header(offsets).size();
	for (size_t i = 0; i < objects.size(); ++i) {
		offsets[i] = uint32_t(pos);
		pos += data_size(objects[i], stored[i]);
	}
	f.block(header(offsets));
	for (size_t i = 0; i < objects.size(); ++i) {
		const Object &o = objects[i];
		f.block(stored[i]);
		f.block(o.references);
		f.block(o.hierarchy);
		f.u32(uint32_t(o.ex.size()));
		for (const Ex &e : o.ex) {
			f.block(e.key);
			f.u32(uint32_t(pos));
			pos += 1 + 4 + e.data.size();
		}
	}
	for (const Object &o : objects) {
		for (const Ex &e : o.ex) {
			f.u8(e.flag);
			f.block(e.data);
		}
	}
	f.bytes(trailing);
	if (f.size() > 0xFFFFFFFFu) throw Error("the file would be larger than 4 GB");
	return std::move(f.data());
}

}  // namespace deo
