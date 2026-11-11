#include "addon.h"
#include "content.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <set>

namespace ed {

namespace fs = std::filesystem;

std::string guid_key(const Guid &g) { return std::string(reinterpret_cast<const char *>(g.b), 16); }

bool guid_zero(const Guid &g) {
	for (uint8_t b : g.b) {
		if (b) return false;
	}
	return true;
}

Guid fresh_guid() {
	static std::mt19937_64 rng{std::random_device{}()};
	Guid g;
	do {
		for (int i = 0; i < 16; i += 8) {
			uint64_t v = rng();
			memcpy(g.b + i, &v, 8);
		}
		// RFC 4122 version 4, as CoCreateGuid makes them.
		g.b[7] = uint8_t((g.b[7] & 0x0f) | 0x40);
		g.b[8] = uint8_t((g.b[8] & 0x3f) | 0x80);
	} while (guid_zero(g));
	return g;
}

Bytes text_bytes(const std::string &s) {
	Bytes b(s.begin(), s.end());
	b.push_back(0);
	return b;
}

std::string text_of(const Bytes &b) {
	size_t n = b.size();
	while (n && b[n - 1] == 0) --n;
	return std::string(b.begin(), b.begin() + long(n));
}

Chunks split_chunks(const Bytes &block) {
	Chunks out;
	deo::Reader r(block);
	while (r.remaining() >= 2) {
		size_t at = r.pos();
		try {
			RawChunk c;
			size_t end;
			c.version = r.chunk(&end, &c.size);
			c.body = r.bytes(end - r.pos());
			out.chunks.push_back(std::move(c));
		} catch (const deo::Error &) {
			r.seek(at);
			break;
		}
	}
	out.tail = r.bytes(r.remaining());
	return out;
}

Bytes join_chunks(const Chunks &c) {
	deo::Writer w;
	for (const RawChunk &ch : c.chunks) {
		size_t at = w.begin_chunk(ch.version);
		w.bytes(ch.body);
		w.end_chunk(at, ch.size);
	}
	w.bytes(c.tail);
	return w.data();
}

Ref read_ref(deo::Reader &r) {
	Ref ref;
	size_t at = r.pos();
	uint8_t tag = r.u8();
	if (tag == 0) {
		ref.kind = Ref::Null;
	} else if (tag == 1) {
		ref.kind = Ref::Local, ref.index = r.u16();
		ref.raw_form = ref.index <= 0xCC;
	} else if (tag == 2) {
		ref.kind = Ref::Pool, ref.index = r.u8();
	} else if (tag == 3) {
		ref.kind = Ref::Pool, ref.index = r.u16();
		ref.raw_form = ref.index <= 0xFF;
	} else if (tag == 4) {
		ref.kind = Ref::Pool, ref.index = r.u32();
		ref.raw_form = ref.index <= 0xFFFF;
	} else if (tag >= 0x32) {
		ref.kind = Ref::Local, ref.index = uint32_t(tag - 0x32);
		ref.raw_form = tag == 0xFF;
	} else {
		throw deo::Error("unknown reference tag " + std::to_string(tag));
	}
	if (ref.raw_form) {
		size_t end = r.pos();
		r.seek(at);
		ref.raw = r.bytes(end - at);
	}
	return ref;
}

void write_ref(deo::Writer &w, const Ref &ref) {
	if (ref.raw_form && !ref.raw.empty()) {
		w.bytes(ref.raw);
		return;
	}
	if (ref.kind == Ref::Null) {
		w.u8(0);
	} else if (ref.kind == Ref::Local) {
		if (ref.index <= 0xCC) {
			w.u8(uint8_t(0x32 + ref.index));
		} else {
			w.u8(1);
			w.u16(uint16_t(ref.index));
		}
	} else if (ref.index <= 0xFF) {
		w.u8(2);
		w.u8(uint8_t(ref.index));
	} else if (ref.index <= 0xFFFF) {
		w.u8(3);
		w.u16(uint16_t(ref.index));
	} else {
		w.u8(4);
		w.u32(ref.index);
	}
}

namespace {

std::vector<uint8_t> read_prefix(const fs::path &p, size_t n) {
	std::ifstream f(p, std::ios::binary);
	std::vector<uint8_t> out(n);
	f.read(reinterpret_cast<char *>(out.data()), std::streamsize(n));
	out.resize(size_t(f.gcount()));
	return out;
}

Bytes read_all(const fs::path &p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) throw deo::Error("cannot open " + p.string());
	return {std::istreambuf_iterator<char>(f), {}};
}

uint32_t le32(const std::vector<uint8_t> &b, size_t at) {
	if (at + 4 > b.size()) throw deo::Error("file too short");
	return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

// The start of a file up to the end of its header block, enough for File::read(header_only).
Bytes read_header(const fs::path &p) {
	Bytes head = read_prefix(p, 4096);
	size_t sig = le32(head, 0);
	size_t at = 4 + sig + 1 + 4;
	size_t need = at + 4 + le32(head, at);
	return need <= head.size() ? head : read_prefix(p, need);
}

}  // namespace

BaseData::BaseData(const fs::path &dir) : dir_(dir) {
	if (!fs::is_directory(dir)) throw deo::Error(dir.string() + " is not a folder");
	std::vector<std::pair<std::string, fs::path>> paths;
	for (auto &e : fs::directory_iterator(dir)) {
		if (e.path().extension() != ".DEObjects") continue;
		std::string upper = e.path().filename().string();
		for (char &ch : upper) ch = char(toupper(uint8_t(ch)));
		paths.emplace_back(upper, e.path());
	}
	std::sort(paths.begin(), paths.end());
	for (auto &[upper, p] : paths) {
		try {
			deo::File f = deo::File::read(read_header(p), true);
			FileIndex fi;
			fi.stem = p.stem().string();
			fi.path = p;
			fi.agent_name = f.master.name_text();
			fi.agent = f.master.guid;
			for (uint32_t i = 0; i < f.objects.size(); ++i) {
				const deo::Info &info = f.objects[i].info;
				BaseObject o{files_.size(), i, info.name_text(), info.class_text(), info.guid};
				o.category = category_of(o.class_path);
				o.search = o.name + "\n" + o.class_path;
				for (char &ch : o.search) ch = char(tolower(uint8_t(ch)));
				fi.objects.push_back(std::move(o));
			}
			files_.push_back(std::move(fi));
		} catch (const deo::Error &) {
		}
	}
	if (files_.empty()) throw deo::Error("no .DEObjects files in " + dir.string());
	categories_.resize(kCategoryCount);
	for (auto &f : files_) {
		for (auto &o : f.objects) {
			guids_.emplace(guid_key(o.guid), &o);
			categories_[size_t(o.category)].push_back(&o);
		}
	}
}

BaseData::~BaseData() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable()) thread_.join();
}

