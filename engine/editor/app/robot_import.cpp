#include "robot_import.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

#include "assets.h"
#include "gl_util.h"
#include "har.h"
#include "har_import.h"
#include "icons.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "model_export.h"
#include "theme.h"
#include "widgets.h"
#include "world.h"
#include "world_state.h"

namespace fs = std::filesystem;
using namespace theme;

namespace {

using V3 = std::array<float, 3>;

std::string path_text(const fs::path &p) {
	auto u = p.u8string();
	return std::string(u.begin(), u.end());
}

// The pilot editor's preview colours, for the paint mask.
const std::array<std::array<float, 3>, 3> kDemoPaint = {{{0.8f, 0.25f, 0.2f}, {0.85f, 0.85f, 0.9f}, {0.2f, 0.2f, 0.25f}}};

struct RobotState {
	std::string selected;  // a rig bone
	std::shared_ptr<ed::RobotBuild> build;
	std::vector<unsigned> face_colour;
	std::string key, error;
	int sequence = -1;  // -1: the bind pose
	float t = 0;
	bool playing = true, show_base = true, show_bones = true;
	float yaw = 0, pitch = 0.2f, dist = 0;
	Pane pane;
	FlatMesh tris;
	std::vector<std::string> robots;
	std::string filter;
	bool press_on_image = false, pose_applied = false;
};
RobotState &rs() {
	static RobotState s;
	return s;
}

void section(const char *label) {
	ImGui::Dummy(ImVec2(0, ui::px(4)));
	ImGui::PushStyleColor(ImGuiCol_Text, vec(kTextDim));
	ImGui::SeparatorText(label);
	ImGui::PopStyleColor();
}

unsigned source_colour(const ed::BoneMapping &b) {
	switch (b.source) {
	case ed::MapSource::Name:
	case ed::MapSource::Preset: return kOk;
	case ed::MapSource::Guess: return kWarning;
	case ed::MapSource::Manual: return kAccentText;
	default: return b.required ? kError : kTextFaint;
	}
}

std::string build_key(const ed::ImportPlan &plan) {
	const auto &h = plan.har;
	std::string k = path_text(plan.source) + "|" + h.base_robot + "|" + std::to_string(plan.settings.units_per_metre) + (plan.settings.z_up ? "z" : "y") + (h.mirror ? "m" : "") +
	                std::to_string(h.turn) + (h.ground ? "g" : "") + std::to_string(int(h.paint));
	for (auto &b : h.bones) k += "|" + std::to_string(b.joint);
	for (auto &m : h.sequences) {
		if (m.animation >= 0) k += ";" + m.sequence + "=" + std::to_string(m.animation);
	}
	for (auto &l : h.lods) {
		for (int n : l) k += "," + std::to_string(n);
	}
	for (auto &it : plan.items) k += char('a' + int(it.role));
	return k;
}

void ensure_build(const ed::ImportPlan &plan) {
	RobotState &st = rs();
	std::string k = build_key(plan);
	if (k == st.key) return;
	st.key = k;
	st.build.reset();
	st.error.clear();
	st.face_colour.clear();
	if (!plan.har.rig || plan.errors()) return;
	try {
		st.build = std::make_shared<ed::RobotBuild>(ed::build_robot(plan, true));
	} catch (const std::exception &ex) {
		st.error = ex.what();
		return;
	}
	// Each face's colour: the paint mask in the demo paint plus the model's own colours, at its UV centre.
	const ed::RobotBuild &b = *st.build;
	const ed::RobotLod &lod = b.lods[0];
	for (size_t f = 0; f < lod.faces.size(); ++f) {
		std::array<float, 2> uv{0.5f, 0.5f};
		if (!lod.uvs.empty() && f < lod.uvs[0].size()) {
			uv = {0, 0};
			for (auto &c : lod.uvs[0][f]) uv[0] += c[0] / 3, uv[1] += c[1] / 3;
		}
		auto sample = [&](const ed::RobotImage &img, int ch) -> float {
			if (img.rgba.empty() || img.width <= 0) return 0;
			float u = uv[0] - std::floor(uv[0]), v = uv[1] - std::floor(uv[1]);
			int x = std::clamp(int(u * float(img.width)), 0, img.width - 1), y = std::clamp(int(v * float(img.height)), 0, img.height - 1);
			return float(img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4 + size_t(ch)]) / 255.0f;
		};
		float c[3] = {0, 0, 0};
		for (int k2 = 0; k2 < 3; ++k2) {
			for (int m = 0; m < 3; ++m) c[k2] += sample(b.mask, m) * kDemoPaint[size_t(m)][size_t(k2)];
			c[k2] += sample(b.colours, k2) + sample(b.scaled, k2) * 0.7f;
		}
		auto byte = [](float v) { return unsigned(std::clamp(v, 0.0f, 1.0f) * 255); };
		unsigned col = (byte(c[0]) << 16) | (byte(c[1]) << 8) | byte(c[2]);
		st.face_colour.push_back(col ? col : 0x30303a);
	}
}

