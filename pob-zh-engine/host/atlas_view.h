// PobTools atlas planner canvas: pan/zoom camera, ImDrawList rendering of the
// tree (group backgrounds, edges/arcs, node icons + frames, masteries), hover
// hit-testing, tooltips and click-to-(de)allocate.
//
// Owns the GL textures for the sprite sheets. Pure view: all graph state lives
// in AtlasTreeData.
#pragma once

#include "atlas_tree_data.h"

#include <imgui.h>
#include <string>
#include <vector>

class AtlasI18n;

class AtlasView {
public:
	// Uploads every sheet in d.sheets from exeDir/Data/. GL-thread only, call
	// after the context exists. Returns false (with *err) when a sheet fails.
	bool LoadTextures(const std::wstring& exeDir, const AtlasTreeData& d, std::string* err);

	// GL-thread only, while the context is still current (before ImGui shutdown).
	void DestroyTextures();

	// Fills the remaining content region with the tree canvas and handles all
	// interaction. Returns true when the allocation changed this frame.
	// zh (optional) switches tooltip names/stats to Traditional Chinese.
	bool Draw(AtlasTreeData& d, float uiScale, const AtlasI18n* zh = nullptr);

	// Glides the camera to node idx (raising zoom to a readable level when far
	// out) and starts a short fading highlight ring. User pan/zoom cancels the
	// glide. No-op before the first Draw sized the view.
	void CenterOn(const AtlasTreeData& d, int nodeIdx);

	// One-line status for the toolbar (hovered node name, or a rejection note).
	const std::string& StatusLine() const { return status_; }

private:
	ImVec2 worldToScreen(ImVec2 w) const;
	ImVec2 screenToWorld(ImVec2 s) const;
	void updateHover(AtlasTreeData& d, ImVec2 mouseWorld);
	void drawDecos(const AtlasTreeData& d, ImDrawList* dl, const std::vector<AtlasDeco>& decos);
	void drawEdges(const AtlasTreeData& d, ImDrawList* dl);
	void drawNodes(const AtlasTreeData& d, ImDrawList* dl);
	void drawTooltip(const AtlasTreeData& d, float uiScale, const AtlasI18n* zh);

	std::vector<unsigned> tex_;      // aligned with d.sheets
	ImVec2 vpPos_{ 0, 0 }, vpSize_{ 0, 0 };
	ImVec2 center_{ 0, 0 };          // world coords shown at the viewport center
	float zoom_ = 0.0f;              // screen px per world unit; 0 = fit on first frame
	float minZoom_ = 0.02f;

	// click-to-focus from the side panel
	ImVec2 focusTarget_{ 0, 0 };
	float focusZoomTarget_ = 0.0f;
	bool focusAnim_ = false;
	int focusNode_ = -1;
	float focusTimer_ = 0.0f;        // seconds left on the highlight ring

	int hover_ = -1;
	bool allocDirty_ = true;         // recompute hover previews after changes
	int previewFor_ = -1;
	std::vector<int> hoverPath_;     // hovering an unallocated node: nodes to add
	std::vector<int> hoverRemove_;   // hovering an allocated node: nodes lost
	std::vector<char> mark_;         // node -> preview membership (fast edge tint)
	std::string status_;
};
