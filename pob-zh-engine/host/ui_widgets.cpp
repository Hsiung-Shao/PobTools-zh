#include "ui_widgets.h"
#include "ui_theme.h"
#include "ui_icons.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>   // ImDrawListSplitter lives in imgui.h; GetCurrentWindow for wrap maths

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace PobUi {

namespace {

WidgetFonts g_fonts;

// Tool panels draw denser than the launcher (the design's compact density):
// every design px is scaled by this while a ToolDensityScope is open.
float g_density = 1.0f;
std::string g_testOpenSelect;   // TestOpenSelect
bool Compact() { return g_density < 1.0f; }
// Card / row padding: space-5 x space-4 in the launcher, space-3 in tools.
float CardPadX() { return Compact() ? D(14.0f) : D(20.0f); }
float CardPadY() { return Compact() ? D(14.0f) : D(16.0f); }

// The launcher's body face is 19 px where the design's is 16.
constexpr float kDesignToImGui = 19.0f / 16.0f;

ImFont* BodyFont() { return g_fonts.body ? g_fonts.body : ImGui::GetFont(); }
ImFont* SmallFont() { return g_fonts.small ? g_fonts.small : BodyFont(); }
ImFont* HeadingFont() { return g_fonts.heading ? g_fonts.heading : BodyFont(); }
float BodyPx() { return g_fonts.body ? g_fonts.body->FontSize : ImGui::GetFontSize(); }
float SmallPx() { return g_fonts.small ? g_fonts.small->FontSize : BodyPx(); }
float HeadingPx() { return g_fonts.headingPx > 0.0f ? g_fonts.headingPx : (g_fonts.heading ? g_fonts.heading->FontSize : BodyPx()); }

ImVec2 TextSize(ImFont* f, float px, const char* text, float wrap = 0.0f)
{
	if (!text || !*text) return ImVec2(0, 0);
	const char* end = ImGui::FindRenderedTextEnd(text);
	return f->CalcTextSizeA(px, FLT_MAX, wrap, text, end);
}

void DrawText(ImDrawList* dl, ImFont* f, float px, ImVec2 pos, ImU32 col, const char* text, float wrap = 0.0f)
{
	if (!text || !*text) return;
	const char* end = ImGui::FindRenderedTextEnd(text);
	dl->AddText(f, px, pos, col, text, end, wrap);
}

// While a SettingRow is open: the height every inline control box takes, so a
// 22 px switch and a 28 px button sit centred on the same line as a 36 px select.
float g_lineBoxH = 0.0f;

float BoxH(float own) { return g_lineBoxH > own ? g_lineBoxH : own; }

struct CardState {
	ImDrawListSplitter split;
	ImDrawList* dl = nullptr;
	ImVec2 pos;
	float width = 0.0f;
	bool padded = false;
	float minHeight = 0.0f;
	bool anyRow = false;   // a row was already drawn: the next one gets a divider
	bool head = false;
	float headBottom = 0.0f;
};
std::vector<CardState*> g_cards;
float g_cardNatural = 0.0f;   // CardNaturalHeight: the last card CardEnd closed

struct RowState {
	ImVec2 pos;
	float width = 0.0f, height = 0.0f;
	float prevLineBox = 0.0f;
	bool open = false;
};
RowState g_row;

// Toast
std::string g_toastText;
Tone g_toastTone = Tone::Ok;
double g_toastAt = -100.0;
constexpr double kToastSecs = 4.0;
constexpr double kToastFade = 0.25;

ImU32 ToneFg(Tone t)
{
	switch (t) {
		case Tone::Ok:   return Tok::Success;
		case Tone::Run:  return Tok::AccentText;
		case Tone::Warn: return Tok::Warning;
		case Tone::Bad:  return Tok::Danger;
		default:         return Tok::TextMuted;
	}
}
ImU32 ToneBg(Tone t)
{
	switch (t) {
		case Tone::Ok:   return Tok::SuccessSoft;
		case Tone::Run:  return Tok::AccentSoft;
		case Tone::Warn: return Tok::WarningSoft;
		case Tone::Bad:  return Tok::DangerSoft;
		default:         return Tok::Surface2;
	}
}

ImU32 WithAlpha(ImU32 c, float a)
{
	const unsigned alpha = (unsigned)((float)((c >> 24) & 0xFF) * a);
	return (c & 0x00FFFFFFu) | (alpha << 24);
}

} // namespace

void SetWidgetFonts(const WidgetFonts& f) { g_fonts = f; }
const WidgetFonts& Fonts() { return g_fonts; }

float D(float px) { return px * kDesignToImGui * g_fonts.scale * g_density; }

ToolDensityScope::ToolDensityScope() : prev_(g_density) { g_density = kToolDensity; }
ToolDensityScope::~ToolDensityScope() { g_density = prev_; }
bool ToolDensity() { return Compact(); }
float ControlH() { return std::floor(D(36.0f)); }

void PushControlFrame()
{
	const float padY = std::max(1.0f, std::floor((ControlH() - ImGui::GetFontSize()) * 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(D(12.0f), padY));
}
void PopControlFrame() { ImGui::PopStyleVar(); }

// ---- text ---------------------------------------------------------------

void Hint(const char* text, float wrapWidth, std::uint32_t col)
{
	if (!text || !*text) return;
	ImGui::PushFont(SmallFont());
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col ? col : Tok::TextMuted));
	if (wrapWidth > 0.0f) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
	ImGui::TextUnformatted(text);
	if (wrapWidth > 0.0f) ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

void Heading(const char* text, std::uint32_t col)
{
	if (!text || !*text) return;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 ts = TextSize(HeadingFont(), HeadingPx(), text);
	const float lineH = std::max(ts.y, ImGui::GetTextLineHeight());
	ImGui::Dummy(ImVec2(ts.x, lineH));
	DrawText(ImGui::GetWindowDrawList(), HeadingFont(), HeadingPx(), ImVec2(p.x, p.y + std::floor((lineH - ts.y) * 0.5f)),
	         col ? col : Tok::Text, text);
}

void Overline(const char* text)
{
	ImGui::PushFont(SmallFont());
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::TextMuted));
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

void SectionHeader(const char* text, float width)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	Overline(text);
	const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
	const float y = std::floor((mn.y + mx.y) * 0.5f) + 0.5f;
	ImGui::GetWindowDrawList()->AddLine(ImVec2(mx.x + D(12.0f), y), ImVec2(p.x + width, y),
	                                    Tok::BorderSubtle, 1.0f);
}

void Numeric(const char* text, std::uint32_t col)
{
	ImGui::PushFont(SmallFont());
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col ? col : Tok::TextMuted));
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

void Tooltip(const char* text)
{
	if (!text || !*text) return;
	ImGui::BeginTooltip();
	ImGui::PushFont(SmallFont());
	ImGui::PushTextWrapPos(D(300.0f));
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopFont();
	ImGui::EndTooltip();
}

void InfoTip(const char* text)
{
	if (g_fonts.icons) {
		ImGui::PushFont(SmallFont());
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::TextFaint));
		ImGui::TextUnformatted(PobIcon::Info);
		ImGui::PopStyleColor();
		ImGui::PopFont();
	} else {
		ImGui::TextDisabled("(?)");
	}
	if (ImGui::IsItemHovered()) Tooltip(text);
}

bool Link(const char* label, std::uint32_t col)
{
	const ImVec2 sz = ImGui::CalcTextSize(label, nullptr, true);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool hovered = ImGui::IsMouseHoveringRect(p, p + sz) && ImGui::IsWindowHovered();
	ImGui::PushStyleColor(ImGuiCol_Text,
		ImGui::ColorConvertU32ToFloat4(hovered ? Tok::AccentText : (col ? col : Tok::TextMuted)));
	ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
	ImGui::PopStyleColor();
	bool clicked = false;
	if (ImGui::IsItemHovered()) {
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f),
		                                    Tok::AccentText, 1.0f);
		clicked = ImGui::IsMouseClicked(0);
	}
	return clicked;
}

// ---- icons --------------------------------------------------------------

float IconWidth(const char* icon, float px)
{
	if (!g_fonts.icons || !icon) return 0.0f;
	return BodyFont()->CalcTextSizeA(px, FLT_MAX, 0.0f, icon).x;
}

void IconAt(ImDrawList* dl, const ImVec2& pos, const char* icon, std::uint32_t col, float px)
{
	if (!g_fonts.icons || !icon) return;
	ImFont* f = px <= SmallPx() + 0.5f ? SmallFont() : BodyFont();
	dl->AddText(f, px, pos, col, icon);
}

