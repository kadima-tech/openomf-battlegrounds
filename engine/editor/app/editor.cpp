#include "editor.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "de/platform.h"
#include "icons.h"
#include "image_io.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "preview.h"
#include "theme.h"
#include "widgets.h"
#include "world.h"
#include "assets.h"
#include "models.h"

namespace fs = std::filesystem;
using namespace theme;

namespace {

const char *const kVersion = "DE v2.123";

class LevelDocument : public Document {
public:
	std::string title(Editor &) const override { return "Level Editor"; }
	const char *icon() const override { return ICON_MOUNTAIN; }
	bool closable() const override { return false; }
	void draw(Editor &e) override;
};

std::string now_text() {
	std::time_t t = std::time(nullptr);
	char buf[16];
	std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
	return buf;
}

std::string lower(std::string s) {
	for (char &c : s) c = char(tolower(uint8_t(c)));
	return s;
}

bool matches(const std::string &text, const std::string &filter) {
	return filter.empty() || lower(text).find(lower(filter)) != std::string::npos;
}

std::string path_utf8(const fs::path &p) {
	auto s = p.u8string();
	return std::string(s.begin(), s.end());
}

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const std::string &latin1) { return deo::latin1_to_utf8(deo::Bytes(latin1.begin(), latin1.end())); }

// Window names: icon + label, with a stable ID.
std::string window_name(const char *icon, const std::string &label, const char *id) {
	return std::string(icon) + "  " + label + "###" + id;
}

}  // namespace

void LevelDocument::draw(Editor &e) { e.draw_level(); }

Editor::Editor(Options options, SDL_Window *window) : opt_(std::move(options)), window_(window) {
	settings_dir_ = de::user_data_dir() / "editor";
	std::error_code ec;
	fs::create_directories(settings_dir_, ec);
	layout_ini_ = (settings_dir_ / "layout.ini").string();
	if (!opt_.screenshot) ImGui::GetIO().IniFilename = layout_ini_.c_str();
	else ImGui::GetIO().IniFilename = nullptr;
	if (!opt_.screenshot) load_settings();
	docs_.push_back(std::make_unique<LevelDocument>());
	world_ = std::make_unique<WorldEditor>(*this);
	log(LogLine::Info, "DEObjects Editor started (Dear ImGui " IMGUI_VERSION ")");
	if (!opt_.data.empty()) set_base(opt_.data);
	if (!base_) data_prompt_ = true;
	// Only an add-on named on the command line opens at start; otherwise the user picks one.
	if (base_ && !opt_.project.empty()) {
		try {
			if (fs::exists(opt_.project)) set_project(ed::Project::open(*base_, opt_.project));
			else set_project(ed::Project::create(*base_, opt_.project));
		} catch (const std::exception &ex) {
			log(LogLine::Error, std::string("Cannot open ") + opt_.project.string() + ": " + ex.what());
		}
	}
	apply_startup();
	update_title();
}

Editor::~Editor() {
	if (!opt_.screenshot) save_settings();
	stop_sound();
	world_.reset();
	for (auto &[k, v] : addon_thumbs_) delete_texture(v.first);
	for (auto &[k, v] : base_thumbs_) delete_texture(v);
	for (auto &[k, v] : thumbs_) delete_texture(v.texture);
	docs_.clear();
	refs_.reset();
}

void Editor::load_settings() {
	std::ifstream f(settings_dir_ / "settings.txt");
	std::string line;
	while (std::getline(f, line)) {
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		if (k == "data" && opt_.data.empty()) opt_.data = utf8_path(v);
		if (k == "recent") recent_.push_back(utf8_path(v));
	}
}

void Editor::save_settings() {
	std::ofstream f(settings_dir_ / "settings.txt", std::ios::trunc);
	if (base_) f << "data=" << path_utf8(base_->dir()) << "\n";
	for (auto &r : recent_) f << "recent=" << path_utf8(r) << "\n";
}

void Editor::log(LogLine::Level level, const std::string &text) {
	log_.push_back({level, now_text(), text});
	if (log_.size() > 5000) log_.erase(log_.begin(), log_.begin() + 1000);
	if (level != LogLine::Game) status_ = text;
	if (opt_.screenshot) fprintf(stderr, "%s\n", text.c_str());
}

void Editor::set_base(const fs::path &dir) {
	try {
		auto b = std::make_unique<ed::BaseData>(dir);
		fs::path reopen = project_ ? project_->path() : fs::path();
		if (project_ && project_->dirty()) log(LogLine::Warning, "Unsaved changes to " + reopen.filename().string() + " were dropped with the old Data folder");
		docs_.resize(1);
		active_ = 0;
		project_.reset();
		refs_.reset();
		selection_ = {};
		items_key_.clear();
		parents_.clear();
		for (auto &[k, v] : thumbs_) delete_texture(v.texture);
		thumbs_.clear();
		base_ = std::move(b);
		fighters_.clear();
		base_thumbs_.clear();
		log(LogLine::Ok, "Base game data: " + dir.string() + " (" + std::to_string(base_->files().size()) + " files, " +
		                     std::to_string(base_->object_count()) + " objects, read-only)");
		data_prompt_ = false;
		if (!reopen.empty() && fs::exists(reopen)) open_project(reopen);
	} catch (const std::exception &ex) {
		log(LogLine::Error, std::string("Cannot use ") + dir.string() + " as the game's Data folder: " + ex.what());
	}
}

void Editor::set_project(std::unique_ptr<ed::Project> p) {
	for (size_t i = docs_.size(); i-- > 1;) {
		if (docs_[i]->addon_content()) docs_.erase(docs_.begin() + long(i));
	}
	active_ = 0;
	if (selection_.kind == ObjectRef::Addon) selection_ = {};
	if (!scope_.base) items_key_.clear();
	undo_.clear();
	redo_.clear();
	project_ = std::move(p);
	settled_ = project_->file();
	settled_revision_ = project_->revision();
	selected_object_ = -1;
	issues_.clear();
	validated_revision_ = 0;
	recent_.erase(std::remove(recent_.begin(), recent_.end(), project_->path()), recent_.end());
	recent_.insert(recent_.begin(), project_->path());
	if (recent_.size() > 8) recent_.resize(8);
	log(LogLine::Ok, "Project " + project_->path().filename().string() + " (add-on agent \"" + project_->agent_name() + "\", " +
	                     std::to_string(project_->file().objects.size()) + " objects)");
	validate_now();
	update_title();
}

void Editor::close_project() {
	if (!project_) return;
	for (size_t i = docs_.size(); i-- > 1;) {
		if (docs_[i]->addon_content()) docs_.erase(docs_.begin() + long(i));
	}
	active_ = 0;
	project_.reset();
	undo_.clear();
	redo_.clear();
	issues_.clear();
	selected_object_ = -1;
	if (selection_.kind == ObjectRef::Addon) selection_ = {};
	scope_.base = true;
	items_key_.clear();
	log(LogLine::Info, "Add-on closed");
	update_title();
}

void Editor::open_project(const fs::path &p, std::function<void()> then) {
	if (!base_) return;
	try {
		for (auto &f : base_->files()) {
			std::error_code ec;
			if (fs::equivalent(f.path, p, ec)) throw std::runtime_error("that is a base game file; the editor never changes those");
		}
		set_project(ed::Project::open(*base_, p));
		if (then) then();
	} catch (const std::exception &ex) {
		log(LogLine::Error, std::string("Cannot open ") + p.string() + ": " + ex.what());
	}
}

void Editor::update_title() {
	std::string t = "DEObjects Editor";
	if (project_) t += " \xe2\x80\x94 " + project_->path().filename().string();
	SDL_SetWindowTitle(window_, t.c_str());
}