void BaseData::worker() {
	for (;;) {
		size_t file;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
			if (stop_) return;
			file = queue_.front();
			queue_.pop_front();
			if (models_.count(file)) continue;
		}
		std::unique_ptr<deo::File> m;
		try {
			m = std::make_unique<deo::File>(deo::File::read(read_all(files_.at(file).path)));
		} catch (const std::exception &) {
			m = std::make_unique<deo::File>();
		}
		std::lock_guard<std::mutex> lock(mutex_);
		if (!models_.count(file)) models_[file] = std::move(m);
	}
}

const deo::File *BaseData::model_async(size_t file) const {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = models_.find(file);
	if (it != models_.end()) return it->second.get();
	if (std::find(queue_.begin(), queue_.end(), file) == queue_.end()) queue_.push_back(file);
	if (!thread_.joinable()) thread_ = std::thread([this] { const_cast<BaseData *>(this)->worker(); });
	wake_.notify_one();
	return nullptr;
}

bool BaseData::loaded(size_t file) const {
	std::lock_guard<std::mutex> lock(mutex_);
	return models_.count(file) != 0;
}

const BaseObject *BaseData::by_guid(const Guid &g) const {
	auto it = guids_.find(guid_key(g));
	return it == guids_.end() ? nullptr : it->second;
}

const BaseData::FileIndex *BaseData::file(const std::string &stem) const {
	for (auto &f : files_) {
		if (f.stem == stem) return &f;
	}
	return nullptr;
}

std::vector<const BaseObject *> BaseData::of_class(const std::string &class_path) const {
	std::vector<const BaseObject *> out;
	for (auto &f : files_) {
		for (auto &o : f.objects) {
			if (o.class_path == class_path) out.push_back(&o);
		}
	}
	return out;
}

const deo::File &BaseData::model(size_t file) const {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = models_.find(file);
		if (it != models_.end()) return *it->second;
	}
	auto m = std::make_unique<deo::File>(deo::File::read(read_all(files_.at(file).path)));
	std::lock_guard<std::mutex> lock(mutex_);
	auto &slot = models_[file];
	if (!slot) slot = std::move(m);
	return *slot;
}

