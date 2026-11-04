// Windows twin of test-readiness.swift: verification, the cache shared with install.py,
// same-size edits, changed expected patches, malformed caches and missing files.
// Usage: test-readiness <launcher source folder>; OPENBG_TEST_PYTHON names the Python to use.
#include "core.h"

#include <cstdio>
#include <fstream>

using namespace obg;

#define CHECK(condition, message)                         \
	do {                                                  \
		if (!(condition)) {                               \
			fprintf(stderr, "FAILED: %s\n", message);     \
			return 1;                                     \
		}                                                 \
	} while (0)

static void write(const fs::path &path, const char *text) {
	std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

static int run(std::vector<std::wstring> args) {
	std::wstring command;
	for (auto &a : args) command += (command.empty() ? L"" : L" ") + quote_argument(a);
	STARTUPINFOW si{};
	si.cb = sizeof si;
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return int(code);
}

int wmain(int argc, wchar_t **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: test-readiness <launcher folder>\n");
		return 2;
	}
	// Spaces and a non-ASCII name exercise quoting and the JSON escapes Python writes.
	fs::path root = fs::temp_directory_path() / (L"openbg readiness \u00E9 " + std::to_wstring(GetCurrentProcessId()));
	fs::remove_all(root);
	fs::create_directories(root);
	struct Cleanup {
		fs::path p;
		~Cleanup() {
			std::error_code ec;
			fs::remove_all(p, ec);
		}
	} cleanup{root};
	fs::path file = root / L"game-file", cache = root / L"verified.json";
	write(file, "patched");
	auto hash = sha256_file(file);
	CHECK(hash && *hash == "d7017ebcd65455e76e953d5b42fa96c3df28c7c3b616c7f069ed930fb4fae5fd", "SHA-256 must match");
	GameManifest manifest{"2.123", {{"game-file", 7, *hash}}};
	CHECK(verify(root, manifest, cache), "Initial checksums must pass");
	CHECK(fs::exists(cache), "Successful verification must save its cache");
	CHECK(verify(root, manifest, cache), "Unchanged verified files must be ready");

	// The real Python writer and the native reader must share the same cache.
	auto native = load_cache(cache);
	CHECK(native, "The native cache must be readable");
	std::wstring python = env(L"OPENBG_TEST_PYTHON").value_or(L"python");
	int status = run({python, L"-c",
	                  L"import sys\n"
	                  L"sys.path.insert(0, sys.argv[1])\n"
	                  L"import install\n"
	                  L"from pathlib import Path\n"
	                  L"root = Path(sys.argv[2])\n"
	                  L"install.record_verified(root, {'files': [{'path': 'game-file', 'size': 7,\n"
	                  L"    'sha256': install.digest(root / 'game-file')}]}, Path(sys.argv[3]))\n",
	                  argv[1], root.wstring(), cache.wstring()});
	CHECK(status == 0, "Python cache writer must run successfully");
	auto written = load_cache(cache);
	CHECK(written, "The Python cache must be readable");
	CHECK(native->root == written->root, "Python and the launcher must use the same canonical root");
	CHECK(native->files == written->files, "Python and launcher file identities must match");
	CHECK(verify(root, manifest, cache), "Python cache must be accepted by the launcher");

	write(file, "damaged");
	CHECK(!verify(root, manifest, cache), "A same-size edit must invalidate readiness");
	write(file, "patched");
	CHECK(verify(root, manifest, cache), "Restored files must be reverified");

	GameManifest different{"next", {{"game-file", 7, std::string(64, '0')}}};
	CHECK(!verify(root, different, cache), "Expected checksum changes must invalidate the cache");
	write(cache, "invalid json");
	CHECK(verify(root, manifest, cache), "A malformed cache must fall back to real checksums");
	CHECK(!verify(root, GameManifest{"2.123", {}}, cache), "An empty manifest must not count as ready");
	fs::remove(file);
	CHECK(!verify(root, manifest, cache), "Missing game files must not count as ready");
	printf("Native readiness checks passed (verification, shared Python cache, same-size edits, patch changes, corrupt cache, missing files)\n");
	return 0;
}
