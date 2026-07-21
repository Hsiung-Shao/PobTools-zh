#include "ui_theme.h"

#include <imgui.h>

#include <cmath>

namespace {

ImVec4 Rgba(int r, int g, int b, int a = 255)
{
	return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

bool Near(float a, float b)
{
	return std::fabs(a - b) < 0.01f;
}

} // namespace

namespace PobUi {

void ApplyTheme(float scale, Density density)
{
	ImGui::StyleColorsDark();
	ImGuiStyle& s = ImGui::GetStyle();
	ImVec4* c = s.Colors;

	// Graphite surfaces keep the editors quiet; the original PobTools indigo is
	// reserved for focus and primary actions so semantic status colours stay clear.
	c[ImGuiCol_Text]                  = Rgba(237, 243, 245);
	c[ImGuiCol_TextDisabled]          = Rgba(136, 153, 162);
	c[ImGuiCol_WindowBg]              = Rgba(11, 16, 20);
	c[ImGuiCol_ChildBg]               = Rgba(15, 22, 27);
	c[ImGuiCol_PopupBg]               = Rgba(17, 25, 31, 252);
	c[ImGuiCol_Border]                = Rgba(43, 57, 66);
	c[ImGuiCol_BorderShadow]          = Rgba(0, 0, 0, 0);
	c[ImGuiCol_FrameBg]               = Rgba(20, 29, 35);
	c[ImGuiCol_FrameBgHovered]        = Rgba(28, 40, 47);
	c[ImGuiCol_FrameBgActive]         = Rgba(37, 40, 74);
	c[ImGuiCol_TitleBg]               = Rgba(11, 16, 20);
	c[ImGuiCol_TitleBgActive]         = Rgba(11, 16, 20);
	c[ImGuiCol_MenuBarBg]             = Rgba(13, 19, 24);
	c[ImGuiCol_ScrollbarBg]           = Rgba(11, 16, 20, 180);
	c[ImGuiCol_ScrollbarGrab]         = Rgba(55, 72, 81);
	c[ImGuiCol_ScrollbarGrabHovered]  = Rgba(72, 92, 101);
	c[ImGuiCol_ScrollbarGrabActive]   = Rgba(99, 102, 241);
	c[ImGuiCol_CheckMark]             = Rgba(99, 102, 241);
	c[ImGuiCol_SliderGrab]            = Rgba(99, 102, 241);
	c[ImGuiCol_SliderGrabActive]      = Rgba(129, 140, 248);
	c[ImGuiCol_Button]                = Rgba(24, 35, 42);
	c[ImGuiCol_ButtonHovered]         = Rgba(33, 48, 56);
	c[ImGuiCol_ButtonActive]          = Rgba(46, 48, 86);
	c[ImGuiCol_Header]                = Rgba(38, 40, 81);
	c[ImGuiCol_HeaderHovered]         = Rgba(49, 52, 111);
	c[ImGuiCol_HeaderActive]          = Rgba(58, 61, 135);
	c[ImGuiCol_Separator]             = Rgba(39, 52, 60);
	c[ImGuiCol_SeparatorHovered]      = Rgba(99, 102, 241);
	c[ImGuiCol_SeparatorActive]       = Rgba(129, 140, 248);
	c[ImGuiCol_ResizeGrip]            = Rgba(99, 102, 241, 50);
	c[ImGuiCol_ResizeGripHovered]     = Rgba(99, 102, 241, 150);
	c[ImGuiCol_ResizeGripActive]      = Rgba(129, 140, 248, 220);
	c[ImGuiCol_Tab]                   = Rgba(20, 29, 35);
	c[ImGuiCol_TabHovered]            = Rgba(49, 52, 111);
	c[ImGuiCol_TabActive]             = Rgba(43, 45, 96);
	c[ImGuiCol_TabUnfocused]          = Rgba(17, 25, 31);
	c[ImGuiCol_TabUnfocusedActive]    = Rgba(31, 34, 68);
	c[ImGuiCol_PlotHistogram]         = Rgba(99, 102, 241);
	c[ImGuiCol_TableHeaderBg]         = Rgba(19, 28, 34);
	c[ImGuiCol_TableBorderStrong]     = Rgba(43, 57, 66);
	c[ImGuiCol_TableBorderLight]      = Rgba(35, 47, 55);
	c[ImGuiCol_TableRowBg]            = Rgba(15, 22, 27);
	c[ImGuiCol_TableRowBgAlt]         = Rgba(18, 26, 31);
	c[ImGuiCol_TextSelectedBg]        = Rgba(79, 70, 229, 150);
	c[ImGuiCol_NavHighlight]          = Rgba(99, 102, 241);
	c[ImGuiCol_ModalWindowDimBg]      = Rgba(0, 0, 0, 150);

	s.WindowRounding = 0.0f;
	s.ChildRounding = 6.0f;
	s.FrameRounding = 5.0f;
	s.PopupRounding = 6.0f;
	s.ScrollbarRounding = 6.0f;
	s.GrabRounding = 3.0f;
	s.TabRounding = 5.0f;
	s.WindowBorderSize = 0.0f;
	s.ChildBorderSize = 1.0f;
	s.PopupBorderSize = 1.0f;
	s.FrameBorderSize = 1.0f;
	s.TabBorderSize = 0.0f;
	s.ScrollbarSize = 12.0f;
	s.GrabMinSize = 10.0f;
	s.IndentSpacing = 18.0f;

	s.WindowPadding = ImVec2(22.0f, 18.0f);
	s.FramePadding = ImVec2(10.0f, 6.0f);
	s.ItemSpacing = ImVec2(8.0f, 8.0f);
	s.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
	s.CellPadding = ImVec2(10.0f, 7.0f);
	if (density == Density::Compact) {
		s.WindowPadding = ImVec2(16.0f, 12.0f);
		s.FramePadding = ImVec2(8.0f, 5.0f);
		s.ItemSpacing = ImVec2(7.0f, 7.0f);
		s.CellPadding = ImVec2(8.0f, 6.0f);
	} else if (density == Density::Canvas) {
		s.WindowPadding = ImVec2(12.0f, 10.0f);
		s.FramePadding = ImVec2(8.0f, 5.0f);
		s.ItemSpacing = ImVec2(7.0f, 6.0f);
		s.CellPadding = ImVec2(8.0f, 5.0f);
	}
	s.ScaleAllSizes(scale);
}

void PushPrimaryButton()
{
	ImGui::PushStyleColor(ImGuiCol_Button, Rgba(55, 48, 137));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgba(79, 70, 229));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgba(49, 46, 129));
	ImGui::PushStyleColor(ImGuiCol_Text, Rgba(248, 252, 251));
}

