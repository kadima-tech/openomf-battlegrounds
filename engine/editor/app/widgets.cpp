#include "widgets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "icons.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "theme.h"

namespace ui {

using namespace theme;

float px(float v) { return v * ImGui::GetStyle().FontScaleDpi; }

namespace {

ImVec2 text_size(const char *t) { return ImGui::CalcTextSize(t, nullptr, true); }

void tooltip_for_last(const char *tooltip) {
	if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
		ImGui::SetTooltip("%s", tooltip);
	}
}

}  // namespace

bool toolbar_button(const char *icon, const char *label, Tone tone, bool enabled, const char *tooltip, float height) {
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	const ImGuiID id = w->GetID(label);
	const float h = height > 0 ? height : px(32);
	const float pad = px(12), gap = px(7);
	const float icon_w = icon ? text_size(icon).x : 0;
	const float label_w = text_size(label).x;
	const ImVec2 size(pad * 2 + icon_w + (icon && label_w > 0 ? gap : 0) + label_w, h);
	const ImVec2 pos = w->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(size);
	if (!ImGui::ItemAdd(bb, id)) return false;
	bool hovered = false, held = false;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0);
	if (!enabled) hovered = held = pressed = false;
	unsigned bg = tone == Tone::Play ? (hovered ? kPlayHover : kPlay)
	              : tone == Tone::Accent ? (hovered ? 0x4a4380 : kSelection)
	              : held ? kButtonActive : hovered ? kButtonHover : kButton;
	unsigned border = tone == Tone::Play ? kPlay : tone == Tone::Accent ? kAccentLine : kInputBorder;
	ImDrawList *dl = w->DrawList;
	if (tone != Tone::Ghost || hovered) {
		dl->AddRectFilled(bb.Min, bb.Max, rgb(bg, enabled ? 255 : 150), px(6));
		dl->AddRect(bb.Min, bb.Max, rgb(border, enabled ? 255 : 120), px(6));
	}
	unsigned fg = enabled ? (tone == Tone::Play || tone == Tone::Accent ? 0xffffff : kText) : kTextFaint;
	float y = pos.y + (h - ImGui::GetFontSize()) * 0.5f;
	float x = pos.x + pad;
	if (icon) {
		unsigned ic = !enabled ? kTextFaint : tone == Tone::Play ? 0xffffff : kTextSoft;
		dl->AddText(ImVec2(x, y), rgb(ic), icon);
		x += icon_w + gap;
	}
	dl->AddText(ImVec2(x, y), rgb(fg), label, ImGui::FindRenderedTextEnd(label));
	tooltip_for_last(tooltip);
	return pressed;
}

bool icon_button(const char *id_text, const char *icon, const char *tooltip, bool enabled, float side) {
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	const ImGuiID id = w->GetID(id_text);
	const float s = side > 0 ? side : ImGui::GetFrameHeight();
	const ImVec2 pos = w->DC.CursorPos;
	const ImRect bb(pos, pos + ImVec2(s, s));
	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, id)) return false;
	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0);
	ImDrawList *dl = w->DrawList;
	dl->AddRectFilled(bb.Min, bb.Max, rgb(held ? kButtonActive : hovered && enabled ? kButtonHover : kButton), px(5));
	dl->AddRect(bb.Min, bb.Max, rgb(kInputBorder), px(5));
	ImVec2 ts = text_size(icon);
	dl->AddText(bb.Min + (ImVec2(s, s) - ts) * 0.5f + ImVec2(0, px(1)), rgb(enabled ? kTextSoft : kTextFaint), icon);
	tooltip_for_last(tooltip);
	return pressed && enabled;
}

bool overlay_button(const char *label, bool active) {
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	const ImGuiID id = w->GetID(label);
	ImVec2 ts = text_size(label);
	const ImVec2 size(ts.x + px(16), px(26));
	const ImVec2 pos = w->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(size);
	if (!ImGui::ItemAdd(bb, id)) return false;
	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
	ImDrawList *dl = w->DrawList;
	dl->AddRectFilled(bb.Min, bb.Max, rgb(active ? kSelection : hovered ? 0x2a2b36 : 0x1e1f27, 235), px(5));
	dl->AddRect(bb.Min, bb.Max, rgb(0x3a3b48, 220), px(5));
	dl->AddText(bb.Min + ImVec2(px(8), (size.y - ImGui::GetFontSize()) * 0.5f), rgb(kTextSoft), label,
	            ImGui::FindRenderedTextEnd(label));
	return pressed;
}

