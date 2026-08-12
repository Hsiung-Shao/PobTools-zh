#pragma once

struct ImVec4;
// Declared at global scope on purpose: a `struct ImGuiStyle&` written inside
// namespace PobUi would declare PobUi::ImGuiStyle, a brand new incomplete type
// that has nothing to do with ImGui's.
struct ImGuiStyle;

namespace PobUi {

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

ImVec4 Accent();
ImVec4 MutedText();
ImVec4 StatusColor(StatusTone tone);

// Creates an ImGui context, verifies the shared style at two scales, and
// destroys the context. This stays renderer-free so CI can run it headlessly.
bool RunThemeSelfTest();

} // namespace PobUi
