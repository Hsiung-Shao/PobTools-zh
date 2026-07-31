// PobTools atlas planner canvas: pan/zoom camera, ImDrawList rendering of the
// tree (group backgrounds, edges/arcs, node icons + frames, masteries), hover
// hit-testing, tooltips and click-to-(de)allocate.
//
// Owns the GL textures for the sprite sheets. Pure view: all graph state lives
// in AtlasTreeData.
#pragma once

#include "atlas_tree_data.h"
#include "atlas_optimize.h"

#include <imgui.h>
#include <string>
#include <unordered_map>
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
	//
	// planning = the sandbox planning mode: left click cycles a node through
	// want -> avoid -> clear (so several core nodes can be marked quickly) and
	// the allocation is re-derived after every mark. The caller is responsible
	// for the sandbox itself — this class never touches the build file.
	bool Draw(AtlasTreeData& d, float uiScale, const AtlasI18n* zh = nullptr, bool planning = false);

	// Glides the camera to node idx (raising zoom to a readable level when far
	// out) and starts a short fading highlight ring. User pan/zoom cancels the
	// glide. No-op before the first Draw sized the view.
	void CenterOn(const AtlasTreeData& d, int nodeIdx);

	// One-line status for the toolbar (hovered node name, or a rejection note).
	const std::string& StatusLine() const { return status_; }
	// True while a refused click is still worth shouting about: the toolbar
	// line alone reads as "nothing happened", which is what users reported.
	bool RejectActive() const { return rejectFlash_ > 0.0f; }

	// Version-compare overlay: draw a colored ring on each listed node index
	// (added = green, modified = amber). Pass an empty map to clear.
	void SetDiffOverlay(const std::unordered_map<int, ImU32>& ringByNodeIdx) { diffRing_ = ringByNodeIdx; }
	void ClearDiffOverlay() { diffRing_.clear(); }

private:
	ImVec2 worldToScreen(ImVec2 w) const;
	ImVec2 screenToWorld(ImVec2 s) const;
	void updateHover(AtlasTreeData& d, ImVec2 mouseWorld, float dt);
	void solvePreview(AtlasTreeData& d);
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
	float dwell_ = 0.0f;             // seconds the cursor has rested on hover_

	// Preview of what clicking hover_ would do. A click applies exactly this
	// cached plan rather than re-solving, so the number in the tooltip and the
	// allocation the user gets can never disagree.
	bool planReady_ = false;
	std::vector<int> planTargets_;   // target set the plan was solved for
	std::vector<int> planNodes_;     // resulting allocation (node indices)
	std::vector<int> hoverAdd_;      // nodes the click would allocate
	std::vector<int> hoverDrop_;     // nodes the click would give up (re-routing!)
	int planPoints_ = 0;
	bool planExact_ = true;
	// Clicking would change neither the targets nor the allocation. A minimal
	// plan cannot produce this -- prune_redundant guarantees every non-target
	// node is a cut vertex (asserted by --atlas-opt-selftest) -- so this is a
	// guard for allocations that are not minimal (hand-edited file, a build
	// migrated with "keep as is"). Saying so beats a click that appears to do
	// nothing, and beats writing the file for it.
	bool planNoop_ = false;

	std::vector<char> mark_;         // node -> preview membership (fast edge tint)
	std::string status_;
	float rejectFlash_ = 0.0f;       // seconds left on the "not enough points" banner

	std::unordered_map<int, ImU32> diffRing_; // version-compare overlay (node idx -> ring color)
};