bool tree_row(const char *id_text, const TreeRow &row) {
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	const ImGuiID id = w->GetID(id_text);
	const float h = px(26);
	const ImVec2 pos = w->DC.CursorPos;
	const float width = ImGui::GetContentRegionAvail().x;
	const ImRect bb(pos, ImVec2(pos.x + width, pos.y + h));
	ImGui::ItemSize(ImVec2(width, h), 0);
	if (!ImGui::ItemAdd(bb, id)) return false;
	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
	ImDrawList *dl = w->DrawList;
	if (row.selected) dl->AddRectFilled(bb.Min, bb.Max, rgb(kSelection), px(4));
	else if (hovered && !row.disabled) dl->AddRectFilled(bb.Min, bb.Max, rgb(0xffffff, 10), px(4));
	float x = pos.x + px(8) + float(row.depth) * px(16);
	const float ty = pos.y + (h - ImGui::GetFontSize()) * 0.5f;
	if (row.has_children) {
		const bool is_open = row.open && *row.open;
		const ImVec2 c(x + px(5), pos.y + h * 0.5f);
		const float r = px(3.5f);
		if (is_open) dl->AddTriangleFilled(c + ImVec2(-r, -r * 0.6f), c + ImVec2(r, -r * 0.6f), c + ImVec2(0, r * 0.8f), rgb(kTextMuted));
		else dl->AddTriangleFilled(c + ImVec2(-r * 0.6f, -r), c + ImVec2(r * 0.8f, 0), c + ImVec2(-r * 0.6f, r), rgb(kTextMuted));
		if (pressed && row.open && ImGui::GetIO().MousePos.x < x + px(14)) {
			*row.open = !*row.open;
			pressed = false;
		}
	}
	x += px(16);
	const unsigned fg = row.disabled ? kTextFaint : row.selected ? 0xffffff : kTextSoft;
	if (row.icon) {
		dl->AddText(ImVec2(x, ty), rgb(row.disabled ? kTextFaint : row.icon_colour ? row.icon_colour : kTextDim), row.icon);
		x += text_size(row.icon).x + px(7);
	}
	dl->AddText(ImVec2(x, ty), rgb(fg), row.label.c_str());
	x += text_size(row.label.c_str()).x;
	if (row.count >= 0) {
		char n[32];
		snprintf(n, sizeof n, "(%d)", row.count);
		dl->AddText(ImVec2(x + px(5), ty), rgb(row.selected ? 0xd8d4ff : kTextMuted), n);
		x += px(5) + text_size(n).x;
	}
	if (row.badge) {
		ImVec2 bs = text_size(row.badge);
		ImVec2 b0(x + px(8), pos.y + px(5)), b1 = b0 + ImVec2(bs.x + px(10), h - px(10));
		dl->AddRectFilled(b0, b1, rgb(kPanelHead), px(3));
		dl->AddRect(b0, b1, rgb(kBorder), px(3));
		ImGui::PushFont(nullptr, kFontSmall - 2);
		dl->AddText(b0 + ImVec2(px(5), (b1.y - b0.y - ImGui::GetFontSize()) * 0.5f), rgb(kTextMuted), row.badge);
		ImGui::PopFont();
	}
	return pressed && !row.disabled;
}

