#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

#include "editor.h"
#include "icons.h"
#include "imgui_stdlib.h"
#include "theme.h"
#include "widgets.h"
#include "world.h"
#include "world_state.h"

using namespace theme;

namespace {

std::string utf8(const std::string &latin1) { return deo::latin1_to_utf8(deo::Bytes(latin1.begin(), latin1.end())); }

bool matches(const std::string &text, const std::string &filter) {
	if (filter.empty()) return true;
	std::string a = text, b = filter;
	for (auto &c : a) c = char(tolower(uint8_t(c)));
	for (auto &c : b) c = char(tolower(uint8_t(c)));
	return a.find(b) != std::string::npos;
}

const char *icon_of(ed::NodeKind k) {
	switch (k) {
	case ed::NodeKind::Environment: return ICON_MOUNTAIN;
	case ed::NodeKind::SpawnPoint: return ICON_MAP_PIN;
	case ed::NodeKind::PickupSpawner: return ICON_PACKAGE;
	case ed::NodeKind::Hazard: return ICON_FLAME;
	case ed::NodeKind::Surface: return ICON_LAYERS;
	case ed::NodeKind::ForceAffector: return ICON_WIND;
	case ed::NodeKind::DeathVolume: return ICON_TRIANGLE_ALERT;
	case ed::NodeKind::NavArea: return ICON_NAVIGATION;
	case ed::NodeKind::NavLink: return ICON_ROUTE;
	case ed::NodeKind::Light: return ICON_LIGHTBULB;
	case ed::NodeKind::Camera: return ICON_VIDEO;
	case ed::NodeKind::Effect: return ICON_SPARKLES;
	case ed::NodeKind::Sound: return ICON_VOLUME_2;
	case ed::NodeKind::Geometry: return ICON_BOX;
	case ed::NodeKind::Group: return ICON_FOLDER;
	case ed::NodeKind::Affector: return ICON_SCAN;
	case ed::NodeKind::Property: return ICON_COMPONENT;
	case ed::NodeKind::Value: return ICON_HASH;
	default: return ICON_FILE;
	}
}

unsigned tint_of(ed::NodeKind k) {
	switch (k) {
	case ed::NodeKind::SpawnPoint: return 0x4fc3f7;
	case ed::NodeKind::PickupSpawner: return 0x8fe3a8;
	case ed::NodeKind::Hazard: return 0xff8f6b;
	case ed::NodeKind::Surface: return 0xb18cff;
	case ed::NodeKind::ForceAffector: return 0x7fd1c7;
	case ed::NodeKind::DeathVolume: return 0xff5a5a;
	case ed::NodeKind::NavArea:
	case ed::NodeKind::NavLink: return 0xf5d36b;
	case ed::NodeKind::Light: return 0xf5d36b;
	case ed::NodeKind::Environment: return 0x8b7cf6;
	case ed::NodeKind::Group: return 0xd9b45a;
	default: return 0xa9acbd;
	}
}

float yaw_of(const de::Transform &t) { return std::atan2(t.r[6], t.r[8]) * 57.29578f; }

de::Transform with_yaw(const de::Transform &t, float degrees) {
	de::Transform o = t;
	float a = degrees / 57.29578f, c = std::cos(a), s = std::sin(a);
	o.r = {c, 0, -s, 0, 1, 0, s, 0, c};
	return o;
}

void row_label(const char *label) {
	ui::form_label(label, ui::px(110));
	ImGui::AlignTextToFramePadding();
}

}  // namespace

