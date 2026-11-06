#include "launch.h"

#include <SDL.h>

#include <cstdlib>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

struct GameProcess::Impl {
	std::thread reader;
#ifdef _WIN32
	HANDLE process = nullptr;
	HANDLE out = nullptr;
#else
	pid_t pid = -1;
	int out = -1;
#endif
};

namespace {

void read_lines(std::function<size_t(char *, size_t)> read, GameProcess::Sink sink) {
	std::string line;
	char buf[4096];
	for (;;) {
		size_t n = read(buf, sizeof buf);
		if (!n) break;
		for (size_t i = 0; i < n; ++i) {
			if (buf[i] == '\n') {
				if (!line.empty() && line.back() == '\r') line.pop_back();
				sink(line);
				line.clear();
			} else {
				line += buf[i];
			}
		}
	}
	if (!line.empty()) sink(line);
}

}  // namespace

#ifdef _WIN32

namespace {

std::wstring wide(const std::string &s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
	return w;
}

std::wstring quote(const std::wstring &arg) {
	std::wstring out = L"\"";
	size_t slashes = 0;
	for (wchar_t c : arg) {
		if (c == L'\\') {
			++slashes;
		} else if (c == L'"') {
			out.append(slashes * 2 + 1, L'\\');
			slashes = 0;
		} else {
			out.append(slashes, L'\\');
			slashes = 0;
		}
		if (c != L'\\') out += c;
	}
	out.append(slashes * 2, L'\\');
	return out + L"\"";
}

}  // namespace

std::unique_ptr<GameProcess> GameProcess::start(const std::filesystem::path &program, const std::vector<std::string> &args, Sink sink) {
	SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
	HANDLE rd, wr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) throw std::runtime_error("cannot create a pipe");
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	std::wstring cmd = quote(program.wstring());
	for (auto &a : args) cmd += L" " + quote(wide(a));
	STARTUPINFOW si{};
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = wr;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION pi{};
	BOOL ok = CreateProcessW(program.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(wr);
	if (!ok) {
		CloseHandle(rd);
		throw std::runtime_error("cannot start " + program.string());
	}
	CloseHandle(pi.hThread);
	auto p = std::unique_ptr<GameProcess>(new GameProcess);
	p->impl_ = std::make_unique<Impl>();
	p->impl_->process = pi.hProcess;
	p->impl_->out = rd;
	p->impl_->reader = std::thread([rd, sink] {
		read_lines(
		    [rd](char *b, size_t n) -> size_t {
			    DWORD got = 0;
			    return ReadFile(rd, b, DWORD(n), &got, nullptr) ? got : 0;
		    },
		    sink);
	});
	return p;
}

bool GameProcess::running() {
	if (!impl_ || !impl_->process) return false;
	if (WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT) return true;
	DWORD code = 0;
	GetExitCodeProcess(impl_->process, &code);
	exit_code_ = int(code);
	CloseHandle(impl_->process);
	impl_->process = nullptr;
	return false;
}

GameProcess::~GameProcess() {
	if (!impl_) return;
	if (impl_->reader.joinable()) impl_->reader.detach();
	if (impl_->process) CloseHandle(impl_->process);
}

#else

std::unique_ptr<GameProcess> GameProcess::start(const std::filesystem::path &program, const std::vector<std::string> &args, Sink sink) {
	int fds[2];
	if (pipe(fds) != 0) throw std::runtime_error("cannot create a pipe");
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	std::vector<std::string> all{program.string()};
	all.insert(all.end(), args.begin(), args.end());
	std::vector<char *> argv;
	for (auto &a : all) argv.push_back(a.data());
	argv.push_back(nullptr);
	pid_t pid = -1;
	int err = posix_spawn(&pid, all[0].c_str(), &fa, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (err != 0) {
		close(fds[0]);
		throw std::runtime_error("cannot start " + program.string());
	}
	auto p = std::unique_ptr<GameProcess>(new GameProcess);
	p->impl_ = std::make_unique<Impl>();
	p->impl_->pid = pid;
	p->impl_->out = fds[0];
	int fd = fds[0];
	p->impl_->reader = std::thread([fd, sink] {
		read_lines(
		    [fd](char *b, size_t n) -> size_t {
			    ssize_t got = read(fd, b, n);
			    return got > 0 ? size_t(got) : 0;
		    },
		    sink);
		close(fd);
	});
	return p;
}

bool GameProcess::running() {
	if (!impl_ || impl_->pid <= 0) return false;
	int status = 0;
	pid_t r = waitpid(impl_->pid, &status, WNOHANG);
	if (r == 0) return true;
	exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	impl_->pid = -1;
	return false;
}

GameProcess::~GameProcess() {
	if (!impl_) return;
	if (impl_->reader.joinable()) impl_->reader.detach();
}

#endif

std::filesystem::path game_program() {
	if (const char *env = getenv("OPENBG_GAME")) return env;
	std::filesystem::path dir;
	if (char *base = SDL_GetBasePath()) {
		dir = base;
		SDL_free(base);
	}
#ifdef _WIN32
	return dir / "openbg.exe";
#else
	return dir / "openbg";
#endif
}