bool tile(const char *id_text, const Tile &t, ImVec2 size, bool *double_clicked) {
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	const ImGuiID id = w->GetID(id_text);
	const ImVec2 pos = w->DC.CursorPos;
	const ImRect bb(pos, pos + size);
	ImGui::ItemSize(size);
	if (!ImGui::ItemAdd(bb, id)) return false;
	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
	if (double_clicked) *double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !t.disabled;
	ImDrawList *dl = w->DrawList;
	const float label_h = px(t.sublabel.empty() ? 30 : 44);
	const ImRect img(bb.Min, ImVec2(bb.Max.x, bb.Max.y - label_h));
	const float r = px(8);
	if (t.add) {
		dl->AddRectFilled(img.Min, img.Max, rgb(hovered ? 0x22232c : 0x1a1b22), r);
		const float dash = px(6);
		for (float x = img.Min.x + r; x < img.Max.x - r; x += dash * 2) {
			dl->AddLine(ImVec2(x, img.Min.y), ImVec2(std::min(x + dash, img.Max.x - r), img.Min.y), rgb(0x4a4c5c), px(1));
			dl->AddLine(ImVec2(x, img.Max.y), ImVec2(std::min(x + dash, img.Max.x - r), img.Max.y), rgb(0x4a4c5c), px(1));
		}
		for (float y = img.Min.y + r; y < img.Max.y - r; y += dash * 2) {
			dl->AddLine(ImVec2(img.Min.x, y), ImVec2(img.Min.x, std::min(y + dash, img.Max.y - r)), rgb(0x4a4c5c), px(1));
			dl->AddLine(ImVec2(img.Max.x, y), ImVec2(img.Max.x, std::min(y + dash, img.Max.y - r)), rgb(0x4a4c5c), px(1));
		}
		ImGui::PushFont(nullptr, 30);
		ImVec2 ps = text_size(ICON_PLUS);
		dl->AddText(img.GetCenter() - ps * 0.5f, rgb(t.disabled ? kTextFaint : hovered ? kText : kTextDim), ICON_PLUS);
		ImGui::PopFont();
	} else {
		dl->AddRectFilled(img.Min, img.Max, rgb(0x272833), r);
		if (t.texture && t.aspect > 0) {
			ImVec2 inner = img.GetSize() - ImVec2(px(12), px(12));
			ImVec2 fit = t.aspect > inner.x / inner.y ? ImVec2(inner.x, inner.x / t.aspect) : ImVec2(inner.y * t.aspect, inner.y);
			ImVec2 c = img.GetCenter();
			const float sq = px(8);
			ImVec2 a = c - fit * 0.5f, b = c + fit * 0.5f;
			dl->PushClipRect(a, b, true);
			for (float y = a.y; y < b.y; y += sq) {
				for (float x = a.x; x < b.x; x += sq) {
					bool dark = (int((x - a.x) / sq) + int((y - a.y) / sq)) & 1;
					dl->AddRectFilled(ImVec2(x, y), ImVec2(x + sq, y + sq), rgb(dark ? 0x2e2f3a : 0x383946));
				}
			}
			dl->PopClipRect();
			dl->AddImage(t.texture, a, b);
		} else if (t.texture) {
			ImVec2 inset(px(1), px(1));
			dl->AddImageRounded(t.texture, img.Min + inset, img.Max - inset, ImVec2(0, 0), ImVec2(1, 1),
			                    t.disabled ? IM_COL32(255, 255, 255, 110) : IM_COL32_WHITE, r);
		} else if (t.icon) {
			dl->AddRectFilledMultiColor(img.Min + ImVec2(px(1), px(1)), img.Max - ImVec2(px(1), px(1)), rgb(0x2b2c38), rgb(0x2b2c38),
			                            rgb(0x1f2029), rgb(0x1f2029));
			ImGui::PushFont(nullptr, 40);
			ImVec2 is = text_size(t.icon);
			dl->AddText(img.GetCenter() - is * 0.5f, rgb(t.tint ? t.tint : kTextMuted, t.disabled ? 110 : 255), t.icon);
			ImGui::PopFont();
		}
		dl->AddRect(img.Min, img.Max, rgb(t.selected ? kAccent : hovered && !t.disabled ? 0x4a4c5c : kBorder), r, 0,
		            t.selected ? px(2.5f) : px(1));
		if (t.is_new || t.badge) {
			const char *text = t.is_new ? "NEW" : t.badge;
			ImGui::PushFont(theme::fonts.semibold, kFontSmall - 2);
			ImVec2 bs = text_size(text);
			ImVec2 b0 = img.Min + ImVec2(px(8), px(8)), b1 = b0 + bs + ImVec2(px(12), px(6));
			dl->AddRectFilled(b0, b1, t.is_new ? rgb(kNew) : rgb(t.badge_colour ? t.badge_colour : 0x3b3d4b, 235), px(4));
			dl->AddText(b0 + ImVec2(px(6), px(3)), rgb(t.is_new ? 0xffffff : kTextSoft), text);
			ImGui::PopFont();
		}
	}
	const float ty = img.Max.y + px(7);
	ImVec2 ls = text_size(t.label.c_str());
	float lx = bb.Min.x + std::max(0.0f, (size.x - ls.x) * 0.5f);
	ImGui::PushClipRect(ImVec2(bb.Min.x, ty), ImVec2(bb.Max.x, bb.Max.y), true);
	dl->AddText(ImVec2(lx, ty), rgb(t.disabled ? kTextFaint : t.selected ? 0xffffff : kTextSoft), t.label.c_str());
	if (!t.sublabel.empty()) {
		ImGui::PushFont(nullptr, kFontSmall - 1);
		ImVec2 ss = text_size(t.sublabel.c_str());
		dl->AddText(ImVec2(bb.Min.x + std::max(0.0f, (size.x - ss.x) * 0.5f), ty + ls.y + px(3)), rgb(kTextMuted), t.sublabel.c_str());
		ImGui::PopFont();
	}
	ImGui::PopClipRect();
	return pressed && !t.disabled;
}