void Editor::apply_startup() {
	if (!base_) return;
	for (int c = 0; c < ed::kCategoryCount; ++c) {
		if (opt_.content == ed::category_name(ed::Category(c))) scope_.category = c;
	}
	if (!opt_.content.empty() || opt_.base_content) scope_.base = opt_.base_content || !project_;
	if (!opt_.browse_file.empty()) {
		for (size_t f = 0; f < base_->files().size(); ++f) {
			if (base_->files()[f].stem == opt_.browse_file) scope_ = Scope{true, true, -1, int(f), -1}, file_open_[f] = true;
		}
	}
	for (const std::string *spec : {&opt_.select_base, &opt_.open_base}) {
		if (spec->empty()) continue;
		auto colon = spec->rfind(':');
		std::string cls = spec->substr(0, colon), name = colon == std::string::npos ? "" : spec->substr(colon + 1);
		for (const ed::BaseObject *o : base_->of_class(cls)) {
			if (o->name == name) {
				select(ObjectRef::base(*o));
				if (spec == &opt_.open_base) open_object(ObjectRef::base(*o));
				break;
			}
		}
	}
	if (!opt_.copy_base.empty()) {
		for (const ed::BaseObject *o : base_->of_class("Actor\\Creature\\Pilot")) {
			if (o->name == opt_.copy_base) {
				copy_pilot_to_addon(*o);
				break;
			}
		}
	}
	models_startup(*this);
	if (!project_) return;
	if (!opt_.new_pilot.empty()) {
		auto colon = opt_.new_pilot.find(':');
		std::string from = opt_.new_pilot.substr(0, colon), name = colon == std::string::npos ? from + " Copy" : opt_.new_pilot.substr(colon + 1);
		bool exists = false;
		for (uint32_t pi : ed::addon_pilots(*project_)) exists |= utf8(project_->file().objects[pi].info.name_text()) == name;
		if (!exists) {
			try {
				uint32_t made = ed::create_pilot(*project_, from, name);
				log(LogLine::Ok, "New pilot " + name + " (a copy of " + from + ")");
				if (opt_.open_pilot.empty()) opt_.open_pilot = name;
				(void)made;
			} catch (const std::exception &ex) {
				log(LogLine::Error, ex.what());
			}
		}
	}
	if (!opt_.open_pilot.empty()) {
		for (uint32_t pi : ed::addon_pilots(*project_)) {
			if (utf8(project_->file().objects[pi].info.name_text()) == opt_.open_pilot) {
				ed::Guid g = project_->file().objects[pi].info.guid;
				if (!opt_.photo.empty()) import_photo(g, opt_.photo);
				open_pilot(g);
			}
		}
	}
	if (!opt_.select.empty()) {
		for (size_t i = 0; i < project_->file().objects.size(); ++i) {
			if (selected_object_ < 0 && utf8(project_->file().objects[i].info.name_text()) == opt_.select) {
				selected_object_ = int(i);
				selection_ = ObjectRef::addon(project_->file().objects[i].info.guid);
			}
		}
	}
	for (uint32_t pi : ed::addon_pilots(*project_)) {
		if (!opt_.open_pilot.empty() && utf8(project_->file().objects[pi].info.name_text()) == opt_.open_pilot) {
			selection_ = ObjectRef::addon(project_->file().objects[pi].info.guid);
		}
	}
	assets_startup(*this);
	if (opt_.level) active_ = 0;
	if (opt_.save) save();
	if (opt_.launch) launch_game();
	settled_ = project_->file();
	settled_revision_ = project_->revision();
	validate_now();
}

void Editor::drop_file(const std::filesystem::path &file) { models_drop(*this, file); }

void Editor::validate_now() {
	if (!project_) return;
	issues_ = ed::validate(*project_);
	if (world_) {
		auto more = world_->arena_issues();
		issues_.insert(issues_.end(), more.begin(), more.end());
	}
	validated_revision_ = project_->revision();
}

void Editor::save() {
	if (!project_) return;
	assets_before_save(*this);
	try {
		project_->save();
		log(LogLine::Ok, "Saved " + project_->path().string() + " (" + std::to_string(fs::file_size(project_->path())) + " bytes)");
		validate_now();
		int errors = 0;
		for (auto &i : issues_) errors += i.severity == ed::Issue::Error;
		if (errors) log(LogLine::Warning, std::to_string(errors) + " validation error(s): see the Validation panel");
	} catch (const std::exception &ex) {
		log(LogLine::Error, std::string("Save failed: ") + ex.what());
	}
	update_title();
}

void Editor::launch_game(const std::vector<std::string> &extra_args) {
	if (!project_ || !base_) return;
	if (game_ && game_->running()) {
		log(LogLine::Warning, "The game is already running");
		return;
	}
	fs::path dir = settings_dir_ / "launch";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	fs::path addon = dir / project_->path().filename();
	try {
		auto bytes = project_->bytes();
		std::ofstream f(addon, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
	} catch (const std::exception &ex) {
		log(LogLine::Error, std::string("Cannot write the add-on for the game: ") + ex.what());
		return;
	}
	fs::path program = game_program();
	if (!fs::exists(program)) {
		log(LogLine::Error, "The game program is missing: " + program.string() + " (build the openbg target, or set OPENBG_GAME)");
		return;
	}
	try {
		auto mutex = game_mutex_;
		auto lines = game_lines_;
		std::vector<std::string> args{base_->dir().string(), "--addon", addon.string()};
		args.insert(args.end(), extra_args.begin(), extra_args.end());
		game_ = GameProcess::start(program, args, [mutex, lines](const std::string &l) {
			std::lock_guard<std::mutex> lock(*mutex);
			lines->push_back(l);
		});
		log(LogLine::Ok, "Launched " + program.filename().string() + " on " + base_->dir().string() + " with " + addon.filename().string());
	} catch (const std::exception &ex) {
		log(LogLine::Error, ex.what());
	}
}

void Editor::drain_game_output() {
	std::vector<std::string> lines;
	{
		std::lock_guard<std::mutex> lock(*game_mutex_);
		lines.swap(*game_lines_);
	}
	for (auto &l : lines) log(LogLine::Game, l);
	if (game_ && !game_->running()) {
		log(LogLine::Info, "The game exited (code " + std::to_string(game_->exit_code()) + ")");
		game_.reset();
	}
}

void Editor::open_pilot(const ed::Guid &pilot) {
	for (size_t i = 1; i < docs_.size(); ++i) {
		if (pilot_document_is(docs_[i].get(), pilot)) {
			active_ = i;
			return;
		}
	}
	docs_.push_back(make_pilot_document(*this, pilot));
	active_ = docs_.size() - 1;
}

void Editor::add_document(std::unique_ptr<Document> doc, const ObjectRef &shows) {
	for (size_t i = 1; i < docs_.size(); ++i) {
		if (docs_[i]->shows(shows)) {
			active_ = i;
			return;
		}
	}
	docs_.push_back(std::move(doc));
	active_ = docs_.size() - 1;
}

void Editor::new_pilot_dialog(const std::string &from) {
	if (!project_) {
		with_addon("The new pilot goes into an add-on.", [this, from] { new_pilot_dialog(from); });
		return;
	}
	if (!from.empty()) new_pilot_base_ = from;
	int n = int(ed::addon_pilots(*project_).size()) + 1;
	new_pilot_name_ = "New Pilot " + std::to_string(n);
	new_pilot_open_ = true;
}

void Editor::import_photo_dialog(const ed::Guid &pilot) {
	file_dialog_.open(FileDialog::OpenFile, "Import Photo", recent_.empty() ? fs::path() : recent_.front().parent_path(), {".png", ".bmp"},
	                  [this, pilot](const fs::path &p) { import_photo(pilot, p); });
}

void Editor::import_photo(const ed::Guid &pilot, const fs::path &file) {
	int idx = find_object(pilot);
	if (idx < 0) return;
	try {
		ed::RgbaImage img = ed::load_image(file);
		ed::import_photo(*project_, ed::pilot_objects(*project_, uint32_t(idx)), img.width, img.height, img.rgba, file.filename().string());
		log(LogLine::Ok, "Imported " + file.filename().string() + " (" + std::to_string(img.width) + "x" + std::to_string(img.height) +
		                     ", stored as 256x256 Image\\Standard)");
	} catch (const std::exception &ex) {
		log(LogLine::Error, "Cannot import " + file.string() + ": " + ex.what());
	}
}

GLuint Editor::base_pilot_texture(const std::string &name) {
	auto it = base_thumbs_.find(name);
	if (it != base_thumbs_.end()) return it->second;
	GLuint tex = 0;
	if (base_) {
		for (const ed::BaseObject *o : base_->of_class("Actor\\Creature\\Pilot")) {
			if (o->name != name) continue;
			int w, h;
			auto px = ed::base_pilot_photo(*base_, *o, &w, &h);
			if (!px.empty()) tex = upload_texture(w, h, px);
			break;
		}
	}
	base_thumbs_[name] = tex;
	return tex;
}

GLuint addon_photo_texture(Editor &e, const ed::Guid &pilot, std::map<std::string, std::pair<GLuint, uint64_t>> &cache) {
	ed::Project *p = e.project();
	int idx = p ? p->find(pilot) : -1;
	if (idx < 0) return 0;
	auto &entry = cache[ed::guid_key(pilot)];
	if (entry.first && entry.second == p->revision()) return entry.first;
	int w, h;
	auto px = ed::photo_rgba(*p, ed::pilot_objects(*p, uint32_t(idx)), &w, &h);
	if (px.empty()) {
		delete_texture(entry.first);
		entry = {0, p->revision()};
		return 0;
	}
	entry.first = upload_texture(w, h, px, entry.first);
	entry.second = p->revision();
	return entry.first;
}

const std::vector<std::string> &Editor::fighter_names() {
	if (fighters_.empty() && base_) {
		for (const ed::BaseObject *o : base_->of_class("Description\\Creature\\Fighter")) fighters_.push_back(o->name);
		std::sort(fighters_.begin(), fighters_.end());
	}
	return fighters_;
}

std::vector<std::pair<std::string, ed::Guid>> Editor::cast_choices() {
	std::vector<std::pair<std::string, ed::Guid>> out;
	if (!base_) return out;
	for (const ed::BaseObject *o : base_->of_class("Description\\Cast")) out.emplace_back(o->name, o->guid);
	std::sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.first < b.first; });
	return out;
}

