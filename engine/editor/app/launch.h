// Starting the game (openbg) as a separate process, its output forwarded line by line.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class GameProcess {
public:
	using Sink = std::function<void(const std::string &line)>;
	~GameProcess();
	// Throws std::runtime_error when the program cannot start.
	static std::unique_ptr<GameProcess> start(const std::filesystem::path &program, const std::vector<std::string> &args,
	                                          Sink sink);
	bool running();
	int exit_code() const { return exit_code_; }

	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
	int exit_code_ = -1;
};

// openbg next to this program (openbg.exe on Windows), or OPENBG_GAME when set.
std::filesystem::path game_program();
