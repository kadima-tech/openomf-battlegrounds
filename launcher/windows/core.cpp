#include "core.h"

#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace obg {

// ---------------------------------------------------------------- strings and environment

std::wstring widen(std::string_view utf8) {
	if (utf8.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
	std::wstring out(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
	return out;
}

std::string narrow(std::wstring_view wide) {
	if (wide.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
	std::string out(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), n, nullptr, nullptr);
	return out;
}

std::optional<std::wstring> env(const wchar_t *name) {
	DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
	if (n == 0) return std::nullopt;
	std::wstring value(n, L'\0');
	n = GetEnvironmentVariableW(name, value.data(), n);
	value.resize(n);
	return value;
}

// ---------------------------------------------------------------- JSON

namespace {

struct Parser {
	std::string_view s;
	size_t i = 0;
	int depth = 0;

	void space() {
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
	}
	bool literal(std::string_view word) {
		if (s.substr(i, word.size()) != word) return false;
		i += word.size();
		return true;
	}
	static void utf8(std::string &out, uint32_t c) {
		if (c < 0x80) out += char(c);
		else if (c < 0x800) out += char(0xc0 | (c >> 6)), out += char(0x80 | (c & 0x3f));
		else if (c < 0x10000) out += char(0xe0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
		else out += char(0xf0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3f)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
	}
	bool hex4(uint32_t &v) {
		if (i + 4 > s.size()) return false;
		v = 0;
		for (int k = 0; k < 4; ++k) {
			char c = s[i++];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
			else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
			else return false;
		}
		return true;
	}
	bool string(std::string &out) {
		if (i >= s.size() || s[i] != '"') return false;
		++i;
		while (i < s.size()) {
			char c = s[i++];
			if (c == '"') return true;
			if (uint8_t(c) < 0x20) return false;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (i >= s.size()) return false;
			switch (char e = s[i++]) {
			case '"': case '\\': case '/': out += e; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				uint32_t c1;
				if (!hex4(c1)) return false;
				if (c1 >= 0xd800 && c1 < 0xdc00) {
					uint32_t c2;
					if (!literal("\\u") || !hex4(c2) || c2 < 0xdc00 || c2 >= 0xe000) return false;
					c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
				} else if (c1 >= 0xdc00 && c1 < 0xe000) {
					return false;
				}
				utf8(out, c1);
				break;
			}
			default: return false;
			}
		}
		return false;
	}
	bool value(Json &out) {
		if (++depth > 64) return false;
		space();
		if (i >= s.size()) return false;
		bool ok = true;
		char c = s[i];
		if (c == '{') {
			out.kind = Json::Object;
			++i;
			space();
			if (i < s.size() && s[i] == '}') ++i;
			else {
				for (;;) {
					std::string key;
					space();
					if (!string(key)) return false;
					space();
					if (i >= s.size() || s[i++] != ':') return false;
					Json member;
					if (!value(member)) return false;
					out.members.emplace_back(std::move(key), std::move(member));
					space();
					if (i < s.size() && s[i] == ',') { ++i; continue; }
					if (i < s.size() && s[i] == '}') { ++i; break; }
					return false;
				}
			}
		} else if (c == '[') {
			out.kind = Json::Array;
			++i;
			space();
			if (i < s.size() && s[i] == ']') ++i;
			else {
				for (;;) {
					Json item;
					if (!value(item)) return false;
					out.items.push_back(std::move(item));
					space();
					if (i < s.size() && s[i] == ',') { ++i; continue; }
					if (i < s.size() && s[i] == ']') { ++i; break; }
					return false;
				}
			}
		} else if (c == '"') {
			out.kind = Json::String;
			ok = string(out.text);
		} else if (literal("true")) {
			out.kind = Json::Bool, out.boolean = true;
		} else if (literal("false")) {
			out.kind = Json::Bool;
		} else if (literal("null")) {
			out.kind = Json::Null;
		} else if (c == '-' || (c >= '0' && c <= '9')) {
			out.kind = Json::Number;
			size_t start = i;
			while (i < s.size() && (strchr("+-.eE", s[i]) || (s[i] >= '0' && s[i] <= '9'))) ++i;
			out.text = std::string(s.substr(start, i - start));
		} else {
			ok = false;
		}
		--depth;
		return ok;
	}
};

}  // namespace

const Json *Json::get(std::string_view key) const {
	for (auto &[k, v] : members)
		if (k == key) return &v;
	return nullptr;
}

std::optional<std::string> Json::string(std::string_view key) const {
	const Json *v = get(key);
	if (!v || v->kind != String) return std::nullopt;
	return v->text;
}

std::optional<uint64_t> Json::unsigned_number(std::string_view key) const {
	const Json *v = get(key);
	if (!v || v->kind != Number || v->text.empty() || v->text.size() > 20) return std::nullopt;
	uint64_t n = 0;
	for (char c : v->text) {
		if (c < '0' || c > '9') return std::nullopt;
		uint64_t next = n * 10 + uint64_t(c - '0');
		if (next / 10 != n) return std::nullopt;  // overflow (e.g. 128-bit ReFS file IDs)
		n = next;
	}
	return n;
}

std::optional<Json> Json::parse(std::string_view text) {
	Parser p{text};
	Json out;
	if (!p.value(out)) return std::nullopt;
	p.space();
	if (p.i != text.size()) return std::nullopt;
	return out;
}

std::string json_quote(std::string_view utf8) {
	std::string out = "\"";
	for (char c : utf8) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (uint8_t(c) < 0x20) {
				char buf[8];
				snprintf(buf, sizeof buf, "\\u%04x", unsigned(uint8_t(c)));
				out += buf;
			} else {
				out += c;
			}
		}
	}
	return out + "\"";
}