bool tab_strip(const char *id_text, const std::vector<std::string> &labels, int *active) {
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	ImGui::PushID(id_text);
	const ImVec2 start = w->DC.CursorPos;
	const float h = px(42);
	const float width = ImGui::GetContentRegionAvail().x;
	bool changed = false;
	float x = start.x;
	for (size_t i = 0; i < labels.size(); ++i) {
		const ImGuiID id = w->GetID(int(i));
		ImVec2 ts = text_size(labels[i].c_str());
		ImRect bb(ImVec2(x, start.y), ImVec2(x + ts.x + px(32), start.y + h));
		ImGui::ItemAdd(bb, id);
		bool hovered, held;
		if (ImGui::ButtonBehavior(bb, id, &hovered, &held)) {
			*active = int(i);
			changed = true;
		}
		bool on = *active == int(i);
		if (on) w->DrawList->AddRectFilled(bb.Min, bb.Max, rgb(kSelection), px(6), ImDrawFlags_RoundCornersTop);
		else if (hovered) w->DrawList->AddRectFilled(bb.Min, bb.Max, rgb(0xffffff, 10), px(6), ImDrawFlags_RoundCornersTop);
		w->DrawList->AddText(ImVec2(bb.Min.x + px(16), start.y + (h - ImGui::GetFontSize()) * 0.5f), rgb(on ? 0xffffff : kLabel),
		                     labels[i].c_str());
		x = bb.Max.x + px(4);
	}
	w->DrawList->AddRectFilled(ImVec2(start.x, start.y + h - px(1)), ImVec2(start.x + width, start.y + h + px(1)), rgb(kAccentLine));
	ImGui::ItemSize(ImVec2(width, h + px(2)));
	ImGui::PopID();
	return changed;
}

bool doc_tab(const char *id_text, const char *icon, const std::string &label, bool active, bool closable, bool *close, bool dirty) {
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	const ImGuiID id = w->GetID(id_text);
	const float h = px(34);
	const float pad = px(14), gap = px(8);
	std::string text = label + (dirty ? " *" : "");
	float iw = icon ? text_size(icon).x + gap : 0;
	float cw = closable ? px(22) : 0;
	ImVec2 size(pad * 2 + iw + text_size(text.c_str()).x + cw, h);
	ImVec2 pos = w->DC.CursorPos;
	ImRect bb(pos, pos + size);
	ImGui::ItemSize(size);
	if (!ImGui::ItemAdd(bb, id)) return false;
	ImRect close_bb(ImVec2(bb.Max.x - pad - px(14), pos.y + (h - px(18)) * 0.5f), ImVec2(bb.Max.x - pad + px(4), pos.y + (h + px(18)) * 0.5f));
	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
	bool over_close = closable && close_bb.Contains(ImGui::GetIO().MousePos);
	ImDrawList *dl = w->DrawList;
	if (active) dl->AddRectFilled(bb.Min, bb.Max, rgb(kDocTabActive), px(7), ImDrawFlags_RoundCornersTop);
	else if (hovered) dl->AddRectFilled(bb.Min, bb.Max, rgb(0xffffff, 12), px(7), ImDrawFlags_RoundCornersTop);
	float x = pos.x + pad;
	float ty = pos.y + (h - ImGui::GetFontSize()) * 0.5f;
	if (icon) {
		dl->AddText(ImVec2(x, ty), rgb(active ? 0xc9c2ff : kTextMuted), icon);
		x += iw;
	}
	dl->AddText(ImVec2(x, ty), rgb(active ? 0xffffff : kTextDim), text.c_str());
	if (closable) {
		if (over_close && hovered) dl->AddRectFilled(close_bb.Min, close_bb.Max, rgb(0xffffff, 25), px(4));
		ImVec2 c = close_bb.GetCenter();
		float r = px(4);
		unsigned cc = rgb(active || hovered ? kTextSoft : kTextMuted);
		dl->AddLine(c - ImVec2(r, r), c + ImVec2(r, r), cc, px(1.4f));
		dl->AddLine(c + ImVec2(-r, r), c + ImVec2(r, -r), cc, px(1.4f));
		if (pressed && over_close) {
			*close = true;
			return false;
		}
	}
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && closable) *close = true;
	return pressed;
}