std::vector<V3> pose_lod(const ed::RobotLod &lod, const std::vector<de::Transform> &binds, const std::vector<de::SkinBone> &skin, const de::AnimBank &bank, int seq, float t) {
	if (seq < 0 || size_t(seq) >= bank.sequences.size()) return lod.vertices;
	auto pose = bank.pose(bank.sequences[size_t(seq)], t);
	std::vector<de::Transform> m(binds.size());
	for (size_t b = 0; b < binds.size(); ++b) {
		int c = bank.channel(skin[b].name);
		m[b] = c >= 0 ? pose[size_t(c)].then(binds[b].inverse()) : de::Transform{};
	}
	std::vector<V3> out(lod.vertices.size());
	for (size_t v = 0; v < lod.vertices.size(); ++v) {
		float acc[3] = {0, 0, 0}, ws = 0;
		for (size_t b = 0; b < binds.size(); ++b) {
			uint8_t w = b < lod.weights.size() && v < lod.weights[b].size() ? lod.weights[b][v] : 0;
			if (!w) continue;
			auto p = m[b].apply(lod.vertices[v]);
			for (int k = 0; k < 3; ++k) acc[k] += p[size_t(k)] * float(w);
			ws += float(w);
		}
		out[v] = ws > 0 ? V3{acc[0] / ws, acc[1] / ws, acc[2] / ws} : lod.vertices[v];
	}
	return out;
}

void add_triangles(FlatMesh &fm, const std::vector<V3> &pos, const std::vector<de::Mesh::Face> &faces, const std::vector<unsigned> &colours, unsigned fallback, const V3 &offset, float alpha) {
	const V3 light = {0.35f, 0.8f, 0.48f};
	for (size_t f = 0; f < faces.size(); ++f) {
		const auto &F = faces[f];
		V3 a = pos[F.v[0]], b = pos[F.v[1]], c = pos[F.v[2]];
		V3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		V3 n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
		float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		float d = l > 0 ? std::fabs(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / l : 0.5f;
		float shade = 0.4f + 0.6f * d;
		unsigned col = f < colours.size() ? colours[f] : fallback;
		for (const V3 &p : {a, b, c}) fm.vertex({p[0] + offset[0], p[1] + offset[1], p[2] + offset[2]}, col, alpha, shade);
	}
}

bool project(const float *vp, const V3 &p, ImVec2 origin, ImVec2 size, ImVec2 *out) {
	float x = vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12];
	float y = vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13];
	float w = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
	if (w <= 1e-4f) return false;
	*out = ImVec2(origin.x + (x / w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / w * 0.5f) * size.y);
	return true;
}

void set_joint(ed::ImportPlan &plan, Editor &e, const std::string &bone, int joint) {
	for (auto &b : plan.har.bones) {
		if (b.bone != bone && b.joint == joint && joint >= 0) b.joint = -1, b.source = ed::MapSource::None, b.confidence = 0;
	}
	for (auto &b : plan.har.bones) {
		if (b.bone != bone) continue;
		b.joint = joint;
		b.source = joint >= 0 ? ed::MapSource::Manual : ed::MapSource::None;
		b.confidence = joint >= 0 ? 1 : 0;
	}
	if (plan.har.auto_orient) ed::detect_orientation(plan);
	ed::revalidate(plan, e.base());
}

float skeleton_span(const ed::ImportPlan &plan) {
	gltf::Mat4 fix = ed::robot_fix(plan);
	float lo = 1e30f, hi = -1e30f;
	for (auto &b : plan.har.bones) {
		if (b.joint < 0 || !b.required) continue;
		float y = gltf::transform_point(gltf::multiply(fix, ed::joint_bind(plan, b.joint)), {0, 0, 0})[1];
		lo = std::min(lo, y), hi = std::max(hi, y);
	}
	return hi > lo ? hi - lo : 0;
}

}  // namespace

void robot_analyse(Editor &e, ed::ImportPlan &plan) {
	if (!e.base()) return;
	ed::analyse_robot(plan, *e.base());
	if (ed::Project *p = e.project()) {
		fs::path m = ed::robot_mapping_path(*p, plan.settings.name);
		std::error_code ec;
		if (fs::exists(m, ec)) {
			try {
				std::ifstream f(m, std::ios::binary);
				std::string text{std::istreambuf_iterator<char>(f), {}};
				ed::apply_robot_mapping(plan, json::Value::parse(text));
				if (plan.har.auto_orient) ed::detect_orientation(plan);
				e.log(LogLine::Info, "Import: the bone mapping kept with " + plan.settings.name + " was restored (" + path_text(m) + ")");
			} catch (const std::exception &ex) {
				e.log(LogLine::Warning, std::string("Import: the kept bone mapping could not be read: ") + ex.what());
			}
		}
	}
	ed::revalidate(plan, e.base());
	rs().key.clear();
	if (rs().selected.empty() && !plan.har.bones.empty()) rs().selected = plan.har.bones[0].bone;
}