std::optional<std::string> read_file(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return std::nullopt;
	std::ostringstream buffer;
	buffer << in.rdbuf();
	if (in.bad()) return std::nullopt;
	return buffer.str();
}

std::optional<std::map<std::string, std::string>> parse_event(std::string_view line) {
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
	auto json = Json::parse(line);
	if (!json || json->kind != Json::Object) return std::nullopt;
	std::map<std::string, std::string> event;
	for (auto &[k, v] : json->members)
		if (v.kind == Json::String) event[k] = v.text;
	return event;
}

// ---------------------------------------------------------------- readiness

std::optional<GameManifest> load_manifest(const fs::path &path) {
	auto text = read_file(path);
	if (!text) return std::nullopt;
	auto json = Json::parse(*text);
	if (!json || json->kind != Json::Object) return std::nullopt;
	GameManifest m;
	auto version = json->string("version");
	const Json *files = json->get("files");
	if (!version || !files || files->kind != Json::Array) return std::nullopt;
	m.version = *version;
	for (auto &item : files->items) {
		auto p = item.string("path");
		auto size = item.unsigned_number("size");
		auto sha = item.string("sha256");
		if (!p || !size || !sha) return std::nullopt;
		m.files.push_back({*p, *size, *sha});
	}
	return m;
}

std::optional<ReadinessCache> load_cache(const fs::path &path) {
	auto text = read_file(path);
	if (!text) return std::nullopt;
	auto json = Json::parse(*text);
	if (!json || json->kind != Json::Object) return std::nullopt;
	auto root = json->string("root");
	const Json *files = json->get("files");
	if (!root || !files || files->kind != Json::Array) return std::nullopt;
	ReadinessCache cache{*root, {}};
	for (auto &item : files->items) {
		Stamp s;
		auto p = item.string("path"), sha = item.string("sha256"), mod = item.string("modified"), chg = item.string("changed");
		auto size = item.unsigned_number("size"), inode = item.unsigned_number("inode");
		if (!p || !sha || !mod || !chg || !size || !inode) return std::nullopt;
		cache.files.push_back({*p, *sha, *size, *mod, *chg, *inode});
	}
	return cache;
}

