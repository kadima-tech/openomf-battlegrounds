#include "file_dialog.h"

#include <algorithm>
#include <cctype>

#include "icons.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "theme.h"
#include "widgets.h"

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
	for (char &c : s) c = char(tolower(uint8_t(c)));
	return s;
}

}  // namespace

void FileDialog::open(Mode mode, const std::string &title, const fs::path &start, std::vector<std::string> extensions, Done done,
                      const std::string &default_name) {
	mode_ = mode;
	title_ = title;
	std::error_code ec;
	dir_ = fs::is_directory(start, ec) ? start : start.has_parent_path() && fs::is_directory(start.parent_path(), ec) ? start.parent_path() : fs::current_path(ec);
	extensions_ = std::move(extensions);
	done_ = std::move(done);
	name_ = default_name;
	error_.clear();
	selected_ = -1;
	visible_ = open_popup_ = true;
	refresh();
}

void FileDialog::refresh() {
	entries_.clear();
	path_text_ = dir_.string();
	std::error_code ec;
	for (fs::directory_iterator it(dir_, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
		std::string name = it->path().filename().string();
		if (name.empty() || name[0] == '.') continue;
		bool folder = it->is_directory(ec);
		if (!folder) {
			if (mode_ == PickFolder) continue;
			std::string ext = lower(it->path().extension().string());
			if (!extensions_.empty() && std::find(extensions_.begin(), extensions_.end(), ext) == extensions_.end()) continue;
		}
		entries_.emplace_back(name, folder);
	}
	std::sort(entries_.begin(), entries_.end(), [](auto &a, auto &b) {
		if (a.second != b.second) return a.second > b.second;
		return lower(a.first) < lower(b.first);
	});
}

void FileDialog::draw() {
	if (!visible_) return;
	using namespace theme;
	if (open_popup_) {
		ImGui::OpenPopup(title_.c_str());
		open_popup_ = false;
	}
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(ui::px(720), ui::px(520)), ImGuiCond_Appearing);
	bool keep = true;
	if (!ImGui::BeginPopupModal(title_.c_str(), &keep)) {
		if (!keep) visible_ = false;
		return;
	}
	auto finish = [&](const fs::path &p) {
		visible_ = false;
		ImGui::CloseCurrentPopup();
		if (done_) done_(p);
	};
	if (ui::icon_button("up", ICON_ARROW_UP_RIGHT, "Parent folder") && dir_.has_parent_path()) {
		dir_ = dir_.parent_path();
		refresh();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##path", &path_text_, ImGuiInputTextFlags_EnterReturnsTrue)) {
		std::error_code ec;
		if (fs::is_directory(path_text_, ec)) dir_ = path_text_, refresh();
		else if (mode_ == OpenFile && fs::is_regular_file(path_text_, ec)) finish(path_text_);
	}
	float footer = ImGui::GetFrameHeightWithSpacing() * (mode_ == SaveFile ? 2.2f : 1.2f) + ui::px(8);
	ImGui::BeginChild("entries", ImVec2(0, -footer), ImGuiChildFlags_Borders);
	for (size_t i = 0; i < entries_.size(); ++i) {
		auto &[name, folder] = entries_[i];
		std::string label = std::string(folder ? ICON_FOLDER : ICON_FILE_TEXT) + "  " + name;
		if (ImGui::Selectable(label.c_str(), selected_ == int(i), ImGuiSelectableFlags_AllowDoubleClick)) {
			selected_ = int(i);
			if (!folder && mode_ == SaveFile) name_ = name;
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				if (folder) {
					dir_ /= name;
					refresh();
					break;
				}
				if (mode_ == OpenFile) {
					finish(dir_ / name);
					break;
				}
			}
		}
	}
	ImGui::EndChild();
	if (mode_ == SaveFile) {
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##name", "File name", &name_);
	}
	if (!error_.empty()) {
		ui::dim_text(error_.c_str(), kError);
		ImGui::SameLine();
	}
	float bw = ui::px(110);
	ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
	if (ImGui::Button("Cancel", ImVec2(bw, 0))) {
		visible_ = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	const char *ok = mode_ == SaveFile ? "Save" : mode_ == PickFolder ? "Choose" : "Open";
	if (ImGui::Button(ok, ImVec2(bw, 0))) {
		if (mode_ == PickFolder) {
			finish(selected_ >= 0 && entries_[size_t(selected_)].second ? dir_ / entries_[size_t(selected_)].first : dir_);
		} else if (mode_ == SaveFile) {
			if (name_.empty()) error_ = "Enter a file name.";
			else {
				fs::path p = dir_ / name_;
				if (!extensions_.empty() && lower(p.extension().string()) != extensions_[0]) p += extensions_[0] == ".deobjects" ? ".DEObjects" : extensions_[0];
				finish(p);
			}
		} else if (selected_ >= 0 && !entries_[size_t(selected_)].second) {
			finish(dir_ / entries_[size_t(selected_)].first);
		} else if (selected_ >= 0) {
			dir_ /= entries_[size_t(selected_)].first;
			refresh();
		}
	}
	ImGui::EndPopup();
	if (!keep) visible_ = false;
}
