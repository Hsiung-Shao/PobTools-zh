// PobTools tier-list index: a read-only view derived from a parsed FilterFile.
//
// NeverSink-style filters tag every block header comment with "$type->category",
// "$tier->name" and an optional "%style" code. This layer parses that metadata
// and groups blocks into "category -> tiers -> items (BaseType values)" so the
// UI can present a tier-list and move items between tiers. It holds only indices
// back into the FilterFile (plus a copy of item display text), never duplicating
// conditions/actions; edits go straight back to the FilterFile and the index is
// rebuilt.
#pragma once

#include "filter_model.h"
#include <string>
#include <vector>

struct TierMarkers {
	std::string styleCode;              // "%D8" -> "D8"; empty when absent
	std::vector<std::string> typePath;  // "$type->a->b" -> {"a","b"}
	std::string tierName;               // "$tier->name" -> "name"
	bool hasType() const { return !typePath.empty(); }
};

struct TierEntry {
	int blockIndex = -1;                // index into FilterFile::blocks (stable identity)
	TierMarkers markers;
	std::string tierLabel;              // tierName, else a short condition fallback
	bool hide = false;
	int baseTypeLineIdx = -1;           // FilterFile::lines index of the BaseType line; -1 = mod-based
	bool baseTypeUsesEqEq = true;       // the BaseType line used "==" (vs no operator)
	std::vector<std::string> items;     // the BaseType line's value texts (display + drag key)
	bool draggable() const { return baseTypeLineIdx >= 0; }
};

struct TierCategory {
	std::string key;                    // friendly group key ("currency"/"gear"/"other"...)
	std::string display;                // friendly Chinese label
	std::vector<int> tierEntryIdx;      // indices into TierListIndex::tiers, file order
	int totalItems = 0;
	int draggableTiers = 0;
	int order = 99;                     // sort order for the category list
};

struct TierListIndex {
	std::vector<TierEntry> tiers;        // 1:1 with FilterFile::blocks, same order
	std::vector<TierCategory> categories;// by first appearance of the top-level type
};

// Parse "%style $type->a->b $tier->name" tokens out of a block header comment.
// Tolerant: scans all tokens, keeps the ones it recognises, ignores the rest
// (some blocks have no %style, some carry extra $tokens).
TierMarkers ParseTierMarkers(const std::string& headerComment);

// Build the tier-list index from a parsed filter.
TierListIndex BuildTierList(const FilterFile& f);

enum class TierMoveResult { Ok, SameTier, DstNotDraggable, ItemMissing, Duplicate };

// Move one BaseType value from the src block to the dst block (both identified by
// FilterFile::blocks index). Only the two BaseType lines are touched + marked
// dirty. Returns a status the UI can surface.
TierMoveResult MoveItemToTier(FilterFile& f, int srcBlockIndex, int dstBlockIndex,
                              const std::string& itemText);