void PushDangerButton()
{
	ImGui::PushStyleColor(ImGuiCol_Button, Rgba(101, 43, 47));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgba(133, 51, 57));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgba(79, 34, 38));
	ImGui::PushStyleColor(ImGuiCol_Text, Rgba(255, 235, 236));
}

void PopButtonStyle()
{
	ImGui::PopStyleColor(4);
}

ImVec4 Accent()
{
	return Rgba(99, 102, 241);
}

ImVec4 MutedText()
{
	return Rgba(136, 153, 162);
}

ImVec4 StatusColor(StatusTone tone)
{
	switch (tone) {
		case StatusTone::Success: return Rgba(102, 211, 143);
		case StatusTone::Warning: return Rgba(232, 181, 91);
		case StatusTone::Error:   return Rgba(239, 105, 111);
		default:                  return MutedText();
	}
}

bool RunThemeSelfTest()
{
	ImGui::CreateContext();
	ApplyTheme(1.0f, Density::Comfortable);
	const ImGuiStyle first = ImGui::GetStyle();
	bool ok = Near(first.FrameRounding, 5.0f) && Near(first.WindowPadding.x, 22.0f) &&
		first.Colors[ImGuiCol_Text].x > first.Colors[ImGuiCol_WindowBg].x &&
		first.Colors[ImGuiCol_CheckMark].z > first.Colors[ImGuiCol_CheckMark].y;

	ApplyTheme(1.5f, Density::Compact);
	const ImGuiStyle second = ImGui::GetStyle();
	// ImGui floors scaled dimensions to whole pixels.
	ok = ok && Near(second.FrameRounding, 7.0f) && Near(second.WindowPadding.x, 24.0f) &&
		Near(second.FramePadding.y, 7.0f) && Near(second.ScrollbarSize, 18.0f);
	ImGui::DestroyContext();
	return ok;
}

} // namespace PobUi