bool WorldEditor::draw_outliner() {
	if (!arena_) return false;
	ArenaState &s = *arena_;
	if (!s.built) {
		ui::centered_note(ICON_MOUNTAIN, ("Opening " + s.entry.name).c_str(), "The arena's objects appear here once its files are read.");
		return true;
	}
	std::string hint = "Search " + std::to_string(s.scene.nodes.size()) + " objects...";
	ui::search_box("arena_outliner", hint.c_str(), &s.outliner_filter, -1);
	ImGui::BeginChild("arena_tree");
	static std::map<std::string, bool> open;
	auto folder = [&](const char *id, const char *icon, const std::string &label, int count, int depth, bool def, unsigned colour = 0xd9b45a) {
		bool &o = open.try_emplace(id, def).first->second;
		ui::TreeRow r;
		r.icon = o ? ICON_FOLDER_OPEN : (icon ? icon : ICON_FOLDER);
		if (icon && strcmp(icon, ICON_FOLDER)) r.icon = icon;
		r.icon_colour = colour, r.label = label, r.count = count, r.depth = depth, r.has_children = count != 0, r.open = &o;
		ui::tree_row(id, r);
		return o || !s.outliner_filter.empty();
	};
	auto node_row = [&](int k, int depth, bool children) {
		const auto &n = s.scene.nodes[size_t(k)];
		std::string name = utf8(n.name);
		if (!matches(name, s.outliner_filter) && !matches(n.class_path, s.outliner_filter)) return false;
		bool &o = open.try_emplace("n" + std::to_string(k), false).first->second;
		ui::TreeRow r;
		r.icon = icon_of(n.kind), r.icon_colour = tint_of(n.kind), r.label = name, r.depth = depth;
		r.has_children = children && !n.children.empty(), r.open = &o, r.selected = k == s.selected;
		if (n.kind == ed::NodeKind::NavArea) r.count = int(n.children.size());
		ImGui::PushID(k);
		if (ui::tree_row("node", r)) select_node(k);
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frame_selection(active_pane_);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s  (%s)", n.class_path.c_str(), ed::node_kind_name(n.kind));
		ImGui::PopID();
		return o;
	};
	std::function<void(int, int)> tree = [&](int k, int depth) {
		bool o = node_row(k, depth, true);
		if (o || !s.outliner_filter.empty()) {
			for (int c : s.scene.nodes[size_t(k)].children) tree(c, depth + 1);
		}
	};
	ui::TreeRow root;
	static bool open_root = true;
	root.icon = ICON_MOUNTAIN, root.icon_colour = kAccentText, root.label = s.entry.name + (s.entry.addon ? " (Variant)" : " (Arena)");
	root.has_children = true, root.open = &open_root, root.badge = s.entry.addon ? "NEW" : "BASE";
	root.selected = s.selected == s.scene.root;
	if (ui::tree_row("arena_root", root)) select_node(s.scene.root);
	if (open_root) {
		struct Group {
			const char *id, *icon, *label;
			std::vector<ed::NodeKind> kinds;
		};
		const Group gameplay[] = {{"g_spawn", ICON_MAP_PIN, "Spawn Points", {ed::NodeKind::SpawnPoint}},
		                          {"g_pick", ICON_PACKAGE, "Pickup Spawners", {ed::NodeKind::PickupSpawner}},
		                          {"g_haz", ICON_FLAME, "Hazards", {ed::NodeKind::Hazard}},
		                          {"g_surf", ICON_LAYERS, "Surfaces", {ed::NodeKind::Surface}},
		                          {"g_force", ICON_WIND, "Force Affectors", {ed::NodeKind::ForceAffector}},
		                          {"g_death", ICON_TRIANGLE_ALERT, "Death Volumes", {ed::NodeKind::DeathVolume}},
		                          {"g_nav", ICON_ROUTE, "Nav Links", {ed::NodeKind::NavArea}}};
		int total = 0;
		for (auto &g : gameplay) {
			for (auto k : g.kinds) total += int(s.scene.of_kind(k).size());
		}
		if (folder("gameplay", ICON_GAMEPAD_2, "Gameplay", total, 1, true, kTextMuted)) {
			for (auto &g : gameplay) {
				std::vector<int> items;
				for (auto k : g.kinds) {
					auto v = s.scene.of_kind(k);
					items.insert(items.end(), v.begin(), v.end());
				}
				std::string label = g.label;
				if (!strcmp(g.id, "g_nav")) label += "  \xc2\xb7  " + std::to_string(s.scene.of_kind(ed::NodeKind::NavLink).size()) + " links";
				if (folder(g.id, ICON_FOLDER, label, int(items.size()), 2, !strcmp(g.id, "g_spawn"))) {
					for (int k : items) {
						bool o = node_row(k, 3, !strcmp(g.id, "g_nav"));
						if (o && !strcmp(g.id, "g_nav")) {
							for (int c : s.scene.nodes[size_t(k)].children) node_row(c, 4, false);
						}
					}
				}
			}
		}
		std::vector<int> lights;
		for (int k : s.scene.of_kind(ed::NodeKind::Light)) {
			if (s.scene.nodes[size_t(k)].class_path == "Light") lights.push_back(k);
		}
		auto cams = s.scene.of_kind(ed::NodeKind::Camera);
		int env_count = int(s.scene.nodes[size_t(s.scene.root)].children.size());
		if (folder("environment", ICON_SUN, "Environment", env_count, 1, false, kTextMuted)) {
			if (folder("e_lights", ICON_FOLDER, "Lights", int(lights.size()), 2, true)) {
				for (int k : lights) node_row(k, 3, false);
			}
			if (folder("e_cams", ICON_FOLDER, "Cameras", int(cams.size()), 2, false)) {
				for (int k : cams) node_row(k, 3, false);
			}
			if (folder("e_all", ICON_FOLDER, "All Objects", int(s.scene.nodes.size()), 2, false)) {
				for (int c : s.scene.nodes[size_t(s.scene.root)].children) tree(c, 3);
			}
		}
	}
	ImGui::EndChild();
	return true;
}

