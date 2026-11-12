// The editor's shell: menus, document tabs, toolbar, the level editor's dockable panels, the
// status bar, and the documents (asset editors) it opens.
#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "addon.h"
#include "content.h"
#include "file_dialog.h"
#include "gl_util.h"
#include "imgui.h"
#include "launch.h"
#include "pilot.h"
#include "validate.h"

struct SDL_Window;
class WorldEditor;

struct Options {
	std::filesystem::path data, project;
	std::string new_pilot;    // "Base:Name": create that pilot at start
	std::string open_pilot;   // open this add-on pilot's editor at start
	int pilot_tab = 0;
	std::string preview;      // "photo" or "robot"
	std::filesystem::path photo;  // import into the opened pilot
	std::string content;      // Content Browser type (Pilots, Images, ...)
	bool base_content = false;  // show the base game's folder
	std::string browse_file;  // Content Browser by file: this file's objects
	std::string open_base;    // "Class:Name": open that base object at start
	std::string select_base;  // "Class:Name": select it (Details) at start
	std::string copy_base;    // copy this base pilot into the add-on at start
	bool screenshot = false;
	bool level = false;
	bool save = false;
	bool launch = false;      // Launch Game once the start-up actions ran
	std::string select;       // Outliner selection by object name
	// The world side (world.h): open an arena, make a variant, set up the viewports, play.
	std::string arena, select_node, camera, debug, move_to, light_colour;
	bool variant = false, play = false, simulate = false, eject = false, inspector = false, replay = false;
	int play_ticks = 0, layout = 0, shading = -1;
	// The asset side (assets.h): tournaments and HAR variants.
	std::string new_tournament, open_tournament, new_har, open_har;
	std::vector<std::string> har_constants;  // NAME=VALUE: set in the HAR being made or opened
	std::vector<std::string> har_paints;     // Job[:Image=file.png]: a paint job made or its image set
	int asset_tab = -1, asset_select = -100, paint_job = -1;
	bool compile = false;
	// 3D models (models.h): import a .glb / .gltf (straight in, or the dialog at a step), export an
	// object, place an imported prop in the open arena.
	std::filesystem::path import_file, export_out;
	std::string import_kind, import_name, import_template, import_base, import_pose, export_spec, place_prop;
	std::vector<std::string> import_paints;  // Name=image.png: a robot import's further paint jobs
	bool import_dialog = false;
	int import_step = 0;
};

class Editor;

// An object the editor shows: one of the add-on's (by GUID, as indices move on undo) or a base
// file's (read-only).
struct ObjectRef {
	enum Kind { None, Addon, Base } kind = None;
	size_t file = 0;
	uint32_t index = 0;
	ed::Guid guid;
	static ObjectRef base(const ed::BaseObject &o) { return {Base, o.file, o.index, o.guid}; }
	static ObjectRef addon(const ed::Guid &g) { return {Addon, 0, 0, g}; }
	explicit operator bool() const { return kind != None; }
	bool operator==(const ObjectRef &o) const { return kind == o.kind && guid == o.guid && (kind != Base || (file == o.file && index == o.index)); }
};