void Editor::track_undo() {
	if (!project_ || ImGui::IsAnyItemActive()) return;
	if (project_->revision() == settled_revision_) return;
	undo_.push_back(std::move(settled_));
	if (undo_.size() > 64) undo_.pop_front();
	redo_.clear();
	settled_ = project_->file();
	settled_revision_ = project_->revision();
}

void Editor::undo() {
	if (!project_ || undo_.empty()) return;
	redo_.push_back(project_->file());
	project_->file() = std::move(undo_.back());
	undo_.pop_back();
	project_->touch();
	settled_ = project_->file();
	settled_revision_ = project_->revision();
	log(LogLine::Info, "Undo");
}

void Editor::redo() {
	if (!project_ || redo_.empty()) return;
	undo_.push_back(project_->file());
	project_->file() = std::move(redo_.back());
	redo_.pop_back();
	project_->touch();
	settled_ = project_->file();
	settled_revision_ = project_->revision();
	log(LogLine::Info, "Redo");
}

void Editor::new_project_dialog(std::function<void()> then) {
	if (!base_) return;
	fs::path start = recent_.empty() ? de::user_data_dir() / "addons" : recent_.front().parent_path();
	std::error_code ec;
	fs::create_directories(start, ec);
	file_dialog_.open(FileDialog::SaveFile, "New Add-on", start, {".deobjects"},
	                  [this, then](const fs::path &p) {
		                  try {
			                  auto proj = ed::Project::create(*base_, p);
			                  set_project(std::move(proj));
			                  save();
			                  if (then) then();
		                  } catch (const std::exception &ex) {
			                  log(LogLine::Error, ex.what());
		                  }
	                  },
	                  "MyMod.DEObjects");
}

void Editor::open_project_dialog(std::function<void()> then) {
	if (!base_) return;
	file_dialog_.open(FileDialog::OpenFile, "Open Add-on", recent_.empty() ? de::user_data_dir() : recent_.front().parent_path(), {".deobjects"},
	                  [this, then](const fs::path &p) { open_project(p, then); });
}

void Editor::with_addon(const std::string &why, std::function<void()> then) {
	if (project_) {
		then();
		return;
	}
	chooser_open_ = true;
	chooser_why_ = why;
	chooser_then_ = std::move(then);
}

void Editor::copy_pilot_to_addon(const ed::BaseObject &pilot) {
	const ed::BaseObject *bo = &pilot;
	with_addon("Copy " + utf8(pilot.name) + " into which add-on? The base game's files are never changed: the copy (with fresh GUIDs) goes into an add-on.",
	           [this, bo] {
		           std::string name = utf8(bo->name) + " Copy";
		           auto taken = [&](const std::string &n) {
			           for (uint32_t pi : ed::addon_pilots(*project_)) {
				           if (utf8(project_->file().objects[pi].info.name_text()) == n) return true;
			           }
			           return false;
		           };
		           for (int k = 2; taken(name); ++k) name = utf8(bo->name) + " Copy " + std::to_string(k);
		           try {
			           uint32_t made = ed::create_pilot(*project_, *bo, name);
			           ed::Guid g = project_->file().objects[made].info.guid;
			           log(LogLine::Ok, "Copied " + utf8(bo->name) + " (" + base_->files()[bo->file].stem + ") to " + project_->path().filename().string() +
			                                " as " + name + ": fresh GUIDs, listed and unlocked");
			           select(ObjectRef::addon(g));
			           open_pilot(g);
		           } catch (const std::exception &ex) {
			           log(LogLine::Error, std::string("Cannot copy the pilot: ") + ex.what());
		           }
	           });
}

// ---------------------------------------------------------------------------------------------
// Frame