bool WorldEditor::draw_details_header() {
	if (!arena_ || !arena_->built) return false;
	ArenaState &s = *arena_;
	ed::Project *p = e_.project();
	const bool editable = s.entry.addon && p;
	int sel = s.selected;
	if (sel < 0 && e_.selection()) return false;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(8), ui::px(6)));
	auto arena_section = [&] {
		ImGui::SeparatorText(editable ? "Arena (variant)" : "Arena (base, read-only)");
		ed::ArenaDescriptionData d = s.description;
		bool changed = false;
		ImGui::BeginDisabled(!editable);
		row_label("Name");
		ImGui::SetNextItemWidth(-1);
		changed |= ImGui::InputText("##aname", &d.name);
		row_label("Players");
		ImGui::SetNextItemWidth(ui::px(70));
		changed |= ImGui::InputInt("##minp", &d.min_players, 0);
		ImGui::SameLine();
		ui::dim_text("to", kTextMuted);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(70));
		changed |= ImGui::InputInt("##maxp", &d.max_players, 0);
		row_label("Listed");
		changed |= ui::checkbox("##listed", &d.listed, editable);
		ImGui::SameLine(0, ui::px(20));
		ui::dim_text("Locked", kLabel);
		ImGui::SameLine();
		changed |= ui::checkbox("##locked", &d.locked, editable);
		ImGui::EndDisabled();
		row_label("Casts");
		std::string casts;
		for (auto &g : d.casts) casts += (casts.empty() ? "" : ", ") + (p ? ed::object_name(*p, g) : g.str());
		ImGui::PushTextWrapPos(0);
		ui::dim_text(casts.empty() ? (s.entry.addon ? "-" : "(as the base description)") : casts.c_str(), kTextDim);
		ImGui::PopTextWrapPos();
		if (changed && editable) {
			int di = p->find(s.entry.description);
			d.min_players = std::clamp(d.min_players, 1, 10), d.max_players = std::clamp(d.max_players, d.min_players, 10);
			if (di >= 0 && !(d == s.description)) {
				ed::write_arena_description(*p, uint32_t(di), d);
				s.description = d;
				s.entry.name = d.name;
			}
		}
		ImGui::SeparatorText("Checks");
		for (auto &i : s.issues) {
			unsigned c = i.severity == ed::Issue::Error ? kError : i.severity == ed::Issue::Warning ? kWarning : kOk;
			const char *icon = i.severity == ed::Issue::Error ? ICON_CIRCLE_X : i.severity == ed::Issue::Warning ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_CHECK;
			ImGui::PushStyleColor(ImGuiCol_Text, vec(c));
			ImGui::TextUnformatted(icon);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::PushTextWrapPos(0);
			ImGui::TextUnformatted(i.text.c_str());
			ImGui::PopTextWrapPos();
		}
	};
	if (sel < 0 || sel == s.scene.root) {
		arena_section();
		ImGui::PopStyleVar();
		return sel < 0;
	}
	const ed::SceneNode &n = s.scene.nodes[size_t(sel)];
	ImGui::PushFont(fonts.semibold, kFontUi + 2);
	ImGui::PushStyleColor(ImGuiCol_Text, vec(tint_of(n.kind)));
	ImGui::TextUnformatted(icon_of(n.kind));
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::TextUnformatted(utf8(n.name).c_str());
	ImGui::PopFont();
	ImGui::SameLine();
	ui::badge(ed::node_kind_name(n.kind), 0x2c2d38, tint_of(n.kind));
	int target = sel;
	if (n.class_path == "Light" && n.parent >= 0) target = n.parent;
	const ed::SceneNode &t = s.scene.nodes[size_t(target)];
	if (t.actor) {
		ImGui::SeparatorText("Transform");
		const bool movable = editable && t.kind != ed::NodeKind::Environment;
		de::Transform w = t.world;
		float pos[3] = {w.t[0], w.t[1], w.t[2]};
		float yaw = yaw_of(w);
		ImGui::BeginDisabled(!movable);
		row_label("Location");
		ImGui::SetNextItemWidth(-1);
		bool moved = ImGui::DragFloat3("##pos", pos, 1.0f, -100000, 100000, "%.1f");
		row_label("Yaw");
		ImGui::SetNextItemWidth(ui::px(120));
		bool turned = ImGui::DragFloat("##yaw", &yaw, 0.5f, -360, 360, "%.1f\xc2\xb0");
		ImGui::EndDisabled();
		if ((moved || turned) && movable) {
			de::Transform nw = turned ? with_yaw(w, yaw) : w;
			nw.t = {pos[0], pos[1], pos[2]};
			de::Transform parent = t.parent >= 0 ? s.scene.nodes[size_t(t.parent)].world : de::Transform{};
			int obj = p->find(t.guid);
			if (obj >= 0) ed::set_actor_local(*p, uint32_t(obj), parent.inverse().then(nw));
		}
		if (!movable && s.entry.addon) ui::dim_text("The environment itself stays where the base arena has it.", kTextMuted);
	}
	if (n.light) {
		ImGui::SeparatorText("Light");
		ed::LightData ld;
		int obj = editable ? p->find(n.guid) : -1;
		bool embedded = obj >= 0 && ed::read_light_data(p->file().objects[size_t(obj)], &ld);
		if (!embedded) {
			auto c = n.light->colour.eval(0);
			ld.rgba = c, ld.intensity = n.light->intensity;
		}
		ImGui::BeginDisabled(!embedded);
		row_label("Colour");
		float col[4] = {ld.rgba[0], ld.rgba[1], ld.rgba[2], ld.rgba[3]};
		bool changed = ImGui::ColorEdit4("##lcol", col, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
		row_label("Intensity");
		ImGui::SetNextItemWidth(ui::px(120));
		changed |= ImGui::DragFloat("##lint", &ld.intensity, 0.01f, 0, 16, "%.2f");
		ImGui::EndDisabled();
		if (changed && embedded) {
			ld.rgba = {col[0], col[1], col[2], col[3]};
			ed::write_light_data(*p, uint32_t(obj), ld);
		}
		if (!editable) ui::dim_text("Copy the arena to an add-on to change its lights.", kTextMuted);
		else if (!embedded) ui::dim_text("This light's colour comes from a value object.", kTextMuted);
		if (t.shape == ed::SceneNode::Shape::Cone) {
			row_label("Cone");
			ImGui::Text("%.0f to %.0f, %.0f\xc2\xb0 / %.0f\xc2\xb0", t.far_begin, t.far_end, t.umbra, t.penumbra);
		} else if (t.shape == ed::SceneNode::Shape::Sphere) {
			row_label("Sphere");
			ImGui::Text("full to %.0f, fades to %.0f", t.hotspot, t.falloff);
		}
	}
	if (n.shape != ed::SceneNode::Shape::None && !n.light) {
		ImGui::SeparatorText("Affector");
		static const char *const shapes[] = {"none", "sphere", "cone", "mesh", "box"};
		row_label("Shape");
		ImGui::TextUnformatted(shapes[std::min<size_t>(size_t(n.shape), 4)]);
		if (n.shape == ed::SceneNode::Shape::Sphere) {
			row_label("Radius");
			ImGui::Text("%.0f (hotspot %.0f)", n.falloff, n.hotspot);
		} else if (n.shape == ed::SceneNode::Shape::Box) {
			row_label("Box");
			ImGui::Text("%.0f %.0f %.0f .. %.0f %.0f %.0f", n.box_min[0], n.box_min[1], n.box_min[2], n.box_max[0], n.box_max[1], n.box_max[2]);
		} else if (n.shape == ed::SceneNode::Shape::Mesh) {
			row_label("Faces");
			ImGui::Text("%zu", n.triangles.size() / 3);
		}
		row_label("Groups");
		ImGui::PushFont(fonts.mono);
		ImGui::Text("0x%08x", n.groups);
		ImGui::PopFont();
	}
	if (n.kind == ed::NodeKind::NavArea || n.kind == ed::NodeKind::NavLink) {
		ImGui::SeparatorText("Navigation");
		auto link_row = [&](int l) {
			const auto &ln = s.scene.nodes[size_t(l)];
			std::string to = ln.nav_to >= 0 ? utf8(s.scene.nodes[size_t(ln.nav_to)].name) : std::string("(nowhere)");
			ImGui::PushID(l);
			ImGui::PushStyleColor(ImGuiCol_Text, vec(kAccentText));
			if (ImGui::Selectable((std::string(ICON_ROUTE "  ") + to).c_str(), false) && ln.nav_to >= 0) select_node(ln.nav_to);
			ImGui::PopStyleColor();
			ImGui::PopID();
		};
		if (n.kind == ed::NodeKind::NavLink) link_row(sel);
		else {
			for (int c : n.children) {
				if (s.scene.nodes[size_t(c)].kind == ed::NodeKind::NavLink) link_row(c);
			}
		}
	}
	if (!n.target.empty()) {
		ImGui::SeparatorText("Passive reference");
		row_label("Makes");
		ImGui::TextUnformatted(utf8(n.target).c_str());
	}
	if (s.entry.addon && (n.kind == ed::NodeKind::SpawnPoint || n.kind == ed::NodeKind::PickupSpawner)) {
		ImGui::Dummy(ImVec2(0, ui::px(2)));
		if (ui::toolbar_button(ICON_COPY, "Duplicate")) place(n.kind == ed::NodeKind::SpawnPoint ? "Spawn Point" : "Pickup Spawner");
		ImGui::SameLine();
		if (ui::toolbar_button(ICON_TRASH_2, "Remove")) delete_selected();
	}
	ImGui::Dummy(ImVec2(0, ui::px(6)));
	ImGui::PopStyleVar();
	return false;
}

