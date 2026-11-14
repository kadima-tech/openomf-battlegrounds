#include "de/platform.h"

#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#else
#include <sys/mman.h>
#endif

namespace de {

namespace {

#ifdef _WIN32
// A crash (access violation, stack overflow, ...) the program does not handle: say what and where
// on stderr (the launcher's game.log) and write a minidump next to it (dbghelp, loaded only now),
// then let Windows end the process as it would have.
LONG WINAPI report_crash(EXCEPTION_POINTERS *info) {
	static LONG reported = 0;
	if (InterlockedExchange(&reported, 1)) return EXCEPTION_CONTINUE_SEARCH;  // once (unwinding can raise more)
	const EXCEPTION_RECORD *r = info->ExceptionRecord;
	const char *kind = r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION    ? " (access violation)"
	                   : r->ExceptionCode == EXCEPTION_STACK_OVERFLOW    ? " (stack overflow)"
	                   : r->ExceptionCode == 0x20474343                  ? " (a C++ exception nothing caught)"  // GCC's SEH code, "GCC "
	                   : r->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO ? " (integer division by zero)"
	                                                                      : "";
	const void *at = r->ExceptionAddress;
	wchar_t module[MAX_PATH] = L"?";
	HMODULE m = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(at), &m)) {
		GetModuleFileNameW(m, module, MAX_PATH);
	}
	fprintf(stderr, "\n[crash] exception 0x%08lx%s at %p (%ls+0x%llx)", r->ExceptionCode, kind, at, module,
	        static_cast<unsigned long long>(static_cast<const char *>(at) - reinterpret_cast<const char *>(m)));
	if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
		fprintf(stderr, ", %s address 0x%llx", r->ExceptionInformation[0] == 0 ? "reading" : r->ExceptionInformation[0] == 1 ? "writing" : "executing",
		        static_cast<unsigned long long>(r->ExceptionInformation[1]));
	}
	fprintf(stderr, "\n");
	char name[64];
	time_t now = time(nullptr);
	strftime(name, sizeof name, "crash-%Y%m%d-%H%M%S.dmp", localtime(&now));
	using WriteDump = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
	                                 PMINIDUMP_CALLBACK_INFORMATION);
	if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
		auto write = reinterpret_cast<WriteDump>(reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
		HANDLE file = CreateFileA(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (write && file != INVALID_HANDLE_VALUE) {
			MINIDUMP_EXCEPTION_INFORMATION ex{GetCurrentThreadId(), info, FALSE};
			const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
			if (write(GetCurrentProcess(), GetCurrentProcessId(), file, type, &ex, nullptr, nullptr)) fprintf(stderr, "[crash] minidump written to %s\n", name);
		}
		if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
	}
	fflush(stderr);
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// std::terminate (an exception nothing caught, a noexcept function that threw) and abort():
// say so before the process ends.
void report_terminate() {
	const char *what = "no exception";
	try {
		if (auto e = std::current_exception()) std::rethrow_exception(e);
	} catch (const std::exception &e) {
		fprintf(stderr, "\n[crash] terminate: %s\n", e.what());
		what = nullptr;
	} catch (...) {
		what = "an exception that is not a std::exception";
	}
	if (what) fprintf(stderr, "\n[crash] terminate: %s\n", what);
	fflush(stderr);
	std::abort();
}

void report_abort(int) {
	fprintf(stderr, "\n[crash] abort\n");
	fflush(stderr);
	std::signal(SIGABRT, SIG_DFL);
	std::abort();
}

}  // namespace

void init_platform() {
#ifdef _WIN32
	// Only character conversion: numbers keep the "C" locale's decimal point.
	setlocale(LC_CTYPE, ".UTF-8");
	// Crash reports (report_crash): with room left on the stack after an overflow to write one.
	SetUnhandledExceptionFilter(report_crash);
	ULONG guarantee = 64 * 1024;
	SetThreadStackGuarantee(&guarantee);
	// The C runtime buffers redirected output in full on Windows (_IOLBF too): unbuffered, so
	// what was printed before a crash is in the log.
	setvbuf(stdout, nullptr, _IONBF, 0);
#endif
	std::set_terminate(report_terminate);
	std::signal(SIGABRT, report_abort);
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