void Editor::frame() {
	thumb_budget_ = opt_.screenshot ? 1000 : 6;
	drain_game_output();
	if (world_) world_->frame();
	if (project_ && validated_revision_ != project_->revision() && !ImGui::IsAnyItemActive()) validate_now();
	ImGuiIO &io = ImGui::GetIO();
	if (!io.WantTextInput || io.KeyCtrl || io.KeySuper) {
		const bool cmd = io.KeyCtrl || io.KeySuper;
		if (cmd && ImGui::IsKeyPressed(ImGuiKey_S, false)) save();
		if (cmd && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? redo() : undo();
		if (cmd && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
		if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) validate_now();
		if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) launch_game();
	}

	draw_menu();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	const ImGuiWindowFlags bar = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(12), 0));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(kChrome));
	if (ImGui::BeginViewportSideBar("##DocTabs", vp, ImGuiDir_Up, ui::px(kDocTabsHeight), bar)) draw_doc_tabs();
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(kPanelHead));
	if (ImGui::BeginViewportSideBar("##Toolbar", vp, ImGuiDir_Up, ui::px(kToolbarHeight), bar)) draw_toolbar();
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(kChrome));
	if (ImGui::BeginViewportSideBar("##Status", vp, ImGuiDir_Down, ui::px(kStatusHeight), bar)) draw_status();
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);

	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(kGround));
	ImGui::Begin("##Host", nullptr,
	             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
	                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
	if (!base_) {
		draw_start_page();
	} else {
		for (size_t i = 0; i < docs_.size();) {
			if (!docs_[i]->alive(*this)) {
				docs_.erase(docs_.begin() + long(i));
				if (active_ >= docs_.size()) active_ = docs_.size() - 1;
			} else {
				++i;
			}
		}
		if (active_ != 0) ImGui::DockSpace(ImGui::GetID("LevelDock"), ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
		docs_[active_]->draw(*this);
	}
	ImGui::End();

	draw_new_pilot_popup();
	assets_popups(*this);
	models_popups(*this);
	draw_addon_chooser();
	draw_data_prompt();
	draw_about();
	draw_quit_prompt();
	file_dialog_.draw();
	track_undo();
}

void Editor::draw_menu() {
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(10), ui::px(8)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(10), ui::px(8)));
	if (!ImGui::BeginMainMenuBar()) {
		ImGui::PopStyleVar(2);
		return;
	}
	{
		ImDrawList *dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float h = ImGui::GetFrameHeight();
		ImVec2 c(p.x + ui::px(12), p.y + h * 0.5f);
		float r = ui::px(8);
		ImVec2 pts[6];
		for (int k = 0; k < 6; ++k) {
			float a = float(k) * 3.14159265f / 3.0f + 3.14159265f / 6.0f;
			pts[k] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
		}
		dl->AddConvexPolyFilled(pts, 6, rgb(kAccent));
		dl->AddCircleFilled(c, r * 0.38f, rgb(0x16171d));
		ImGui::Dummy(ImVec2(ui::px(26), h));
		ImGui::SameLine(0, ui::px(4));
	}
	const bool has_project = project_ != nullptr;
	if (ImGui::BeginMenu("File")) {
		if (ImGui::MenuItem(ICON_FILE_PLUS "  New Add-on...", nullptr, false, base_ != nullptr)) new_project_dialog();
		if (ImGui::MenuItem(ICON_FOLDER_OPEN "  Open Add-on...", nullptr, false, base_ != nullptr)) open_project_dialog();
		if (ImGui::BeginMenu(ICON_HISTORY "  Recent", !recent_.empty() && base_)) {
			for (auto &r : recent_) {
				if (ImGui::MenuItem(r.filename().string().c_str(), nullptr, false, fs::exists(r))) {
					fs::path chosen = r;
					open_project(chosen);
					break;
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", path_utf8(r).c_str());
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Clear Recent")) recent_.clear();
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem(ICON_X "  Close Add-on", nullptr, false, has_project)) close_project();
		ImGui::Separator();
		models_file_menu(*this);
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_SAVE "  Save", "Ctrl+S", false, has_project)) save();
		if (ImGui::MenuItem("Save As...", nullptr, false, has_project)) {
			file_dialog_.open(FileDialog::SaveFile, "Save Add-on As", project_->path().parent_path(), {".deobjects"},
			                  [this](const fs::path &p) {
				                  auto copy = std::make_unique<ed::Project>(*base_, p, project_->file());
				                  set_project(std::move(copy));
				                  save();
			                  },
			                  project_->path().filename().string());
		}
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_FOLDER "  Game Data Folder...")) data_prompt_ = true;
		ImGui::Separator();
		if (ImGui::MenuItem("Quit")) request_quit();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Edit")) {
		if (ImGui::MenuItem(ICON_UNDO_2 "  Undo", "Ctrl+Z", false, !undo_.empty())) undo();
		if (ImGui::MenuItem(ICON_REDO_2 "  Redo", "Ctrl+Shift+Z", false, !redo_.empty())) redo();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Content")) {
		if (ImGui::MenuItem(ICON_USER "  New Pilot...", nullptr, false, base_ != nullptr)) new_pilot_dialog();
		if (world_) world_->draw_arena_menu();
		assets_content_menu(*this);
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_FOLDER "  Content Browser")) focus_content_ = true, active_ = 0;
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Build")) {
		if (ImGui::MenuItem(ICON_CIRCLE_CHECK "  Validate", "F7", false, has_project)) validate_now();
		if (ImGui::MenuItem(ICON_ROCKET "  Launch Game", "F5", false, has_project)) launch_game();
		if (ImGui::MenuItem(ICON_PLAY "  Play in Editor", "Alt+P", false, base_ && world_ && !world_->playing())) world_->start_play(false);
		if (ImGui::MenuItem(ICON_CIRCLE_PLAY "  Simulate", nullptr, false, base_ && world_ && !world_->playing())) world_->start_play(true);
		if (ImGui::MenuItem(ICON_SQUARE "  Stop", "Esc", false, world_ && world_->playing())) world_->stop_play();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Tools")) {
		if (ImGui::MenuItem(ICON_LAYOUT_GRID "  Reset Layout")) reset_layout_ = true, active_ = 0;
		if (ImGui::MenuItem(ICON_TERMINAL "  Script Console")) {
			active_ = 0;
			ImGui::SetWindowFocus("###ScriptConsole");
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Help")) {
		if (ImGui::MenuItem(ICON_INFO "  About DEObjects Editor")) show_about_ = true;
		ImGui::EndMenu();
	}
	ImGui::EndMainMenuBar();
	ImGui::PopStyleVar(2);
}

void Editor::draw_doc_tabs() {
	ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ui::px(34));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(4), 0));
	if (!base_) {
		bool close = false;
		ui::doc_tab("start", ICON_HEXAGON, "Start", true, false, &close);
	}
	for (size_t i = 0; base_ && i < docs_.size(); ++i) {
		bool close = false;
		ImGui::PushID(int(i));
		if (i) ImGui::SameLine();
		bool dirty = i == 0 && project_ && project_->dirty();
		if (ui::doc_tab("tab", docs_[i]->icon(), docs_[i]->title(*this), i == active_, docs_[i]->closable(), &close, dirty)) active_ = i;
		ImGui::PopID();
		if (close) {
			docs_.erase(docs_.begin() + long(i));
			if (active_ >= i && active_ > 0) --active_;
			break;
		}
	}
	ImGui::PopStyleVar();
	// The active tab's colour runs into the toolbar.
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p0 = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
	dl->AddLine(ImVec2(p0.x, p0.y + sz.y - 1), ImVec2(p0.x + sz.x, p0.y + sz.y - 1), rgb(kDocTabActive), ui::px(2));
}

void Editor::draw_toolbar() {
	const bool has = project_ != nullptr;
	const float h = ui::px(32);
	ImGui::SetCursorPosY((ImGui::GetWindowHeight() - h) * 0.5f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), 0));
	if (ui::toolbar_button(ICON_SAVE, "Save All", ui::Tone::Normal, has, "Write the add-on (Ctrl+S)")) save();
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_FOLDER, "Content", ui::Tone::Normal, base_ != nullptr, "Show the Content Browser")) focus_content_ = true, active_ = 0;
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_CIRCLE_CHECK, "Validate", ui::Tone::Normal, has, "Run the add-on checks (F7)")) {
		validate_now();
		active_ = 0;
		ImGui::SetWindowFocus("###Validation");
	}
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_PLUS, "Place " ICON_CHEVRON_DOWN, ui::Tone::Normal, has)) ImGui::OpenPopup("PlaceMenu");
	if (ImGui::BeginPopup("PlaceMenu")) {
		ImGui::TextDisabled("Placing actors arrives with the arena editor");
		for (const char *item : {"Spawn Point", "Pickup Spawner", "Hazard", "Fighting Surface", "Light", "Camera"}) ImGui::MenuItem(item, nullptr, false, false);
		ImGui::EndPopup();
	}
	ImGui::SameLine(0, ui::px(16));
	world_->draw_toolbar_play();
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_ROCKET, "Launch Game", ui::Tone::Normal, has, "Start openbg on the base data plus this add-on (F5)")) launch_game();
	if (game_) {
		ImGui::SameLine();
		ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ImGui::GetFontSize()) * 0.5f);
		ui::status_chip(ICON_CIRCLE_DOT, "Game running", kOk);
	}
	// Play mode, right-aligned.
	const char *label = "Play Mode:";
	float combo_w = ui::px(170);
	float x = ImGui::GetWindowContentRegionMax().x - combo_w - ImGui::CalcTextSize(label).x - ui::px(10);
	ImGui::SameLine(x);
	ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ImGui::GetFontSize()) * 0.5f);
	ui::dim_text(label, kTextDim);
	ImGui::SameLine(0, ui::px(10));
	ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ImGui::GetFrameHeight()) * 0.5f);
	ImGui::SetNextItemWidth(combo_w);
	ImGui::SetNextWindowSizeConstraints(ImVec2(ui::px(340), 0), ImVec2(ui::px(420), FLT_MAX));
	if (ui::begin_combo("##playmode", world_->play_mode_label().c_str(), ImGuiComboFlags_HeightLargest)) {
		world_->draw_play_menu();
		assets_play_menu(*this);
		ImGui::EndCombo();
	}
	ImGui::PopStyleVar();
}