void robot_source_section(Editor &e, ed::ImportPlan &plan) {
	RobotState &st = rs();
	auto &h = plan.har;
	const float lw = ui::px(160);
	if (st.robots.empty() && e.base()) st.robots = ed::base_fighters(*e.base());
	section("Robot");
	ui::form_label("Base robot", lw);
	ImGui::SetNextItemWidth(ui::px(220));
	if (ui::begin_combo("##base", h.base_robot.c_str(), ImGuiComboFlags_HeightLarge)) {
		for (auto &r : st.robots) {
			if (ImGui::Selectable(r.c_str(), r == h.base_robot) && r != h.base_robot) {
				h.base_robot = r;
				robot_analyse(e, plan);
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ui::dim_text("its scripts, moves, sounds and hitboxes; the model is replaced", kTextMuted);
	if (h.rig) {
		ui::form_label("Its rig", lw);
		ImGui::Text("%zu skin bones, %zu channels, %zu sequences, LODs", h.rig->skin.size(), h.rig->bank.channels.size(), h.rig->bank.sequences.size());
		for (auto &l : h.rig->lods) ImGui::SameLine(0, ui::px(4)), ImGui::Text("%zu", l.faces);
	}
	section("Fit");
	ui::form_label("Size", lw);
	float span = skeleton_span(plan) * plan.settings.units_per_metre;
	if (h.rig && span > 0) {
		ImGui::Text("%.1f units tall (%s %.1f)", span, h.base_robot.c_str(), h.rig->height);
		ImGui::SameLine();
		if (ui::toolbar_button(ICON_SCALING, ("Match " + h.base_robot + "'s size").c_str(), ui::Tone::Normal, !plan.from_editor)) {
			plan.settings.units_per_metre *= h.rig->height / span;
			ed::revalidate(plan, e.base());
		}
	} else {
		ui::dim_text("map the bones first", kTextMuted);
	}
	ui::form_label("Facing", lw);
	if (ui::checkbox("##auto", &h.auto_orient)) {
		if (h.auto_orient) ed::detect_orientation(plan);
		ed::revalidate(plan, e.base());
	}
	ImGui::SameLine();
	ImGui::TextUnformatted("From the skeleton");
	ImGui::SameLine(0, ui::px(16));
	ImGui::BeginDisabled(h.auto_orient);
	if (ImGui::Checkbox("Mirror left / right", &h.mirror)) ed::revalidate(plan, e.base());
	ImGui::SameLine(0, ui::px(16));
	const char *turns[] = {"0\xc2\xb0", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0"};
	int turn = (h.turn / 90) % 4;
	ImGui::SetNextItemWidth(ui::px(90));
	if (ui::combo("##turn", &turn, turns, 4)) h.turn = turn * 90, ed::revalidate(plan, e.base());
	ImGui::EndDisabled();
	ui::form_label("Feet on the floor", lw);
	if (ui::checkbox("##ground", &h.ground)) ed::revalidate(plan, e.base());
	ImGui::SameLine();
	ui::dim_text("the lowest vertex at height 0", kTextMuted);
	section("Paint");
	ui::form_label("Colours", lw);
	int paint = int(h.paint);
	if (ImGui::RadioButton("The model's own (RGBStatic)", paint == 1)) h.paint = ed::HarPlan::Paint::Colours;
	ImGui::SameLine(0, ui::px(16));
	if (ImGui::RadioButton("The pilot's, one per material", paint == 0)) h.paint = ed::HarPlan::Paint::Materials;
	ui::form_label("More paint jobs", lw);
	ImGui::BeginGroup();
	for (size_t k = 0; k < h.extra_paints.size(); ++k) {
		ImGui::PushID(int(k));
		ImGui::SetNextItemWidth(ui::px(140));
		ImGui::InputText("##n", &h.extra_paints[k].name);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ui::dim_text(path_text(h.extra_paints[k].image.filename()).c_str(), kTextSoft);
		ImGui::SameLine();
		bool drop = ui::icon_button("x", ICON_X, "Leave this paint job out");
		ImGui::PopID();
		if (drop) {
			h.extra_paints.erase(h.extra_paints.begin() + long(k));
			break;
		}
	}
	if (ui::toolbar_button(ICON_PLUS, "Add from an image...", ui::Tone::Ghost, true, "A PNG laid out as the model's texture (the atlas the import makes: export it from the Paint tab to paint over)", ImGui::GetFrameHeight())) {
		ed::ImportPlan *pp = &plan;
		e.file_dialog().open(FileDialog::OpenFile, "Paint job image", {}, {".png", ".bmp"}, [pp](const fs::path &path) {
			pp->har.extra_paints.push_back({"Paint" + std::to_string(pp->har.extra_paints.size() + 2), path});
		});
	}
	ImGui::EndGroup();
	ui::form_label("Level of detail", lw);
	ui::checkbox("##dec", &h.decimate_lod0);
	ImGui::SameLine();
	ImGui::TextUnformatted(h.rig && !h.rig->lods.empty() ? ("LOD0 to " + std::to_string(h.rig->lods[0].faces) + " triangles too").c_str() : "LOD0 decimated too");
	ImGui::SameLine();
	ui::dim_text("(LOD1 / LOD2 are generated unless the file has them)", kTextMuted);
}

bool robot_bones_done(const ed::ImportPlan &plan) {
	for (auto &b : plan.har.bones) {
		if (b.required && b.joint < 0) return false;
	}
	return plan.har.rig && plan.har.skin >= 0;
}

void robot_page_bones(Editor &e, ed::ImportPlan &plan) {
	RobotState &st = rs();
	auto &h = plan.har;
	const auto &s = plan.scene;
	if (!h.rig) {
		ui::dim_text("The base robot could not be read; see the checks.", kError);
		return;
	}
	ui::dim_text("Each bone of the base robot's skin needs a joint of the file. Select a bone, then pick its joint here or click it in the preview.", kTextMuted);
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	const auto &presets = ed::rig_presets();
	std::string label = h.preset == "Auto" && !h.detected.empty() ? "Auto (" + h.detected + ")" : h.preset;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Preset");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ui::px(200));
	if (ui::begin_combo("##preset", label.c_str())) {
		for (auto &p : presets) {
			if (ImGui::Selectable(p.c_str(), p == h.preset)) {
				h.preset = p;
				ed::map_bones(plan);
				if (h.auto_orient) ed::detect_orientation(plan);
				ed::revalidate(plan, e.base());
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_REFRESH_CW, "Map again", ui::Tone::Normal, true, "Keeps the bones you picked by hand")) {
		ed::map_bones(plan);
		if (h.auto_orient) ed::detect_orientation(plan);
		ed::revalidate(plan, e.base());
	}
	ImGui::SameLine();
	if (ui::toolbar_button(ICON_X, "Clear", ui::Tone::Ghost)) {
		for (auto &b : h.bones) b.joint = -1, b.source = ed::MapSource::None, b.confidence = 0;
		ed::revalidate(plan, e.base());
	}
	int counts[5] = {0, 0, 0, 0, 0}, missing = 0;
	for (auto &b : h.bones) {
		if (b.joint >= 0) ++counts[int(b.source)];
		else missing += b.required;
	}
	ImGui::SameLine(0, ui::px(14));
	if (counts[1] + counts[2]) ui::status_chip(ICON_CIRCLE_CHECK, (std::to_string(counts[1] + counts[2]) + " by name").c_str(), kOk), ImGui::SameLine();
	if (counts[3]) ui::status_chip(ICON_TRIANGLE_ALERT, (std::to_string(counts[3]) + " guessed").c_str(), kWarning), ImGui::SameLine();
	if (counts[4]) ui::status_chip(ICON_USER, (std::to_string(counts[4]) + " by hand").c_str(), kAccentText), ImGui::SameLine();
	if (missing) ui::status_chip(ICON_CIRCLE_X, (std::to_string(missing) + " missing").c_str(), kError);
	ImGui::NewLine();
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("bones", 4, flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Rig bone", ImGuiTableColumnFlags_WidthStretch, 1.3f);
	ImGui::TableSetupColumn("Joint in the file", ImGuiTableColumnFlags_WidthStretch, 1.8f);
	ImGui::TableSetupColumn("Found by", ImGuiTableColumnFlags_WidthFixed, ui::px(80));
	ImGui::TableSetupColumn("Confidence", ImGuiTableColumnFlags_WidthFixed, ui::px(80));
	ImGui::TableHeadersRow();
	std::vector<int> joints = h.skin >= 0 ? s.skins[size_t(h.skin)].joints : std::vector<int>{};
	for (size_t i = 0; i < h.bones.size(); ++i) {
		auto b = h.bones[i];
		ImGui::PushID(int(i));
		ImGui::TableNextRow(0, ui::px(30));
		ImGui::TableNextColumn();
		bool sel = st.selected == b.bone;
		if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, ui::px(26)))) st.selected = b.bone;
		ImGui::SameLine(0, 0);
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, vec(source_colour(b)));
		ImGui::TextUnformatted(b.joint >= 0 ? ICON_CIRCLE_CHECK : b.required ? ICON_CIRCLE_X : ICON_CIRCLE_DOT);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextUnformatted(b.bone.c_str());
		if (!b.required) ImGui::SameLine(), ui::dim_text("optional", kTextFaint);
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-1);
		std::string jl = b.joint >= 0 ? s.nodes[size_t(b.joint)].name : (b.required ? "(choose a joint)" : "(placed from the base)");
		if (ui::begin_combo("##joint", jl.c_str(), ImGuiComboFlags_HeightLarge)) {
			st.selected = b.bone;
			if (ImGui::IsWindowAppearing()) st.filter.clear(), ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##f", ICON_SEARCH "  Search joints", &st.filter);
			std::string f = st.filter;
			for (auto &c : f) c = char(tolower(uint8_t(c)));
			if (ImGui::Selectable("(none)", b.joint < 0)) set_joint(plan, e, b.bone, -1);
			std::vector<int> list = joints;
			if (!b.required) {
				list.clear();
				for (size_t n = 0; n < s.nodes.size(); ++n) {
					if (s.nodes[n].mesh < 0) list.push_back(int(n));
				}
			}
			for (int j : list) {
				std::string n = s.nodes[size_t(j)].name, ln = n;
				for (auto &c : ln) c = char(tolower(uint8_t(c)));
				if (!f.empty() && ln.find(f) == std::string::npos) continue;
				std::string taken;
				for (auto &o : h.bones) {
					if (o.joint == j && o.bone != b.bone) taken = o.bone;
				}
				if (ImGui::Selectable((n + (taken.empty() ? "" : "   (" + taken + ")") + "##" + std::to_string(j)).c_str(), j == b.joint)) set_joint(plan, e, b.bone, j);
			}
			ImGui::EndCombo();
		}
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, vec(source_colour(b)));
		ImGui::TextUnformatted(ed::map_source_name(b.source));
		ImGui::PopStyleColor();
		ImGui::TableNextColumn();
		if (b.joint >= 0) {
			ImVec2 p0 = ImGui::GetCursorScreenPos() + ImVec2(0, ui::px(11));
			float w = ui::px(64);
			ImDrawList *dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(p0, p0 + ImVec2(w, ui::px(5)), rgb(kInput), ui::px(2));
			dl->AddRectFilled(p0, p0 + ImVec2(w * b.confidence, ui::px(5)), rgb(source_colour(b)), ui::px(2));
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

void robot_page_animations(Editor &e, ed::ImportPlan &plan) {
	auto &h = plan.har;
	const auto &s = plan.scene;
	if (!h.rig) return;
	int from_file = 0;
	for (auto &m : h.sequences) from_file += m.animation >= 0;
	ui::dim_text(("Every sequence the scripts play keeps its name. A file animation named as one replaces it (times are scaled to the sequence's 0..1); the others are " +
	              h.base_robot + "'s, retargeted to the new proportions.")
	                 .c_str(),
	             kTextMuted);
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	ui::status_chip(ICON_VIDEO, (std::to_string(from_file) + " from the file").c_str(), from_file ? kOk : kTextMuted);
	ImGui::SameLine();
	ui::status_chip(ICON_REFRESH_CW, (std::to_string(h.sequences.size() - size_t(from_file)) + " retargeted").c_str(), kTextSoft);
	std::set<int> used;
	for (auto &m : h.sequences) {
		if (m.animation >= 0) used.insert(m.animation);
	}
	std::vector<int> unused;
	for (size_t a = 0; a < s.animations.size(); ++a) {
		if (!used.count(int(a))) unused.push_back(int(a));
	}
	if (!unused.empty()) {
		ImGui::SameLine();
		ui::status_chip(ICON_TRIANGLE_ALERT, (std::to_string(unused.size()) + " unused in the file").c_str(), kWarning);
	}
	std::set<std::string> scripted(h.rig->script_sequences.begin(), h.rig->script_sequences.end());
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("seqs", 3, flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Sequence", ImGuiTableColumnFlags_WidthStretch, 1.3f);
	ImGui::TableSetupColumn("Plays", ImGuiTableColumnFlags_WidthStretch, 1.6f);
	ImGui::TableSetupColumn("Scripts", ImGuiTableColumnFlags_WidthFixed, ui::px(60));
	ImGui::TableHeadersRow();
	ImGuiListClipper clip;
	clip.Begin(int(h.sequences.size()), ui::px(30));
	while (clip.Step()) {
		for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
			auto &m = h.sequences[size_t(i)];
			ImGui::PushID(i);
			ImGui::TableNextRow(0, ui::px(30));
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, vec(m.animation >= 0 ? kOk : kTextDim));
			ImGui::TextUnformatted(m.animation >= 0 ? ICON_VIDEO : ICON_REFRESH_CW);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextUnformatted(m.sequence.c_str());
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1);
			std::string label = m.animation >= 0 ? s.animations[size_t(m.animation)].name : h.base_robot + "'s, retargeted";
			if (ui::begin_combo("##a", label.c_str(), ImGuiComboFlags_HeightLarge)) {
				if (ImGui::Selectable((h.base_robot + "'s, retargeted").c_str(), m.animation < 0)) m.animation = -1, m.source = ed::MapSource::Manual, ed::revalidate(plan, e.base());
				for (size_t a = 0; a < s.animations.size(); ++a) {
					char dur[32];
					snprintf(dur, sizeof dur, "   %.2f s", double(s.animations[a].duration()));
					if (ImGui::Selectable((s.animations[a].name + dur + "##" + std::to_string(a)).c_str(), m.animation == int(a))) m.animation = int(a), m.source = ed::MapSource::Manual, ed::revalidate(plan, e.base());
				}
				ImGui::EndCombo();
			}
			ImGui::TableNextColumn();
			if (scripted.count(m.sequence)) {
				ImGui::AlignTextToFramePadding();
				ui::dim_text(ICON_CODE, kAccentText);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("The scripts play it as seq%s", m.sequence.c_str());
			}
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
}

void robot_page_hitboxes(Editor &, ed::ImportPlan &plan) {
	RobotState &st = rs();
	auto &h = plan.har;
	if (!h.rig) return;
	ensure_build(plan);
	ui::dim_text("Hit and attack spheres hang from the bones. An empty named Sphere:<affector> under a joint places one (its scale is the falloff in metres); the rest are the base robot's, "
	             "moved and scaled with their bone.",
	             kTextMuted);
	ImGui::Dummy(ImVec2(0, ui::px(2)));
	size_t from_file = 0;
	for (auto &sp : h.spheres) from_file += sp.known && !sp.bone.empty();
	ui::status_chip(ICON_CIRCLE_DOT, (std::to_string(from_file) + " from the file").c_str(), from_file ? kOk : kTextMuted);
	ImGui::SameLine();
	if (st.build) {
		char b[64];
		snprintf(b, sizeof b, "the rest scaled x%.2f", double(st.build->scale));
		ui::status_chip(ICON_SCALING, b, kTextSoft);
	}
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("spheres", 4, flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Sphere", ImGuiTableColumnFlags_WidthStretch, 1.6f);
	ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthStretch, 1);
	ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 1);
	ImGui::TableSetupColumn("Falloff", ImGuiTableColumnFlags_WidthFixed, ui::px(80));
	ImGui::TableHeadersRow();
	for (size_t i = 0; i < h.rig->spheres.size(); ++i) {
		const auto &rs0 = h.rig->spheres[i];
		const ed::SphereSource *file = nullptr;
		for (auto &sp : h.spheres) {
			if (sp.sphere == rs0.name && sp.known && !sp.bone.empty()) file = &sp;
		}
		ImGui::TableNextRow(0, ui::px(26));
		ImGui::TableNextColumn();
		bool attack = rs0.name.find("Hit") != std::string::npos || rs0.name.find("Leap") != std::string::npos;
		ImGui::PushStyleColor(ImGuiCol_Text, vec(attack ? 0xff6b6b : 0x6bb8ff));
		ImGui::TextUnformatted(ICON_CIRCLE_DOT);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextUnformatted(rs0.name.c_str());
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(file ? file->bone.c_str() : rs0.parent.c_str());
		ImGui::TableNextColumn();
		if (file) ui::dim_text(("the file (" + plan.scene.nodes[size_t(file->node)].name + ")").c_str(), kOk);
		else ui::dim_text(rs0.channel >= 0 ? "base, with its bone" : "base, scaled", kTextDim);
		ImGui::TableNextColumn();
		float f = st.build && i < st.build->spheres.size() ? st.build->spheres[i].falloff : rs0.falloff;
		ImGui::Text("%.1f", double(f));
	}
	ImGui::EndTable();
}

void robot_preview_reset() {
	RobotState &st = rs();
	st.key.clear();
	st.build.reset();
	st.sequence = -1;
}

void robot_preview(Editor &e, ed::ImportPlan &plan, ImVec2 size) {
	RobotState &st = rs();
	auto &h = plan.har;
	ensure_build(plan);
	ImGuiIO &io = ImGui::GetIO();
	const gltf::Scene &s = plan.scene;
	const float u = plan.settings.units_per_metre;
	const gltf::Mat4 F = ed::robot_fix(plan);
	// The new robot: the build's LOD0 posed, else the file's meshes as they are.
	st.tris.clear();
	std::vector<V3> bone_at;          // the new rig's skin bones (posed)
	std::vector<V3> base_at;          // the base's
	std::vector<std::pair<int, int>> bone_links;
	V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	auto grow = [&](const V3 &p) {
		for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], p[size_t(k)]), hi[size_t(k)] = std::max(hi[size_t(k)], p[size_t(k)]);
	};
	if (st.playing && st.sequence >= 0) {
		st.t += io.DeltaTime * 0.6f;
		if (st.t > 1) st.t -= 1;
	}
	const ed::RobotRig *rig = h.rig.get();
	if (st.build && !e.options().import_pose.empty() && !st.pose_applied) {
		st.pose_applied = true;
		for (size_t q = 0; q < st.build->bank.sequences.size(); ++q) {
			if (st.build->bank.sequences[q].name == e.options().import_pose) st.sequence = int(q), st.t = 0.3f, st.playing = false;
		}
	}
	int seq = st.build && st.sequence >= 0 && size_t(st.sequence) < st.build->bank.sequences.size() ? st.sequence : -1;
	if (st.build) {
		const auto &b = *st.build;
		auto pos = pose_lod(b.lods[0], b.binds, rig->skin, b.bank, seq, st.t);
		for (auto &p : b.lods[0].vertices) grow(p);
		add_triangles(st.tris, pos, b.lods[0].faces, st.face_colour, 0xb8bccb, {0, 0, 0}, 1);
		auto pose = seq >= 0 ? b.bank.pose(b.bank.sequences[size_t(seq)], st.t) : std::vector<de::Transform>{};
		for (size_t k = 0; k < rig->skin.size(); ++k) {
			int c = b.bank.channel(rig->skin[k].name);
			bone_at.push_back(seq >= 0 && c >= 0 ? pose[size_t(c)].t : b.binds[k].t);
		}
	} else {
		for (int k = 0; k < 1; ++k) {
			for (int n : h.lods[size_t(k)]) {
				const auto &node = s.nodes[size_t(n)];
				if (node.mesh < 0) continue;
				gltf::Mat4 m = node.skin >= 0 ? F : gltf::multiply(F, s.world(n));
				for (auto &p : s.meshes[size_t(node.mesh)].primitives) {
					std::vector<V3> pos;
					for (auto &q : p.positions) pos.push_back(ed::from_gltf(gltf::transform_point(m, q), u)), grow(pos.back());
					std::vector<de::Mesh::Face> faces;
					for (size_t t = 0; t + 2 < p.indices.size(); t += 3) faces.push_back({{uint16_t(p.indices[t] & 0xffff), uint16_t(p.indices[t + 1] & 0xffff), uint16_t(p.indices[t + 2] & 0xffff)}, {}, 0});
					if (pos.size() <= 65535) add_triangles(st.tris, pos, faces, {}, 0xb8bccb, {0, 0, 0}, 1);
				}
			}
		}
	}
	if (rig) {
		for (size_t a = 0; a < rig->skin.size(); ++a) {
			int c = rig->channel(rig->skin[a].name);
			for (int p = c >= 0 ? rig->bank.channels[size_t(c)].parent : -1; p >= 0; p = rig->bank.channels[size_t(p)].parent) {
				if (rig->deformer[size_t(p)] >= 0) {
					bone_links.push_back({int(a), rig->deformer[size_t(p)]});
					break;
				}
			}
		}
	}
	if (lo[0] > hi[0]) lo = {-10, 0, -10}, hi = {10, 30, 10};
	// The base robot beside it, on its left, posed by the same sequence when it has it.
	V3 base_offset{0, 0, 0};
	if (rig && st.show_base) {
		float blo = 1e30f, bhi = -1e30f;
		for (auto &v : rig->mesh.vertices) {
			float x = v[0] * rig->left[0] + v[2] * rig->left[2];
			blo = std::min(blo, x), bhi = std::max(bhi, x);
		}
		float nlo = std::min(lo[0] * rig->left[0] + lo[2] * rig->left[2], hi[0] * rig->left[0] + hi[2] * rig->left[2]);
		float gap = (bhi - 0) + (0 - nlo) + 6;
		base_offset = {-rig->left[0] * gap, 0, -rig->left[2] * gap};
		ed::RobotLod base_lod;
		base_lod.vertices = rig->mesh.vertices;
		base_lod.faces = rig->mesh.faces;
		std::vector<de::Transform> binds;
		for (auto &b : rig->skin) base_lod.weights.push_back(b.weights), binds.push_back(b.bind);
		int bseq = -1;
		if (seq >= 0) {
			for (size_t q = 0; q < rig->bank.sequences.size(); ++q) {
				if (rig->bank.sequences[q].name == st.build->bank.sequences[size_t(seq)].name) bseq = int(q);
			}
		}
		auto pos = pose_lod(base_lod, binds, rig->skin, rig->bank, bseq, st.t);
		for (auto &p : pos) grow({p[0] + base_offset[0], p[1] + base_offset[1], p[2] + base_offset[2]});
		add_triangles(st.tris, pos, base_lod.faces, {}, 0x6b6e80, base_offset, 1);
		auto pose = bseq >= 0 ? rig->bank.pose(rig->bank.sequences[size_t(bseq)], st.t) : std::vector<de::Transform>{};
		for (size_t k = 0; k < rig->skin.size(); ++k) {
			int c = rig->channel(rig->skin[k].name);
			V3 p = bseq >= 0 && c >= 0 ? pose[size_t(c)].t : rig->skin[k].bind.t;
			base_at.push_back({p[0] + base_offset[0], p[1] + base_offset[1], p[2] + base_offset[2]});
		}
	}
	st.tris.upload();
	// Camera: in front of the robot (it faces rig->forward), turned by dragging.
	V3 fwd = rig ? rig->forward : V3{0, 0, 1};
	V3 c{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
	float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1.0f});
	if (st.dist <= 0) st.dist = extent * 1.5f;
	float cy = std::cos(st.yaw), sy = std::sin(st.yaw);
	V3 dir{fwd[0] * cy - fwd[2] * sy, 0, fwd[0] * sy + fwd[2] * cy};
	float eye[3] = {c[0] + dir[0] * std::cos(st.pitch) * st.dist, c[1] + std::sin(st.pitch) * st.dist, c[2] + dir[2] * std::cos(st.pitch) * st.dist};
	float f[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]}, up[3] = {0, 1, 0};
	const float footer = ui::px(34);
	ImVec2 img(size.x, size.y - footer);
	const int w = std::max(16, int(img.x * io.DisplayFramebufferScale.x)), hgt = std::max(16, int(img.y * io.DisplayFramebufferScale.y));
	float proj[16], view[16], vp[16];
	glu::perspective(proj, 45, float(w) / float(hgt), st.dist * 0.02f, st.dist * 20);
	glu::look_view(view, eye, f, up);
	glu::mul(vp, proj, view);
	GLint prev_fbo = 0, prev_vp[4];
	bind_pane(st.pane, w, hgt, &prev_fbo, prev_vp);
	glClearColor(0.071f, 0.075f, 0.094f, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	st.tris.draw(vp, GL_TRIANGLES, true, false);
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
	glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
	ImVec2 p0 = ImGui::GetCursorScreenPos();
	ImGui::Image((ImTextureID)(intptr_t)st.pane.tex, img, ImVec2(0, 1), ImVec2(1, 0));
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(p0, p0 + img, true);
	// Skeletons: the file's joints (bind pose), the new rig's and the base's bones.
	struct Dot {
		ImVec2 at;
		int joint = -1;
		std::string bone;
	};
	std::vector<Dot> dots;
	auto bone_joint = [&](const std::string &bone) {
		for (auto &b : h.bones) {
			if (b.bone == bone) return b.joint;
		}
		return -1;
	};
	const int sel_joint = bone_joint(st.selected);
	if (st.show_bones && seq < 0 && h.skin >= 0) {
		const auto &joints = s.skins[size_t(h.skin)].joints;
		std::map<int, ImVec2> at;
		for (int j : joints) {
			V3 p = ed::from_gltf(gltf::transform_point(gltf::multiply(F, ed::joint_bind(plan, j)), {0, 0, 0}), u);
			ImVec2 q;
			if (project(vp, p, p0, img, &q)) at[j] = q;
		}
		for (auto &[j, q] : at) {
			int pj = s.nodes[size_t(j)].parent;
			if (at.count(pj)) dl->AddLine(at[pj], q, rgb(0x9a8cff, 140), ui::px(1.2f));
		}
		for (auto &[j, q] : at) {
			bool mapped = false;
			for (auto &b : h.bones) mapped = mapped || b.joint == j;
			unsigned col = j == sel_joint ? kWarningStrong : mapped ? kAccentText : 0x8a8d9e;
			dl->AddCircleFilled(q, ui::px(j == sel_joint ? 5 : mapped ? 3.5f : 2.5f), rgb(col));
			dots.push_back({q, j, ""});
		}
	}
	auto draw_rig = [&](const std::vector<V3> &pts, unsigned col) {
		if (!st.show_bones || !rig || pts.size() != rig->skin.size()) return;
		std::vector<ImVec2> q(pts.size());
		std::vector<bool> ok(pts.size());
		for (size_t k = 0; k < pts.size(); ++k) ok[k] = project(vp, pts[k], p0, img, &q[k]);
		for (auto &[a, b] : bone_links) {
			if (ok[size_t(a)] && ok[size_t(b)]) dl->AddLine(q[size_t(b)], q[size_t(a)], rgb(col, 200), ui::px(2));
		}
		for (size_t k = 0; k < pts.size(); ++k) {
			if (!ok[k]) continue;
			bool sel = rig->skin[k].name == st.selected;
			dl->AddCircleFilled(q[k], ui::px(sel ? 5.5f : 3.5f), rgb(sel ? kWarningStrong : col));
			dots.push_back({q[k], -1, rig->skin[k].name});
		}
	};
	if (seq >= 0 || !st.show_bones || h.skin < 0) draw_rig(bone_at, kOk);
	draw_rig(base_at, 0x6bb8ff);
	if (rig && !st.selected.empty()) {
		ImVec2 q;
		for (size_t k = 0; k < base_at.size(); ++k) {
			if (rig->skin[k].name == st.selected && project(vp, base_at[k], p0, img, &q)) {
				dl->AddText(q + ImVec2(ui::px(8), -ui::px(8)), rgb(kWarningStrong), st.selected.c_str());
			}
		}
	}
	{
		dl->AddText(p0 + ImVec2(ui::px(10), ui::px(8)), rgb(kTextDim), (plan.settings.name + (st.show_base && rig ? "   |   " + h.base_robot + " (blue skeleton)" : "")).c_str());
		if (!st.error.empty()) dl->AddText(p0 + ImVec2(ui::px(10), ui::px(26)), rgb(kError), st.error.c_str());
		else if (!st.build && plan.errors()) dl->AddText(p0 + ImVec2(ui::px(10), ui::px(26)), rgb(kWarning), "Fix the errors to see it posed");
	}
	dl->PopClipRect();
	dl->AddRect(p0, p0 + img, rgb(kBorder), ui::px(6));
	// Drag turns; a click picks a joint (the selected bone takes it) or a bone.
	if (ImGui::IsItemHovered()) {
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) st.press_on_image = true;
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3)) {
			st.yaw -= io.MouseDelta.x * 0.01f;
			st.pitch = std::clamp(st.pitch + io.MouseDelta.y * 0.01f, -1.4f, 1.4f);
		}
		if (io.MouseWheel != 0) st.dist *= std::pow(0.88f, io.MouseWheel);
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && st.press_on_image && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x +
		                                                                                         ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).y < 9) {
			const Dot *best = nullptr;
			float bd = ui::px(9) * ui::px(9);
			for (auto &d : dots) {
				ImVec2 v = d.at - io.MousePos;
				float dd = v.x * v.x + v.y * v.y;
				if (dd < bd) bd = dd, best = &d;
			}
			if (best && !best->bone.empty()) st.selected = best->bone;
			else if (best && best->joint >= 0) {
				std::string owner;
				for (auto &b : h.bones) {
					if (b.joint == best->joint) owner = b.bone;
				}
				if (!owner.empty() && owner != st.selected) st.selected = owner;
				else if (!st.selected.empty()) set_joint(plan, e, st.selected, best->joint);
			}
		}
	}
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) st.press_on_image = false;
	// Footer: the pose, the base robot and skeletons.
	ImGui::SetNextItemWidth(ui::px(200));
	std::string pose_label = seq >= 0 ? st.build->bank.sequences[size_t(seq)].name : "Bind pose";
	if (ui::begin_combo("##pose", pose_label.c_str(), ImGuiComboFlags_HeightLarge)) {
		if (ImGui::Selectable("Bind pose", seq < 0)) st.sequence = -1;
		if (st.build) {
			for (size_t q = 0; q < st.build->bank.sequences.size(); ++q) {
				if (ImGui::Selectable(st.build->bank.sequences[q].name.c_str(), int(q) == seq)) st.sequence = int(q), st.t = 0;
			}
		}
		ImGui::EndCombo();
	}
	if (!st.build && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("The sequences play once every required bone is mapped");
	ImGui::SameLine();
	if (ui::icon_button("##play", st.playing ? ICON_PAUSE : ICON_PLAY, st.playing ? "Pause" : "Play")) st.playing = !st.playing;
	ImGui::SameLine();
	ImGui::Checkbox("Base robot", &st.show_base);
	ImGui::SameLine();
	ImGui::Checkbox("Skeletons", &st.show_bones);
	ImGui::SameLine();
	ui::dim_text("drag to turn, click a joint", kTextFaint);
}