std::vector<ed::Issue> WorldEditor::arena_issues() {
	std::vector<ed::Issue> out;
	if (arena_ && arena_->built && arena_->entry.addon) {
		for (auto i : arena_->issues) {
			i.text = arena_->entry.name + ": " + i.text;
			out.push_back(i);
		}
	}
	return out;
}

void WorldEditor::draw_arena_menu() {
	if (!e_.base()) return;
	if (ImGui::BeginMenu(ICON_MOUNTAIN "  Open Arena")) {
		if (e_.project()) {
			auto mine = ed::addon_arenas(*e_.project());
			if (!mine.empty()) {
				ImGui::TextDisabled("ADD-ON VARIANTS");
				for (auto &a : mine) {
					if (ImGui::MenuItem((a.name + "##v").c_str(), a.stem.c_str())) open_arena(a);
				}
				ImGui::Separator();
			}
		}
		ImGui::TextDisabled("BASE GAME");
		for (auto &a : ed::base_arenas(*e_.base())) {
			if (!a.listed || a.stem.rfind("Cinematic", 0) == 0 || a.name.find("Cinematic") != std::string::npos) continue;
			if (ImGui::MenuItem(a.name.c_str(), a.stem.c_str())) open_arena(a);
		}
		ImGui::EndMenu();
	}
	if (ImGui::MenuItem(ICON_COPY "  Copy Arena to Add-on...", nullptr, false, arena_ && arena_->built && !arena_->entry.addon)) copy_to_addon();
	if (ImGui::MenuItem(ICON_X "  Close Arena", nullptr, false, arena_ != nullptr && !play_)) close_arena();
}
