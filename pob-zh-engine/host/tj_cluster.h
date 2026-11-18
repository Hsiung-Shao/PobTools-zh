// Cluster jewels a POB build has socketed, expanded the way POB does it, for the timeless jewel calculator's tree.
// POB builds the subgraphs itself (PassiveSpec:BuildSubgraph); a one-shot headless POB child (bridge.lua
// get_tree_state + tree_assets) hands over the result and this file turns it into drawable nodes/edges.
// Display only: timeless / Abyss jewels never touch cluster passives.
#pragma once

#include "passive_tree_data.h"

#include <json.hpp>

#include <string>
#include <vector>

struct TJClusterNode {
	int id = 0;                          // POB node id (cluster passives: 0x10000+...; inner sockets: tree id)
	std::string name, nameZh;
	int kind = kPtNormal;                // kPtNormal / kPtNotable / kPtKeystone / kPtSocket
	float x = 0, y = 0;                  // world coords (same space as PassiveTreeData)
	std::vector<std::string> stats, statsZh;
	PtSprite on, off;                    // icon (allocated / unallocated); invalid when the sheet is not ours
	PtSprite frameOn, frameOff;          // frame art for the two states; invalid when unresolved
	bool alloc = false;
};

struct TJClusterEdge {
	int a = 0, b = 0;                    // index into TJClusterGraph::nodes, or into PassiveTreeData::nodes when *Main
	bool aMain = false, bMain = false;   // that end is a main-tree node (the Large socket the cluster hangs off)
	bool hasArc = false;                 // both ends on the same group orbit: draw on the circle
	float cx = 0, cy = 0, r = 0;
	float a0 = 0, sweep = 0;             // ImGui PathArcTo angles (radians), sweep >= 0
};

struct TJClusterGraph {
	std::vector<TJClusterNode> nodes;
	std::vector<TJClusterEdge> edges;
};

// treeState: bridge get_tree_state result (dynamicNodes / dynamicConnectors / dynamicGroups).
// treeAssets: bridge tree_assets result (assets / disabled: sprite name -> {file,x,y,w,h}).
// A sprite resolves only when its file's basename is one of tree.sheets (uv = rect / that sheet's w,h; world size =
// px * tree.worldPerPx). Mastery nodes are skipped. A connector end that is neither a kept cluster node nor a main
// tree node drops the edge. false (with *err) only when treeState has no dynamicNodes array.
bool ParseClusterGraph(const nlohmann::json& treeState, const nlohmann::json& treeAssets, const PassiveTreeData& tree,
                       TJClusterGraph& out, std::string* err);

// Boots a headless POB on a sandbox of pobDir (the MCP server's sandbox rules), loads buildPath, reads
// get_tree_state + tree_assets (sprites of assetsTreeVersion when given, else the build's tree) and stops the child. Blocking (seconds): call it off the UI thread.
bool TJClusterFetchSync(const std::wstring& exeDir, const std::wstring& pobDir, const std::wstring& buildPath,
                        nlohmann::json& treeState, nlohmann::json& treeAssets, std::string* err,
                        const std::string& assetsTreeVersion = {});

// --tj-cluster-selftest [pobDir]: report at <exeDir>tj_cluster_selftest.txt; returns the failed-check count.
int RunTJClusterSelfTest(const std::wstring& exeDir, const std::wstring& pobDirArg);
