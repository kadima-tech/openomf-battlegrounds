// Windows twin of test-launcher.swift: setup starts only after explicit consent. Also covers
// folder detection, command-line quoting and setup event parsing.
//
// test-launcher <app folder> <disc image>: additionally runs the packaged setup on an image whose
// installer cabinets are fake, through the launcher's own process, job and pipe handling. Setup
// must read the image, start the bundled extractor and report its failure as an event.
#include "core.h"

#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

using namespace obg;

#define CHECK(condition, message)                         \
	do {                                                  \
		if (!(condition)) {                               \
			fprintf(stderr, "FAILED: %s\n", message);     \
			return 1;                                     \
		}                                                 \
	} while (0)

static int packaged_setup(const fs::path &app, const fs::path &image, const fs::path &home) {
	SetEnvironmentVariableW(L"OPENBG_HOME", home.c_str());
	LauncherModel model(Locations::detect(app));
	std::mutex lock;
	std::deque<std::function<void()>> queue;
	model.post = [&](std::function<void()> f) {
		std::lock_guard<std::mutex> hold(lock);
		queue.push_back(std::move(f));
	};
	bool reading = false;
	model.changed = [&] { reading = reading || model.stages[0].status == "active"; };
	model.request_install(image);
	model.confirm_install();
	CHECK(model.busy && model.worker, "Confirmed setup must start the bundled Python");
	auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
	while (model.busy && std::chrono::steady_clock::now() < deadline) {
		std::function<void()> next;
		{
			std::lock_guard<std::mutex> hold(lock);
			if (!queue.empty()) next = std::move(queue.front()), queue.pop_front();
		}
		if (next) next();
		else std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	// On failure, show what setup reported and logged: CI has no other window into it.
	auto dump = [&] {
		fprintf(stderr, "setup error: %s\n", model.error ? narrow(*model.error).c_str() : "(none)");
		for (auto name : {L"install.log", L"bootstrap.log"}) {
			auto text = read_file(home / name);
			fprintf(stderr, "--- %ls ---\n%s\n", name, text ? text->c_str() : "(missing)");
		}
	};
#define CHECK_SETUP(condition, message) \
	do {                                \
		if (!(condition)) dump();       \
		CHECK(condition, message);      \
	} while (0)
	CHECK_SETUP(!model.busy && !model.worker, "Setup must finish");
	CHECK_SETUP(reading, "Setup must report reading the disc");
	CHECK_SETUP(model.error && model.error->find(L"unshield") != std::wstring::npos, "The extractor's failure must be reported");
	CHECK_SETUP(!model.ready && !fs::exists(home / L"game"), "A failed setup must not publish a game");
	auto log = read_file(home / L"install.log");
	CHECK_SETUP(log && log->find("read ISO 9660 image") != std::string::npos, "Setup must read the image itself");
	CHECK_SETUP(fs::exists(home / L"bootstrap.log"), "Setup diagnostics must go to bootstrap.log");
#undef CHECK_SETUP
	printf("Packaged setup checks passed (bundled Python, image reading, extractor, error events)\n");
	return 0;
}

int wmain(int argc, wchar_t **argv) {
	fs::path root = fs::temp_directory_path() / (L"openbg launcher " + std::to_wstring(GetCurrentProcessId()));
	fs::remove_all(root);
	fs::create_directories(root);
	struct Cleanup {
		fs::path p;
		~Cleanup() {
			std::error_code ec;
			fs::remove_all(p, ec);
		}
	} cleanup{root};

	// Folder detection: a source checkout keeps setup in source\launcher; OPENBG_HOME wins.
	fs::path repo = root / L"repo";
	fs::create_directories(repo / L"engine");
	fs::create_directories(repo / L"dist" / L"Battlegrounds");
	std::ofstream(repo / L"engine" / L"CMakeLists.txt") << "project(openbg)";
	SetEnvironmentVariableW(L"OPENBG_HOME", nullptr);
	SetEnvironmentVariableW(L"OPENBG_PROJECT_ROOT", nullptr);
	Locations checkout = Locations::detect(repo / L"dist" / L"Battlegrounds");
	CHECK(checkout.project && *checkout.project == repo, "A source checkout must be recognized");
	CHECK(checkout.home == repo / L"source" / L"launcher", "A checkout must install into source\\launcher");
	CHECK(checkout.resources == repo / L"dist" / L"Battlegrounds" / L"resources", "Resources sit next to the launcher");
	fs::create_directories(root / L"portable" / L"Battlegrounds");
	Locations portable = Locations::detect(root / L"portable" / L"Battlegrounds");
	CHECK(!portable.project && portable.home == root / L"portable" / L"Battlegrounds Files",
	      "A standalone copy in a writable folder keeps its files next to the app folder");

	fs::path home = root / L"private-state";
	SetEnvironmentVariableW(L"OPENBG_HOME", home.c_str());
	LauncherModel model(Locations::detect(root / L"app"));
	CHECK(model.where.home == home, "OPENBG_HOME must override the install location");
	fs::path setup = root / L"Setup";
	fs::create_directories(setup);
	std::ofstream(setup / L"setup.ini") << "ProductGUID=B8EF8B5A-6D15-436C-AC07-5F2BD5E1AA1B";
	for (auto name : {L"data1.cab", L"data1.hdr", L"data2.cab"}) std::ofstream(setup / name) << "";
	fs::path disc = root;
	auto idle = [&] { return !model.busy && !model.worker && !fs::exists(home); };

	model.scan_discs({disc});
	CHECK(model.disc == disc && !model.pending_source, "Detection must only advertise the disc");
	CHECK(idle(), "No setup process before confirmation");
	model.request_install(disc);
	CHECK(model.pending_source == disc, "CD selection must request confirmation");
	CHECK(idle(), "No setup writes before confirmation");
	model.cancel_pending();
	model.scan_discs({});
	CHECK(!model.disc, "Ejecting must clear the detected disc");
	model.scan_discs({disc});
	CHECK(idle() && !model.pending_source, "Reinserting after cancellation must not start setup");
	model.confirm_install();
	CHECK(idle(), "Confirming with nothing pending does nothing");
	model.request_install(root / L"readme.txt");
	CHECK(model.error && !model.pending_source && idle(), "Only ISO images and discs are accepted");
	fs::path iso = root / L"game.iso";
	model.request_install(iso);
	CHECK(model.pending_source == iso, "ISO selection must request confirmation");
	CHECK(idle(), "No setup writes before confirmation");
	model.error.reset();
	model.confirm_install();
	// This test has no bundled Python. Reaching its error proves only confirmation enters setup.
	CHECK(!model.pending_source && model.error && !model.busy, "Confirmation must enter setup");
	CHECK(fs::exists(home), "Confirmation must create the game folder");

	// Arguments survive the round trip through the Windows command line.
	for (std::wstring a : {L"plain", L"with space", L"C:\\Games\\", L"C:\\Program Files\\x\\", L"quote\"inside", L"", L"tail\\\\"}) {
		std::wstring line = L"prog.exe " + quote_argument(a);
		int n = 0;
		LPWSTR *parsed = CommandLineToArgvW(line.c_str(), &n);
		bool same = parsed && n == 2 && a == parsed[1];
		LocalFree(parsed);
		CHECK(same, "Command-line quoting must round-trip");
	}

	auto event = parse_event("{\"stage\": \"patches\", \"status\": \"active\", \"detail\": \"Applying \\u00b7 1/76\"}\r\n");
	CHECK(event && (*event)["stage"] == "patches" && widen((*event)["detail"]) == L"Applying \u00b7 1/76", "Setup events must parse");
	model.receive(*event);
	CHECK(model.stages[1].status == "active" && model.stages[1].detail == L"Applying \u00b7 1/76", "Events must update their stage");
	model.receive({{"stage", "error"}, {"detail", "Setup is already running in another window."}});
	CHECK(model.error == L"Setup is already running in another window.", "Error events must be shown");
	CHECK(!parse_event("not json") && !parse_event("[1, 2]"), "Other output must be ignored");
	CHECK(Json::parse("{\"inode\": 340282366920938463463374607431768211455}")->unsigned_number("inode") == std::nullopt,
	      "File IDs too large for 64 bits must not be truncated");
	printf("Launcher consent checks passed (detection, selection, cancellation, reinsertion, confirmation, quoting, events)\n");
	if (argc == 3) return packaged_setup(argv[1], argv[2], root / L"packaged setup \u00E9");
	return 0;
}
