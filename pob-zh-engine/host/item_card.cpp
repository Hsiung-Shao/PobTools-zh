#include "item_card.h"
#include "icon_manager.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

float ItemCardHeight(float scale, bool hasValue)
{
	const float pad = 8 * scale;
	const float iconSz = 38 * scale;
	const float lineH = ImGui::GetTextLineHeight();
	const float spacing = 3 * scale;
	const float contentH = (iconSz > 2 * lineH + spacing) ? iconSz : (2 * lineH + spacing);
	return pad + contentH + (hasValue ? (spacing + lineH) : 0) + pad;
}

bool DrawItemCard(const ItemCard& c, float cardW, float scale, IconManager& icons)
{
	const float pad = 8 * scale;
	const float iconSz = 38 * scale;
	const float lineH = ImGui::GetTextLineHeight();
	const float spacing = 3 * scale;
	const bool hasValue = !c.value.empty();
	const float cardH = ItemCardHeight(scale, hasValue);

	bool clicked = ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
	const bool hovered = ImGui::IsItemHovered();
	const ImVec2 pmin = ImGui::GetItemRectMin();
	const ImVec2 pmax = ImGui::GetItemRectMax();
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// card surface
	ImU32 bg = ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
	dl->AddRectFilled(pmin, pmax, bg, 6 * scale);
	dl->AddRect(pmin, pmax, ImGui::GetColorU32(ImGuiCol_Border), 6 * scale);

	dl->PushClipRect(pmin, pmax, true);

	// icon (lazy fetch; placeholder box until ready)
	icons.Request(c.en);
	unsigned tex = icons.Texture(c.en);
	ImVec2 ip(pmin.x + pad, pmin.y + pad);
	ImVec2 ip2(ip.x + iconSz, ip.y + iconSz);
	if (tex) dl->AddImage((ImTextureID)(intptr_t)tex, ip, ip2);
	else     dl->AddRectFilled(ip, ip2, ImGui::GetColorU32(ImGuiCol_ChildBg), 4 * scale);

	// text block beside the icon
	const float tx = ip2.x + pad;
	dl->AddText(ImVec2(tx, pmin.y + pad), ImGui::GetColorU32(ImGuiCol_Text), c.zh.c_str());
	dl->AddText(ImVec2(tx, pmin.y + pad + lineH + spacing), ImGui::GetColorU32(ImGuiCol_TextDisabled), c.en.c_str());

	// economy value (bottom-right) when present
	if (hasValue) {
		ImVec2 vsz = ImGui::CalcTextSize(c.value.c_str());
		dl->AddText(ImVec2(pmax.x - pad - vsz.x, pmax.y - pad - lineH), IM_COL32(176, 214, 138, 255), c.value.c_str());
	}

	dl->PopClipRect();

	// suggested-tier badge (top-right, drawn over the clip so it stays crisp)
	if (!c.badge.empty()) {
		ImVec2 bsz = ImGui::CalcTextSize(c.badge.c_str());
		float bw = bsz.x + 8 * scale, bh = bsz.y + 4 * scale;
		ImVec2 bmax(pmax.x - 4 * scale, pmin.y + 4 * scale + bh);
		ImVec2 bmin(bmax.x - bw, pmin.y + 4 * scale);
		ImU32 badgeBg = c.badgeUpgrade ? IM_COL32(56, 130, 72, 235) : IM_COL32(150, 112, 48, 235);
		dl->AddRectFilled(bmin, bmax, badgeBg, 4 * scale);
		dl->AddText(ImVec2(bmin.x + 4 * scale, bmin.y + 2 * scale), IM_COL32(242, 242, 242, 255), c.badge.c_str());
	}

	return clicked;
}