void Editor::draw_status() {
	ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ImGui::GetFontSize()) * 0.5f);
	ImGui::PushFont(nullptr, kFontSmall);
	ui::dim_text(status_.c_str(), kTextDim);
	std::string right;
	if (project_) {
		if (project_->dirty()) right = "\xe2\x97\x8f Unsaved changes    ";
		right += project_->path().filename().string() + " (Add-on)  |  " + kVersion;
	} else if (base_) {
		right = std::string("No add-on open  |  ") + kVersion;
	} else {
		right = kVersion;
	}
	float w = ImGui::CalcTextSize(right.c_str()).x;
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - w);
	ui::dim_text(right.c_str(), kTextDim);
	ImGui::PopFont();
}

void Editor::draw_start_page() {
	ImVec2 avail = ImGui::GetContentRegionAvail();
	float w = std::min(avail.x - ui::px(80), ui::px(560));
	ImGui::SetCursorPos(ImVec2((avail.x - w) * 0.5f, avail.y * 0.22f));
	ImGui::BeginGroup();
	ImGui::PushFont(fonts.semibold, kFontHeading);
	ImGui::TextUnformatted("DEObjects Editor");
	ImGui::PopFont();
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
	ui::dim_text("Browse and make content for One Must Fall: Battlegrounds 2.123. First, point the editor at the game's Data folder; "
	             "it is only read, never changed.");
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, ui::px(12)));
	if (ui::toolbar_button(ICON_FOLDER, "Choose the game's Data folder...", ui::Tone::Accent)) data_prompt_ = true;
	ImGui::EndGroup();
}

void Editor::draw_welcome_card(ImVec2 p0, ImVec2 size) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	std::vector<fs::path> recent;
	for (auto &r : recent_) {
		if (fs::exists(r) && recent.size() < 5) recent.push_back(r);
	}
	float rows = recent.size() > 1 ? float(recent.size() - 1) : 0;
	ImVec2 card(std::min(size.x - ui::px(40), ui::px(540)), ui::px(150) + (rows > 0 ? ui::px(30) + rows * ui::px(28) : 0));
	card.y = std::min(card.y, size.y - ui::px(60));
	ImVec2 c0 = p0 + (size - card) * 0.5f;
	dl->AddRectFilled(c0, c0 + card, rgb(0x1d1e26, 240), ui::px(8));
	dl->AddRect(c0, c0 + card, rgb(kBorder), ui::px(8));
	ImGui::SetCursorScreenPos(c0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(22), ui::px(18)));
	ImGui::BeginChild("##welcome", card, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	ImGui::PushFont(fonts.semibold, kFontUi + 3);
	ImGui::TextUnformatted("No add-on open");
	ImGui::PopFont();
	ImGui::PushTextWrapPos(card.x - ui::px(22));
	ui::dim_text("The whole base game is in the Content Browser below: double-click anything to view it. To make your own content, "
	             "open an add-on or start a new one.");
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, ui::px(4)));
	if (!recent.empty()) {
		std::string label = "Reopen " + recent.front().filename().string();
		if (ui::toolbar_button(ICON_HISTORY, label.c_str(), ui::Tone::Accent, true, path_utf8(recent.front()).c_str())) open_project(recent.front());
		ImGui::SameLine();
	}
	if (ui::toolbar_button(ICON_FOLDER_OPEN, "Open Add-on...", recent.empty() ? ui::Tone::Accent : ui::Tone::Normal)) open_project_dialog();
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_FILE_PLUS, "New Add-on...")) new_project_dialog();
	if (recent.size() > 1) {
		ImGui::Dummy(ImVec2(0, ui::px(2)));
		ui::dim_text("RECENT", kTextMuted);
		for (size_t k = 1; k < recent.size(); ++k) {
			ImGui::PushID(int(k));
			std::string row = std::string(ICON_FILE_TEXT "  ") + recent[k].filename().string();
			bool clicked = ImGui::Selectable(row.c_str(), false, 0, ImVec2(0, ui::px(24)));
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", path_utf8(recent[k]).c_str());
			ImGui::PopID();
			if (clicked) {
				open_project(recent[k]);
				break;
			}
		}
	}
	ImGui::EndChild();
}

void Editor::draw_addon_chooser() {
	if (chooser_open_) ImGui::OpenPopup("Choose an Add-on"), chooser_open_ = false;
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(ui::px(580), 0), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("Choose an Add-on", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::PushTextWrapPos(ui::px(560));
	ImGui::TextUnformatted(chooser_why_.c_str());
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, ui::px(4)));
	std::function<void()> run;
	auto then = chooser_then_;
	std::vector<fs::path> recent;
	for (auto &r : recent_) {
		if (fs::exists(r)) recent.push_back(r);
	}
	if (!recent.empty()) {
		ui::dim_text("RECENT ADD-ONS", kTextMuted);
		for (size_t k = 0; k < recent.size(); ++k) {
			ImGui::PushID(int(k));
			std::string row = std::string(ICON_FILE_TEXT "  ") + recent[k].filename().string() + "    " + path_utf8(recent[k].parent_path());
			if (ImGui::Selectable(row.c_str(), false, 0, ImVec2(ui::px(560), ui::px(26)))) {
				fs::path chosen = recent[k];
				run = [this, chosen, then] { open_project(chosen, then); };
			}
			ImGui::PopID();
		}
		ImGui::Dummy(ImVec2(0, ui::px(4)));
	} else {
		ui::dim_text("No recent add-ons yet: open one you have, or make a new one.", kTextMuted);
		ImGui::Dummy(ImVec2(0, ui::px(2)));
	}
	if (ui::toolbar_button(ICON_FOLDER_OPEN, "Open Add-on...", recent.empty() ? ui::Tone::Accent : ui::Tone::Normal)) {
		run = [this, then] { open_project_dialog(then); };
	}
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_FILE_PLUS, "New Add-on...")) run = [this, then] { new_project_dialog(then); };
	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Cancel").x - ui::px(24));
	if (ui::toolbar_button(nullptr, "Cancel")) {
		chooser_then_ = {};
		ImGui::CloseCurrentPopup();
	}
	if (run) {
		chooser_then_ = {};
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
	if (run) run();
}

// ---------------------------------------------------------------------------------------------
// Level editor

void Editor::default_layout(ImGuiID id) {
	ImGui::DockBuilderRemoveNode(id);
	ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(id, ImGui::GetContentRegionAvail());
	ImGuiID bottom, top, left, centre, right, right_top, right_bottom;
	ImGui::DockBuilderSplitNode(id, ImGuiDir_Down, 0.45f, &bottom, &top);
	ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.15f, &left, &centre);
	ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.24f, &right, &centre);
	ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.36f, &right_top, &right_bottom);
	ImGui::DockBuilderDockWindow("###Place", left);
	ImGui::DockBuilderDockWindow("###Viewport", centre);
	ImGui::DockBuilderDockWindow("###Outliner", right_top);
	ImGui::DockBuilderDockWindow("###LiveInspector", right_bottom);
	ImGui::DockBuilderDockWindow("###Details", right_bottom);
	ImGui::DockBuilderDockWindow("###ContentBrowser", bottom);
	ImGui::DockBuilderDockWindow("###OutputLog", bottom);
	ImGui::DockBuilderDockWindow("###Validation", bottom);
	ImGui::DockBuilderDockWindow("###ScriptConsole", bottom);
	if (ImGuiDockNode *n = ImGui::DockBuilderGetNode(centre)) n->SetLocalFlags(ImGuiDockNodeFlags_NoTabBar);
	ImGui::DockBuilderFinish(id);
}

