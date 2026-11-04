#include "de/platform.h"

#include <clocale>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/mman.h>
#endif

namespace de {

void init_platform() {
#ifdef _WIN32
	// Only character conversion: numbers keep the "C" locale's decimal point.
	setlocale(LC_CTYPE, ".UTF-8");
#endif
}

#ifdef _WIN32

void *reserve_pages(size_t size) { return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }

bool commit_pages(void *p, size_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != nullptr; }

void release_pages(void *p, size_t) {
	if (p) VirtualFree(p, 0, MEM_RELEASE);
}

namespace {

// Wide APIs keep non-ASCII user names intact whatever the process code page is.
std::filesystem::path known_folder(REFKNOWNFOLDERID id, const wchar_t *fallback_env) {
	std::filesystem::path base;
	PWSTR known = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &known))) base = known;
	CoTaskMemFree(known);
	if (base.empty()) {
		if (const wchar_t *value = _wgetenv(fallback_env)) base = value;
		else base = L".";
	}
	return base;
}

}  // namespace

std::filesystem::path user_data_dir() { return known_folder(FOLDERID_RoamingAppData, L"APPDATA") / L"openbg"; }

std::filesystem::path pictures_dir() { return known_folder(FOLDERID_Pictures, L"USERPROFILE"); }

#else

void *reserve_pages(size_t size) {
	void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
}

bool commit_pages(void *p, size_t size) { return mprotect(p, size, PROT_READ | PROT_WRITE) == 0; }

void release_pages(void *p, size_t size) {
	if (p) munmap(p, size);
}

std::filesystem::path pictures_dir() {
	const char *home = getenv("HOME");
	return std::filesystem::path(home ? home : ".") / "Pictures";
}

std::filesystem::path user_data_dir() {
	const char *home = getenv("HOME");
	std::filesystem::path base = home ? home : ".";
#ifdef __APPLE__
	return base / "Library/Application Support/openbg";
#else
	if (const char *xdg = getenv("XDG_DATA_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "openbg";
	return base / ".local/share/openbg";
#endif
}

#endif

}  // namespace de