class Document {
public:
	virtual ~Document() = default;
	virtual std::string title(Editor &e) const = 0;
	virtual const char *icon() const = 0;
	virtual bool closable() const { return true; }
	virtual void draw(Editor &e) = 0;
	// False once its object is gone (an undo removed it).
	virtual bool alive(Editor &) const { return true; }
	// Shows the open add-on's content (closed when another add-on opens).
	virtual bool addon_content() const { return false; }
	virtual bool shows(const ObjectRef &) const { return false; }
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
	void launch_game(const std::vector<std::string> &extra_args = {});
	// Opens a document (or shows the open one that shows the same object).
	void add_document(std::unique_ptr<Document> doc, const ObjectRef &shows);
	Document *active_document() { return active_ < docs_.size() ? docs_[active_].get() : nullptr; }
	void open_pilot(const ed::Guid &pilot);
	void new_pilot_dialog(const std::string &base = "");
	// Runs `then` on the open add-on, or first asks which add-on to use (recent, open, new).
	void with_addon(const std::string &why, std::function<void()> then);
	void copy_pilot_to_addon(const ed::BaseObject &pilot);
	void select(const ObjectRef &r);
	const ObjectRef &selection() const { return selection_; }
	// Double-click: the pilot editor (read-only for base pilots) or the object viewer.
	void open_object(const ObjectRef &r);
	// The file an object lives in; null while a base file is still being read.
	const deo::File *file_of(const ObjectRef &r);
	int addon_index(const ObjectRef &r) const;
	ObjectRef ref_for_guid(const ed::Guid &g) const;
	std::string object_label(const ObjectRef &r);
	ed::ReferenceIndex *reference_index();
	// A thumbnail for a tile (generated lazily, a few per frame); 0 for none yet.
	GLuint thumbnail(const ObjectRef &r, float *aspect);
	// The image that stands for an object (itself, a pilot's photo, a description's icon or its
	// pilot's photo); *pending while a file it needs is still being read.
	ObjectRef image_source(const ObjectRef &r, bool *pending = nullptr);
	bool sync_loads() const { return opt_.screenshot; }
	void play_wave(const ed::Bytes &wav);
	void stop_sound();
	bool sound_playing();
	void import_photo_dialog(const ed::Guid &pilot);
	void import_photo(const ed::Guid &pilot, const std::filesystem::path &file);
	// A file dropped on the window.
	void drop_file(const std::filesystem::path &file);
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
	WorldEditor *world() { return world_.get(); }

private:
	void load_settings();
	void save_settings();
	void set_base(const std::filesystem::path &dir);
	void set_project(std::unique_ptr<ed::Project> p);
	void open_project(const std::filesystem::path &p, std::function<void()> then = {});
	void close_project();
	void new_project_dialog(std::function<void()> then = {});
	void open_project_dialog(std::function<void()> then = {});
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
	void draw_welcome_card(ImVec2 p0, ImVec2 size);
	void draw_addon_chooser();
	void draw_browser_tree();
	void draw_browser_items();
	void rebuild_items();
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
	ObjectRef selection_;
	std::string outliner_filter_, place_filter_, log_filter_;

	// Content Browser: where (add-on or base; a type, or a file and folder), what (type filter,
	// search) and how (tiles or list).
	struct Scope {
		bool base = true;
		bool by_file = false;
		int category = -1;  // -1: every type
		int file = -1;
		int folder = -1;    // an object of `file` whose children are shown; -1: the file's roots
		bool operator==(const Scope &) const = default;
	} scope_;
	int type_filter_ = -1;
	std::string content_filter_;
	bool list_view_ = false;
	std::vector<ObjectRef> items_;
	std::string items_key_;
	bool items_waiting_ = false;
	std::map<size_t, std::vector<int>> parents_;  // base file -> object parents
	const std::vector<int> *parents_of(size_t file);
	std::map<uint64_t, bool> file_open_;

	// Add-on chooser.
	bool chooser_open_ = false;
	std::string chooser_why_;
	std::function<void()> chooser_then_;
	std::unique_ptr<ed::ReferenceIndex> refs_;
	std::string console_input_;
	std::vector<std::string> console_history_;
	bool focus_content_ = false;
	std::string play_mode_ = "Free Camera";

	// Thumbnails.
	std::map<std::string, std::pair<GLuint, uint64_t>> addon_thumbs_;
	std::map<std::string, GLuint> base_thumbs_;
	struct Thumb {
		GLuint texture = 0;
		float aspect = 1;
		bool done = false;
	};
	std::map<uint64_t, Thumb> thumbs_;
	int thumb_budget_ = 0;
	unsigned audio_device_ = 0;
	std::vector<std::string> fighters_;

	// Launched game.
	std::unique_ptr<GameProcess> game_;
	std::unique_ptr<WorldEditor> world_;
	std::shared_ptr<std::mutex> game_mutex_ = std::make_shared<std::mutex>();
	std::shared_ptr<std::vector<std::string>> game_lines_ = std::make_shared<std::vector<std::string>>();

	friend class PilotDocument;
};

GLuint addon_photo_texture(Editor &e, const ed::Guid &pilot, std::map<std::string, std::pair<GLuint, uint64_t>> &cache);
std::unique_ptr<Document> make_pilot_document(Editor &e, const ed::Guid &pilot);
// A base pilot in the pilot editor, read-only, with "Copy to add-on".
std::unique_ptr<Document> make_base_pilot_document(Editor &e, const ed::BaseObject &pilot);
bool pilot_document_is(const Document *d, const ed::Guid &pilot);
std::unique_ptr<Document> make_object_document(Editor &e, const ObjectRef &r);
// The Details panel's body for any object: identity, decoded fields, references, chunks.
void draw_object_info(Editor &e, const ObjectRef &r, bool in_document);
const char *class_icon(const std::string &class_path);
unsigned class_tint(const std::string &class_path);
std::string utf8_text(const std::string &latin1);