void form_label(const char *label, float label_width, float field_height) {
	ImVec2 pos = ImGui::GetCursorScreenPos();
	float h = field_height > 0 ? field_height : ImGui::GetFrameHeight();
	float y = pos.y + std::min(h, ImGui::GetFrameHeight()) * 0.5f - ImGui::GetFontSize() * 0.5f;
	ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, y), rgb(kLabel), label);
	ImGui::SetCursorScreenPos(ImVec2(pos.x + label_width, pos.y));
}

bool begin_combo(const char *id, const char *preview, int flags) {
	bool open = ImGui::BeginCombo(id, preview, flags | ImGuiComboFlags_NoArrowButton);
	ImGuiContext &g = *GImGui;
	ImRect r = open ? g.LastItemData.Rect : ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
	ImGuiWindow *w = open ? g.CurrentWindow->ParentWindow : ImGui::GetCurrentWindow();
	if (w) {
		ImVec2 cs = text_size(ICON_CHEVRON_DOWN);
		w->DrawList->AddText(ImVec2(r.Max.x - cs.x - px(10), r.Min.y + (r.GetHeight() - cs.y) * 0.5f + px(1)), rgb(kTextDim), ICON_CHEVRON_DOWN);
	}
	return open;
}

bool combo(const char *id, int *current, const char *const items[], int count) {
	bool changed = false;
	if (begin_combo(id, items[*current])) {
		for (int k = 0; k < count; ++k) {
			if (ImGui::Selectable(items[k], k == *current)) *current = k, changed = true;
		}
		ImGui::EndCombo();
	}
	return changed;
}

bool checkbox(const char *id_text, bool *v, bool enabled) {
	if (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) enabled = false;
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	const ImGuiID id = w->GetID(id_text);
	const float s = px(22);
	ImVec2 pos = w->DC.CursorPos;
	pos.y += (ImGui::GetFrameHeight() - s) * 0.5f;
	ImRect bb(pos, pos + ImVec2(s, s));
	ImGui::ItemSize(ImVec2(s, ImGui::GetFrameHeight()));
	if (!ImGui::ItemAdd(bb, id)) return false;
	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0);
	if (pressed && enabled) *v = !*v;
	ImDrawList *dl = w->DrawList;
	if (*v) {
		dl->AddRectFilled(bb.Min, bb.Max, rgb(kAccent, enabled ? 255 : 120), px(5));
		ImVec2 a = bb.Min + ImVec2(s * 0.24f, s * 0.52f), b = bb.Min + ImVec2(s * 0.43f, s * 0.70f), c = bb.Min + ImVec2(s * 0.76f, s * 0.31f);
		dl->PathLineTo(a);
		dl->PathLineTo(b);
		dl->PathLineTo(c);
		dl->PathStroke(rgb(0xffffff), 0, px(2.2f));
	} else {
		dl->AddRectFilled(bb.Min, bb.Max, rgb(kInput), px(5));
		dl->AddRect(bb.Min, bb.Max, rgb(hovered && enabled ? 0x5a5c70 : kInputBorder), px(5), 0, px(1.2f));
	}
	return pressed && enabled;
}

