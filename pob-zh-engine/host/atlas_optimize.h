// PobTools atlas planner: minimum-point allocation solver.
//
// The planner's click model is "every node you click yourself is a TARGET; the
// nodes the solver adds are just wiring". The allocation is therefore a pure
// function of the target set:
//
//     alloc = fewest nodes that connect {start} + targets
//
// which is a Steiner tree in a graph. Every node costs exactly one point and
// the start node is free, so any connected subgraph can be spanned by a tree
// with nodes = edges + 1, i.e. points = nodes - 1 = edge count. That makes the
// plain unit-edge-weight Dreyfus-Wagner DP exact here -- no node-weighted
// Steiner reduction is needed.
//
// Pure data + graph logic; no ImGui / GL.
#pragma once

#include "atlas_tree_data.h"

#include <string>
#include <vector>

// Largest target count (start node excluded) still solved exactly. Above this
// the DP's O(3^k * n) blows up and the heuristic takes over. The value is set
// from the benchmark in RunAtlasOptSelfTest, which prints the measured timings.
int AtlasOptExactCap();

struct AtlasPlan {
	std::vector<int> nodes;        // node indices of the tree, ascending, includes root
	int points = 0;                // nodes.size() - 1  (the start node is free)
	bool exact = false;            // false => the heuristic produced this
	std::vector<int> unreachable;  // targets that cannot be connected at all
	bool ok() const { return unreachable.empty(); }
	bool has(int idx) const;
};

// Fewest-point allocation connecting the start node to every target, never
// routing through a blocked node.
//
// `blocked` (may be empty) are nodes the user has ruled out. Blocking simply
// takes the induced subgraph, so the result stays exactly as optimal as it was
// -- there is no approximation introduced by the exclusion itself.
//
// warmStart (may be empty) is the allocation currently on screen. It is used
// two ways, both deliberate:
//   - as a heuristic starting point above the exact cap, and
//   - as a TIE-BREAK: when it is already a valid solution of the same size it
//     is returned unchanged, so wiring never shuffles without buying a point.
//
// Targets that cannot be reached from the start node -- whether because the
// data is broken or because blocking walled them off -- are reported in
// AtlasPlan::unreachable and left out of the solve. They are NEVER left sitting
// in `nodes` disconnected from the root: every node in a returned plan is
// reachable from the start through the plan itself.
AtlasPlan AtlasPlanMinimal(const AtlasTreeData& d,
                           const std::vector<int>& targets,
                           const std::vector<int>& blocked,
                           const std::vector<int>& warmStart);

// The target set clicking `idx` produces, given what is allocated now. This is
// the whole click model in one place:
//   unallocated node  -> becomes a target
//   allocated target  -> stops being one
//   allocated wiring  -> gives up the targets that hang off it (the rule the
//                        planner has always used for a removal, expressed on
//                        targets instead of on nodes)
// The allocation is then re-derived with AtlasPlanMinimal; nothing edits it
// directly. Returns the current targets unchanged for the start node.
std::vector<int> AtlasTargetsAfterClick(const AtlasTreeData& d, int idx);

// Target set inferred for a build saved before targets existed: every
// allocated notable / keystone / wormhole, plus every leaf of the allocated
// subgraph. Interior small nodes are treated as wiring, which is what lets an
// old build compress at all. Never applied automatically -- the planner shows
// the result and lets the user choose (see atlas_planner.cpp).
std::vector<int> AtlasInferTargets(const AtlasTreeData& d);

// Headless solver check driven by "pob-zh.exe --atlas-opt-selftest": exactness
// against brute force, order independence, heuristic quality and timings.
// Prints a report (attached console + atlas_opt_selftest.txt) and returns 0 on
// full pass.
int RunAtlasOptSelfTest(const std::wstring& exeDir);
