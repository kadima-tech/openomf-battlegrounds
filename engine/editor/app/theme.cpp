#include "theme.h"

#include "icons.h"

extern const unsigned char font_inter_regular[], font_inter_semibold[], font_jetbrains_mono[], font_lucide[];
extern const unsigned int font_inter_regular_size, font_inter_semibold_size, font_jetbrains_mono_size, font_lucide_size;

namespace theme {

Fonts fonts;

namespace {

ImFont *add(const unsigned char *data, unsigned int size, float px, bool icons) {
	ImGuiIO &io = ImGui::GetIO();
	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;
	cfg.OversampleH = 2;
	// Inter has its own private-use glyphs; the icons must win there.
	static const ImWchar no_pua[] = {ICON_MIN, ICON_MAX, 0};
	if (icons) cfg.GlyphExcludeRanges = no_pua;
	ImFont *f = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(data), int(size), px, &cfg);
	if (icons) {
		static const ImWchar range[] = {ICON_MIN, ICON_MAX, 0};
		ImFontConfig ic;
		ic.FontDataOwnedByAtlas = false;
		ic.MergeMode = true;
		ic.PixelSnapH = true;
		ic.GlyphOffset = ImVec2(0, 2.5f);
		ic.GlyphMinAdvanceX = px * 1.15f;
		io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(font_lucide), int(font_lucide_size), px, &ic, range);
	}
	return f;
}

}  // namespace

void load_fonts() {
	fonts.ui = add(font_inter_regular, font_inter_regular_size, kFontUi, true);
	fonts.semibold = add(font_inter_semibold, font_inter_semibold_size, kFontUi, true);
	fonts.mono = add(font_jetbrains_mono, font_jetbrains_mono_size, kFontUi, false);
}

void apply_style(float scale) {
	ImGuiStyle &s = ImGui::GetStyle();
	s = ImGuiStyle();
	s.WindowPadding = ImVec2(10, 10);
	s.FramePadding = ImVec2(10, 7);
	s.CellPadding = ImVec2(8, 6);
	s.ItemSpacing = ImVec2(8, 8);
	s.ItemInnerSpacing = ImVec2(8, 6);
	s.IndentSpacing = 18;
	s.ScrollbarSize = 12;
	s.GrabMinSize = 10;
	s.WindowBorderSize = 1;
	s.ChildBorderSize = 1;
	s.PopupBorderSize = 1;
	s.FrameBorderSize = 1;
	s.TabBorderSize = 0;
	s.TabBarBorderSize = 1;
	s.TabBarOverlineSize = 0;
	s.WindowRounding = 6;
	s.ChildRounding = 8;
	s.FrameRounding = 5;
	s.PopupRounding = 6;
	s.ScrollbarRounding = 6;
	s.GrabRounding = 4;
	s.TabRounding = 5;
	s.WindowTitleAlign = ImVec2(0, 0.5f);
	s.WindowMenuButtonPosition = ImGuiDir_None;
	s.DockingSeparatorSize = 2;
	s.SeparatorTextBorderSize = 1;

	ImVec4 *c = s.Colors;
	c[ImGuiCol_Text] = vec(kText);
	c[ImGuiCol_TextDisabled] = vec(kTextFaint);
	c[ImGuiCol_WindowBg] = vec(kPanel);
	c[ImGuiCol_ChildBg] = vec(kPanel, 0);
	c[ImGuiCol_PopupBg] = vec(0x23242e);
	c[ImGuiCol_Border] = vec(kBorder);
	c[ImGuiCol_BorderShadow] = vec(0, 0);
	c[ImGuiCol_FrameBg] = vec(kInput);
	c[ImGuiCol_FrameBgHovered] = vec(0x1b1c24);
	c[ImGuiCol_FrameBgActive] = vec(0x1e1f28);
	c[ImGuiCol_TitleBg] = vec(kPanelHead);
	c[ImGuiCol_TitleBgActive] = vec(kPanelHead);
	c[ImGuiCol_TitleBgCollapsed] = vec(kPanelHead);
	c[ImGuiCol_MenuBarBg] = vec(kChrome);
	c[ImGuiCol_ScrollbarBg] = vec(kPanel, 0);
	c[ImGuiCol_ScrollbarGrab] = vec(0x3a3b48);
	c[ImGuiCol_ScrollbarGrabHovered] = vec(0x4a4b5c);
	c[ImGuiCol_ScrollbarGrabActive] = vec(0x5a5b6e);
	c[ImGuiCol_CheckMark] = vec(0xffffff);
	c[ImGuiCol_SliderGrab] = vec(kAccent);
	c[ImGuiCol_SliderGrabActive] = vec(0x8577f0);
	c[ImGuiCol_Button] = vec(kButton);
	c[ImGuiCol_ButtonHovered] = vec(kButtonHover);
	c[ImGuiCol_ButtonActive] = vec(kButtonActive);
	c[ImGuiCol_Header] = vec(kSelection);
	c[ImGuiCol_HeaderHovered] = vec(0x2b2a3f);
	c[ImGuiCol_HeaderActive] = vec(kSelection);
	c[ImGuiCol_Separator] = vec(kDivider);
	c[ImGuiCol_SeparatorHovered] = vec(kAccentLine);
	c[ImGuiCol_SeparatorActive] = vec(kAccent);
	c[ImGuiCol_ResizeGrip] = vec(0, 0);
	c[ImGuiCol_ResizeGripHovered] = vec(kAccentLine, 0.6f);
	c[ImGuiCol_ResizeGripActive] = vec(kAccent);
	c[ImGuiCol_InputTextCursor] = vec(kText);
	c[ImGuiCol_Tab] = vec(kPanelHead);
	c[ImGuiCol_TabHovered] = vec(0x2b2c38);
	c[ImGuiCol_TabSelected] = vec(kPanel);
	c[ImGuiCol_TabSelectedOverline] = vec(kAccentText);
	c[ImGuiCol_TabDimmed] = vec(kPanelHead);
	c[ImGuiCol_TabDimmedSelected] = vec(kPanel);
	c[ImGuiCol_TabDimmedSelectedOverline] = vec(kAccentText, 0);
	c[ImGuiCol_DockingPreview] = vec(kAccent, 0.5f);
	c[ImGuiCol_DockingEmptyBg] = vec(kGround);
	c[ImGuiCol_PlotHistogram] = vec(kAccent);
	c[ImGuiCol_TableHeaderBg] = vec(kPanelHead);
	c[ImGuiCol_TableBorderStrong] = vec(kBorder);
	c[ImGuiCol_TableBorderLight] = vec(kDivider);
	c[ImGuiCol_TableRowBg] = vec(0, 0);
	c[ImGuiCol_TableRowBgAlt] = vec(0xffffff, 0.015f);
	c[ImGuiCol_TextSelectedBg] = vec(kAccent, 0.45f);
	c[ImGuiCol_DragDropTarget] = vec(kAccent);
	c[ImGuiCol_NavCursor] = vec(kAccent);
	c[ImGuiCol_ModalWindowDimBg] = vec(0x000000, 0.55f);
	s.ScaleAllSizes(scale);
	s.FontScaleDpi = scale;
	s.FontSizeBase = kFontUi;
}

}  // namespace theme