size_t BaseData::object_count() const {
	size_t n = 0;
	for (auto &f : files_) n += f.objects.size();
	return n;
}

Project::Project(const BaseData &base, fs::path path, deo::File file)
    : base_(base), path_(std::move(path)), file_(std::move(file)) {}

std::unique_ptr<Project> Project::create(const BaseData &base, const fs::path &path) {
	deo::File f;
	f.signature = text_bytes("Diversions Entertainment Objects File2");
	f.master.name = text_bytes(path.stem().string());
	f.master.class_path = text_bytes("TransferAgent\\Win\\File");
	f.master.guid = fresh_guid();
	auto p = std::make_unique<Project>(base, path, std::move(f));
	p->dirty_ = true;
	return p;
}

std::unique_ptr<Project> Project::open(const BaseData &base, const fs::path &path) {
	return std::make_unique<Project>(base, path, deo::File::read(read_all(path)));
}

void Project::save() {
	Bytes data = file_.write();
	fs::path tmp = path_;
	tmp += ".saving";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) throw deo::Error("cannot write " + tmp.string());
		out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
		if (!out) throw deo::Error("cannot write " + tmp.string());
	}
	std::error_code ec;
	fs::rename(tmp, path_, ec);
	if (ec) {
		fs::remove(path_, ec);
		fs::rename(tmp, path_);
	}
	dirty_ = false;
}

uint32_t Project::pool_index(const Guid &g) {
	for (uint32_t i = 0; i < file_.guid_pool.size(); ++i) {
		if (file_.guid_pool[i] == g) return i;
	}
	file_.guid_pool.push_back(g);
	return uint32_t(file_.guid_pool.size() - 1);
}

int Project::find(const Guid &g) const {
	for (size_t i = 0; i < file_.objects.size(); ++i) {
		if (file_.objects[i].info.guid == g) return int(i);
	}
	return -1;
}

Project::Target Project::target(const Ref &r) const {
	Target t;
	if (r.kind == Ref::Local && r.index < file_.objects.size()) {
		t.local = int(r.index), t.guid = file_.objects[r.index].info.guid, t.resolved = true;
	} else if (r.kind == Ref::Pool && r.index < file_.guid_pool.size()) {
		t.guid = file_.guid_pool[r.index];
		t.local = find(t.guid);
		t.resolved = t.local >= 0 || base_.by_guid(t.guid);
	}
	return t;
}

namespace {

// Reference chunk layouts: L a BOOL-terminated list, O a BOOL then a reference, A a nested
// chunk holding an O (CAdvancedReference), R raw bytes to the end; a trailing * repeats lists.
struct RefLayout {
	const char *class_prefix;
	size_t index;
	const char *ops;
};
const RefLayout kRefLayouts[] = {
    {"Object", 0, "*"},
    {"Actor\\Creature\\Pilot", 1, "*"},
    {"Actor\\Creature\\Pilot", 2, "*"},
    {"Actor\\Creature\\Pilot", 3, "AAA*"},
    {"Description", 1, "LOOLLOR"},
    {"Image", 1, "*"},
    {"Value", 1, "*"},
    {"Value\\ObjectReference", 2, "*"},
    {"Class", 1, "*"},
    {"ClassExecutor", 1, "O"},
    {"ControlCreator", 1, "*"},
    {"StateManager", 1, "*"},
    {"StateManager", 2, "*"},
    {"SessionModifier\\StandardSessionMaster", 1, "*"},
    {"SessionModifier\\StandardSessionMaster", 2, "*"},
    {"Description\\SessionMaster", 2, "*"},
    {"Description\\SessionModifier", 2, "*"},
    {"Behavior", 1, "B"},
    {"AnimationSequenceManager", 1, "LN"},
    {"Mesh\\LODSwitchingMesh", 1, "*"},
    {"Mesh\\LODSwitchingMesh", 2, "D"},
    {"TextureProcess\\MultiStage", 1, "*"},
    {"TextureProcess\\MultiStage", 2, "S"},
};

const char *find_layout(const std::string &class_path, size_t index) {
	const char *best = nullptr;
	size_t best_len = 0;
	for (auto &l : kRefLayouts) {
		size_t n = strlen(l.class_prefix);
		if (l.index == index && class_path.compare(0, n, l.class_prefix) == 0 &&
		    (class_path.size() == n || class_path[n] == '\\') && (!best || n > best_len)) {
			best = l.ops, best_len = n;
		}
	}
	return best;
}

bool map_list(deo::Reader &r, deo::Writer &w, const std::function<Ref(const Ref &)> &map) {
	for (;;) {
		uint8_t b = r.u8();
		w.u8(b);
		if (b == 0) return true;
		if (b != 1) return false;
		write_ref(w, map(read_ref(r)));
	}
}

bool map_opt(deo::Reader &r, deo::Writer &w, const std::function<Ref(const Ref &)> &map) {
	uint8_t b = r.u8();
	w.u8(b);
	if (b == 1) write_ref(w, map(read_ref(r)));
	return b <= 1;
}

}  // namespace