void Icon(const char* icon, std::uint32_t col, float px)
{
	if (!g_fonts.icons || !icon) return;
	const float size = px > 0.0f ? px : ImGui::GetFontSize();
	const float w = IconWidth(icon, size);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float lineH = ImGui::GetTextLineHeight();
	ImGui::Dummy(ImVec2(w, lineH));
	IconAt(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + (lineH - size) * 0.5f), icon, col, size);
}

// ---- buttons ------------------------------------------------------------

namespace {
struct BtnMetrics { float h, padX, px; ImFont* font; };
BtnMetrics Metrics(BtnSize s)
{
	switch (s) {
		case BtnSize::Sm: return { std::floor(D(28.0f)), D(10.0f), SmallPx(), SmallFont() };
		case BtnSize::Lg: return { std::floor(D(44.0f)), D(22.0f), BodyPx(), BodyFont() };
		default:          return { ControlH(), D(16.0f), BodyPx(), BodyFont() };
	}
}
} // namespace

float ButtonWidth(const char* label, BtnSize size, const char* icon, float minWidth)
{
	const BtnMetrics m = Metrics(size);
	float w = TextSize(m.font, m.px, label).x + m.padX * 2.0f;
	const float iw = icon ? IconWidth(icon, m.px) : 0.0f;
	if (iw > 0.0f) w += iw + (TextSize(m.font, m.px, label).x > 0 ? D(8.0f) : 0.0f);
	return std::ceil(std::max(w, minWidth));
}

bool Button(const char* label, BtnKind kind, BtnSize size, const char* icon, float minWidth, bool enabled)
{
	const BtnMetrics m = Metrics(size);
	const float w = ButtonWidth(label, size, icon, minWidth);
	const float boxH = BoxH(m.h);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::BeginDisabled(!enabled);
	const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, boxH));
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();
	const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
	ImGui::EndDisabled();
	p.y += std::floor((boxH - m.h) * 0.5f);
	const ImVec2 q = p + ImVec2(w, m.h);

	ImU32 fill = Tok::Surface2, edge = Tok::Border, text = Tok::Text;
	switch (kind) {
		case BtnKind::Primary:
			fill = (hovered || held) ? Tok::AccentHover : Tok::Accent;
			edge = fill;
			text = Tok::OnAccent;
			break;
		case BtnKind::Ghost:
			fill = (hovered || held) ? Tok::Surface2 : 0;
			edge = 0;
			text = Tok::AccentText;
			break;
		case BtnKind::Danger:
			fill = (hovered || held) ? Tok::DangerFillHover : Tok::DangerFill;
			edge = fill;
			text = Tok::OnDanger;
			break;
		case BtnKind::Update:
			fill = (hovered || held) ? Tok::AccentSoft : Tok::Surface2;
			edge = Tok::Update;
			text = Tok::Update;
			break;
		default:
			if (hovered || held) { fill = Tok::Surface3; edge = Tok::BorderStrong; }
			break;
	}
	if (!enabled) {
		fill = kind == BtnKind::Ghost ? 0 : Tok::Surface1;
		edge = kind == BtnKind::Ghost ? 0 : Tok::BorderSubtle;
		text = Tok::TextFaint;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float r = D(5.0f);
	if (fill) dl->AddRectFilled(p, q, fill, r);
	if (edge) dl->AddRect(p, q, edge, r, 0, 1.0f);
	if (focused) dl->AddRect(p - ImVec2(3, 3), q + ImVec2(3, 3), Tok::Accent, r + 3.0f, 0, 2.0f);

	const float tw = TextSize(m.font, m.px, label).x;
	const float iw = icon ? IconWidth(icon, m.px) : 0.0f;
	const float gap = (iw > 0.0f && tw > 0.0f) ? D(8.0f) : 0.0f;
	float x = p.x + std::floor((w - (iw + gap + tw)) * 0.5f);
	const float ty = p.y + std::floor((m.h - m.px) * 0.5f);
	if (iw > 0.0f) {
		IconAt(dl, ImVec2(x, ty), icon, text, m.px);
		x += iw + gap;
	}
	DrawText(dl, m.font, m.px, ImVec2(x, ty), text, label);
	if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return clicked && enabled;
}

// ---- switch ---------------------------------------------------------------

float SwitchWidth() { return std::floor(D(40.0f)); }

