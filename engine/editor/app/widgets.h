// The editor's custom-drawn controls (toolbar buttons, tree rows, tiles, tab strips, form rows).
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

namespace ui {

float px(float v);  // unscaled pixels -> current UI pixels

enum class Tone { Normal, Play, Ghost };
bool toolbar_button(const char *icon, const char *label, Tone tone = Tone::Normal, bool enabled = true,
                    const char *tooltip = nullptr, float height = 0);
bool icon_button(const char *id, const char *icon, const char *tooltip = nullptr, bool enabled = true, float side = 0);
// A small dark overlay button for viewports.
bool overlay_button(const char *label, bool active = false);

// A tree row with an icon, a label and an optional count. `open` toggles with the arrow.
struct TreeRow {
	const char *icon = nullptr;
	unsigned icon_colour = 0;
	std::string label;
	int count = -1;
	int depth = 0;
	bool has_children = false;
	bool *open = nullptr;
	bool selected = false;
	bool disabled = false;
	const char *badge = nullptr;
};
bool tree_row(const char *id, const TreeRow &row);

// A content browser tile: a thumbnail (or an icon), the name below, a NEW badge.
struct Tile {
	ImTextureID texture = 0;
	const char *icon = nullptr;
	unsigned tint = 0;
	std::string label;
	std::string sublabel;
	bool selected = false;
	bool is_new = false;
	bool disabled = false;
	bool add = false;  // the "+ New ..." tile
};
// Returns true on click; *double_clicked set on a double click.
bool tile(const char *id, const Tile &t, ImVec2 size, bool *double_clicked = nullptr);

// Form tabs with a violet underline across the strip.
bool tab_strip(const char *id, const std::vector<std::string> &labels, int *active);
// A document tab; returns true when clicked, *close when its x is clicked.
bool doc_tab(const char *id, const char *icon, const std::string &label, bool active, bool closable, bool *close,
             bool dirty = false);

// Label column for two-column forms; call before the field.
void form_label(const char *label, float label_width, float field_height = 0);
// BeginCombo with a chevron instead of the arrow button.
bool begin_combo(const char *id, const char *preview, int flags = 0);
bool combo(const char *id, int *current, const char *const items[], int count);
bool checkbox(const char *id, bool *v, bool enabled = true);
bool search_box(const char *id, const char *hint, std::string *text, float width);
// A filled panel with rounded corners; pairs with end_panel.
void begin_panel(const char *id, ImVec2 size, unsigned bg, bool border = true, float rounding = 8, ImVec2 padding = ImVec2(0, 0));
void end_panel();
void status_chip(const char *icon, const char *text, unsigned colour);
void dim_text(const char *text, unsigned colour = 0);
// Centered multi-line note (empty states).
void centered_note(const char *icon, const char *title, const char *body);

}  // namespace ui