bool search_box(const char *id, const char *hint, std::string *text, float width) {
	ImGui::PushID(id);
	ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::SetNextItemWidth(width);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(30), ImGui::GetStyle().FramePadding.y));
	bool changed = ImGui::InputTextWithHint("##search", hint, text);
	ImGui::PopStyleVar();
	ImGui::GetWindowDrawList()->AddText(pos + ImVec2(px(9), ImGui::GetStyle().FramePadding.y), rgb(kTextMuted), ICON_SEARCH);
	ImGui::PopID();
	return changed;
}

void begin_panel(const char *id, ImVec2 size, unsigned bg, bool border, float rounding, ImVec2 padding) {
	ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(bg));
	ImGui::PushStyleColor(ImGuiCol_Border, vec(kBorder));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(rounding));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, border ? 1.0f : 0.0f);
	ImGui::BeginChild(id, size, border ? ImGuiChildFlags_Borders : 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void end_panel() {
	ImGui::EndChild();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
}

void status_chip(const char *icon, const char *text, unsigned colour) {
	ImGui::PushStyleColor(ImGuiCol_Text, vec(colour));
	if (icon) {
		ImGui::TextUnformatted(icon);
		ImGui::SameLine(0, px(6));
	}
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
}

void dim_text(const char *text, unsigned colour) {
	ImGui::PushStyleColor(ImGuiCol_Text, vec(colour ? colour : kTextMuted));
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
}

void badge(const char *text, unsigned bg, unsigned fg) {
	ImGui::PushFont(theme::fonts.semibold, kFontSmall - 2);
	ImVec2 ts = text_size(text);
	ImVec2 size = ts + ImVec2(px(12), px(6));
	ImVec2 p = ImGui::GetCursorScreenPos();
	float dy = std::max(0.0f, (ImGui::GetTextLineHeight() - size.y) * 0.5f);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p + ImVec2(0, dy), p + ImVec2(0, dy) + size, rgb(bg), px(4));
	dl->AddText(p + ImVec2(px(6), dy + px(3)), rgb(fg), text);
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(size.x, std::max(size.y, ImGui::GetTextLineHeight())));
}

namespace {

bool word_char(char c) { return std::isalnum(uint8_t(c)) || c == '_'; }

bool is_keyword(const std::string &w) {
	static const char *const kw[] = {"if", "else", "while", "for", "do", "return", "break", "continue", "switch", "case", "default",
	                                 "true", "false", "null", "this", "param", "rparam", "instance", "final", "global", "new",
	                                 "delete", "int", "float", "bool", "string", "void", "class", "import", "static", "const"};
	for (const char *k : kw) {
		if (w == k) return true;
	}
	return false;
}

// Colours one line; `in_comment` carries a /* */ comment across lines.
void draw_code_line(ImDrawList *dl, ImVec2 p, const std::string &line, bool *in_comment) {
	const unsigned kComment = 0x6a9955, kString = 0xce9178, kNumber = 0xb5cea8, kKeyword = 0xc586c0, kType = 0x4ec9b0,
	               kMember = 0x9cdcfe, kPlain = 0xd4d4d4;
	float x = p.x;
	size_t i = 0;
	auto emit = [&](size_t from, size_t to, unsigned colour) {
		if (to <= from) return;
		const char *a = line.c_str() + from, *b = line.c_str() + to;
		dl->AddText(ImVec2(x, p.y), rgb(colour), a, b);
		x += ImGui::CalcTextSize(a, b).x;
	};
	while (i < line.size()) {
		if (*in_comment) {
			size_t end = line.find("*/", i);
			size_t stop = end == std::string::npos ? line.size() : end + 2;
			emit(i, stop, kComment);
			i = stop;
			if (end != std::string::npos) *in_comment = false;
			continue;
		}
		char c = line[i];
		if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
			emit(i, line.size(), kComment);
			break;
		}
		if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
			*in_comment = true;
			continue;
		}
		if (c == '"' || c == '\'') {
			size_t j = i + 1;
			while (j < line.size() && line[j] != c) j += line[j] == '\\' ? 2 : 1;
			j = std::min(line.size(), j + 1);
			emit(i, j, kString);
			i = j;
			continue;
		}
		if (std::isdigit(uint8_t(c))) {
			size_t j = i;
			while (j < line.size() && (std::isalnum(uint8_t(line[j])) || line[j] == '.')) ++j;
			emit(i, j, kNumber);
			i = j;
			continue;
		}
		if (word_char(c)) {
			size_t j = i;
			while (j < line.size() && word_char(line[j])) ++j;
			std::string w = line.substr(i, j - i);
			unsigned colour = kPlain;
			if (is_keyword(w)) colour = kKeyword;
			else if (w.size() > 1 && (w[0] == 'C' || w[0] == 'E' || w[0] == 'B') && std::isupper(uint8_t(w[1]))) colour = kType;
			else if (w.size() > 2 && (w[0] == 'm' || w[0] == 'g') && w[1] == '_') colour = kMember;
			emit(i, j, colour);
			i = j;
			continue;
		}
		size_t j = i + 1;
		while (j < line.size() && !word_char(line[j]) && line[j] != '"' && line[j] != '/' && line[j] != '\'') ++j;
		emit(i, j, kPlain);
		i = j;
	}
}

}  // namespace

