#include "code_view.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "icons.h"
#include "imgui_stdlib.h"
#include "theme.h"
#include "tournament.h"
#include "widgets.h"

using namespace theme;

namespace {

std::vector<std::string> split_lines(const std::string &s) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at <= s.size()) {
		size_t e = s.find('\n', at);
		if (e == std::string::npos) {
			if (at < s.size()) out.push_back(s.substr(at));
			break;
		}
		out.push_back(s.substr(at, e - at));
		at = e + 1;
	}
	return out;
}

bool keyword(const std::string &w) {
	static const char *const kw[] = {"if", "else", "while", "for", "do", "return", "break", "continue", "switch", "case", "default",
	                                 "int", "float", "bool", "dword", "word", "byte", "true", "false", "null", "instance", "interface",
	                                 "final", "param", "rparam", "replicate", "enum", "class", "string", "void"};
	for (const char *k : kw) {
		if (w == k) return true;
	}
	return false;
}

void draw_line(ImDrawList *dl, ImVec2 p, const std::string &l, bool *in_comment) {
	size_t i = 0;
	float x = p.x;
	auto emit = [&](size_t from, size_t to, unsigned colour) {
		if (to <= from) return;
		std::string s = l.substr(from, to - from);
		for (char &c : s) {
			if (c == '\t') c = ' ';
		}
		dl->AddText(ImVec2(x, p.y), rgb(colour), s.c_str());
		x += ImGui::CalcTextSize(s.c_str()).x;
	};
	while (i < l.size()) {
		if (*in_comment) {
			size_t e = l.find("*/", i);
			size_t to = e == std::string::npos ? l.size() : e + 2;
			emit(i, to, 0x6a9955);
			if (e != std::string::npos) *in_comment = false;
			i = to;
			continue;
		}
		if (l.compare(i, 2, "//") == 0) {
			bool user = l.compare(i, 9, "// <user ") == 0 || l.compare(i, 10, "// </user>") == 0;
			emit(i, l.size(), user ? kAccentText : 0x6a9955);
			break;
		}
		if (l.compare(i, 2, "/*") == 0) {
			*in_comment = true;
			continue;
		}
		char c = l[i];
		if (c == '"') {
			size_t e = l.find('"', i + 1);
			size_t to = e == std::string::npos ? l.size() : e + 1;
			emit(i, to, 0xce9178);
			i = to;
			continue;
		}
		if (isdigit(uint8_t(c))) {
			size_t e = i;
			while (e < l.size() && (isalnum(uint8_t(l[e])) || l[e] == '.')) ++e;
			emit(i, e, 0xb5cea8);
			i = e;
			continue;
		}
		if (isalpha(uint8_t(c)) || c == '_') {
			size_t e = i;
			while (e < l.size() && (isalnum(uint8_t(l[e])) || l[e] == '_')) ++e;
			std::string w = l.substr(i, e - i);
			unsigned colour = keyword(w) ? 0x569cd6 : w.rfind("g_", 0) == 0 ? 0x4ec9b0 : w.rfind("m_", 0) == 0 ? 0x9cdcfe
			                  : (e < l.size() && l[e] == '(') ? 0xdcdcaa : kTextSoft;
			emit(i, e, colour);
			i = e;
			continue;
		}
		size_t e = i + 1;
		emit(i, e, kTextDim);
		i = e;
	}
}

}  // namespace

