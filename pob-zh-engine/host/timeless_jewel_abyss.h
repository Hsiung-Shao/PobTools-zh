// Abyss timeless jewels (types 7-11), PoB 2.67's ABYS / ABYN containers.
//
// This is a separate engine on purpose. The Legion jewels answer "what does
// node N become under seed S?" one node at a time, and the caller decides which
// nodes to ask about by walking the jewel's radius. The Abyss jewels invert
// that: the file is keyed by (jewel socket, seed) and NAMES the conquered
// passives itself. There is no radius and no per-node lookup, so none of
// TJReadLUT's addressing applies.
//
//   ABYS  types 7-10 (Tecrod / Ulaman / Kurgal / Amanamu): one block per jewel
//         socket, holding one record per seed. A record lists the conquered
//         nodes and what each becomes.
//   ABYN  type 11 (Zorath): one block per passive node, plus an "ASCS" section
//         choosing one notable per Ascendancy. Reading it needs the character's
//         allocated path from the socket to their class start, which PobTools
//         does not track, so type 11 is parsed but not offered.
//
// Records are variable length: a seed's offset is only knowable by counting
// from the start of its block. Offsets are therefore cached per block on first
// use, exactly as PoB does.
//
// Verified against PoB 2.67.1's own reader and, independently, against
// Parazeya/abyss-jewels (MIT), whose walk derives the same node sets from the
// game's algorithm without reading PoB's files at all. See --abyss-selftest.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "timeless_jewel.h"

// One part of what a conquered passive becomes.
struct TJAbyssComponent {
	int type = 0;            // 1 = the node is replaced, 2 = stats are added to it
	int globalId = 0;        // already mapped through localIdToGlobalId
	std::vector<int> rolls;  // signed as stored; see TJAbyssRollFor
};

// What one conquered passive becomes. Usually one component, occasionally more.
typedef std::vector<TJAbyssComponent> TJAbyssMod;

// Parsed container: header plus where each block starts. Building it walks every
// record once (the files are ~70-100 MB inflated), so it is done once per jewel
// type and kept.
struct TJAbyssLUT {
	bool ok = false;
	std::string fmt;                          // "ABYS" or "ABYN"
	int jewelType = 0;
	int seedMin = 0, seedMax = 0, seedInc = 0, seedCount = 0;
	int abyssSize = 0;                        // ABYS only; PoB reads it but never uses it
	std::vector<int> socketIds;               // ABYS: jewel sockets, in file order
	std::map<int, size_t> blockOffsets;       // ABYS: socket id -> block; ABYN: node id
	std::map<std::string, size_t> ascOffsets; // ABYN only: Ascendancy name -> block

	// Filled lazily, per block. Key is the block key (socket id / node id).
	std::map<int, std::vector<size_t> > seedOffsets;
};

// Types this file handles. 7-10 are usable; 11 parses but has no caller.
bool TJIsAbyss(int jewelType);
bool TJAbyssUsable(int jewelType); // 7-10 only

// Parse the container header and index its blocks. Fails loudly rather than
// half-working: a header that does not match `jewelType` is a mismatched or
// truncated file, not something to guess around.
bool TJAbyssParse(const std::string& blob, int jewelType, TJAbyssLUT& out, std::string* err);

// Seed -> index within a block, or -1 when the seed is not addressable (out of
// range, or not on the increment).
int TJAbyssSeedIndex(const TJAbyssLUT& lut, int seed);

// Conquered passives for one (socket, seed): node id -> what it becomes.
// ABYS only. `lut` is mutated (offset cache), hence non-const.
bool TJAbyssReadSocket(const TJDataset& ds, const std::string& blob, TJAbyssLUT& lut,
                       int socketId, int seed, std::map<int, TJAbyssMod>& out);

// The value to show for one stat of a component. A stored negative is shown
// positive when the stat's own range is non-negative, because those templates
// already carry the direction in the word "reduced" -- printing "-16% reduced"
// would say the opposite of what the game shows.
double TJAbyssRollFor(const TJAbyssComponent& c, const TJStatMod& m);

// Render one conquered passive: the lines the jewel grants, English + Chinese.
// `replaced` is true when the passive becomes a different one entirely.
TJTransform TJAbyssApply(const TJDataset& ds, const TJAbyssMod& mod);

// Rank seeds for one socket, using the same wanted-stat rules as TJSearch so a
// hit means the same thing in both engines.
//
// `nodeKind` maps a passive id to passive_tree_data.h's kPt* value and decides
// what q.scope means. It is not optional in practice: the Abyss walk conquers
// keystones, and 48 of the ones it reaches are absent from ds.nodeIndex (the
// Legion LUT only indexes what that LUT addresses). Falling back to the index
// would file every keystone under "small passive", so a "notables only" search
// would silently drop Crimson Dance and friends. Pass nullptr only when no tree
// is available, and expect that blind spot.
std::vector<TJSeedHit> TJAbyssSearch(const TJDataset& ds, const std::string& blob,
                                     TJAbyssLUT& lut, const TJSearchQuery& q,
                                     int socketId, int topN,
                                     const std::map<int, int>* nodeKind = nullptr,
                                     const volatile bool* cancel = nullptr);

// scope -> does this passive count? Shared so the UI marks exactly what the
// search counted. scope 1 keeps notables AND keystones (both are "the big ones"
// to anyone reading the tree); scope 2 keeps only small passives.
bool TJAbyssInScope(int scope, int nodeId, const TJDataset& ds,
                    const std::map<int, int>* nodeKind);

// Unique stat templates an Abyss jewel can produce, for the picker. Mirrors
// TJStatTemplates but keyed on the Abyss id prefixes.
std::vector<TJStatTemplate> TJAbyssStatTemplates(const TJDataset& ds, int jewelType);

// --abyss-selftest
int RunAbyssSelfTest(const std::wstring& exeDir);

// --abyss <jewelType> <socketId> <seed>; socketId 0 lists the sockets instead.
int RunAbyssCli(const std::wstring& exeDir, int jewelType, int socketId, int seed);