bool Switch(const char* id, bool* value, bool enabled)
{
	const float w = SwitchWidth(), h = std::floor(D(22.0f));
	const float boxH = BoxH(h);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::BeginDisabled(!enabled);
	bool clicked = ImGui::InvisibleButton(id, ImVec2(w, boxH));
	const bool hovered = ImGui::IsItemHovered();
	const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
	ImGui::EndDisabled();
	if (clicked && enabled) *value = !*value;
	p.y += std::floor((boxH - h) * 0.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const bool on = *value;
	ImU32 track = on ? Tok::Accent : Tok::Border;
	if (hovered && enabled) track = on ? Tok::AccentHover : Tok::BorderStrong;
	if (!enabled) track = on ? WithAlpha(Tok::Accent, 0.45f) : Tok::BorderSubtle;
	dl->AddRectFilled(p, p + ImVec2(w, h), track, h * 0.5f);
	const float knob = std::floor(D(16.0f));
	const float inset = (h - knob) * 0.5f;
	const float kx = on ? p.x + w - inset - knob : p.x + inset;
	dl->AddCircleFilled(ImVec2(kx + knob * 0.5f, p.y + h * 0.5f), knob * 0.5f,
	                    on ? Tok::OnAccent : (enabled ? Tok::TextMuted : Tok::TextFaint), 20);
	if (focused) dl->AddRect(p - ImVec2(3, 3), p + ImVec2(w + 3, h + 3), Tok::Accent, h * 0.5f + 3.0f, 0, 2.0f);
	if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return clicked && enabled;
}

// ---- segmented ------------------------------------------------------------

namespace {
float SegItemW(const char* label) { return TextSize(SmallFont(), SmallPx(), label).x + D(28.0f); }
}

float SegmentedWidth(const char* const* labels, int count)
{
	float w = D(6.0f) + D(2.0f) * (float)(count > 0 ? count - 1 : 0);
	for (int i = 0; i < count; i++) w += SegItemW(labels[i]);
	return std::ceil(w);
}

bool Segmented(const char* id, int* selected, const char* const* labels, int count, bool enabled)
{
	if (enabled) return SegmentedEx(id, selected, labels, count, nullptr, nullptr);
	bool flags[16] = {};   // all false: the whole control is disabled
	return SegmentedEx(id, selected, labels, count > 16 ? 16 : count, flags, nullptr);
}

bool SegmentedEx(const char* id, int* selected, const char* const* labels, int count,
                 const bool* itemEnabled, const char* const* tips)
{
	ImGui::PushID(id);
	const float h = ControlH();
	const float boxH = BoxH(h);
	const float w = SegmentedWidth(labels, count);
	ImVec2 p = ImGui::GetCursorScreenPos();
	p.y += std::floor((boxH - h) * 0.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(w, h), Tok::Surface2, D(8.0f));
	dl->AddRect(p, p + ImVec2(w, h), Tok::Border, D(8.0f), 0, 1.0f);
	const ImVec2 start = ImGui::GetCursorScreenPos();
	bool changed = false;
	float x = p.x + D(3.0f);
	const float ih = h - D(6.0f);
	for (int i = 0; i < count; i++) {
		const float iw = SegItemW(labels[i]);
		ImGui::SetCursorScreenPos(ImVec2(x, p.y + D(3.0f)));
		ImGui::PushID(i);
		const bool itemOn = !itemEnabled || itemEnabled[i];
		ImGui::BeginDisabled(!itemOn);
		const bool click = ImGui::InvisibleButton("##seg", ImVec2(iw, ih));
		const bool hov = ImGui::IsItemHovered();
		const bool hovAny = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		ImGui::EndDisabled();
		ImGui::PopID();
		if (hovAny && tips && tips[i] && *tips[i]) Tooltip(tips[i]);
		const bool sel = (*selected == i);
		const ImVec2 a(x, p.y + D(3.0f)), b(x + iw, p.y + D(3.0f) + ih);
		if (sel) {
			dl->AddRectFilled(a, b, Tok::AccentSoft, D(5.0f));
			dl->AddRect(a, b, itemOn ? Tok::Accent : WithAlpha(Tok::Accent, 0.5f), D(5.0f), 0, 1.0f);
		} else if (hov && itemOn) {
			dl->AddRectFilled(a, b, Tok::Surface3, D(5.0f));
		}
		const ImVec2 ts = TextSize(SmallFont(), SmallPx(), labels[i]);
		const ImU32 tc = (!itemOn && !sel) ? Tok::TextFaint : (sel ? Tok::Text : (hov ? Tok::Text : Tok::TextMuted));
		DrawText(dl, SmallFont(), SmallPx(), ImVec2(x + std::floor((iw - ts.x) * 0.5f), a.y + std::floor((ih - ts.y) * 0.5f)),
		         tc, labels[i]);
		if (click && itemOn && !sel) { *selected = i; changed = true; }
		if (hov && itemOn) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		x += iw + D(2.0f);
	}
	// One item for the whole control so SameLine / layout see its real extent.
	ImGui::SetCursorScreenPos(start);
	ImGui::Dummy(ImVec2(w, boxH));
	ImGui::PopID();
	return changed;
}

// ---- select ---------------------------------------------------------------

float SelectFitWidth(const char* const* labels, int count)
{
	float w = 0.0f;
	for (int i = 0; i < count; i++)
		if (labels && labels[i]) w = std::max(w, TextSize(BodyFont(), BodyPx(), labels[i]).x);
	return std::ceil(w + D(12.0f) * 2.0f + ControlH());
}

void TestOpenSelect(const char* id) { g_testOpenSelect = id ? id : ""; }

SelectLayout SelectLayoutFor(const char* const* labels, const char* const* notes, int count, float width)
{
	SelectLayout l;
	l.labelX = g_fonts.icons ? IconWidth(PobIcon::Check, BodyPx()) + D(6.0f) : 0.0f;
	l.gap = D(16.0f);   // space-4
	for (int i = 0; i < count; i++) {
		if (labels && labels[i]) l.labelMaxW = std::max(l.labelMaxW, TextSize(BodyFont(), BodyPx(), labels[i]).x);
		if (notes && notes[i] && *notes[i])
			l.noteMaxW = std::max(l.noteMaxW, TextSize(SmallFont(), SmallPx(), notes[i]).x);
	}
	const float need = std::ceil(l.labelX + l.labelMaxW + (l.noteMaxW > 0.0f ? l.gap + l.noteMaxW : 0.0f));
	// The combo popup pads by FramePadding.x each side (PushControlFrame: D(12)),
	// plus a scrollbar past 20 rows.
	const float chrome = D(12.0f) * 2.0f + (count > 20 ? ImGui::GetStyle().ScrollbarSize : 0.0f);
	l.popupW = std::max(width, need + chrome);
	l.contentW = l.popupW - chrome;
	return l;
}

bool Select(const char* id, int* selected, const char* const* labels, const char* const* notes,
            int count, float width, bool enabled)
{
	bool changed = false;
	const float boxH = BoxH(ControlH());
	const ImVec2 start = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(start + ImVec2(0, std::floor((boxH - ControlH()) * 0.5f)));
	PushControlFrame();
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(Tok::SurfaceRaised));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, D(8.0f));
	ImGui::SetNextItemWidth(width);
	const char* preview = (*selected >= 0 && *selected < count) ? labels[*selected] : "";
	ImGui::BeginDisabled(!enabled);
	// The popup auto-sizes to its items, but the rows are drawn by hand, so tell
	// it how wide they need to be: otherwise it stays at the button's width and a
	// long note lands on top of its label.
	const SelectLayout lay = SelectLayoutFor(labels, notes, count, width);
	if (!g_testOpenSelect.empty() && g_testOpenSelect == id) {
		g_testOpenSelect.clear();
		ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetCurrentWindow()->GetID(id)));
	}
	{
		const float rowH = ImGui::GetTextLineHeight() + D(6.0f);
		const int vis = std::min(count, 20);   // ImGuiComboFlags_HeightLarge
		const float maxH = rowH * (float)vis + ImGui::GetStyle().ItemSpacing.y * (float)std::max(0, vis - 1) +
		                   ImGui::GetStyle().WindowPadding.y * 2.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(lay.popupW, 0.0f), ImVec2(FLT_MAX, maxH));
	}
	if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge)) {
		const float noteRight = ImGui::GetContentRegionAvail().x;
		for (int i = 0; i < count; i++) {
			ImGui::PushID(i);
			const bool sel = (i == *selected);
			const ImVec2 rowStart = ImGui::GetCursorScreenPos();
			if (ImGui::Selectable("##opt", sel, 0, ImVec2(0, ImGui::GetTextLineHeight() + D(6.0f)))) {
				if (!sel) { *selected = i; changed = true; }
			}
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float ty = rowStart.y + D(3.0f);
			const float lx = rowStart.x + lay.labelX;
			if (sel && g_fonts.icons) {
				IconAt(dl, ImVec2(rowStart.x, ty), PobIcon::Check, Tok::AccentText, BodyPx());
			}
			DrawText(dl, BodyFont(), BodyPx(), ImVec2(lx, ty), Tok::Text, labels[i]);
			if (notes && notes[i] && *notes[i]) {
				const ImVec2 ns = TextSize(SmallFont(), SmallPx(), notes[i]);
				DrawText(dl, SmallFont(), SmallPx(),
				         ImVec2(rowStart.x + noteRight - ns.x, ty + (BodyPx() - SmallPx()) * 0.5f),
				         Tok::TextMuted, notes[i]);
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	PopControlFrame();
	return changed;
}

// ---- slider ---------------------------------------------------------------

float SliderWithResetWidth(float trackWidth, const char* resetLabel)
{
	return trackWidth + D(12.0f) + D(48.0f) + D(8.0f) +
	       std::max(ButtonWidth(resetLabel, BtnSize::Sm), D(72.0f));
}

SliderResult SliderWithReset(const char* id, int* edit, int min, int max, int def,
                             const char* fmt, const char* resetLabel, const char* defaultLabel,
                             float trackWidth)
{
	SliderResult r;
	ImGui::PushID(id);
	const float boxH = BoxH(ControlH());
	const ImVec2 start = ImGui::GetCursorScreenPos();
	const float trackH = std::max(2.0f, std::floor(D(4.0f)));
	const float knob = std::floor(D(16.0f));
	// The real widget, invisible: keeps keyboard / drag behaviour and the
	// IsItemDeactivatedAfterEdit contract the callers already rely on.
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, std::floor((boxH - ImGui::GetFontSize()) * 0.5f)));
	ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, knob);
	ImGui::SetNextItemWidth(trackWidth);
	r.changed = ImGui::SliderInt("##s", edit, min, max, "", ImGuiSliderFlags_AlwaysClamp);
	const bool active = ImGui::IsItemActive();
	r.active = active;
	const bool hovered = ImGui::IsItemHovered();
	r.released = ImGui::IsItemDeactivatedAfterEdit();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(6);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float cy = start.y + std::floor(boxH * 0.5f);
	const float x0 = start.x + knob * 0.5f, x1 = start.x + trackWidth - knob * 0.5f;
	const float t = max > min ? (float)(*edit - min) / (float)(max - min) : 0.0f;
	const float kx = x0 + (x1 - x0) * t;
	dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(x1, cy + trackH * 0.5f), Tok::Border, trackH * 0.5f);
	dl->AddRectFilled(ImVec2(x0, cy - trackH * 0.5f), ImVec2(kx, cy + trackH * 0.5f), Tok::Accent, trackH * 0.5f);
	dl->AddCircleFilled(ImVec2(kx, cy), knob * 0.5f + D(3.0f), Tok::Bg, 24);
	dl->AddCircleFilled(ImVec2(kx, cy), knob * 0.5f, (active || hovered) ? Tok::OnAccent : Tok::Text, 24);

	// current value, fixed width so the row does not jump while dragging
	char val[32];
	snprintf(val, sizeof(val), fmt ? fmt : "%d", *edit);
	ImGui::SameLine(0, D(12.0f));
	{
		const ImVec2 vp = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(D(48.0f), boxH));
		const ImVec2 ts = TextSize(SmallFont(), SmallPx(), val);
		DrawText(dl, SmallFont(), SmallPx(), ImVec2(vp.x, vp.y + std::floor((boxH - ts.y) * 0.5f)), Tok::Text, val);
	}
	ImGui::SameLine(0, D(8.0f));
	const float resetW = std::max(ButtonWidth(resetLabel, BtnSize::Sm), D(72.0f));
	if (*edit == def && !active) {
		const ImVec2 hp = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(resetW, boxH));
		const ImVec2 ts = TextSize(SmallFont(), SmallPx(), defaultLabel);
		DrawText(dl, SmallFont(), SmallPx(),
		         ImVec2(hp.x + std::floor((resetW - ts.x) * 0.5f), hp.y + std::floor((boxH - ts.y) * 0.5f)),
		         Tok::TextMuted, defaultLabel);
	} else {
		const float prev = g_lineBoxH;
		g_lineBoxH = boxH;
		if (Button(resetLabel, BtnKind::Ghost, BtnSize::Sm, nullptr, resetW)) {
			*edit = def;
			r.reset = true;
		}
		g_lineBoxH = prev;
	}
	ImGui::PopID();
	return r;
}

