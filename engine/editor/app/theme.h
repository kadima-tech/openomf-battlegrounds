// The editor's look (DESIGN.md, "Content editor"): Unreal Engine 5's dark charcoal with a violet
// accent, Inter for the interface, JetBrains Mono for code and numbers, Lucide icons.
#pragma once

#include "imgui.h"

namespace theme {

constexpr ImU32 rgb(unsigned hex, unsigned alpha = 255) {
	return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, alpha);
}
inline ImVec4 vec(unsigned hex, float alpha = 1) {
	return ImVec4(float((hex >> 16) & 255) / 255.0f, float((hex >> 8) & 255) / 255.0f, float(hex & 255) / 255.0f, alpha);
}

// Palette.
constexpr unsigned kChrome = 0x141519;     // title, menu and tab strips, status bar
constexpr unsigned kGround = 0x191a21;     // window background
constexpr unsigned kPanel = 0x1d1e26;      // panels
constexpr unsigned kPanelHead = 0x1f2028;  // panel tab strips, footers
constexpr unsigned kViewport = 0x121318;
constexpr unsigned kBorder = 0x33343f;
constexpr unsigned kDivider = 0x2c2d38;
constexpr unsigned kInput = 0x16171d;
constexpr unsigned kInputBorder = 0x3b3d4b;
constexpr unsigned kButton = 0x2c2d38;
constexpr unsigned kButtonHover = 0x373847;
constexpr unsigned kButtonActive = 0x424356;
constexpr unsigned kText = 0xe8e8f0;
constexpr unsigned kTextSoft = 0xd3d5e0;
constexpr unsigned kLabel = 0xc3c5d3;
constexpr unsigned kTextDim = 0xa9acbd;
constexpr unsigned kTextMuted = 0x9295a6;
constexpr unsigned kTextFaint = 0x6c6f80;
constexpr unsigned kAccent = 0x6b5ce7;       // checkboxes, focus
constexpr unsigned kAccentLine = 0x5a4fc0;   // tab underline
constexpr unsigned kAccentText = 0x8b7cf6;   // logo, links
constexpr unsigned kSelection = 0x3a3566;    // selected rows, active form tab
constexpr unsigned kDocTabActive = 0x2e2a52; // active document tab
constexpr unsigned kPlay = 0x2f8f4e;
constexpr unsigned kPlayHover = 0x37a05a;
constexpr unsigned kNew = 0x2f8f4e;          // NEW badge
constexpr unsigned kWarning = 0xffc98a;
constexpr unsigned kWarningStrong = 0xe8a33d;
constexpr unsigned kOk = 0x8fe3a8;
constexpr unsigned kError = 0xff8f8f;

// Sizes in unscaled pixels.
constexpr float kFontUi = 14.0f;
constexpr float kFontSmall = 13.0f;
constexpr float kFontHeading = 24.0f;
constexpr float kMenuHeight = 32.0f;
constexpr float kDocTabsHeight = 40.0f;
constexpr float kToolbarHeight = 48.0f;
constexpr float kStatusHeight = 28.0f;

struct Fonts {
	ImFont *ui = nullptr;        // Inter Regular + icons
	ImFont *semibold = nullptr;  // Inter SemiBold + icons
	ImFont *mono = nullptr;      // JetBrains Mono
};
extern Fonts fonts;

void load_fonts();
// `scale` is the display's content scale (1 on macOS, where Retina comes from the framebuffer).
void apply_style(float scale);

}  // namespace theme