bool code_view(const char *id, CodeView &v, const std::string &text, ImVec2 size, const std::vector<std::pair<int, std::string>> &errors,
               std::map<std::string, std::string> *blocks) {
	bool applied = false;
	ImGui::PushID(id);
	const float bar = ImGui::GetFrameHeight() + ui::px(8);
	if (!blocks) {
		ImGui::AlignTextToFramePadding();
		ui::dim_text(errors.empty() ? "Read-only" : (std::to_string(errors.size()) + " compile error(s)").c_str(), errors.empty() ? kTextMuted : kError);
	} else if (!v.editing) {
		if (ui::toolbar_button(ICON_CODE, "Edit user blocks", ui::Tone::Normal, true,
		                       "Hand-written code goes between \"// <user ...>\" and \"// </user>\"; regenerating keeps it", ImGui::GetFrameHeight())) {
			v.editing = true, v.buffer = text, v.error.clear();
		}
	} else {
		if (ui::toolbar_button(ICON_CHECK, "Apply", ui::Tone::Accent, true, nullptr, ImGui::GetFrameHeight())) {
			if (ed::without_user_blocks(v.buffer) != ed::without_user_blocks(text)) {
				v.error = "Only the code inside user blocks can change; the rest is generated from the form.";
			} else {
				blocks->clear();
				for (auto &[bid, body] : ed::user_blocks(v.buffer)) (*blocks)[bid] = body;
				v.editing = false;
				applied = true;
			}
		}
		ImGui::SameLine();
		if (ui::toolbar_button(ICON_X, "Cancel", ui::Tone::Ghost, true, nullptr, ImGui::GetFrameHeight())) v.editing = false;
	}
	if (blocks) {
		ImGui::SameLine(0, ui::px(14));
		ImGui::AlignTextToFramePadding();
		if (!v.error.empty()) ui::dim_text(v.error.c_str(), kError);
		else ui::dim_text(errors.empty() ? "Generated from the form" : (std::to_string(errors.size()) + " compile error(s)").c_str(), errors.empty() ? kTextMuted : kError);
	}
	ImVec2 area(size.x, size.y - bar);
	if (v.editing) {
		ImGui::PushFont(fonts.mono, kFontSmall);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(0x16171d));
		ImGui::InputTextMultiline("##edit", &v.buffer, area, ImGuiInputTextFlags_AllowTabInput);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::PopID();
		return applied;
	}
	std::vector<std::string> lines = split_lines(text);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(0x16171d));
	ImGui::BeginChild("code", area, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::PushFont(fonts.mono, kFontSmall);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	const float line_h = ImGui::GetTextLineHeight() + ui::px(3);
	const float gutter = ImGui::CalcTextSize("00000").x + ui::px(14);
	if (v.jump_line > 0) {
		ImGui::SetScrollY(std::max(0.0f, float(v.jump_line - 6) * line_h));
		v.highlight = v.jump_line;
		v.jump_line = 0;
	}
	std::vector<uint8_t> comment(lines.size() + 1, 0), user(lines.size(), 0);
	bool in_user = false;
	for (size_t k = 0; k < lines.size(); ++k) {
		bool in = comment[k];
		const std::string &l = lines[k];
		for (size_t i = 0; i + 1 < l.size(); ++i) {
			if (!in && l[i] == '/' && l[i + 1] == '/') break;
			if (!in && l[i] == '/' && l[i + 1] == '*') in = true, ++i;
			else if (in && l[i] == '*' && l[i + 1] == '/') in = false, ++i;
		}
		comment[k + 1] = in;
		std::string t = l;
		t.erase(0, t.find_first_not_of(" \t"));
		if (t.rfind("// <user ", 0) == 0) in_user = true;
		user[k] = in_user;
		if (t.rfind("// </user>", 0) == 0) in_user = false;
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float widest = 0;
	ImGuiListClipper clip;
	clip.Begin(int(lines.size()), line_h);
	while (clip.Step()) {
		for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
			ImVec2 p = ImGui::GetCursorScreenPos();
			float w = std::max(ImGui::GetContentRegionAvail().x, widest + gutter);
			const std::string *err = nullptr;
			for (auto &e : errors) {
				if (e.first == k + 1) err = &e.second;
			}
			if (err) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + line_h), rgb(0x5a2a2a, 150));
			else if (user[size_t(k)]) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + line_h), rgb(0x2e2a52, 120));
			if (v.highlight == k + 1) dl->AddRect(p, ImVec2(p.x + w, p.y + line_h), rgb(kAccent));
			char num[16];
			snprintf(num, sizeof num, "%d", k + 1);
			float nw = ImGui::CalcTextSize(num).x;
			dl->AddText(ImVec2(p.x + gutter - ui::px(14) - nw, p.y), rgb(err ? kError : kTextFaint), num);
			bool in = comment[size_t(k)];
			draw_line(dl, ImVec2(p.x + gutter, p.y), lines[size_t(k)], &in);
			widest = std::max(widest, ImGui::CalcTextSize(lines[size_t(k)].c_str()).x + ui::px(40));
			ImGui::Dummy(ImVec2(gutter + widest, line_h));
			if (err && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", err->c_str());
		}
	}
	ImGui::PopStyleVar();
	ImGui::PopFont();
	ImGui::EndChild();
	ImGui::PopStyleColor();
	ImGui::PopID();
	return applied;
}