// ---- status ---------------------------------------------------------------

float PillWidth(const char* text)
{
	return std::ceil(TextSize(SmallFont(), SmallPx(), text).x + D(20.0f) + D(6.0f) + D(6.0f));
}

void StatusPill(Tone tone, const char* text)
{
	const float h = std::floor(D(24.0f));
	const float boxH = BoxH(h);
	const float w = PillWidth(text);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(w, boxH));
	p.y += std::floor((boxH - h) * 0.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(w, h), ToneBg(tone), h * 0.5f);
	const float dot = D(6.0f);
	const ImU32 fg = ToneFg(tone);
	dl->AddCircleFilled(ImVec2(p.x + D(10.0f) + dot * 0.5f, p.y + h * 0.5f), dot * 0.5f, fg, 12);
	const ImVec2 ts = TextSize(SmallFont(), SmallPx(), text);
	DrawText(dl, SmallFont(), SmallPx(), ImVec2(p.x + D(10.0f) + dot + D(6.0f), p.y + std::floor((h - ts.y) * 0.5f)), fg, text);
}

BannerResult Banner(const char* id, BannerTone tone, const char* icon, const char* title,
                    const char* desc, bool descMono, const char* action, bool closable,
                    bool actionEnabled, float bannerWidth)
{
	BannerResult res = BannerResult::None;
	ImGui::PushID(id);
	const float width = bannerWidth > 0.0f ? bannerWidth : ImGui::GetContentRegionAvail().x;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float padX = D(16.0f), padY = D(12.0f), gap = D(12.0f);
	ImU32 bg = Tok::Surface1, edge = Tok::Border, iconCol = Tok::AccentText;
	if (tone == BannerTone::Warn) { bg = Tok::WarningSoft; edge = Tok::BannerWarnEdge; iconCol = Tok::Warning; }
	if (tone == BannerTone::Bad)  { bg = Tok::DangerSoft;  edge = Tok::BannerBadEdge;  iconCol = Tok::Danger; }

	const float iconPx = BodyPx();
	const float iw = g_fonts.icons ? IconWidth(icon, iconPx) + gap : 0.0f;
	const float actW = action ? ButtonWidth(action, BtnSize::Sm) : 0.0f;
	const float closeW = closable ? std::floor(D(28.0f)) : 0.0f;
	const float textW = std::max(D(80.0f), width - padX * 2.0f - iw - (actW > 0 ? actW + gap : 0.0f) -
	                                         (closeW > 0 ? closeW + D(4.0f) : 0.0f));
	const ImVec2 titleSz = TextSize(BodyFont(), BodyPx(), title, textW);
	const ImVec2 descSz = (desc && *desc) ? TextSize(SmallFont(), SmallPx(), desc, textW) : ImVec2(0, 0);
	const float textH = titleSz.y + (descSz.y > 0 ? D(2.0f) + descSz.y : 0.0f);
	const float h = std::max(textH, std::floor(D(28.0f))) + padY * 2.0f;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(width, h), bg, D(8.0f));
	dl->AddRect(p, p + ImVec2(width, h), edge, D(8.0f), 0, 1.0f);
	float x = p.x + padX;
	if (g_fonts.icons && icon) {
		IconAt(dl, ImVec2(x, p.y + padY + (BodyPx() - iconPx) * 0.5f + D(1.0f)), icon, iconCol, iconPx);
		x += iw;
	}
	DrawText(dl, BodyFont(), BodyPx(), ImVec2(x, p.y + padY), Tok::Text, title, textW);
	if (descSz.y > 0)
		DrawText(dl, SmallFont(), SmallPx(), ImVec2(x, p.y + padY + titleSz.y + D(2.0f)),
		         descMono ? Tok::TextMuted : Tok::TextMuted, desc, textW);

	const float prev = g_lineBoxH;
	g_lineBoxH = 0.0f;
	float bx = p.x + width - padX;
	if (closable) {
		bx -= closeW;
		ImGui::SetCursorScreenPos(ImVec2(bx, p.y + padY));
		if (Button(g_fonts.icons ? "##close" : "x##close", BtnKind::Ghost, BtnSize::Sm,
		           g_fonts.icons ? PobIcon::X : nullptr, closeW))
			res = BannerResult::Close;
		bx -= D(4.0f);
	}
	if (action) {
		bx -= actW;
		ImGui::SetCursorScreenPos(ImVec2(bx, p.y + padY));
		if (Button(action, BtnKind::Secondary, BtnSize::Sm, nullptr, 0.0f, actionEnabled))
			res = BannerResult::Action;
	}
	g_lineBoxH = prev;
	ImGui::SetCursorScreenPos(p);
	ImGui::Dummy(ImVec2(width, h));
	ImGui::PopID();
	return res;
}

void ProgressBar(float fraction, float width)
{
	const float h = std::max(3.0f, std::floor(D(6.0f)));
	const float boxH = BoxH(h);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(width, boxH));
	p.y += std::floor((boxH - h) * 0.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(width, h), Tok::Surface2, h * 0.5f);
	const float f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
	if (f > 0.0f) dl->AddRectFilled(p, p + ImVec2(std::max(h, width * f), h), Tok::Accent, h * 0.5f);
}

// ---- cards and rows -------------------------------------------------------

void CardBegin(const char* id, const char* icon, const char* title, const char* note, bool padded, float width,
               float minHeight)
{
	ImGui::PushID(id);
	CardState* c = new CardState();
	c->dl = ImGui::GetWindowDrawList();
	c->pos = ImGui::GetCursorScreenPos();
	c->width = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
	c->padded = padded;
	c->minHeight = minHeight;
	c->split.Split(c->dl, 2);
	c->split.SetCurrentChannel(c->dl, 1);
	g_cards.push_back(c);
	ImGui::BeginGroup();
	const float padX = CardPadX(), padY = CardPadY();
	if (title) {
		c->head = true;
		const float headH = std::max(HeadingPx(), BodyPx()) + padY * 2.0f;
		float x = c->pos.x + padX;
		const float cy = c->pos.y + headH * 0.5f;
		if (icon && g_fonts.icons) {
			IconAt(c->dl, ImVec2(x, cy - BodyPx() * 0.5f), icon, Tok::AccentText, BodyPx());
			x += IconWidth(icon, BodyPx()) + D(12.0f);
		}
		DrawText(c->dl, HeadingFont(), HeadingPx(), ImVec2(x, cy - HeadingPx() * 0.5f), Tok::Text, title);
		if (note && *note) {
			const ImVec2 ns = TextSize(SmallFont(), SmallPx(), note);
			DrawText(c->dl, SmallFont(), SmallPx(),
			         ImVec2(c->pos.x + c->width - padX - ns.x, cy - ns.y * 0.5f), Tok::TextMuted, note);
		}
		c->headBottom = c->pos.y + headH;
		c->dl->AddLine(ImVec2(c->pos.x + 1.0f, c->headBottom), ImVec2(c->pos.x + c->width - 1.0f, c->headBottom),
		               Tok::BorderSubtle, 1.0f);
		ImGui::Dummy(ImVec2(c->width, headH));
		ImGui::SetCursorScreenPos(ImVec2(c->pos.x, c->headBottom));
	}
	if (padded) {
		const float top = title ? c->headBottom + padY : c->pos.y + padY;
		ImGui::SetCursorScreenPos(ImVec2(c->pos.x + padX, top));
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + c->width - padX * 2.0f);
	}
}

bool CardHeadButton(const char* label, BtnKind kind, const char* icon)
{
	if (g_cards.empty() || !g_cards.back()->head) return false;
	CardState* c = g_cards.back();
	const ImVec2 keep = ImGui::GetCursorScreenPos();
	const float w = ButtonWidth(label, BtnSize::Sm, icon);
	const float h = std::floor(D(28.0f));
	const float headH = c->headBottom - c->pos.y;
	ImGui::SetCursorScreenPos(ImVec2(c->pos.x + c->width - CardPadX() - w, c->pos.y + std::floor((headH - h) * 0.5f)));
	const float prev = g_lineBoxH;
	g_lineBoxH = 0.0f;
	const bool clicked = Button(label, kind, BtnSize::Sm, icon);
	g_lineBoxH = prev;
	ImGui::SetCursorScreenPos(keep);
	return clicked;
}

float CardInnerX()
{
	if (g_cards.empty()) return ImGui::GetCursorScreenPos().x;
	return g_cards.back()->pos.x + CardPadX();
}
float CardInnerWidth()
{
	if (g_cards.empty()) return ImGui::GetContentRegionAvail().x;
	return g_cards.back()->width - CardPadX() * 2.0f;
}