void Editor::draw_level() {
	ImGuiID id = ImGui::GetID("LevelDock");
	if (reset_layout_ || !ImGui::DockBuilderGetNode(id)) {
		default_layout(id);
		reset_layout_ = false;
	}
	ImGui::SetCursorPos(ImVec2(ui::px(4), ui::px(4)));
	ImGui::DockSpace(id, ImGui::GetContentRegionAvail() - ImVec2(ui::px(4), ui::px(4)));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::px(10), ui::px(10)));
	draw_place();
	draw_viewport();
	draw_outliner();
	if (world_) world_->draw_inspector();
	draw_details();
	if (focus_content_) ImGui::SetNextWindowFocus(), focus_content_ = false;
	draw_content_browser();
	draw_output_log();
	draw_validation();
	draw_script_console();
	ImGui::PopStyleVar();
	if (opt_.screenshot && !opt_.play && ImGui::GetFrameCount() < 3) ImGui::SetWindowFocus("###ContentBrowser");
}

void Editor::draw_place() {
	if (!ImGui::Begin(window_name(ICON_PLUS, "Place", "Place").c_str())) {
		ImGui::End();
		return;
	}
	if (world_ && world_->draw_place()) {
		ImGui::End();
		return;
	}
	ui::search_box("place", "Search placables...", &place_filter_, -1);
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	static bool open_recent = true, open_gameplay = true, open_env = true;
	struct Item {
		const char *icon, *label;
		unsigned colour;
	};
	auto group = [&](const char *id, const char *icon, const char *label, bool *open, std::initializer_list<Item> items) {
		ui::TreeRow r;
		r.icon = icon, r.label = label, r.has_children = items.size() > 0, r.open = open, r.icon_colour = kTextMuted;
		ui::tree_row(id, r);
		if (!*open) return;
		for (auto &it : items) {
			if (!matches(it.label, place_filter_)) continue;
			ui::TreeRow c;
			c.icon = it.icon, c.label = it.label, c.depth = 1, c.icon_colour = it.colour;
			ui::tree_row(it.label, c);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Placing actors arrives with the arena editor (a later iteration)");
		}
	};
	group("recent", ICON_CLOCK, "Recently Used", &open_recent, {});
	group("gameplay", ICON_GAMEPAD_2, "Gameplay", &open_gameplay,
	      {{ICON_MAP_PIN, "Spawn Point", 0x4fc3f7}, {ICON_PACKAGE, "Pickup Spawner", 0x8fe3a8}, {ICON_FLAME, "Hazard", 0xff8f6b},
	       {ICON_LAYERS, "Fighting Surface", 0xb18cff}, {ICON_WIND, "Force Affector", 0x7fd1c7}, {ICON_ROUTE, "Nav Link", 0xf5d36b}});
	group("environment", ICON_SUN, "Environment", &open_env,
	      {{ICON_LIGHTBULB, "Light", 0xf5d36b}, {ICON_VIDEO, "Camera", 0xa9acbd}, {ICON_SPARKLES, "Effect", 0xff9bd2},
	       {ICON_VOLUME_2, "Sound", 0x8fb8ff}, {ICON_BOXES, "Prefab", 0xc3a6ff}});
	ImGui::End();
}

void Editor::draw_viewport() {
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	bool open = ImGui::Begin(window_name(ICON_VIDEO, "Viewport", "Viewport").c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	if (!open) {
		ImGui::End();
		return;
	}
	if (world_ && world_->draw_viewport()) {
		ImGui::End();
		return;
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p0 = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
	ImVec2 p1 = p0 + size;
	dl->AddRectFilledMultiColor(p0, p1, rgb(0x1b1c26), rgb(0x1b1c26), rgb(0x101116), rgb(0x101116));
	// A ground grid in perspective, for orientation.
	const float horizon = p0.y + size.y * 0.42f;
	const ImVec2 vanish(p0.x + size.x * 0.5f, horizon);
	dl->PushClipRect(p0, p1, true);
	for (int i = -24; i <= 24; ++i) {
		float x = vanish.x + float(i) * size.x * 0.09f;
		dl->AddLine(vanish + ImVec2(float(i) * ui::px(2), 0), ImVec2(x * 1.0f + (x - vanish.x) * 3.0f, p1.y), rgb(i == 0 ? 0x3a5aa8 : 0x262835), i == 0 ? ui::px(1.5f) : 1);
	}
	for (int k = 1; k < 18; ++k) {
		float t = 1.0f / (1.0f + float(k) * 0.35f);
		float y = horizon + (p1.y - horizon) * (1.0f - t) * 1.2f;
		if (y > p1.y) break;
		dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), rgb(k == 4 ? 0x7a3a3a : 0x262835), 1);
	}
	dl->PopClipRect();
	// Axis triad, bottom-left.
	ImVec2 o(p0.x + ui::px(34), p1.y - ui::px(34));
	dl->AddLine(o, o + ImVec2(ui::px(22), 0), rgb(0xff5a5a), ui::px(2));
	dl->AddLine(o, o + ImVec2(0, -ui::px(22)), rgb(0x5ad16b), ui::px(2));
	dl->AddLine(o, o + ImVec2(-ui::px(12), ui::px(10)), rgb(0x4f9cff), ui::px(2));
	// Overlay controls.
	ImGui::SetCursorScreenPos(p0 + ImVec2(ui::px(10), ui::px(10)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(6), 0));
	ui::overlay_button(ICON_LAYOUT_GRID);
	ImGui::SameLine();
	ui::overlay_button("Perspective");
	ImGui::SameLine();
	ui::overlay_button("Lit");
	ImGui::SameLine();
	ui::overlay_button("Show " ICON_CHEVRON_DOWN);
	float right_w = ui::px(10);
	for (const char *b : {ICON_MOVE, ICON_ROTATE_CW, ICON_SCALING, ICON_GRID_3X3 " 10", ICON_MAXIMIZE}) right_w += ImGui::CalcTextSize(b).x + ui::px(16 + 6);
	ImGui::SetCursorScreenPos(ImVec2(p1.x - right_w, p0.y + ui::px(10)));
	ui::overlay_button(ICON_MOVE, true);
	ImGui::SameLine();
	ui::overlay_button(ICON_ROTATE_CW);
	ImGui::SameLine();
	ui::overlay_button(ICON_SCALING);
	ImGui::SameLine();
	ui::overlay_button(ICON_GRID_3X3 " 10");
	ImGui::SameLine();
	ui::overlay_button(ICON_MAXIMIZE);
	ImGui::PopStyleVar();
	if (!project_) {
		draw_welcome_card(p0, size);
		ImGui::End();
		return;
	}
	ImVec2 card(std::min(size.x - ui::px(40), ui::px(440)), ui::px(150));
	ImVec2 c0 = p0 + (size - card) * 0.5f;
	dl->AddRectFilled(c0, c0 + card, rgb(0x1d1e26, 235), ui::px(8));
	dl->AddRect(c0, c0 + card, rgb(kBorder), ui::px(8));
	ImGui::SetCursorScreenPos(c0);
	ImGui::BeginChild("##empty", card, 0, ImGuiWindowFlags_NoBackground);
	ui::centered_note(ICON_MOUNTAIN, "No arena open",
	                  "Open an arena from Content > Open Arena, or double-click one in the Content Browser (Arenas). Pilots open from the Content Browser too.");
	ImGui::EndChild();
	ImGui::End();
}

