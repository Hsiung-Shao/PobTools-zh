#pragma once

struct ImVec4;

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