void CardEnd()
{
	if (g_cards.empty()) return;
	CardState* c = g_cards.back();
	float bottom = 0.0f;
	if (c->padded) {
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		bottom = ImGui::GetItemRectMax().y + CardPadY();
	}
	ImGui::EndGroup();
	if (!c->padded) bottom = ImGui::GetItemRectMax().y;
	if (c->head && bottom < c->headBottom) bottom = c->headBottom;
	g_cardNatural = bottom - c->pos.y;
	if (c->minHeight > 0.0f && bottom < c->pos.y + c->minHeight) bottom = c->pos.y + c->minHeight;
	const ImVec2 a = c->pos, b(c->pos.x + c->width, bottom);
	c->split.SetCurrentChannel(c->dl, 0);
	c->dl->AddRectFilled(a, b, Tok::Surface1, D(8.0f));
	c->dl->AddRect(a, b, Tok::Border, D(8.0f), 0, 1.0f);
	c->split.Merge(c->dl);
	g_cards.pop_back();
	delete c;
	ImGui::SetCursorScreenPos(a);
	ImGui::Dummy(ImVec2(b.x - a.x, b.y - a.y));
	ImGui::PopID();
}

float CardNaturalHeight() { return g_cardNatural; }

float RowGap() { return D(8.0f); }

void RowBegin(const char* label, const char* hint, float ctrlWidth, float ctrlHeight, const char* tip, bool disabled)
{
	const float padX = CardPadX(), padY = Compact() ? D(10.0f) : D(16.0f);
	float x0, w;
	if (!g_cards.empty()) {
		x0 = g_cards.back()->pos.x;
		w = g_cards.back()->width;
	} else {
		x0 = ImGui::GetCursorScreenPos().x;
		w = ImGui::GetContentRegionAvail().x;
	}
	const float y = ImGui::GetCursorScreenPos().y;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (!g_cards.empty()) {
		if (g_cards.back()->anyRow)
			dl->AddLine(ImVec2(x0 + 1.0f, y), ImVec2(x0 + w - 1.0f, y), Tok::BorderSubtle, 1.0f);
		g_cards.back()->anyRow = true;
	}
	const float ch = ctrlHeight > 0.0f ? ctrlHeight : ControlH();
	const float tipW = tip ? (g_fonts.icons ? IconWidth(PobIcon::Info, SmallPx()) : D(20.0f)) + D(6.0f) : 0.0f;
	const float labelW = std::max(D(120.0f), w - padX * 2.0f - ctrlWidth - D(32.0f));
	const ImVec2 ls = TextSize(BodyFont(), BodyPx(), label, labelW - tipW);
	const ImVec2 hs = (hint && *hint) ? TextSize(SmallFont(), SmallPx(), hint, labelW) : ImVec2(0, 0);
	const float textH = ls.y + (hs.y > 0 ? D(2.0f) + hs.y : 0.0f);
	const float rowH = std::max(textH, ch) + padY * 2.0f;
	const float ty = y + std::floor((rowH - textH) * 0.5f);
	DrawText(dl, BodyFont(), BodyPx(), ImVec2(x0 + padX, ty), disabled ? Tok::TextFaint : Tok::Text, label, labelW - tipW);
	if (hs.y > 0)
		DrawText(dl, SmallFont(), SmallPx(), ImVec2(x0 + padX, ty + ls.y + D(2.0f)), Tok::TextMuted, hint, labelW);
	if (tip) {
		ImGui::SetCursorScreenPos(ImVec2(x0 + padX + ls.x + D(6.0f), ty + (BodyPx() - SmallPx()) * 0.5f));
		InfoTip(tip);
	}
	g_row.pos = ImVec2(x0, y);
	g_row.width = w;
	g_row.height = rowH;
	g_row.prevLineBox = g_lineBoxH;
	g_row.open = true;
	g_lineBoxH = ch;
	ImGui::SetCursorScreenPos(ImVec2(x0 + w - padX - ctrlWidth, y + std::floor((rowH - ch) * 0.5f)));
}

void RowEnd()
{
	if (!g_row.open) return;
	g_row.open = false;
	g_lineBoxH = g_row.prevLineBox;
	ImGui::SetCursorScreenPos(g_row.pos);
	ImGui::Dummy(ImVec2(g_row.width, g_row.height));
	// Rows abut: the divider IS the spacing.
	ImGui::SetCursorScreenPos(ImVec2(g_row.pos.x, g_row.pos.y + g_row.height));
}

bool SideNavItem(const char* icon, const char* label, bool active, float width)
{
	const float h = std::floor(BodyPx() + D(16.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool click = ImGui::InvisibleButton(label, ImVec2(width, h));
	const bool hov = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (active) dl->AddRectFilled(p, p + ImVec2(width, h), Tok::AccentSoft, D(5.0f));
	else if (hov) dl->AddRectFilled(p, p + ImVec2(width, h), Tok::Surface1, D(5.0f));
	float x = p.x + D(12.0f);
	const float ty = p.y + std::floor((h - BodyPx()) * 0.5f);
	if (icon && g_fonts.icons) {
		IconAt(dl, ImVec2(x, ty), icon, active ? Tok::AccentText : (hov ? Tok::Text : Tok::TextMuted), BodyPx());
		x += IconWidth(icon, BodyPx()) + D(12.0f);
	}
	DrawText(dl, BodyFont(), BodyPx(), ImVec2(x, ty), (active || hov) ? Tok::Text : Tok::TextMuted, label);
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return click;
}

bool ToolTile(const char* id, const char* icon, const char* name, const char* badge,
              const char* hint, bool dot, const ImVec2& size, bool enabled)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::BeginDisabled(!enabled);
	const bool click = ImGui::InvisibleButton(id, size);
	const bool hov = ImGui::IsItemHovered();
	ImGui::EndDisabled();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float r = D(8.0f);
	dl->AddRectFilled(p, p + size, (hov && enabled) ? Tok::Surface2 : Tok::Surface1, r);
	dl->AddRect(p, p + size, (hov && enabled) ? Tok::BorderStrong : Tok::Border, r, 0, 1.0f);
	const float pad = D(16.0f);
	float y = p.y + pad;
	const float iconPx = std::floor(D(20.0f));
	if (icon && g_fonts.icons) {
		IconAt(dl, ImVec2(p.x + pad, y), icon, enabled ? Tok::AccentText : Tok::TextFaint, iconPx);
		y += iconPx + D(8.0f);
	}
	const float textW = size.x - pad * 2.0f;
	DrawText(dl, BodyFont(), BodyPx(), ImVec2(p.x + pad, y), enabled ? Tok::Text : Tok::TextFaint, name);
	if (badge && *badge) {
		const ImVec2 ns = TextSize(BodyFont(), BodyPx(), name);
		const ImVec2 bs = TextSize(SmallFont(), SmallPx() * 0.85f, badge);
		const ImVec2 b0(p.x + pad + ns.x + D(6.0f), y + std::floor((BodyPx() - bs.y - D(2.0f)) * 0.5f));
		const ImVec2 b1 = b0 + ImVec2(bs.x + D(12.0f), bs.y + D(2.0f));
		dl->AddRectFilled(b0, b1, Tok::Surface3, (b1.y - b0.y) * 0.5f);
		DrawText(dl, SmallFont(), SmallPx() * 0.85f, b0 + ImVec2(D(6.0f), D(1.0f)), Tok::TextMuted, badge);
	}
	y += BodyPx() + D(4.0f);
	if (hint && *hint) {
		// one line: clip rather than wrap, the tile height is fixed
		dl->PushClipRect(ImVec2(p.x + pad, y), ImVec2(p.x + pad + textW, y + SmallPx() * 1.4f), true);
		DrawText(dl, SmallFont(), SmallPx(), ImVec2(p.x + pad, y), Tok::TextMuted, hint);
		dl->PopClipRect();
	}
	if (dot) {
		const float d = D(8.0f);
		dl->AddCircleFilled(ImVec2(p.x + size.x - D(14.0f) - d * 0.5f, p.y + D(14.0f) + d * 0.5f), d * 0.5f, Tok::Accent, 12);
	}
	if (hov && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return click && enabled;
}

// ---- toast ----------------------------------------------------------------

void ShowToast(const char* text, Tone tone)
{
	g_toastText = text ? text : "";
	g_toastTone = tone;
	g_toastAt = ImGui::GetTime();
}

bool ToastVisible()
{
	return !g_toastText.empty() && ImGui::GetTime() - g_toastAt < kToastSecs;
}

void DrawToast()
{
	if (!ToastVisible()) return;
	const double age = ImGui::GetTime() - g_toastAt;
	float alpha = 1.0f;
	if (age < kToastFade) alpha = (float)(age / kToastFade);
	else if (age > kToastSecs - kToastFade) alpha = (float)((kToastSecs - age) / kToastFade);
	alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);

	const ImGuiIO& io = ImGui::GetIO();
	const float margin = D(24.0f);
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin), ImGuiCond_Always, ImVec2(1, 1));
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin("##pobtools_toast", nullptr,
	             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
	             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
	             ImGuiWindowFlags_NoBackground);
	const ImVec2 ts = TextSize(BodyFont(), BodyPx() * 0.92f, g_toastText.c_str());
	const float iconPx = BodyPx() * 0.92f;
	const float iw = g_fonts.icons ? IconWidth(PobIcon::CircleCheck, iconPx) + D(12.0f) : D(18.0f);
	const ImVec2 sz(ts.x + iw + D(28.0f), std::max(ts.y, iconPx) + D(20.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(sz);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// shadow-pop, approximated by two offset fills (no blur in ImGui)
	dl->AddRectFilled(p + ImVec2(0, D(6.0f)), p + sz + ImVec2(0, D(10.0f)), WithAlpha(0x60000000u, alpha), D(10.0f));
	dl->AddRectFilled(p, p + sz, WithAlpha(Tok::SurfaceRaised, alpha), D(8.0f));
	dl->AddRect(p, p + sz, WithAlpha(Tok::Border, alpha), D(8.0f), 0, 1.0f);
	float x = p.x + D(14.0f);
	const float cy = p.y + sz.y * 0.5f;
	const ImU32 fg = WithAlpha(ToneFg(g_toastTone), alpha);
	if (g_fonts.icons) {
		const char* ic = g_toastTone == Tone::Bad ? PobIcon::CircleX
		               : g_toastTone == Tone::Warn ? PobIcon::TriangleAlert
		               : g_toastTone == Tone::Ok ? PobIcon::CircleCheck : PobIcon::Info;
		IconAt(dl, ImVec2(x, cy - iconPx * 0.5f), ic, fg, iconPx);
	} else {
		dl->AddCircleFilled(ImVec2(x + D(4.0f), cy), D(4.0f), fg, 12);
	}
	x += iw;
	DrawText(dl, BodyFont(), BodyPx() * 0.92f, ImVec2(x, cy - ts.y * 0.5f), WithAlpha(Tok::Text, alpha), g_toastText.c_str());
	ImGui::End();
	ImGui::PopStyleVar(2);
}

// ---- dialog ---------------------------------------------------------------

namespace {
float g_dialogInner = 0.0f;
}

bool BeginDialog(const char* popupId, bool* open, const char* title, const char* body, const char* mono)
{
	if (open && *open) {
		ImGui::OpenPopup(popupId);
		*open = false;
	}
	const float w = std::floor(D(440.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, FLT_MAX));
	const ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(Tok::SurfaceRaised));
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImGui::ColorConvertU32ToFloat4(Tok::Scrim));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, D(12.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(24.0f), D(24.0f)));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
	if (!ImGui::BeginPopupModal(popupId, nullptr,
	                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
	                            ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
		return false;
	}
	const float inner = w - D(48.0f);
	g_dialogInner = inner;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
	{
		// heading size without a face of its own: draw-list text, then the room
		const ImVec2 tp = ImGui::GetCursorScreenPos();
		const ImVec2 ts = TextSize(HeadingFont(), HeadingPx(), title, inner);
		DrawText(ImGui::GetWindowDrawList(), HeadingFont(), HeadingPx(), tp, Tok::Text, title, inner);
		ImGui::Dummy(ts);
	}
	ImGui::Dummy(ImVec2(0, D(4.0f)));
	if (body && *body) {
		ImGui::PushFont(SmallFont());
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::TextMuted));
		ImGui::TextUnformatted(body);
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}
	if (mono && *mono) {
		ImGui::Dummy(ImVec2(0, D(2.0f)));
		Numeric(mono, Tok::Text);
	}
	ImGui::PopTextWrapPos();
	return true;
}

