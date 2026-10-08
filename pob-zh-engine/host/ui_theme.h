#pragma once

#include <cstdint>

struct ImVec4;
// Declared at global scope on purpose: a `struct ImGuiStyle&` written inside
// namespace PobUi would declare PobUi::ImGuiStyle, a brand new incomplete type
// that has nothing to do with ImGui's.
struct ImGuiStyle;

namespace PobUi {

// Design-system colour tokens (PobTools design system, tokens.json), packed the
// way IM_COL32 packs them (R in the low byte) so they can go straight into a draw
// list. One value per meaning: the dozen hand-typed status colours that used to
// be scattered through launcher_ui.cpp are these now.
namespace Tok {
constexpr std::uint32_t Rgb(unsigned r, unsigned g, unsigned b, unsigned a = 255)
{
	return (std::uint32_t)((a << 24) | (b << 16) | (g << 8) | r);
}
inline constexpr std::uint32_t Bg            = Rgb(0x0b, 0x10, 0x14);
inline constexpr std::uint32_t Surface1      = Rgb(0x0f, 0x16, 0x1b);
inline constexpr std::uint32_t Surface2      = Rgb(0x14, 0x1d, 0x23);
inline constexpr std::uint32_t Surface3      = Rgb(0x1c, 0x28, 0x2f);
inline constexpr std::uint32_t SurfaceRaised = Rgb(0x11, 0x19, 0x1f);
inline constexpr std::uint32_t Border        = Rgb(0x2b, 0x39, 0x42);
inline constexpr std::uint32_t BorderSubtle  = Rgb(0x22, 0x2e, 0x36);
inline constexpr std::uint32_t BorderStrong  = Rgb(0x3b, 0x4e, 0x58);
inline constexpr std::uint32_t Text          = Rgb(0xed, 0xf3, 0xf5);
inline constexpr std::uint32_t TextMuted     = Rgb(0x88, 0x99, 0xa2);
inline constexpr std::uint32_t TextFaint     = Rgb(0x66, 0x75, 0x7e);
inline constexpr std::uint32_t Accent        = Rgb(0x63, 0x66, 0xf1);
inline constexpr std::uint32_t AccentHover   = Rgb(0x4f, 0x46, 0xe5);
inline constexpr std::uint32_t AccentText    = Rgb(0xa5, 0xb4, 0xfc);
inline constexpr std::uint32_t AccentSoft    = Rgb(0x25, 0x28, 0x4a);
inline constexpr std::uint32_t OnAccent      = Rgb(0xff, 0xff, 0xff);
inline constexpr std::uint32_t Success       = Rgb(0x66, 0xd3, 0x8f);
inline constexpr std::uint32_t SuccessSoft   = Rgb(0x12, 0x27, 0x1c);
inline constexpr std::uint32_t Warning       = Rgb(0xe8, 0xb5, 0x5b);
inline constexpr std::uint32_t WarningSoft   = Rgb(0x2a, 0x23, 0x12);
inline constexpr std::uint32_t Danger        = Rgb(0xef, 0x69, 0x6f);
inline constexpr std::uint32_t DangerSoft    = Rgb(0x2c, 0x15, 0x17);
inline constexpr std::uint32_t DangerFill    = Rgb(0x65, 0x2b, 0x2f);
inline constexpr std::uint32_t DangerFillHover = Rgb(0x85, 0x33, 0x39);  // PushDangerButton's hover
inline constexpr std::uint32_t OnDanger      = Rgb(0xff, 0xeb, 0xec);
inline constexpr std::uint32_t Update        = Rgb(0x81, 0x8c, 0xf8);
inline constexpr std::uint32_t Scrim         = Rgb(0x00, 0x00, 0x00, 0xa0);
inline constexpr std::uint32_t Canvas        = Rgb(0x08, 0x0a, 0x0c);
// Banner outlines (component styles, not tokens of their own in tokens.json).
inline constexpr std::uint32_t BannerWarnEdge = Rgb(0x4a, 0x3c, 0x1c);
inline constexpr std::uint32_t BannerBadEdge  = Rgb(0x4d, 0x24, 0x27);
// Canvas nodes shared by the atlas planner and the timeless-jewel view.
inline constexpr std::uint32_t TreeKeystone  = Rgb(0xd9, 0x73, 0xd9);
inline constexpr std::uint32_t TreeNotable   = Rgb(0xf2, 0xcc, 0x66);
inline constexpr std::uint32_t TreeSocket    = Rgb(0x6f, 0xa8, 0xff);
inline constexpr std::uint32_t TreeLink      = Rgb(0x5a, 0x5f, 0x6e);
inline constexpr std::uint32_t TreeLinkOn    = Rgb(0x74, 0xca, 0xf4);
inline constexpr std::uint32_t TreeAdd       = Rgb(0x66, 0xd3, 0x8f);
inline constexpr std::uint32_t TreeRemove    = Rgb(0xef, 0x69, 0x6f);
inline constexpr std::uint32_t TreeHit       = TreeNotable;
// The two kinds the atlas planner lists beside keystones and notables (2026-10-08).
inline constexpr std::uint32_t TreeWormhole  = Rgb(0x8c, 0xcc, 0xf2);
inline constexpr std::uint32_t TreeSmall     = Rgb(0xb8, 0xc2, 0xd1);
// The game's eleven beam / minimap colour tokens and its rarity colours, as the
// filter editor shows them (2026-10-09 design: FilterDetail palette).
inline constexpr std::uint32_t FxRed         = Rgb(0xe0, 0x40, 0x40);
inline constexpr std::uint32_t FxGreen       = Rgb(0x40, 0xc0, 0x60);
inline constexpr std::uint32_t FxBlue        = Rgb(0x40, 0x80, 0xe0);
inline constexpr std::uint32_t FxWhite       = Rgb(0xe0, 0xe0, 0xe0);
inline constexpr std::uint32_t FxYellow      = Rgb(0xe0, 0xc0, 0x40);
inline constexpr std::uint32_t FxCyan        = Rgb(0x40, 0xd0, 0xd0);
inline constexpr std::uint32_t FxGrey        = Rgb(0xa0, 0xa0, 0xa0);
inline constexpr std::uint32_t FxPink        = Rgb(0xe0, 0x80, 0xc0);
inline constexpr std::uint32_t FxOrange      = Rgb(0xe0, 0x80, 0x40);
inline constexpr std::uint32_t FxPurple      = Rgb(0xa0, 0x60, 0xe0);
inline constexpr std::uint32_t FxBrown       = Rgb(0x80, 0x60, 0x40);
inline constexpr std::uint32_t RarityMagic   = Rgb(0x88, 0x88, 0xff);
inline constexpr std::uint32_t RarityRare    = Rgb(0xd6, 0xb5, 0x6a);
inline constexpr std::uint32_t RarityUnique  = Rgb(0xaf, 0x60, 0x25);
// The translation editor's source-file badges (2026-10-09 design, te.css .src.a-d):
// a file keeps its hue by a hash of its name.
inline constexpr std::uint32_t SrcBlueBg     = Rgb(0x1e, 0x2a, 0x44);
inline constexpr std::uint32_t SrcBlueFg     = Rgb(0x9f, 0xb8, 0xff);
inline constexpr std::uint32_t SrcPurpleBg   = Rgb(0x2a, 0x21, 0x40);
inline constexpr std::uint32_t SrcPurpleFg   = Rgb(0xc9, 0xa8, 0xff);
inline constexpr std::uint32_t SrcGreenBg    = Rgb(0x1f, 0x33, 0x28);
inline constexpr std::uint32_t SrcGreenFg    = Rgb(0x8f, 0xd6, 0xa6);
inline constexpr std::uint32_t SrcAmberBg    = Rgb(0x3a, 0x2a, 0x1a);
inline constexpr std::uint32_t SrcAmberFg    = Rgb(0xe8, 0xb5, 0x5b);
} // namespace Tok

// Node kinds on a passive-style canvas, and the one colour each is drawn and
// listed in. Shared so the atlas planner and the timeless-jewel view cannot
// drift apart again (each used to carry its own copy of the hues).
enum class TreeKind { Keystone, Wormhole, Notable, Small, Socket };
std::uint32_t TreeKindColor(TreeKind kind);

// A token as the float colour ImGui's style API takes.
ImVec4 TokV4(std::uint32_t c);

enum class Density {
	Comfortable,
	Compact,
	Canvas,
};

enum class StatusTone {
	Neutral,
	Success,
	Warning,
	Error,
};

void ApplyTheme(float scale, Density density = Density::Comfortable);

// Fill a style from scratch, without touching the active one.
//
// Exists so a container can hold several densities at once and swap between them
// per tab: an embedded tool draws at its own density inside a launcher drawn at
// another. ApplyTheme cannot be used for that -- it ends in ScaleAllSizes, which
// COMPOUNDS if applied to an already-scaled style, so calling it per frame would
// grow every padding without bound.
//
// Two rules for swapping, both of which the tab body satisfies naturally:
//   * only while the style-var stack is empty. PushStyleVar records the OLD value,
//     so replacing the whole style underneath makes PopStyleVar restore garbage.
//   * only between widgets, never mid-window.
void BuildStyle(::ImGuiStyle& out, float scale, Density density);

void PushPrimaryButton();
void PushDangerButton();
void PopButtonStyle();

// Glyph ranges every ImGui host adds to its body face on top of Default +
// ChineseFull (+ Korean): the Arrows and Mathematical Operators blocks. Neither
// is in ImGui's CJK table, so "≥ ≤ ⇐ →" used to come out of the atlas as '?'
// whatever the font -- FZ_ZY and both Noto faces all carry ≥ / ≤ (the shipped
// fonts are merged as fallbacks, so a gap in one is filled by another).
// ImWchar is 16-bit in this build (static_assert in ui_theme.cpp).
const unsigned short* SymbolGlyphRanges();

ImVec4 Accent();
ImVec4 MutedText();
ImVec4 StatusColor(StatusTone tone);

// Creates an ImGui context, verifies the shared style at two scales, and
// destroys the context. This stays renderer-free so CI can run it headlessly.
bool RunThemeSelfTest();

} // namespace PobUi