bool map_reference_chunk(const std::string &class_path, size_t chunk_index, const RawChunk &in, RawChunk &out,
                         const std::function<Ref(const Ref &)> &map) {
	out = in;
	if (in.body.empty()) return true;
	const char *ops = find_layout(class_path, chunk_index);
	if (!ops) return false;
	try {
		deo::Reader r(in.body);
		deo::Writer w;
		for (const char *op = ops; *op; ++op) {
			if (*op == 'L') {
				if (!map_list(r, w, map)) return false;
			} else if (*op == 'O') {
				if (!map_opt(r, w, map)) return false;
			} else if (*op == 'A') {
				deo::ChunkSize size;
				size_t end;
				uint8_t v = r.chunk(&end, &size);
				size_t at = w.begin_chunk(v);
				if (r.pos() < end && !map_opt(r, w, map)) return false;
				if (r.pos() != end) return false;
				w.end_chunk(at, size);
			} else if (*op == 'B') {
				// Behavior: a chunk of globals (BOOL, reference each), the imports list, then
				// one chunk per element.
				deo::ChunkSize size;
				size_t end;
				uint8_t v = r.chunk(&end, &size);
				size_t at = w.begin_chunk(v);
				while (r.pos() < end) {
					if (!map_opt(r, w, map)) return false;
				}
				w.end_chunk(at, size);
				v = r.chunk(&end, &size);
				at = w.begin_chunk(v);
				if (!map_list(r, w, map) || r.pos() != end) return false;
				w.end_chunk(at, size);
				w.bytes(r.bytes(r.remaining()));
			} else if (*op == 'D') {
				// LOD meshes: BOOL-terminated (reference, f32 switch distance) pairs.
				for (;;) {
					uint8_t b = r.u8();
					w.u8(b);
					if (b == 0) break;
					if (b != 1) return false;
					write_ref(w, map(read_ref(r)));
					w.bytes(r.bytes(4));
				}
			} else if (*op == 'S') {
				// Texture stages: a u32 count of (texture manager, 28 bytes of stage settings).
				uint32_t n = r.u32();
				w.u32(n);
				for (uint32_t k = 0; k < n; ++k) {
					write_ref(w, map(read_ref(r)));
					w.bytes(r.bytes(28));
				}
			} else if (*op == 'N') {
				while (r.remaining()) {
					uint32_t n = r.u32();
					w.u32(n);
					for (uint32_t k = 0; k < n; ++k) write_ref(w, map(read_ref(r)));
				}
			} else if (*op == 'R') {
				w.bytes(r.bytes(r.remaining()));
			} else if (*op == '*') {
				while (r.remaining()) {
					if (!map_list(r, w, map)) return false;
				}
			}
		}
		if (r.remaining()) return false;
		out.body = w.data();
		return true;
	} catch (const deo::Error &) {
		return false;
	}
}

std::vector<Ref> object_references(const std::string &class_path, const Bytes &references, bool *understood) {
	std::vector<Ref> out;
	bool ok = true;
	Chunks cs = split_chunks(references);
	for (size_t i = 0; i < cs.chunks.size(); ++i) {
		RawChunk dummy;
		ok &= map_reference_chunk(class_path, i, cs.chunks[i], dummy, [&](const Ref &r) {
			out.push_back(r);
			return r;
		});
	}
	if (understood) *understood = ok && cs.tail.empty();
	return out;
}

namespace {

void replace_all(Bytes &b, const Bytes &from, const Bytes &to) {
	if (from.empty() || b.size() < from.size()) return;
	for (size_t i = 0; i + from.size() <= b.size();) {
		if (std::equal(from.begin(), from.end(), b.begin() + long(i))) {
			std::copy(to.begin(), to.end(), b.begin() + long(i));
			i += from.size();
		} else {
			++i;
		}
	}
}

Bytes guid_bytes(const Guid &g) { return Bytes(g.b, g.b + 16); }

}  // namespace