DialogResult DialogButtons(const char* cancel, const char* secondary, const char* danger, const char* primary,
                           bool primaryEnabled, bool enterIsPrimary)
{
	DialogResult res = DialogResult::None;
	const float inner = g_dialogInner > 0.0f ? g_dialogInner : ImGui::GetContentRegionAvail().x;
	ImGui::Dummy(ImVec2(0, D(12.0f)));
	float total = 0.0f;
	const float gap = D(8.0f);
	const char* btns[4] = { cancel, secondary, danger, primary };
	int n = 0;
	for (const char* b : btns) if (b) { total += ButtonWidth(b, BtnSize::Md, nullptr, D(88.0f)); n++; }
	total += gap * (float)(n > 0 ? n - 1 : 0);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, inner - total));
	bool first = true;
	auto place = [&]() { if (!first) ImGui::SameLine(0, gap); first = false; };
	if (cancel) { place(); if (Button(cancel, BtnKind::Secondary, BtnSize::Md, nullptr, D(88.0f))) res = DialogResult::Cancel; }
	if (secondary) { place(); if (Button(secondary, BtnKind::Secondary, BtnSize::Md, nullptr, D(88.0f))) res = DialogResult::Secondary; }
	if (danger) { place(); if (Button(danger, BtnKind::Danger, BtnSize::Md, nullptr, D(88.0f))) res = DialogResult::Danger; }
	if (primary) {
		place();
		if (Button(primary, BtnKind::Primary, BtnSize::Md, nullptr, D(88.0f), primaryEnabled)) res = DialogResult::Primary;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Escape)) res = DialogResult::Cancel;
	if (enterIsPrimary && primary && primaryEnabled &&
	    (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
		res = DialogResult::Primary;
	if (res != DialogResult::None) ImGui::CloseCurrentPopup();
	return res;
}

void EndDialog()
{
	ImGui::EndPopup();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
	g_dialogInner = 0.0f;
}

DialogResult ConfirmDialog(const char* popupId, bool* open, const char* title, const char* body,
                           const char* mono, const char* cancel, const char* danger, const char* primary)
{
	if (!BeginDialog(popupId, open, title, body, mono)) return DialogResult::None;
	const DialogResult res = DialogButtons(cancel, nullptr, danger, primary);
	EndDialog();
	return res;
}

// ---- tabs, sections, slots, menus -------------------------------------------

int PageTabs(const char* id, int selected, const char* const* labels, int count, float width)
{
	ImGui::PushID(id);
	const float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float h = std::floor(D(38.0f));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	float x = p.x;
	int out = selected;
	for (int i = 0; i < count; i++) {
		const ImVec2 ts = TextSize(BodyFont(), BodyPx(), labels[i]);
		const float tw = ts.x + D(28.0f);
		ImGui::SetCursorScreenPos(ImVec2(x, p.y));
		ImGui::PushID(i);
		const bool click = ImGui::InvisibleButton("##tab", ImVec2(tw, h));
		const bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		const bool sel = i == selected;
		if (hov && !sel)
			dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + tw, p.y + h), Tok::Surface1, D(5.0f), ImDrawFlags_RoundCornersTop);
		DrawText(dl, BodyFont(), BodyPx(), ImVec2(x + D(14.0f), p.y + std::floor((h - ts.y) * 0.5f)),
		         (sel || hov) ? Tok::Text : Tok::TextMuted, labels[i]);
		if (sel) dl->AddRectFilled(ImVec2(x, p.y + h - 2.0f), ImVec2(x + tw, p.y + h), Tok::Accent);
		if (click) out = i;
		if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		x += tw + D(2.0f);
	}
	dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f), Tok::Border, 1.0f);
	ImGui::SetCursorScreenPos(p);
	ImGui::Dummy(ImVec2(w, h));
	ImGui::PopID();
	return out;
}

