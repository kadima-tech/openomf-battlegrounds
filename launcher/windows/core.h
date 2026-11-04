// Windows launcher logic without any user interface: JSON, readiness verification shared with
// install.py, game-disc detection, folder locations and the setup consent/state model. The Win32
// window in Launcher.cpp and the native tests both build on this. Mirrors Launcher.swift and
// Readiness.swift.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace obg {

namespace fs = std::filesystem;

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
std::optional<std::wstring> env(const wchar_t *name);

// --- JSON (enough for game-files.json, the readiness cache and setup events) ---
struct Json {
	enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
	bool boolean = false;
	std::string text;  // a string's UTF-8 value, or a number's literal
	std::vector<Json> items;
	std::vector<std::pair<std::string, Json>> members;

	const Json *get(std::string_view key) const;
	std::optional<std::string> string(std::string_view key) const;
	std::optional<uint64_t> unsigned_number(std::string_view key) const;
	static std::optional<Json> parse(std::string_view text);
};
std::string json_quote(std::string_view utf8);
std::optional<std::string> read_file(const fs::path &path);
// Setup's stdout events: one flat object of strings per line.
std::optional<std::map<std::string, std::string>> parse_event(std::string_view line);

// --- Readiness: the actual files, their patch level and the bundled engine decide; never a flag ---
struct GameManifest {
	struct File {
		std::string path;
		uint64_t size = 0;
		std::string sha256;
	};
	std::string version;
	std::vector<File> files;
};
std::optional<GameManifest> load_manifest(const fs::path &path);

struct Stamp {
	std::string path, sha256;
	uint64_t size = 0;
	std::string modified, changed;  // "seconds:nanoseconds" of last write and creation time
	uint64_t inode = 0;             // NTFS file ID
	bool operator==(const Stamp &) const = default;
};
struct ReadinessCache {
	std::string root;
	std::vector<Stamp> files;
};
std::optional<ReadinessCache> load_cache(const fs::path &path);
// The path with links resolved and the "\\?\" prefix removed, as Python's Path.resolve() gives it.
std::optional<std::wstring> canonical_path(const fs::path &path);
std::optional<std::string> sha256_file(const fs::path &path);
// Full verification against the manifest. Files whose size, write/creation time, file ID and
// expected digest match the cache are not hashed again; the cache is rewritten only on success.
bool verify(const fs::path &root, const GameManifest &manifest, const fs::path &cache);

// --- Discs and folders ---
// The original English disc: Setup\setup.ini names the product GUID and the cabinets exist.
bool setup_on_disc(const fs::path &root);
// Mounted CD/DVD drives (including mounted ISOs) and removable drives that have media.
std::vector<fs::path> mounted_volumes();
fs::path executable_dir();

struct Locations {
	fs::path resources;  // ...\Battlegrounds\resources
	std::optional<fs::path> project;
	fs::path home;
	// OPENBG_PROJECT_ROOT / a source checkout two folders up; OPENBG_HOME, else
	// <checkout>\source\launcher, else "Battlegrounds Files" next to the app folder when that
	// location is writable and not under Program Files, else %LOCALAPPDATA%\OpenBG.
	static Locations detect(const fs::path &app_dir);
};

// Windows command-line quoting that CommandLineToArgvW and the C runtime parse back exactly.
std::wstring quote_argument(const std::wstring &argument);

// --- The launcher's state, independent of the window ---
struct Stage {
	std::wstring title, detail;
	std::string status = "waiting";  // waiting / active / done
};

class LauncherModel {
public:
	explicit LauncherModel(Locations where, bool preview = false);
	~LauncherModel();

	std::vector<Stage> stages;
	bool busy = false, ready = false;
	std::optional<std::wstring> error;
	std::optional<fs::path> disc, pending_source, last_source, ready_game;
	std::optional<std::wstring> source_name;
	Locations where;
	bool preview;
	HANDLE worker = nullptr, job = nullptr, game = nullptr;

	// Hooks the window provides. post() runs a function later on the UI thread.
	std::function<void(std::function<void()>)> post = [](std::function<void()> f) { f(); };
	std::function<void()> changed = [] {}, needs_window = [] {}, hide_window = [] {}, quit = [] {};

	void start();  // verify an existing installation off the UI thread; launch or show setup
	void scan_discs(const std::vector<fs::path> &volumes);
	void request_install(const fs::path &source);  // asks for confirmation; never installs
	void confirm_install();
	void cancel_pending() { pending_source.reset(); changed(); }
	void receive(const std::map<std::string, std::string> &event);
	void launch();
	void cancel();  // stops setup and everything it started
	std::wstring log_to_open() const;

private:
	void install(const fs::path &source);
	void fail(std::wstring message);
};

}  // namespace obg
