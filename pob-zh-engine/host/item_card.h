// PobTools tier-list item card.
//
// One item rendered as a card: icon + Chinese name + English name, with optional
// economy value and a "suggested tier" badge (filled in once the economy service is
// available; empty otherwise). Display only — the English name is the drag key and
// the value/badge never change what gets written to the .filter.
#pragma once

#include <string>

class IconManager;

struct ItemCard {
	std::string en;             // English name (drag key, icon lookup, tooltip)
	std::string zh;             // Chinese display name (shown large)
	std::string value;          // economy value text; empty -> not shown
	std::string badge;          // suggested-tier hint e.g. "紅階 → 紫階"; empty -> not shown
	bool badgeUpgrade = true;   // green when promoting, amber when demoting
};

// Fixed card height at the given scale (so list clippers can assume uniform rows).
float ItemCardHeight(float scale, bool hasValue);

// Draw an interactive item card of width cardW. Returns true if clicked this frame.
// The card's clickable widget is the "last item" afterward, so the caller may attach
// ImGui::BeginDragDropSource() / ImGui::IsItemHovered() to it.
bool DrawItemCard(const ItemCard& c, float cardW, float scale, IconManager& icons);