bool robot_commit(Editor &e, ed::ImportPlan &plan, std::string *error) {
	ed::Project *p = e.project();
	if (!p) return false;
	try {
		ed::ImportResult r = ed::import_robot(*p, plan);
		fs::path kept;
		try {
			if (!plan.source.empty()) kept = ed::keep_source(*p, plan.source, plan.settings.name);
			fs::path m = ed::robot_mapping_path(*p, plan.settings.name);
			fs::create_directories(m.parent_path());
			std::ofstream(m, std::ios::binary) << ed::robot_mapping_json(plan).dump();
		} catch (const std::exception &ex) {
			e.log(LogLine::Warning, std::string("The source file could not be kept in the sidecar folder: ") + ex.what());
		}
		e.log(LogLine::Ok, "Imported robot " + plan.settings.name + " on " + plan.har.base_robot + "'s rig: " + r.summary());
		for (auto &n : r.notes) e.log(LogLine::Info, n);
		if (!kept.empty()) e.log(LogLine::Info, "Source and bone mapping kept in " + path_text(kept.parent_path()));
		assets_open_har(e, r.root);
		e.validate_now();
		robot_preview_reset();
		return true;
	} catch (const std::exception &ex) {
		*error = ex.what();
		e.log(LogLine::Error, std::string("Import failed: ") + ex.what());
		return false;
	}
}