bool CollapsingSection(const char* label, const char* id, const char* icon, const char* note, bool defaultOpen)
{
	const ImGuiID gid = ImGui::GetID(id);
	ImGuiStorage* st = ImGui::GetStateStorage();
	bool open = st->GetInt(gid, defaultOpen ? 1 : 0) != 0;
	const float w = ImGui::GetContentRegionAvail().x;
	const float h = std::floor(BodyPx() + D(20.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool click = ImGui::InvisibleButton(id, ImVec2(w, h));
	const bool hov = ImGui::IsItemHovered();
	if (click) {
		open = !open;
		st->SetInt(gid, open ? 1 : 0);
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (hov) dl->AddRectFilled(p, p + ImVec2(w, h), Tok::Surface2, D(5.0f));
	dl->AddLine(ImVec2(p.x, p.y + 0.5f), ImVec2(p.x + w, p.y + 0.5f), Tok::BorderSubtle, 1.0f);
	float x = p.x + D(4.0f);
	const float ty = p.y + std::floor((h - BodyPx()) * 0.5f);
	if (g_fonts.icons) {
		IconAt(dl, ImVec2(x, ty), open ? PobIcon::ChevronDown : PobIcon::ChevronRight, Tok::TextMuted, BodyPx());
		x += IconWidth(PobIcon::ChevronDown, BodyPx()) + D(6.0f);
		if (icon) {
			IconAt(dl, ImVec2(x, ty), icon, Tok::AccentText, BodyPx());
			x += IconWidth(icon, BodyPx()) + D(8.0f);
		}
	} else {
		DrawText(dl, BodyFont(), BodyPx(), ImVec2(x, ty), Tok::TextMuted, open ? "-" : "+");
		x += TextSize(BodyFont(), BodyPx(), "+ ").x;
	}
	DrawText(dl, BodyFont(), BodyPx(), ImVec2(x, ty), Tok::Text, label);
	if (note && *note) {
		const float lx = x + TextSize(BodyFont(), BodyPx(), label).x + D(12.0f);
		const ImVec2 ns = TextSize(SmallFont(), SmallPx(), note);
		const float nx = std::max(lx, p.x + w - D(6.0f) - ns.x);
		dl->PushClipRect(ImVec2(lx, p.y), ImVec2(p.x + w - D(4.0f), p.y + h), true);
		DrawText(dl, SmallFont(), SmallPx(), ImVec2(nx, p.y + std::floor((h - ns.y) * 0.5f)), Tok::TextMuted, note);
		dl->PopClipRect();
	}
	if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return open;
}

namespace {
void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash)
{
	auto line = [&](ImVec2 p, ImVec2 q) {
		const float len = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
		if (len <= 0.0f) return;
		const ImVec2 dir((q.x - p.x) / len, (q.y - p.y) / len);
		for (float t = 0.0f; t < len; t += dash * 2.0f) {
			const float e = std::min(len, t + dash);
			dl->AddLine(ImVec2(p.x + dir.x * t, p.y + dir.y * t), ImVec2(p.x + dir.x * e, p.y + dir.y * e), col, 1.0f);
		}
	};
	a += ImVec2(0.5f, 0.5f);
	b -= ImVec2(0.5f, 0.5f);
	line(a, ImVec2(b.x, a.y));
	line(ImVec2(b.x, a.y), b);
	line(b, ImVec2(a.x, b.y));
	line(ImVec2(a.x, b.y), a);
}
} // namespace

bool Slot(const char* id, unsigned texture, const char* text, bool filled, float size, bool enabled)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::BeginDisabled(!enabled);
	const bool click = ImGui::InvisibleButton(id, ImVec2(size, size));
	const bool hov = ImGui::IsItemHovered();
	ImGui::EndDisabled();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 q = p + ImVec2(size, size);
	const float r = D(5.0f);
	dl->AddRectFilled(p, q, (hov && enabled) ? Tok::Surface3 : Tok::Surface2, r);
	if (filled) {
		dl->AddRect(p, q, (hov && enabled) ? Tok::BorderStrong : Tok::Border, r, 0, 1.0f);
		if (texture) {
			const float pad = std::floor(size * 0.08f);
			dl->AddImage((ImTextureID)(intptr_t)texture, p + ImVec2(pad, pad), q - ImVec2(pad, pad));
		} else if (text && *text) {
			const ImVec2 ts = TextSize(SmallFont(), SmallPx(), text);
			dl->PushClipRect(p, q, true);
			DrawText(dl, SmallFont(), SmallPx(), p + ImVec2(std::floor((size - ts.x) * 0.5f), std::floor((size - ts.y) * 0.5f)),
			         Tok::AccentText, text);
			dl->PopClipRect();
		}
	} else {
		DashedRect(dl, p, q, (hov && enabled) ? Tok::TextMuted : Tok::BorderStrong, D(3.0f));
		const ImU32 c = (hov && enabled) ? Tok::Text : Tok::TextFaint;
		if (g_fonts.icons) {
			const float px = SmallPx();
			const float iw = IconWidth(PobIcon::Plus, px);
			IconAt(dl, p + ImVec2(std::floor((size - iw) * 0.5f), std::floor((size - px) * 0.5f)), PobIcon::Plus, c, px);
		} else {
			const ImVec2 ts = TextSize(BodyFont(), BodyPx(), "+");
			DrawText(dl, BodyFont(), BodyPx(), p + ImVec2(std::floor((size - ts.x) * 0.5f), std::floor((size - ts.y) * 0.5f)), c, "+");
		}
	}
	if (hov && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	return click && enabled;
}

bool SearchField(const char* id, char* buf, int bufSize, const char* hint, float width)
{
	std::string h = hint ? hint : "";
	if (g_fonts.icons) h = std::string(PobIcon::Search) + "  " + h;
	PushControlFrame();
	ImGui::SetNextItemWidth(width);
	const bool changed = ImGui::InputTextWithHint(id, h.c_str(), buf, (size_t)bufSize, ImGuiInputTextFlags_EscapeClearsAll);
	PopControlFrame();
	return changed;
}

bool BeginMenuPopup(const char* id)
{
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(Tok::SurfaceRaised));
	ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(Tok::Border));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, D(8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(6.0f), D(6.0f)));
	ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
	if (ImGui::BeginPopup(id)) return true;
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
	return false;
}

void EndMenuPopup()
{
	ImGui::EndPopup();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
}

bool MenuRow(const char* icon, const char* label, const char* shortcut, bool enabled, bool danger)
{
	const float h = std::floor(BodyPx() + D(12.0f));
	const float iconW = g_fonts.icons ? IconWidth(PobIcon::Check, BodyPx()) + D(10.0f) : 0.0f;
	const float scW = (shortcut && *shortcut) ? TextSize(SmallFont(), SmallPx(), shortcut).x + D(24.0f) : 0.0f;
	const float need = D(10.0f) + iconW + TextSize(BodyFont(), BodyPx(), label).x + scW + D(10.0f);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::PushID(label);
	ImGui::BeginDisabled(!enabled);
	// `need` is what the row reports, so an auto-sized popup grows to its widest
	// row; SpanAvailWidth then stretches the highlight across the popup.
	const bool click = ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAvailWidth, ImVec2(need, h));
	ImGui::EndDisabled();
	ImGui::PopID();
	const float w = std::max(ImGui::GetItemRectSize().x, need);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImU32 fg = !enabled ? Tok::TextFaint : danger ? Tok::Danger : Tok::Text;
	float x = p.x + D(10.0f);
	const float ty = p.y + std::floor((h - BodyPx()) * 0.5f);
	if (g_fonts.icons) {
		if (icon) IconAt(dl, ImVec2(x, ty), icon, !enabled ? Tok::TextFaint : danger ? Tok::Danger : Tok::TextMuted, BodyPx());
		x += iconW;
	}
	DrawText(dl, BodyFont(), BodyPx(), ImVec2(x, ty), fg, label);
	if (shortcut && *shortcut) {
		const ImVec2 ss = TextSize(SmallFont(), SmallPx(), shortcut);
		DrawText(dl, SmallFont(), SmallPx(), ImVec2(p.x + w - D(10.0f) - ss.x, p.y + std::floor((h - ss.y) * 0.5f)),
		         Tok::TextMuted, shortcut);
	}
	return click && enabled;
}

void MenuSeparator()
{
	ImGui::Dummy(ImVec2(0, D(2.0f)));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), Tok::BorderSubtle, 1.0f);
	ImGui::Dummy(ImVec2(0, D(3.0f)));
}

// ---- empty state ----------------------------------------------------------

namespace {
struct EmptyMetrics { float padX, padY, gap, iconPx, textW; };
EmptyMetrics EmptyLayout(float width)
{
	EmptyMetrics m;
	m.padX = Compact() ? D(16.0f) : D(24.0f);
	m.padY = Compact() ? D(16.0f) : D(24.0f);
	m.gap = D(8.0f);
	m.iconPx = std::floor(D(28.0f));
	m.textW = std::max(D(60.0f), width - m.padX * 2.0f);
	return m;
}
} // namespace

float EmptyStateHeight(const char* title, const char* hint, bool action, float width)
{
	const EmptyMetrics m = EmptyLayout(width);
	float h = m.padY * 2.0f;
	if (g_fonts.icons) h += m.iconPx + m.gap;
	h += TextSize(BodyFont(), BodyPx(), title, m.textW).y;
	if (hint && *hint) h += m.gap * 0.5f + TextSize(SmallFont(), SmallPx(), hint, m.textW).y;
	if (action) h += m.gap * 1.5f + std::floor(D(36.0f));
	return std::ceil(h);
}