std::vector<uint32_t> Project::copy_from_base(size_t file, const std::vector<uint32_t> &roots, std::map<uint32_t, uint32_t> *source_to_copy,
                                              bool guess_layouts) {
	const deo::File &src = base_.model(file);
	const Guid src_agent = src.master.guid;
	std::set<uint32_t> set;
	std::vector<uint32_t> stack(roots.begin(), roots.end());
	while (!stack.empty()) {
		uint32_t i = stack.back();
		stack.pop_back();
		if (i >= src.objects.size() || !set.insert(i).second) continue;
		Chunks h = split_chunks(src.objects[i].hierarchy);
		if (!h.chunks.empty()) {
			RawChunk out;
			map_reference_chunk("Object", 0, h.chunks[0], out, [&](const Ref &r) {
				if (r.kind == Ref::Local) stack.push_back(r.index);
				return r;
			});
		}
	}
	std::map<uint32_t, uint32_t> local;
	std::map<uint32_t, Guid> fresh;
	uint32_t next = uint32_t(file_.objects.size());
	for (uint32_t i : set) local[i] = next++, fresh[i] = fresh_guid();

	auto map = [&](const Ref &r) -> Ref {
		Ref out;
		if (r.kind == Ref::Null) return out;
		if (r.kind == Ref::Local) {
			if (r.index >= src.objects.size()) throw deo::Error("reference to object " + std::to_string(r.index) + " is out of range");
			if (auto it = local.find(r.index); it != local.end()) {
				out.kind = Ref::Local, out.index = it->second;
			} else {
				out.kind = Ref::Pool, out.index = pool_index(src.objects[r.index].info.guid);
			}
			return out;
		}
		if (r.index >= src.guid_pool.size()) throw deo::Error("GUID pool reference out of range");
		out.kind = Ref::Pool, out.index = pool_index(src.guid_pool[r.index]);
		return out;
	};

	std::vector<deo::Object> made;
	for (uint32_t i : set) {
		deo::Object o = src.objects[i];
		const std::string cls = o.info.class_text();
		o.info.guid = fresh[i];
		for (auto &[old, g] : fresh) {
			Bytes from = guid_bytes(src_agent), to = guid_bytes(file_.master.guid);
			Bytes og = guid_bytes(src.objects[old].info.guid), ng = guid_bytes(g);
			from.insert(from.end(), og.begin(), og.end());
			to.insert(to.end(), ng.begin(), ng.end());
			replace_all(o.contents, from, to);
			replace_all(o.contents, og, ng);
		}
		Chunks rc = split_chunks(o.references);
		for (size_t k = 0; k < rc.chunks.size(); ++k) {
			RawChunk out;
			if (!map_reference_chunk(cls, k, rc.chunks[k], out, map) &&
			    !(guess_layouts && guess_map_reference_chunk(rc.chunks[k], out, src.objects.size(), src.guid_pool.size(), map))) {
				throw deo::Error("cannot copy " + o.info.name_text() + ": the references of a " + cls + " are not decoded yet");
			}
			rc.chunks[k] = std::move(out);
		}
		o.references = join_chunks(rc);
		Chunks hc = split_chunks(o.hierarchy);
		for (size_t k = 0; k < hc.chunks.size(); ++k) {
			RawChunk out;
			if (!map_reference_chunk("Object", k == 0 ? 0 : 99, hc.chunks[k], out, map)) {
				throw deo::Error("cannot copy " + o.info.name_text() + ": unexpected hierarchy layout");
			}
			hc.chunks[k] = std::move(out);
		}
		o.hierarchy = join_chunks(hc);
		made.push_back(std::move(o));
	}
	for (auto &o : made) file_.objects.push_back(std::move(o));
	if (source_to_copy) *source_to_copy = local;
	touch();
	std::vector<uint32_t> out;
	for (uint32_t r : roots) out.push_back(local.at(r));
	return out;
}

std::vector<uint32_t> hierarchy_children(const deo::Object &o) {
	std::vector<uint32_t> out;
	Chunks h = split_chunks(o.hierarchy);
	if (h.chunks.empty()) return out;
	RawChunk dummy;
	map_reference_chunk("Object", 0, h.chunks[0], dummy, [&](const Ref &r) {
		if (r.kind == Ref::Local) out.push_back(r.index);
		return r;
	});
	return out;
}

std::vector<uint32_t> Project::children(uint32_t object) const {
	if (object >= file_.objects.size()) return {};
	return hierarchy_children(file_.objects[object]);
}

}  // namespace ed