void code_view(const char *id, const std::vector<std::string> &lines, ImVec2 size) {
	ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(0x16171d));
	ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::PushFont(theme::fonts.mono, kFontSmall);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	const float line_h = ImGui::GetTextLineHeight() + px(3);
	const float gutter = ImGui::CalcTextSize("00000").x + px(14);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	std::vector<uint8_t> state(lines.size() + 1, 0);
	for (size_t k = 0; k < lines.size(); ++k) {
		bool in = state[k];
		const std::string &l = lines[k];
		for (size_t i = 0; i + 1 < l.size(); ++i) {
			if (!in && l[i] == '/' && l[i + 1] == '/') break;
			if (!in && l[i] == '/' && l[i + 1] == '*') in = true, ++i;
			else if (in && l[i] == '*' && l[i + 1] == '/') in = false, ++i;
		}
		state[k + 1] = in;
	}
	float widest = 0;
	ImGuiListClipper clip;
	clip.Begin(int(lines.size()), line_h);
	while (clip.Step()) {
		for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
			ImVec2 p = ImGui::GetCursorScreenPos();
			char num[16];
			snprintf(num, sizeof num, "%d", k + 1);
			float nw = ImGui::CalcTextSize(num).x;
			dl->AddText(ImVec2(p.x + gutter - px(14) - nw, p.y), rgb(kTextFaint), num);
			bool in = state[size_t(k)];
			draw_code_line(dl, ImVec2(p.x + gutter, p.y), lines[size_t(k)], &in);
			widest = std::max(widest, ImGui::CalcTextSize(lines[size_t(k)].c_str()).x);
			ImGui::Dummy(ImVec2(gutter + widest, line_h));
		}
	}
	ImGui::PopStyleVar();
	ImGui::PopFont();
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void centered_note(const char *icon, const char *title, const char *body) {
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float wrap = std::min(avail.x - px(40), px(420));
	ImGui::PushFont(nullptr, 34);
	ImVec2 is = text_size(icon);
	ImGui::PopFont();
	ImGui::PushFont(theme::fonts.semibold, kFontUi + 1);
	ImVec2 ts = text_size(title);
	ImGui::PopFont();
	ImVec2 bs = ImGui::CalcTextSize(body, nullptr, false, wrap);
	float total = is.y + px(12) + ts.y + px(8) + bs.y;
	float y = origin.y + std::max(px(10), (avail.y - total) * 0.5f);
	ImGui::PushFont(nullptr, 34);
	dl->AddText(ImVec2(origin.x + (avail.x - is.x) * 0.5f, y), rgb(0x55576a), icon);
	ImGui::PopFont();
	y += is.y + px(12);
	ImGui::PushFont(theme::fonts.semibold, kFontUi + 1);
	dl->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, y), rgb(kTextSoft), title);
	ImGui::PopFont();
	y += ts.y + px(8);
	ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - std::min(wrap, bs.x)) * 0.5f, y));
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
	ImGui::PushStyleColor(ImGuiCol_Text, vec(kTextMuted));
	ImGui::TextUnformatted(body);
	ImGui::PopStyleColor();
	ImGui::PopTextWrapPos();
}

}  // namespace ui