namespace {

struct Handle {
	HANDLE h = INVALID_HANDLE_VALUE;
	Handle() = default;
	explicit Handle(HANDLE handle) : h(handle) {}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	~Handle() {
		if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
	}
	explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

// Seconds:nanoseconds since 1970, as Python's st_mtime_ns // 10**9 and % 10**9 give them.
std::string file_time(FILETIME t) {
	int64_t ticks = int64_t((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) - 116444736000000000LL;
	int64_t ns = ticks * 100;
	int64_t sec = ns >= 0 ? ns / 1000000000 : -((-ns + 999999999) / 1000000000);
	int64_t rem = ns - sec * 1000000000;
	return std::to_string(sec) + ":" + std::to_string(rem);
}

std::wstring strip_extended_prefix(std::wstring p) {
	if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
	if (p.rfind(L"\\\\?\\", 0) == 0) return p.substr(4);
	return p;
}

}  // namespace

std::optional<std::wstring> canonical_path(const fs::path &path) {
	Handle h(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                     FILE_FLAG_BACKUP_SEMANTICS, nullptr));
	if (!h) return std::nullopt;
	std::wstring out(MAX_PATH, L'\0');
	DWORD n = GetFinalPathNameByHandleW(h.h, out.data(), DWORD(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (n >= out.size()) {
		out.resize(n + 1);
		n = GetFinalPathNameByHandleW(h.h, out.data(), DWORD(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	}
	if (n == 0 || n >= out.size()) return std::nullopt;
	out.resize(n);
	return strip_extended_prefix(out);
}

std::optional<std::string> sha256_file(const fs::path &path) {
	Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                        FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
	if (!file) return std::nullopt;
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return std::nullopt;
	std::optional<std::string> result;
	if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
		std::vector<uint8_t> buffer(1 << 20);
		bool ok = true;
		for (;;) {
			DWORD got = 0;
			if (!ReadFile(file.h, buffer.data(), DWORD(buffer.size()), &got, nullptr)) { ok = false; break; }
			if (got == 0) break;
			if (BCryptHashData(hash, buffer.data(), got, 0) < 0) { ok = false; break; }
		}
		uint8_t digest[32];
		if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0) {
			static const char hex[] = "0123456789abcdef";
			std::string text;
			for (uint8_t b : digest) text += hex[b >> 4], text += hex[b & 15];
			result = text;
		}
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(alg, 0);
	return result;
}

bool verify(const fs::path &root, const GameManifest &manifest, const fs::path &cache_path) {
	auto canonical = canonical_path(root);
	if (!canonical) return false;
	const std::string canonical_root = narrow(*canonical);
	std::map<std::string, Stamp> previous;
	if (auto cache = load_cache(cache_path); cache && cache->root == canonical_root)
		for (auto &s : cache->files) previous[s.path] = s;
	std::vector<Stamp> stamps;
	for (auto &file : manifest.files) {
		fs::path path = root / fs::path(widen(file.path)).make_preferred();
		Handle h(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                     nullptr, OPEN_EXISTING, 0, nullptr));
		BY_HANDLE_FILE_INFORMATION info;
		if (!h || !GetFileInformationByHandle(h.h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
		uint64_t size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
		if (size != file.size) return false;
		Stamp stamp{file.path, file.sha256, file.size, file_time(info.ftLastWriteTime), file_time(info.ftCreationTime),
		            (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
		stamps.push_back(stamp);
		if (auto it = previous.find(file.path); it != previous.end() && it->second == stamp) continue;
		auto digest = sha256_file(path);
		if (!digest || *digest != file.sha256) return false;
	}
	if (manifest.files.empty()) return false;
	// An optimization for ordinary local changes, not an authenticity boundary. A changed file ID,
	// size, write or creation time or expected digest requires hashing again. Never cache failures.
	std::string out = "{\"root\": " + json_quote(canonical_root) + ", \"files\": [";
	for (size_t k = 0; k < stamps.size(); ++k) {
		auto &s = stamps[k];
		out += (k ? ", " : "") + std::string("{\"path\": ") + json_quote(s.path) + ", \"size\": " + std::to_string(s.size) +
		       ", \"sha256\": " + json_quote(s.sha256) + ", \"modified\": " + json_quote(s.modified) +
		       ", \"changed\": " + json_quote(s.changed) + ", \"inode\": " + std::to_string(s.inode) + "}";
	}
	out += "]}";
	std::error_code ec;
	fs::create_directories(cache_path.parent_path(), ec);
	fs::path temporary = cache_path;
	temporary += L".tmp";
	{
		std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
		f << out;
		if (!f) return true;
	}
	MoveFileExW(temporary.c_str(), cache_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	return true;
}

// ---------------------------------------------------------------- discs and folders

namespace {

std::optional<fs::path> child(const fs::path &root, std::wstring_view name) {
	std::error_code ec;
	for (auto &entry : fs::directory_iterator(root, ec)) {
		std::wstring n = entry.path().filename().wstring();
		if (n.size() == name.size() &&
		    CompareStringOrdinal(n.data(), int(n.size()), name.data(), int(name.size()), TRUE) == CSTR_EQUAL)
			return entry.path();
	}
	return std::nullopt;
}

}  // namespace

bool setup_on_disc(const fs::path &root) {
	auto setup = child(root, L"Setup");
	if (!setup) return false;
	auto ini = child(*setup, L"setup.ini");
	if (!ini) return false;
	auto text = read_file(*ini);
	if (!text) return false;
	std::string lower = *text;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
	if (lower.find("b8ef8b5a-6d15-436c-ac07-5f2bd5e1aa1b") == std::string::npos) return false;
	for (auto name : {L"data1.cab", L"data1.hdr", L"data2.cab"})
		if (!child(*setup, name)) return false;
	return true;
}

std::vector<fs::path> mounted_volumes() {
	std::vector<fs::path> out;
	wchar_t drives[512];
	DWORD n = GetLogicalDriveStringsW(DWORD(std::size(drives)), drives);
	if (n == 0 || n >= std::size(drives)) return out;
	for (const wchar_t *d = drives; *d; d += wcslen(d) + 1) {
		UINT type = GetDriveTypeW(d);
		if (type != DRIVE_CDROM && type != DRIVE_REMOVABLE) continue;
		// An empty drive has no volume information; skip it instead of touching it.
		if (!GetVolumeInformationW(d, nullptr, 0, nullptr, nullptr, nullptr, nullptr, 0)) continue;
		out.emplace_back(d);
	}
	return out;
}

fs::path executable_dir() {
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
		if (n < path.size()) {
			path.resize(n);
			break;
		}
		path.resize(path.size() * 2);
	}
	return fs::path(path).parent_path();
}

namespace {

bool writable_dir(const fs::path &dir) {
	fs::path probe = dir / L".openbg-write-test";
	HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
	                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	CloseHandle(h);
	return true;
}

bool under(const fs::path &path, const std::optional<std::wstring> &base) {
	if (!base || base->empty()) return false;
	std::wstring p = path.wstring(), b = fs::path(*base).wstring();
	while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
	return p.size() >= b.size() &&
	       CompareStringOrdinal(p.data(), int(b.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL &&
	       (p.size() == b.size() || p[b.size()] == L'\\' || p[b.size()] == L'/');
}

fs::path known_folder(REFKNOWNFOLDERID id, const wchar_t *fallback_env) {
	PWSTR raw = nullptr;
	fs::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) out = raw;
	CoTaskMemFree(raw);
	if (out.empty()) out = env(fallback_env).value_or(L".");
	return out;
}

}  // namespace

Locations Locations::detect(const fs::path &dir) {
	Locations l;
	std::error_code ec;
	// Setup and the game run in the game folder, so every path handed to them must be absolute.
	const fs::path app_dir = fs::absolute(dir, ec);
	l.resources = app_dir / L"resources";
	fs::path adjacent = app_dir.parent_path().parent_path();
	if (auto root = env(L"OPENBG_PROJECT_ROOT")) l.project = fs::path(*root);
	else if (fs::exists(adjacent / L"engine" / L"CMakeLists.txt", ec)) l.project = adjacent;
	if (auto home = env(L"OPENBG_HOME")) {
		l.home = fs::path(*home);
	} else if (l.project) {
		l.home = *l.project / L"source" / L"launcher";
	} else {
		fs::path parent = app_dir.parent_path();
		if (writable_dir(parent) && !under(parent, env(L"ProgramFiles")) && !under(parent, env(L"ProgramFiles(x86)")) &&
		    !under(parent, env(L"ProgramW6432")))
			l.home = parent / L"Battlegrounds Files";
		else
			l.home = known_folder(FOLDERID_LocalAppData, L"LOCALAPPDATA") / L"OpenBG";
	}
	return l;
}

std::wstring quote_argument(const std::wstring &a) {
	if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
	std::wstring r = L"\"";
	for (size_t i = 0;; ++i) {
		size_t slashes = 0;
		while (i < a.size() && a[i] == L'\\') ++i, ++slashes;
		if (i == a.size()) {
			r.append(slashes * 2, L'\\');
			break;
		}
		if (a[i] == L'"') r.append(slashes * 2 + 1, L'\\');
		else r.append(slashes, L'\\');
		r.push_back(a[i]);
	}
	return r + L"\"";
}

// ---------------------------------------------------------------- model

LauncherModel::LauncherModel(Locations w, bool p) : where(std::move(w)), preview(p) {
	stages = {
	    {L"Game files", L"From your original CD or ISO"},
	    {L"Bundled update", L"Updated offline to version 2.123"},
	    {L"Local setup", L"Everything installed in your game folder"},
	};
}

LauncherModel::~LauncherModel() {
	if (job) CloseHandle(job);  // kill-on-close: setup never outlives the launcher
	if (worker) CloseHandle(worker);
	if (game) CloseHandle(game);
}

void LauncherModel::fail(std::wstring message) {
	error = std::move(message);
	changed();
}

void LauncherModel::start() {
	if (preview) {
		needs_window();
		return;
	}
	// Threads hold a copy of post(), never the model: they may outlive it when the launcher
	// closes. Only the posted functions, which run on the UI thread while it exists, use it.
	std::thread([this, post = post, resources = where.resources, home = where.home, project = where.project] {
		auto manifest = load_manifest(resources / L"game-files.json");
		std::vector<fs::path> candidates{home / L"game"};
		if (project) candidates.push_back(*project / L"source" / L"patched" / L"v2123");
		std::optional<fs::path> installed;
		if (manifest)
			for (auto &c : candidates)
				if (verify(c, *manifest, home / L"verified-files.json")) {
					installed = c;
					break;
				}
		bool engine = fs::exists(resources / L"bin" / L"openbg.exe");
		bool has_manifest = manifest.has_value();
		post([this, installed, engine, has_manifest] {
			if (installed && engine) {
				ready_game = installed;
				ready = true;
				launch();
			} else {
				needs_window();
				if (!has_manifest) error = L"The app is incomplete. Rebuild or download a complete copy.";
				changed();
			}
		});
	}).detach();
}

void LauncherModel::scan_discs(const std::vector<fs::path> &volumes) {
	if (preview || busy || ready) return;
	disc.reset();
	for (auto &v : volumes)
		if (setup_on_disc(v)) {
			disc = v;
			break;
		}
	changed();
}

void LauncherModel::request_install(const fs::path &requested) {
	if (busy || preview || ready) return;
	std::error_code ec;
	// Setup runs in the game folder, so a relative path would point somewhere else there.
	const fs::path source = fs::absolute(requested, ec);
	std::wstring ext = source.extension().wstring();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
	if (!fs::is_directory(source, ec) && ext != L".iso") {
		fail(L"Choose an ISO disc image, or insert your original Battlegrounds CD.");
		return;
	}
	pending_source = source;
	changed();
}

void LauncherModel::confirm_install() {
	if (!pending_source) return;
	fs::path source = *pending_source;
	pending_source.reset();
	install(source);
}

void LauncherModel::install(const fs::path &source) {
	if (busy || preview || ready) return;
	last_source = source;
	std::wstring name = source.filename().wstring();
	source_name = name.empty() ? source.wstring() : name;  // a drive root has no file name
	error.reset();
	busy = true;
	for (auto &s : stages) s.status = "waiting";
	stages[2].detail = L"Bundled setup tools ready \u00B7 no downloads needed";
	auto stop = [this](std::wstring message) {
		busy = false;
		fail(std::move(message));
	};
	std::error_code ec;
	fs::create_directories(where.home, ec);
	if (ec) return stop(L"Could not create your game folder: " + widen(ec.message()));
	fs::path python = where.resources / L"python" / L"python.exe";
	if (!fs::exists(python, ec))
		return stop(L"Could not start the bundled setup tools. Reinstall the app or check bootstrap.log in your game folder.");

	// The private Python ignores user environments (-I). stdout carries JSON progress events;
	// diagnostics go to bootstrap.log, as bootstrap.sh does on macOS.
	std::vector<std::wstring> args{python.wstring(), L"-I", L"-B", L"-X", L"utf8", (where.resources / L"install.py").wstring(),
	                               L"--home", where.home.wstring(), L"--resources", where.resources.wstring(),
	                               L"--source", source.wstring()};
	if (where.project) args.insert(args.end(), {L"--project", where.project->wstring()});
	std::wstring command;
	for (auto &a : args) command += (command.empty() ? L"" : L" ") + quote_argument(a);

	SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
	HANDLE read = nullptr, write = nullptr;
	if (!CreatePipe(&read, &write, &inherit, 0)) return stop(L"Could not start setup.");
	SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
	HANDLE log = CreateFileW((where.home / L"bootstrap.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                         &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	STARTUPINFOW si{};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = write;
	si.hStdError = log != INVALID_HANDLE_VALUE ? log : write;
	si.hStdInput = nullptr;
	// A kill-on-close job is Windows' process group: cancelling, or the launcher ending in any way,
	// stops setup and the extractor it runs.
	if (!job) {
		job = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
	}
	PROCESS_INFORMATION pi{};
	std::wstring mutable_command = command;
	BOOL started = CreateProcessW(python.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
	                              CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
	                              where.home.c_str(), &si, &pi);
	CloseHandle(write);
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
	if (!started) {
		CloseHandle(read);
		return stop(L"Could not start the bundled setup tools. Reinstall the app or check bootstrap.log in your game folder.");
	}
	if (job) AssignProcessToJobObject(job, pi.hProcess);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	worker = pi.hProcess;
	changed();

	// Read whole lines off the UI thread, including partial pipe reads.
	std::thread([this, post = post, read, process = pi.hProcess] {
		std::string pending;
		char buffer[4096];
		DWORD got = 0;
		while (ReadFile(read, buffer, sizeof buffer, &got, nullptr) && got > 0) {
			pending.append(buffer, got);
			size_t end;
			while ((end = pending.find('\n')) != std::string::npos) {
				if (auto event = parse_event(std::string_view(pending).substr(0, end)))
					post([this, e = *event] { receive(e); });
				pending.erase(0, end + 1);
			}
		}
		CloseHandle(read);
		WaitForSingleObject(process, INFINITE);
		post([this] {
			busy = false;
			if (worker) CloseHandle(worker);
			worker = nullptr;
			if (!ready && !error)
				error = L"Setup stopped before it finished. Check your disc and free space, then retry. Details are in the setup log.";
			changed();
		});
	}).detach();
}

void LauncherModel::receive(const std::map<std::string, std::string> &event) {
	auto get = [&](const char *key) {
		auto it = event.find(key);
		return it == event.end() ? std::string() : it->second;
	};
	std::string stage = get("stage");
	if (stage.empty()) return;
	if (stage == "error") {
		// Setup has stopped: no stage is still working.
		for (auto &s : stages)
			if (s.status == "active") s.status = "waiting";
		fail(widen(get("detail")));
		return;
	}
	if (stage == "complete") {
		ready = true;
		ready_game = where.home / L"game";
		busy = false;
		source_name = L"Installation complete. Your next launch goes straight to the game.";
		changed();
		launch();
		return;
	}
	static const char *names[] = {"files", "patches", "runtime"};
	for (size_t k = 0; k < 3; ++k)
		if (stage == names[k]) {
			std::string status = get("status");
			stages[k].status = status.empty() ? "waiting" : status;
			stages[k].detail = widen(get("detail"));
		}
	changed();
}

void LauncherModel::launch() {
	if (!ready_game || game) return;
	std::error_code ec;
	fs::create_directories(where.home, ec);
	fs::path engine = where.resources / L"bin" / L"openbg.exe";
	std::wstring command = quote_argument(engine.wstring()) + L" " + quote_argument((*ready_game / L"Data").wstring());
	if (auto extra = env(L"OPENBG_GAME_ARGS")) {
		std::wistringstream words(*extra);
		for (std::wstring w; words >> w;) command += L" " + quote_argument(w);
	}
	SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
	HANDLE log = CreateFileW((where.home / L"game.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
	                         FILE_ATTRIBUTE_NORMAL, nullptr);
	if (log == INVALID_HANDLE_VALUE) {
		needs_window();
		fail(L"The game could not start: its log file in your game folder cannot be written.");
		return;
	}
	STARTUPINFOW si{};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = si.hStdError = log;
	PROCESS_INFORMATION pi{};
	BOOL started = CreateProcessW(engine.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
	                              where.home.c_str(), &si, &pi);
	DWORD reason = GetLastError();
	CloseHandle(log);
	if (!started) {
		needs_window();
		wchar_t *text = nullptr;
		FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
		               reason, 0, reinterpret_cast<wchar_t *>(&text), 0, nullptr);
		std::wstring why = text ? text : L"unknown error";
		LocalFree(text);
		while (!why.empty() && iswspace(why.back())) why.pop_back();
		fail(L"The game could not start: " + why);
		return;
	}
	CloseHandle(pi.hThread);
	// The game brings itself to the front; the user started it, so it may take the foreground.
	AllowSetForegroundWindow(pi.dwProcessId);
	game = pi.hProcess;
	hide_window();
	changed();
	std::thread([this, post = post, process = pi.hProcess] {
		WaitForSingleObject(process, INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(process, &code);
		post([this, code] {
			if (code == 0) {
				quit();
				return;
			}
			if (game) CloseHandle(game);
			game = nullptr;
			needs_window();
			fail(L"The game could not start (code " + std::to_wstring(long(code)) + L"). Open the game log for details.");
		});
	}).detach();
}

void LauncherModel::cancel() {
	if (job && busy) TerminateJobObject(job, 1);
}

std::wstring LauncherModel::log_to_open() const {
	std::error_code ec;
	if (ready) return (where.home / L"game.log").wstring();
	if (fs::exists(where.home / L"install.log", ec)) return (where.home / L"install.log").wstring();
	return (where.home / L"bootstrap.log").wstring();
}

}  // namespace obg
