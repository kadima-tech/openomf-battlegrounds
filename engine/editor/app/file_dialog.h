// An in-app file and folder picker (SDL2 has no native dialogs), drawn as a modal.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class FileDialog {
public:
	enum Mode { OpenFile, SaveFile, PickFolder };
	using Done = std::function<void(const std::filesystem::path &)>;
	void open(Mode mode, const std::string &title, const std::filesystem::path &start, std::vector<std::string> extensions,
	          Done done, const std::string &default_name = "");
	void draw();
	bool visible() const { return visible_; }

private:
	void refresh();
	Mode mode_ = OpenFile;
	std::string title_;
	std::filesystem::path dir_;
	std::vector<std::string> extensions_;
	std::vector<std::pair<std::string, bool>> entries_;  // name, is folder
	std::string name_, path_text_, error_;
	int selected_ = -1;
	bool visible_ = false, open_popup_ = false;
	Done done_;
};
