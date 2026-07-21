// PobTools passive-tree canvas for the Timeless Jewel calculator: pan/zoom
// camera, ImDrawList rendering of the character tree (group backgrounds,
// edges/arcs, node icons + frames, masteries), the socketed jewel's radius
// circle, and highlight overlays for nodes the jewel transforms.
//
// Read-only view (no allocation): the caller owns the PassiveTreeData and tells
// the view which socket is selected and which nodes are affected. The view
// reports the hovered node / clicked socket back so the caller can drive the
// selection and draw its own transform tooltip.
//
// Owns the GL textures for the sprite sheets, loaded at runtime from the user's
// PoB folder (never bundled).
#pragma once

#include "passive_tree_data.h"

#include <imgui.h>
#include <string>
#include <vector>

// Per-node highlight class the caller computes (parallel to data.nodes).
enum : unsigned char {
	kPtHiNone = 0,
	kPtHiAffected = 1,   // stats rolled/added by the jewel (gold)
	kPtHiReplaced = 2,   // notable/keystone swapped for another (blue)
};

struct PassiveTreeInput {
	int selectedSocket = -1;                 // node index of the socketed jewel (-1 none)
	float radiusWorld = 1800.0f;             // jewel radius (Large = timeless)
	const std::vector<unsigned char>* hi = nullptr;       // per-node highlight class (size==nodes)
	const std::vector<char>* disabled = nullptr;          // per-node: dimmed & excluded
	const std::vector<char>* selected = nullptr;          // per-node: draw a "picked" marker
	int emphasize = -1;                                   // node index to ring-pulse (list pick)
};

struct PassiveTreeOutput {
	int hoveredNode = -1;                    // node under the cursor (-1 none)
	int clickedSocket = -1;                  // a socket clicked this frame (-1 none)
	int clickedNode = -1;                    // a non-socket node clicked this frame (-1 none)
};

class PassiveTreeView {
public:
	// Uploads every sheet from <exeDir>PathOfBuildingCommunity/TreeData/<ver>/.
	// GL-thread only; returns false (with *err) when a sheet is missing/undecodable.
	bool LoadTextures(const std::wstring& exeDir, const PassiveTreeData& d, std::string* err);
	void DestroyTextures();

	// Fills the remaining content region with the tree canvas + interaction.
	PassiveTreeOutput Draw(const PassiveTreeData& d, float uiScale, const PassiveTreeInput& in);

	// Glide the camera to a node (e.g. the newly selected socket).
	void CenterOn(const PassiveTreeData& d, int nodeIdx);
	void ResetView() { zoom_ = 0.0f; }       // refit on next Draw

	// Debug (--pt-render): pin the camera before Draw. zoom<=0 keeps auto-fit.
	void SetCamera(float zoom, float cx, float cy) { zoom_ = zoom; center_ = ImVec2(cx, cy); }

private:
	ImVec2 worldToScreen(ImVec2 w) const;
	ImVec2 screenToWorld(ImVec2 s) const;
	int hitTest(const PassiveTreeData& d, ImVec2 mouseWorld, bool socketsOnly) const;

	std::vector<unsigned> tex_;
	ImVec2 vpPos_{ 0, 0 }, vpSize_{ 0, 0 };
	ImVec2 center_{ 0, 0 };
	float zoom_ = 0.0f;
	float minZoom_ = 0.02f;

	int hover_ = -1;
	ImVec2 focusTarget_{ 0, 0 };
	float focusZoomTarget_ = 0.0f;
	bool focusAnim_ = false;
};