void Editor::draw_outliner() {
	if (!ImGui::Begin(window_name(ICON_LIST_TREE, "Outliner", "Outliner").c_str())) {
		ImGui::End();
		return;
	}
	if (world_ && world_->draw_outliner()) {
		ImGui::End();
		return;
	}
	if (!project_) {
		ui::centered_note(ICON_LIST_TREE, "No add-on open", "The Outliner lists an add-on's objects. Open one, or make a new one, from File or the welcome card.");
		ImGui::End();
		return;
	}
	const auto &objs = project_->file().objects;
	std::string hint = "Search " + std::to_string(objs.size()) + " objects...";
	ui::search_box("outliner", hint.c_str(), &outliner_filter_, -1);
	ImGui::BeginChild("tree");
	static bool open_root = true, open_pilots = true, open_pool = false;
	static std::map<std::string, bool> open_objects;
	ui::TreeRow root;
	root.icon = ICON_FOLDER_OPEN, root.icon_colour = 0xd9b45a, root.label = project_->agent_name() + " (Add-on)";
	root.has_children = true, root.open = &open_root;
	ui::tree_row("root", root);
	if (open_root) {
		auto pilots = ed::addon_pilots(*project_);
		ui::TreeRow pr;
		pr.icon = open_pilots ? ICON_FOLDER_OPEN : ICON_FOLDER, pr.icon_colour = 0xd9b45a, pr.label = "Pilots", pr.count = int(pilots.size());
		pr.depth = 1, pr.has_children = !pilots.empty(), pr.open = &open_pilots;
		ui::tree_row("pilots", pr);
		std::function<void(uint32_t, int)> object_row = [&](uint32_t i, int depth) {
			const auto &info = objs[i].info;
			std::string name = utf8(info.name_text());
			auto kids = project_->children(i);
			bool &open = open_objects.try_emplace(ed::guid_key(info.guid), depth < 3).first->second;
			bool visible = matches(name, outliner_filter_) || matches(info.class_text(), outliner_filter_);
			if (visible) {
				ui::TreeRow r;
				r.icon = class_icon(info.class_text()), r.label = name, r.depth = depth, r.has_children = !kids.empty(), r.open = &open;
				r.selected = selected_object_ == int(i);
				r.icon_colour = info.class_text() == "Actor\\Creature\\Pilot" ? kAccentText : kTextDim;
				ImGui::PushID(int(i));
				if (ui::tree_row("obj", r)) selected_object_ = int(i), selection_ = ObjectRef::addon(info.guid);
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && info.class_text() == "Actor\\Creature\\Pilot") open_pilot(info.guid);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", info.class_text().c_str());
				ImGui::PopID();
			}
			if (open || !outliner_filter_.empty()) {
				for (uint32_t k : kids) {
					if (k < objs.size()) object_row(k, depth + 1);
				}
			}
		};
		if (open_pilots) {
			for (uint32_t pi : pilots) object_row(pi, 2);
		}
		for (const char *cat : {"HARs", "Arenas", "Tournaments"}) {
			ui::TreeRow r;
			r.icon = ICON_FOLDER, r.icon_colour = 0x8a7a4a, r.label = cat, r.count = 0, r.depth = 1, r.disabled = true;
			ui::tree_row(cat, r);
		}
		ui::TreeRow pool;
		pool.label = "GUID Pool", pool.count = int(project_->file().guid_pool.size()), pool.depth = 1;
		pool.icon = ICON_COMPONENT, pool.has_children = !project_->file().guid_pool.empty(), pool.open = &open_pool, pool.icon_colour = kTextMuted;
		ui::tree_row("pool", pool);
		if (open_pool) {
			for (size_t k = 0; k < project_->file().guid_pool.size(); ++k) {
				const ed::Guid &g = project_->file().guid_pool[k];
				const ed::BaseObject *b = base_->by_guid(g);
				ui::TreeRow r;
				r.icon = ICON_ARROW_UP_RIGHT, r.depth = 2, r.icon_colour = kTextMuted;
				r.label = b ? b->name + "  \xc2\xb7  " + base_->files()[b->file].stem : g.str();
				ImGui::PushID(int(k));
				ui::tree_row("pool", r);
				ImGui::PopID();
			}
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

void Editor::draw_details() {
	if (!ImGui::Begin(window_name(ICON_SLIDERS_HORIZONTAL, "Details", "Details").c_str())) {
		ImGui::End();
		return;
	}
	if (world_ && world_->draw_details_header()) {
	} else if (!selection_) {
		ui::centered_note(ICON_SLIDERS_HORIZONTAL, "Nothing selected", "Select an object in the Content Browser or the Outliner to see its details.");
	} else {
		draw_object_info(*this, selection_, false);
	}
	ImGui::End();
}

void Editor::draw_output_log() {
	if (!ImGui::Begin(window_name(ICON_SCROLL_TEXT, "Output Log", "OutputLog").c_str())) {
		ImGui::End();
		return;
	}
	ui::search_box("logfilter", "Filter log...", &log_filter_, ui::px(260));
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_TRASH_2, "Clear", ui::Tone::Normal, true, nullptr, ImGui::GetFrameHeight())) log_.clear();
	ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::PushFont(fonts.mono, kFontSmall);
	for (auto &l : log_) {
		if (!matches(l.text, log_filter_)) continue;
		unsigned c = l.level == LogLine::Error ? kError : l.level == LogLine::Warning ? kWarning : l.level == LogLine::Ok ? kOk
		             : l.level == LogLine::Game ? kTextDim : kTextSoft;
		ImGui::PushStyleColor(ImGuiCol_Text, vec(kTextFaint));
		ImGui::TextUnformatted(l.time.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, vec(c));
		ImGui::TextUnformatted(((l.level == LogLine::Game ? "[game] " : "") + l.text).c_str());
		ImGui::PopStyleColor();
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
	ImGui::PopFont();
	ImGui::EndChild();
	ImGui::End();
}

void Editor::draw_validation() {
	int ok = 0, warn = 0, err = 0;
	for (auto &i : issues_) {
		ok += i.severity == ed::Issue::Ok;
		warn += i.severity == ed::Issue::Warning;
		err += i.severity == ed::Issue::Error;
	}
	std::string title = std::string(ICON_CIRCLE_CHECK "  Validation (\xe2\x9c\x93 ") + std::to_string(ok) + " / \xe2\x9a\xa0 " + std::to_string(warn + err) + ")###Validation";
	if (!ImGui::Begin(title.c_str())) {
		ImGui::End();
		return;
	}
	if (ui::toolbar_button(ICON_REFRESH_CW, "Validate", ui::Tone::Normal, true, nullptr, ImGui::GetFrameHeight())) validate_now();
	ImGui::SameLine(0, ui::px(16));
	ImGui::AlignTextToFramePadding();
	ui::status_chip(ICON_CIRCLE_CHECK, (std::to_string(ok) + " passed").c_str(), kOk);
	ImGui::SameLine(0, ui::px(16));
	ui::status_chip(ICON_TRIANGLE_ALERT, (std::to_string(warn) + " warnings").c_str(), kWarning);
	ImGui::SameLine(0, ui::px(16));
	ui::status_chip(ICON_CIRCLE_X, (std::to_string(err) + " errors").c_str(), err ? kError : kTextMuted);
	ImGui::SameLine(0, ui::px(16));
	ui::dim_text("Rules only the original 2.123 game enforces are warnings for now.", kTextMuted);
	if (ImGui::BeginTable("issues", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH)) {
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ui::px(28));
		ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthFixed, ui::px(60));
		ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
		for (size_t k = 0; k < issues_.size(); ++k) {
			auto &i = issues_[k];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			unsigned c = i.severity == ed::Issue::Error ? kError : i.severity == ed::Issue::Warning ? kWarning : kOk;
			ui::status_chip(i.severity == ed::Issue::Ok ? ICON_CIRCLE_CHECK : i.severity == ed::Issue::Warning ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_X, "", c);
			ImGui::TableNextColumn();
			ui::dim_text(i.rule ? ("\xc2\xa7" "7." + std::to_string(i.rule)).c_str() : "", kTextMuted);
			ImGui::TableNextColumn();
			ImGui::PushID(int(k));
			if (ImGui::Selectable(i.text.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && i.object >= 0) {
				selected_object_ = i.object;
				if (i.line > 0) assets_open_issue(*this, i);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

void Editor::draw_script_console() {
	if (!ImGui::Begin(window_name(ICON_TERMINAL, "Script Console", "ScriptConsole").c_str())) {
		ImGui::End();
		return;
	}
	float input_h = ImGui::GetFrameHeightWithSpacing() + ui::px(4);
	ImGui::BeginChild("out", ImVec2(0, -input_h), ImGuiChildFlags_Borders);
	ImGui::PushFont(fonts.mono, kFontSmall);
	if (console_history_.empty()) ui::dim_text("Editor commands for now (type help). Script evaluation arrives with the script editor.", kTextMuted);
	for (auto &l : console_history_) {
		ImGui::PushStyleColor(ImGuiCol_Text, vec(l.rfind("> ", 0) == 0 ? kAccentText : kTextSoft));
		ImGui::TextUnformatted(l.c_str());
		ImGui::PopStyleColor();
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
	ImGui::PopFont();
	ImGui::EndChild();
	ImGui::SetNextItemWidth(-1);
	ImGui::PushFont(fonts.mono, kFontSmall);
	if (ImGui::InputTextWithHint("##cmd", "> help", &console_input_, ImGuiInputTextFlags_EnterReturnsTrue)) {
		run_command(console_input_);
		console_input_.clear();
		ImGui::SetKeyboardFocusHere(-1);
	}
	ImGui::PopFont();
	ImGui::End();
}

void Editor::run_command(const std::string &line) {
	console_history_.push_back("> " + line);
	std::istringstream in(line);
	std::string cmd;
	in >> cmd;
	auto out = [&](const std::string &s) { console_history_.push_back(s); };
	if (!project_ && cmd != "help" && !cmd.empty()) {
		out("  no add-on open: open or create one first (File menu)");
		return;
	}
	if (cmd == "help") {
		out("help | pilots | casts | validate | save | launch | new-pilot <base> <name> | open <pilot>");
	} else if (cmd == "pilots") {
		for (uint32_t pi : ed::addon_pilots(*project_)) out("  " + utf8(project_->file().objects[pi].info.name_text()));
	} else if (cmd == "casts") {
		for (auto &[n, g] : cast_choices()) out("  " + n + "  " + g.str());
	} else if (cmd == "validate") {
		validate_now();
		for (auto &i : issues_) out(std::string(i.severity == ed::Issue::Ok ? "  ok   " : i.severity == ed::Issue::Warning ? "  warn " : "  ERR  ") + i.text);
	} else if (cmd == "save") {
		save();
		out("  saved");
	} else if (cmd == "launch") {
		launch_game();
	} else if (cmd == "new-pilot") {
		std::string from, name;
		in >> from;
		std::getline(in, name);
		while (!name.empty() && name[0] == ' ') name.erase(0, 1);
		try {
			uint32_t made = ed::create_pilot(*project_, from, name.empty() ? from + " Copy" : name);
			open_pilot(project_->file().objects[made].info.guid);
			out("  created");
		} catch (const std::exception &ex) {
			out(std::string("  error: ") + ex.what());
		}
	} else if (cmd == "open") {
		std::string name;
		std::getline(in, name);
		while (!name.empty() && name[0] == ' ') name.erase(0, 1);
		for (uint32_t pi : ed::addon_pilots(*project_)) {
			if (utf8(project_->file().objects[pi].info.name_text()) == name) open_pilot(project_->file().objects[pi].info.guid);
		}
	} else if (!cmd.empty()) {
		out("  unknown command; try help");
	}
}

void Editor::draw_new_pilot_popup() {
	if (new_pilot_open_) {
		ImGui::OpenPopup("New Pilot");
		new_pilot_open_ = false;
	}
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(ui::px(560), 0), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("New Pilot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ui::dim_text("A new pilot starts as a copy of a base pilot, with fresh GUIDs, listed and unlocked.");
	ImGui::Dummy(ImVec2(0, ui::px(6)));
	GLuint tex = base_pilot_texture(new_pilot_base_);
	ImVec2 thumb(ui::px(128), ui::px(128));
	if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, thumb);
	else ImGui::Dummy(thumb);
	ImGui::SameLine(0, ui::px(16));
	ImGui::BeginGroup();
	const float lw = ui::px(90);
	ui::form_label("Copy from", lw);
	ImGui::SetNextItemWidth(ui::px(280));
	if (ui::begin_combo("##base", new_pilot_base_.c_str(), ImGuiComboFlags_HeightLarge)) {
		for (auto &n : ed::base_pilot_names(*base_)) {
			if (ImGui::Selectable(n.c_str(), n == new_pilot_base_)) new_pilot_base_ = n;
		}
		ImGui::EndCombo();
	}
	ui::form_label("Name", lw);
	ImGui::SetNextItemWidth(ui::px(280));
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	bool enter = ImGui::InputText("##name", &new_pilot_name_, ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::EndGroup();
	ImGui::Dummy(ImVec2(0, ui::px(8)));
	bool valid = !new_pilot_name_.empty();
	float bw = ui::px(110);
	ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
	if (ImGui::Button("Cancel", ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
	ImGui::SameLine();
	ImGui::BeginDisabled(!valid);
	if ((ImGui::Button("Create", ImVec2(bw, 0)) || enter) && valid) {
		try {
			uint32_t made = ed::create_pilot(*project_, new_pilot_base_, new_pilot_name_);
			log(LogLine::Ok, "New pilot " + new_pilot_name_ + " (a copy of " + new_pilot_base_ + ")");
			open_pilot(project_->file().objects[made].info.guid);
			select(ObjectRef::addon(project_->file().objects[made].info.guid));
		} catch (const std::exception &ex) {
			log(LogLine::Error, std::string("Cannot create the pilot: ") + ex.what());
		}
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::EndPopup();
}

void Editor::draw_data_prompt() {
	if (!data_prompt_ || file_dialog_.visible()) return;
	data_prompt_ = false;
	file_dialog_.open(FileDialog::PickFolder, "Choose the game's Data folder (read-only)", base_ ? base_->dir() : de::user_data_dir(), {},
	                  [this](const fs::path &p) {
		                  set_base(p);
		                  if (base_ && opt_.project.empty() == false && !project_ && fs::exists(opt_.project)) {
			                  try {
				                  set_project(ed::Project::open(*base_, opt_.project));
			                  } catch (const std::exception &ex) {
				                  log(LogLine::Error, ex.what());
			                  }
		                  }
	                  });
}

void Editor::draw_about() {
	if (show_about_) ImGui::OpenPopup("About"), show_about_ = false;
	if (!ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::PushFont(fonts.semibold, kFontUi + 4);
	ImGui::TextUnformatted("DEObjects Editor");
	ImGui::PopFont();
	ui::dim_text("openomf-battlegrounds' content editor for One Must Fall: Battlegrounds 2.123 data.");
	ui::dim_text("Dear ImGui " IMGUI_VERSION " (MIT) - Inter and JetBrains Mono (SIL OFL 1.1) - Lucide icons (ISC).");
	if (ImGui::Button("Close", ImVec2(ui::px(100), 0))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void Editor::request_quit() {
	if (project_ && project_->dirty() && !opt_.screenshot) quit_prompt_ = true;
	else quit_ = true;
}

void Editor::draw_quit_prompt() {
	if (quit_prompt_) ImGui::OpenPopup("Unsaved Changes"), quit_prompt_ = false;
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::TextUnformatted(("Save the changes to " + project_->path().filename().string() + " before quitting?").c_str());
	ImGui::Dummy(ImVec2(0, ui::px(6)));
	if (ImGui::Button("Save and Quit", ImVec2(ui::px(130), 0))) {
		save();
		quit_ = !project_->dirty();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Quit Without Saving", ImVec2(ui::px(160), 0))) quit_ = true, ImGui::CloseCurrentPopup();
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(ui::px(100), 0))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}
