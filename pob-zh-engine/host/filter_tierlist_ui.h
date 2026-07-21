// PobTools tier-list UI + shared rule-style editor widget.
//
// DrawBlockStyleEditor is shared by the advanced per-block detail panel and the
// tier-list column style popup. DrawTierListView renders the three-pane tier-list
// (categories / tier columns / item library) with drag-and-drop re-tiering.
#pragma once

#include "filter_model.h"
#include "filter_tierlist.h"
#include "filter_i18n.h"
#include <string>

class IconManager;
class ItemLibrary;
class EconomyService;

// Edit a block's verb + colours + font + alert sound in place (marks dirty).
// Returns true if anything changed this frame. Callers should PushID(block) so
// repeated instances get unique widget IDs.
bool DrawBlockStyleEditor(FilterFile& model, FilterBlock& b, float scale);

// UI state for the tier-list view, owned by ShowFilterEditor across frames.
struct TierListUIState {
	int selectedCategory = 0;
	int activeTargetTier = -1;   // block index of the click-to-move target, or -1
	std::string itemSearch;
	std::string itemSearchLower;
	std::string baseClassFilter; // English item class to show ("" = all)
	std::string tagFilter;       // repoe tag sub-category to show ("" = all)
	std::string selectedItem;    // English name shown in the detail panel ("" = none)
	bool libShowAll = false;     // right pane: show the whole game catalog (add any item)
	bool needsRebuild = false;   // an edit changed items/structure; rebuild the index
	std::string status;          // last action message

	// drag payload (ImGui payloads must be POD, so the text lives here)
	std::string dragItem;
	int dragSrcBlock = -1;
};

// Draw the three-pane tier-list. Item names are shown via i18n (Chinese) while
// the underlying .filter values stay English; icons are fetched lazily via icons.
// Sets ui.needsRebuild when the caller must rebuild the index.
void DrawTierListView(FilterFile& model, TierListIndex& index, TierListUIState& ui,
                      const FilterI18n& i18n, IconManager& icons,
                      const ItemLibrary& library, const EconomyService& economy, float scale);
