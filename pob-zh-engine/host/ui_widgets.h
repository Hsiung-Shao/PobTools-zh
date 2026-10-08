// Launcher widgets built from the PobTools design system (tokens in ui_theme.h).
//
// Every size here is written in DESIGN pixels -- the design system draws at a
// 16 px body -- and converted with D(): design px x 19/16 x the launcher scale
// (monitor DPI x the font-size zoom). So a 36 px button is 36 x 19/16 = 42.75 px
// at the default font size on a 100% monitor, the same proportion to the 19 px
// body face that 36 is to 16 in the design.
//
// ImGui 1.90 WIP: no ImGuiChildFlags, so cards are a BeginGroup whose background
// is drawn afterwards into an earlier draw-list channel (an ImDrawListSplitter
// per open card, so cards and banners can nest).
//
// Fonts are handed over once per frame with SetWidgetFonts: the atlas can be
// rebuilt between frames (font picker, font size), and every ImFont* from before
// that is dangling afterwards.
#pragma once

#include <cstdint>

struct ImFont;
struct ImDrawList;
struct ImVec2;

namespace PobUi {

struct WidgetFonts {
	ImFont* body = nullptr;     // 19 px at 100%: labels, buttons, values
	ImFont* small = nullptr;    // 15 px: hints, pills, overlines, numbers
	ImFont* heading = nullptr;  // card and dialog headings, drawn at headingPx
	float headingPx = 0.0f;     // 20 px at 100% (0 = the face's own size)
	ImFont* title = nullptr;    // 26 px: the window title
	float scale = 1.0f;         // monitor DPI x font-size zoom
	bool icons = false;         // the Lucide subset was merged into the faces
};
void SetWidgetFonts(const WidgetFonts& f);
const WidgetFonts& Fonts();

// Design px -> screen px.
float D(float designPx);
// The height of one control (button md, select, text field): D(36).
float ControlH();

// FramePadding that makes InputText / Combo exactly ControlH() tall.
void PushControlFrame();
void PopControlFrame();

// ---- text ---------------------------------------------------------------
// Heading-size text as one item (card titles, the tool header's name).
void Heading(const char* text, std::uint32_t col = 0);
void Hint(const char* text, float wrapWidth = 0.0f, std::uint32_t col = 0);   // small, text-muted
void Overline(const char* text);                                         // small, text-muted
// Overline + a hairline to the right edge of `width`.
void SectionHeader(const char* text, float width);
// Small mono-ish numeric line (versions, paths). We ship no JetBrains Mono, so
// it is the hint face in a lighter colour.
void Numeric(const char* text, std::uint32_t col = 0);
// Tooltip capped at 300 design px, hint face.
void Tooltip(const char* text);
// A small (i) that shows `text` on hover. Draws nothing when icons are missing
// except a plain "(?)".
void InfoTip(const char* text);
// Muted link that turns accent-text and underlined on hover; true on click.
bool Link(const char* label, std::uint32_t col = 0);

// ---- icons --------------------------------------------------------------
// Inline icon (one of PobIcon::*). With no icon font it draws nothing and takes
// no space.
void Icon(const char* icon, std::uint32_t col, float px = 0.0f);
void IconAt(ImDrawList* dl, const ImVec2& pos, const char* icon, std::uint32_t col, float px);
// Width of the icon at px, 0 without the icon font.
float IconWidth(const char* icon, float px);

// ---- actions ------------------------------------------------------------
enum class BtnKind { Primary, Secondary, Ghost, Danger, Update };
enum class BtnSize { Sm, Md, Lg };
bool Button(const char* label, BtnKind kind = BtnKind::Secondary, BtnSize size = BtnSize::Md,
            const char* icon = nullptr, float minWidth = 0.0f, bool enabled = true);
float ButtonWidth(const char* label, BtnSize size = BtnSize::Md, const char* icon = nullptr,
                  float minWidth = 0.0f);

bool Switch(const char* id, bool* value, bool enabled = true);
float SwitchWidth();

bool Segmented(const char* id, int* selected, const char* const* labels, int count, bool enabled = true);
// Per option: `itemEnabled` (null = all) and a tooltip (null = none) shown on
// hover even while that option is disabled -- the place to say WHY it is.
bool SegmentedEx(const char* id, int* selected, const char* const* labels, int count,
                 const bool* itemEnabled, const char* const* tips);
float SegmentedWidth(const char* const* labels, int count);

// Select (combo) with an optional right-aligned note per option. `labels` and
// `notes` are parallel; a null note means none. True when the selection changed.
bool Select(const char* id, int* selected, const char* const* labels, const char* const* notes,
            int count, float width, bool enabled = true);

struct SliderResult {
	bool changed = false;   // dragged this frame (preview)
	bool released = false;  // drag ended with an edit (commit)
	bool reset = false;     // "reset to default" pressed (commit, value = def)
	bool active = false;    // being dragged right now (do not mirror the setting)
};
// Track + current value (numeric) + "reset" ghost button, or the word "default"
// when the value already is the default. `edit` is the scratch value the caller
// mirrors from its setting while the slider is idle.
SliderResult SliderWithReset(const char* id, int* edit, int min, int max, int def,
                             const char* fmt, const char* resetLabel, const char* defaultLabel,
                             float trackWidth);
float SliderWithResetWidth(float trackWidth, const char* resetLabel);

// ---- status -------------------------------------------------------------
enum class Tone { Ok, Run, Warn, Bad, Idle };
void StatusPill(Tone tone, const char* text);
float PillWidth(const char* text);

enum class BannerTone { Info, Warn, Bad };
enum class BannerResult { None, Action, Close };
// Icon | title + description | action button | optional close. Full width of
// the current content region. `descMono` draws the description as a Numeric.
BannerResult Banner(const char* id, BannerTone tone, const char* icon, const char* title,
                    const char* desc, bool descMono, const char* action, bool closable,
                    bool actionEnabled = true, float width = 0.0f);

void ProgressBar(float fraction, float width);

// ---- layout -------------------------------------------------------------
// Page tabs (Tabs, underline style): label row + 1 px border, the selected tab
// underlined in accent. Returns the selection after this frame's click.
int PageTabs(const char* id, int selected, const char* const* labels, int count, float width = 0.0f);

// A collapsible section header for a side panel: optional icon + label, a
// right-aligned hint, a hairline above. `id` is the ImGui id ("###astrohdr")
// so the label may change (counts, language) without losing the open state.
bool CollapsingSection(const char* label, const char* id, const char* icon = nullptr,
                       const char* note = nullptr, bool defaultOpen = true);

// A square slot (map device, astrolabe quadrant): an image or a short text when
// filled; a dashed outline and a plus when empty. True on click.
bool Slot(const char* id, unsigned texture, const char* text, bool filled, float size, bool enabled = true);

// A search box: TextField look, a magnifier in front of the hint.
bool SearchField(const char* id, char* buf, int bufSize, const char* hint, float width);

// Menus (the tool header's more menu, slot menus): popup styling + rows with an
// optional icon. `danger` colours the row (delete, reset).
bool BeginMenuPopup(const char* id);
void EndMenuPopup();
bool MenuRow(const char* icon, const char* label, const char* shortcut = nullptr, bool enabled = true,
             bool danger = false);
void MenuSeparator();
// Card: surface-1 + border + radius-md, height follows the content. `title`
// draws a head row (icon + heading + right-aligned hint). `padded` = content
// padding (space-4 / space-5); unpadded cards are for SettingRows, which pad
// themselves.
void CardBegin(const char* id, const char* icon = nullptr, const char* title = nullptr,
               const char* note = nullptr, bool padded = false, float width = 0.0f);
void CardEnd();
// A button on the right of the innermost card's head row (cards with a title).
bool CardHeadButton(const char* label, BtnKind kind = BtnKind::Ghost, const char* icon = nullptr);
// Inner content box of the innermost card (screen x and width).
float CardInnerX();
float CardInnerWidth();

// SettingRow: label + hint (max two lines) left, controls right-aligned.
// Between RowBegin and RowEnd the cursor sits where the control block starts,
// `ctrlWidth` wide; lay controls out with SameLine. `ctrlHeight` 0 = ControlH().
// `disabled` greys the label (the hint should then say why).
void RowBegin(const char* label, const char* hint, float ctrlWidth, float ctrlHeight = 0.0f,
              const char* tip = nullptr, bool disabled = false);
void RowEnd();
// Gap between controls inside a row (space-2).
float RowGap();

bool SideNavItem(const char* icon, const char* label, bool active, float width);

// Tool tile: icon + name (+ badge) + one-line hint; `dot` = open in a tab.
bool ToolTile(const char* id, const char* icon, const char* name, const char* badge,
              const char* hint, bool dot, const ImVec2& size, bool enabled = true);

// ---- transient ----------------------------------------------------------
// Bottom-right toast, 4 s, one at a time (a new one replaces the old).
void ShowToast(const char* text, Tone tone = Tone::Ok);
// Call once per frame after the main window has ended.
void DrawToast();
// True while a toast is on screen (frame pacing must keep drawing its fade).
bool ToastVisible();

// Confirmation dialog: title is the question, body the consequence, `mono` the
// path or name involved. Buttons right-aligned: cancel | danger | primary (null
// to omit). Opens itself when `open` is set; Esc = cancel.
enum class DialogResult { None, Cancel, Danger, Primary, Secondary };
DialogResult ConfirmDialog(const char* popupId, bool* open, const char* title, const char* body,
                           const char* mono, const char* cancel, const char* danger,
                           const char* primary);
// The same dialog in parts, for one that needs its own content (a name field):
//   if (BeginDialog(id, &open, title, body)) { ...content...;
//       r = DialogButtons(...); EndDialog(); }
// DialogButtons closes the popup on any answer; Esc = cancel, and Enter =
// primary when `enterIsPrimary` (a single text field).
bool BeginDialog(const char* popupId, bool* open, const char* title, const char* body,
                 const char* mono = nullptr);
DialogResult DialogButtons(const char* cancel, const char* secondary, const char* danger,
                           const char* primary, bool primaryEnabled = true, bool enterIsPrimary = false);
void EndDialog();

// Headless checks for the widget maths (no GL): D(), ControlH, widths.
bool RunWidgetSelfTest();

} // namespace PobUi