bool EmptyState(const char* id, const char* icon, const char* title, const char* hint, const char* action,
                float width, float height)
{
	ImGui::PushID(id);
	const float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
	const float need = EmptyStateHeight(title, hint, action != nullptr, w);
	const float h = std::max(need, height);
	const EmptyMetrics m = EmptyLayout(w);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Dashed outline (border token): ImGui has no dash style, so segments.
	const float dash = D(6.0f), space = D(4.0f);
	auto dashed = [&](ImVec2 a, ImVec2 b) {
		const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
		if (len <= 0.0f) return;
		const ImVec2 dir((b.x - a.x) / len, (b.y - a.y) / len);
		for (float t = 0.0f; t < len; t += dash + space) {
			const float e = std::min(t + dash, len);
			dl->AddLine(ImVec2(a.x + dir.x * t, a.y + dir.y * t), ImVec2(a.x + dir.x * e, a.y + dir.y * e),
			            Tok::Border, 1.0f);
		}
	};
	const ImVec2 q(p.x + w - 1.0f, p.y + h - 1.0f);
	dashed(ImVec2(p.x, p.y), ImVec2(q.x, p.y));
	dashed(ImVec2(q.x, p.y), ImVec2(q.x, q.y));
	dashed(ImVec2(q.x, q.y), ImVec2(p.x, q.y));
	dashed(ImVec2(p.x, q.y), ImVec2(p.x, p.y));

	// Centred column, vertically too when the box is taller than its content.
	float y = p.y + std::floor((h - need) * 0.5f) + m.padY;
	const float cx = p.x + w * 0.5f;
	if (g_fonts.icons && icon) {
		IconAt(dl, ImVec2(std::floor(cx - IconWidth(icon, m.iconPx) * 0.5f), y), icon, Tok::TextFaint, m.iconPx);
	}
	if (g_fonts.icons) y += m.iconPx + m.gap;
	// Each wrapped line centred on its own.
	auto centred = [&](ImFont* f, float px, ImU32 col, const char* text) {
		const char* s = text;
		const char* end = text + std::strlen(text);
		while (s < end) {
			const char* lineEnd = f->CalcWordWrapPositionA(px / f->FontSize, s, end, m.textW);
			if (lineEnd == s) lineEnd = s + 1;
			const char* nl = (const char*)std::memchr(s, '\n', (size_t)(lineEnd - s));
			if (nl) lineEnd = nl;
			const ImVec2 ls = f->CalcTextSizeA(px, FLT_MAX, 0.0f, s, lineEnd);
			dl->AddText(f, px, ImVec2(std::floor(cx - ls.x * 0.5f), y), col, s, lineEnd);
			y += std::max(ls.y, px);   // one line, as CalcTextSizeA counts them
			s = lineEnd;
			while (s < end && (*s == ' ' || *s == '\n')) s++;
		}
	};
	if (title && *title) centred(BodyFont(), BodyPx(), Tok::Text, title);
	if (hint && *hint) {
		y += m.gap * 0.5f;
		centred(SmallFont(), SmallPx(), Tok::TextMuted, hint);
	}
	bool clicked = false;
	if (action) {
		y += m.gap * 1.5f;
		const float bw = ButtonWidth(action, BtnSize::Md);
		ImGui::SetCursorScreenPos(ImVec2(std::floor(cx - bw * 0.5f), y));
		const float prev = g_lineBoxH;
		g_lineBoxH = 0.0f;
		clicked = Button(action, BtnKind::Primary, BtnSize::Md);
		g_lineBoxH = prev;
	}
	ImGui::SetCursorScreenPos(p);
	ImGui::Dummy(ImVec2(w, h));
	ImGui::PopID();
	return clicked;
}

// ---- selftest -------------------------------------------------------------

bool RunWidgetSelfTest()
{
	bool ok = true;
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	ImFont* def = io.Fonts->AddFontDefault();
	io.Fonts->Build();
	WidgetFonts f;
	// No NewFrame here, so ImGui has no current font: every face must be set.
	f.body = f.small = f.heading = f.title = def;
	f.scale = 1.0f;
	SetWidgetFonts(f);
	ok = ok && std::fabs(D(16.0f) - 19.0f) < 0.01f;   // design body == the 19 px face
	ok = ok && std::fabs(ControlH() - 42.0f) < 0.01f;  // floor(36 * 19/16)
	f.scale = 2.0f;
	SetWidgetFonts(f);
	ok = ok && std::fabs(D(16.0f) - 38.0f) < 0.01f;
	ok = ok && SwitchWidth() == std::floor(40.0f * 19.0f / 16.0f * 2.0f);
	f.scale = 1.0f;
	SetWidgetFonts(f);
	const char* seg[2] = { "AB", "ABCD" };
	ok = ok && SegmentedWidth(seg, 2) > SegmentedWidth(seg, 1);
	ok = ok && ButtonWidth("x", BtnSize::Md, nullptr, 200.0f) == 200.0f;
	ok = ok && ButtonWidth("long label here", BtnSize::Lg) > ButtonWidth("long label here", BtnSize::Sm);
	// EmptyState: an action adds a button's height; a narrower box wraps the hint
	// onto more lines and so is never shorter.
	{
		const char* hint = "a hint long enough to wrap onto a second line in a narrow box";
		const float plain = EmptyStateHeight("Title", hint, false, 600.0f);
		ok = ok && EmptyStateHeight("Title", hint, true, 600.0f) > plain;
		ok = ok && EmptyStateHeight("Title", hint, false, 160.0f) > plain;
		ok = ok && EmptyStateHeight("Title", nullptr, false, 600.0f) < plain;
	}
	// Select: with notes far longer than the button, the popup grows so that every
	// label's right edge + space-4 stays left of every note's left edge (notes are
	// right-aligned to contentW), and nothing runs past the row.
	{
		const char* lab[3] = { "Glorious Vanity XX", "Brutal", "Lethal Pride" };
		const char* nts[3] = { "a fairly long english note", nullptr, "Lethal Pride note" };
		const SelectLayout l = SelectLayoutFor(lab, nts, 3, 60.0f);
		ok = ok && l.popupW > 60.0f;
		for (int i = 0; i < 3; i++) {
			const float labelRight = l.labelX + TextSize(BodyFont(), BodyPx(), lab[i]).x;
			const float noteLeft = nts[i] ? l.contentW - TextSize(SmallFont(), SmallPx(), nts[i]).x : l.contentW;
			ok = ok && labelRight + l.gap <= noteLeft + 0.5f && noteLeft >= 0.0f;
		}
		// a wide button is kept as is
		ok = ok && SelectLayoutFor(lab, nts, 3, 2000.0f).popupW == 2000.0f;
		// no notes: no gap is reserved
		ok = ok && SelectLayoutFor(lab, nullptr, 3, 0.0f).noteMaxW == 0.0f;
	}
	// Tool density: controls shrink (36 -> ~31 design px), fonts do not, and the
	// scope restores the launcher's sizes.
	{
		const float launcherH = ControlH();
		{
			ToolDensityScope tool;
			ok = ok && ToolDensity();
			ok = ok && ControlH() < launcherH;
			ok = ok && std::fabs(ControlH() / kDesignToImGui - 36.0f * kToolDensity) < 1.0f;
		}
		ok = ok && !ToolDensity() && ControlH() == launcherH;
	}
	// Tool density at every font-size setting the launcher offers (14 / 19 / 26,
	// i.e. scale = n/19): the faces are 19 / 15 px times the scale (tool_window.cpp,
	// launcher_ui.cpp), the boxes are design px times the same scale, so the
	// ratio is fixed and only the floor() rounding moves -- worst at 14. Every
	// box that holds a line of text keeps at least 3 px above and below it (the
	// tightest is the pill at 14: floor(24.5 * 14/19) = 18 around an 11 px face).
	{
		const float sizes[3] = { 14.0f, 19.0f, 26.0f };
		for (float n : sizes) {
			const float z = n / 19.0f;
			f.scale = z;
			SetWidgetFonts(f);
			ToolDensityScope tool;
			const float body = 19.0f * z, small = 15.0f * z;
			auto room = [](float box, float text) { return (box - text) * 0.5f >= 3.0f; };
			ok = ok && room(ControlH(), body);                    // input / select / Md button
			ok = ok && room(std::floor(D(28.0f)), small);         // Sm button
			ok = ok && room(std::floor(D(44.0f)), body);          // Lg button
			ok = ok && room(std::floor(D(24.0f)), small);         // pill
			ok = ok && room(std::floor(D(22.0f)), 0.0f) && D(10.0f) >= 1.0f;  // switch, gaps never vanish
		}
		f.scale = 1.0f;
		SetWidgetFonts(f);
	}
	ImGui::DestroyContext();
	SetWidgetFonts(WidgetFonts());
	return ok;
}

} // namespace PobUi
