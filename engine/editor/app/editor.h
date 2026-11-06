// The editor's shell: menus, document tabs, toolbar, the level editor's dockable panels, the
// status bar, and the documents (asset editors) it opens.
#pragma once

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "addon.h"
#include "file_dialog.h"
#include "gl_util.h"
#include "imgui.h"
#include "launch.h"
#include "pilot.h"
#include "validate.h"

struct SDL_Window;

struct Options {
	std::filesystem::path data, project;
	std::string new_pilot;    // "Base:Name": create that pilot at start
	std::string open_pilot;   // open this add-on pilot's editor at start
	int pilot_tab = 0;
	std::string preview;      // "photo" or "robot"
	std::filesystem::path photo;  // import into the opened pilot
	std::string content;      // Content Browser folder: Pilots, HARs, Arenas, Tournaments
	bool base_content = false;  // show the base game's folder
	bool screenshot = false;
	bool level = false;       // start on the level editor tab
	std::string select;       // Outliner selection by object name
};

class Editor;

class Document {
public:
	virtual ~Document() = default;
	virtual std::string title(Editor &e) const = 0;
	virtual const char *icon() const = 0;
	virtual bool closable() const { return true; }
	virtual void draw(Editor &e) = 0;
	// False once its object is gone (an undo removed it).
	virtual bool alive(Editor &) const { return true; }
};

struct LogLine {
	enum Level { Info, Ok, Warning, Error, Game } level = Info;
	std::string time, text;
};

class Editor {
public:
	Editor(Options options, SDL_Window *window);
	~Editor();
	void frame();
	bool quit_requested() const { return quit_; }
	// Asks first when the add-on has unsaved changes.
	void request_quit();

	void log(LogLine::Level level, const std::string &text);
	void status(const std::string &text) { status_ = text; }
	void validate_now();
	void save();
	void launch_game();
	void open_pilot(const ed::Guid &pilot);
	void new_pilot_dialog(const std::string &base = "");
	void import_photo_dialog(const ed::Guid &pilot);
	void import_photo(const ed::Guid &pilot, const std::filesystem::path &file);
	int find_object(const ed::Guid &g) const { return project_ ? project_->find(g) : -1; }

	ed::BaseData *base() { return base_.get(); }
	ed::Project *project() { return project_.get(); }
	FileDialog &file_dialog() { return file_dialog_; }
	const std::vector<ed::Issue> &issues() const { return issues_; }
	GLuint base_pilot_texture(const std::string &name);
	const std::vector<std::string> &fighter_names();
	std::vector<std::pair<std::string, ed::Guid>> cast_choices();
	float time() const { return float(ImGui::GetTime()); }
	const Options &options() const { return opt_; }
	void draw_level();

private:
	void load_settings();
	void save_settings();
	void set_base(const std::filesystem::path &dir);
	void set_project(std::unique_ptr<ed::Project> p);
	void new_project_dialog();
	void open_project_dialog();
	void apply_startup();
	void track_undo();
	void undo();
	void redo();
	void update_title();

	void draw_menu();
	void draw_doc_tabs();
	void draw_toolbar();
	void draw_status();
	void draw_start_page();
	void default_layout(ImGuiID dockspace);
	void draw_place();
	void draw_viewport();
	void draw_outliner();
	void draw_details();
	void draw_content_browser();
	void draw_output_log();
	void draw_validation();
	void draw_script_console();
	void draw_new_pilot_popup();
	void draw_data_prompt();
	void draw_about();
	void draw_quit_prompt();
	void run_command(const std::string &line);
	void drain_game_output();

	Options opt_;
	SDL_Window *window_;
	std::unique_ptr<ed::BaseData> base_;
	std::unique_ptr<ed::Project> project_;
	std::vector<std::unique_ptr<Document>> docs_;
	size_t active_ = 0;
	FileDialog file_dialog_;
	std::vector<ed::Issue> issues_;
	uint64_t validated_revision_ = 0;
	std::vector<LogLine> log_;
	std::string status_ = "Ready";
	bool quit_ = false, quit_prompt_ = false;
	bool reset_layout_ = false;
	bool show_about_ = false;
	bool data_prompt_ = false;
	std::filesystem::path settings_dir_;
	std::vector<std::filesystem::path> recent_;
	std::string layout_ini_;

	// Undo: whole-file snapshots of the add-on, taken when an edit settles.
	std::deque<deo::File> undo_, redo_;
	deo::File settled_;
	uint64_t settled_revision_ = 0;

	// New pilot popup.
	bool new_pilot_open_ = false;
	std::string new_pilot_base_ = "Lucas", new_pilot_name_;

	// Selection.
	int selected_object_ = -1;
	std::string content_folder_ = "Pilots";
	bool content_base_ = false;
	std::string content_filter_, content_type_ = "All", outliner_filter_, place_filter_, log_filter_;
	ed::Guid selected_tile_;
	std::string selected_base_tile_;
	std::string console_input_;
	std::vector<std::string> console_history_;
	bool focus_content_ = false;
	std::string play_mode_ = "Free Camera";

	// Thumbnails.
	std::map<std::string, std::pair<GLuint, uint64_t>> addon_thumbs_;
	std::map<std::string, GLuint> base_thumbs_;
	std::vector<std::string> fighters_;

	// Launched game.
	std::unique_ptr<GameProcess> game_;
	std::shared_ptr<std::mutex> game_mutex_ = std::make_shared<std::mutex>();
	std::shared_ptr<std::vector<std::string>> game_lines_ = std::make_shared<std::vector<std::string>>();

	friend class PilotDocument;
};

GLuint addon_photo_texture(Editor &e, const ed::Guid &pilot, std::map<std::string, std::pair<GLuint, uint64_t>> &cache);
std::unique_ptr<Document> make_pilot_document(Editor &e, const ed::Guid &pilot);
bool pilot_document_is(const Document *d, const ed::Guid &pilot);
