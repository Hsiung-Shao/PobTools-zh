// Bookmark folders of the Poe Regex tool: one level, one list per game.
// Ported from exile-appraiser regex/src/folders.ts (step 36); every function
// names the TS function it mirrors, and --regex-selftest replays recorded
// operation sequences from that TS (regex_r5_golden.inc) to identical files.
//
// - A bookmark's `folder` (empty = uncategorised); the folder list (name, order,
//   folded) per game in RegexUiState::folders, the uncategorised fold state in
//   `uncatCollapsed`.
// - ORDER INVARIANT: within one game, `bookmarks` is always sorted "folder order,
//   then uncategorised" (SortBookmarks: stable, and only within the slots that
//   game already occupies, so the other game's / orphan bookmarks never move).
//   Screen order = array order, so no separate ordering is stored.
// - Bookmarks are addressed by index into `bookmarks`; a move returns the new one.
// - Pure: modifies a RegexUiState in place, no ImGui, no files.
#pragma once

#include "regex_state.h"

#include <map>
#include <string>
#include <vector>

namespace RegexFolders {

constexpr int kNameMax = 40;   // FOLDER_NAME_MAX, in code points

// normalizeFolderName: trim, collapse inner whitespace runs to one space, cut to 40.
std::string NormalizeName(const std::string& name);

// sortBookmarks. `track` (optional): an index into bookmarks, updated to where
// that bookmark ended up. Returns whether anything moved.
bool SortBookmarks(RegexUiState& s, const std::string& game, int* track = nullptr);

// normalizeFolders: names normalised / deduplicated / non-empty; a bookmark
// naming a folder that is not listed adds it at the end (nothing is dropped);
// both games sorted. Returns whether anything changed.
bool Normalize(RegexUiState& s);

enum class Result { Ok, Empty, Duplicate, Missing };
const char* ResultId(Result r);   // "ok" / "empty" / "duplicate" / "missing"

Result Add(RegexUiState& s, const std::string& game, const std::string& name);
// Bookmarks follow; renaming to the same name is Ok and changes nothing.
Result Rename(RegexUiState& s, const std::string& game, const std::string& from, const std::string& to);
// Bookmarks go back to uncategorised (never deleted). Returns how many; -1 = no such folder.
int Delete(RegexUiState& s, const std::string& game, const std::string& name);
// moveFolderTo (clamped) / moveFolderBy (-1 up, +1 down).
bool MoveTo(RegexUiState& s, const std::string& game, const std::string& name, int to);
bool MoveBy(RegexUiState& s, const std::string& game, const std::string& name, int delta);

// "" = uncategorised.
bool IsCollapsed(const RegexUiState& s, const std::string& game, const std::string& folder);
bool SetCollapsed(RegexUiState& s, const std::string& game, const std::string& folder, bool on);

// moveBookmark: to the end of `folder` ("" = uncategorised); or, with `before`
// >= 0 (another bookmark of the same game), in front of that one and into its
// folder (a drop on a bookmark). -1 = refused (no such folder, other game, an
// orphan). Returns the bookmark's new index.
int MoveBookmark(RegexUiState& s, int index, const std::string& folder, int before = -1);
// moveBookmarkBy: one step within its folder; at either end = unchanged. New index.
int MoveBookmarkBy(RegexUiState& s, int index, int delta);

// groupBookmarks
struct Group {
	std::string folder;          // "" = uncategorised
	bool collapsed = false;
	std::vector<int> items;      // indices into bookmarks
};
struct Grouped {
	std::vector<Group> groups;
	// false = only the uncategorised group: no headers drawn, folding ignored
	// (looks exactly like the list before folders existed).
	bool headers = false;
};
Grouped GroupBookmarks(const RegexUiState& s, const std::string& game, bool skipEmpty = false);

// folderCounts ("" = uncategorised)
std::map<std::string, int> Counts(const RegexUiState& s, const std::string& game);

} // namespace RegexFolders
